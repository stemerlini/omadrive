#include "graph.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <atomic>
#include <memory>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QUrlQuery>

namespace {

const char kScope[] = "Files.ReadWrite offline_access User.Read";
const char kApi[] = "https://graph.microsoft.com/v1.0/me/drive/root";
constexpr qint64 kSimpleUploadLimit = 4000000;
// Upload session chunks must be a multiple of 320 KiB.
constexpr qint64 kChunk = 32 * 320 * 1024;

QString tr(const char *text) {
    return QCoreApplication::translate("Graph", text);
}

// The refresh token is kept in the keyring through libsecret's secret-tool.
bool keyring(const QStringList &args, const QByteArray &input, QByteArray *output) {
    QProcess process;
    process.start("secret-tool", args);
    if (!process.waitForStarted(3000))
        return false;
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(20000) || process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}

const QStringList kKeyAttributes = {"service", "omadrive", "kind", "refresh-token"};

void forgetToken() {
    QStringList args = {"clear"};
    args += kKeyAttributes;
    keyring(args, {}, nullptr);
}

// One network thread and manager for the whole app, so HTTP connections stay
// open between requests instead of paying a new TLS handshake each time.
struct Io {
    QThread thread;
    QObject *context = nullptr;
    QNetworkAccessManager *manager = nullptr;
    std::atomic<bool> alive{true};

    Io() {
        context = new QObject;
        context->moveToThread(&thread);
        thread.start();
        QMetaObject::invokeMethod(context, [this] { manager = new QNetworkAccessManager(context); },
                                  Qt::BlockingQueuedConnection);
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [this] { shutdown(); });
    }

    void shutdown() {
        if (!alive.exchange(false))
            return;
        QMetaObject::invokeMethod(context, [this] { delete manager; manager = nullptr; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        delete context;
    }

    ~Io() { shutdown(); }
};

Io &ioThread() {
    static Io io;
    return io;
}

QString apiError(const GraphReply &reply) {
    const QJsonObject error = QJsonDocument::fromJson(reply.body).object().value("error").toObject();
    const QString message = error.value("message").toString();
    return message.isEmpty() ? tr("OneDrive answered with HTTP %1").arg(reply.status) : message;
}

}  // namespace

QJsonObject GraphReply::json() const {
    return QJsonDocument::fromJson(body).object();
}

Graph &Graph::instance() {
    static Graph graph;
    return graph;
}

Graph::Graph() {
    QFile file(configPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject config = QJsonDocument::fromJson(file.readAll()).object();
        m_clientId = config.value("clientId").toString().trimmed();
        m_tenant = config.value("tenant").toString("common").trimmed();
    }
    if (m_tenant.isEmpty())
        m_tenant = "common";
}

QString Graph::configPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive/auth.json";
}

QString Graph::authorizeUrl(const QString &redirect, const QString &challenge, const QString &state) const {
    QUrlQuery query;
    query.addQueryItem("client_id", m_clientId);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("redirect_uri", redirect);
    query.addQueryItem("response_mode", "query");
    query.addQueryItem("scope", kScope);
    query.addQueryItem("code_challenge", challenge);
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("state", state);
    query.addQueryItem("prompt", "select_account");
    return QString("https://login.microsoftonline.com/%1/oauth2/v2.0/authorize?").arg(m_tenant)
        + query.toString(QUrl::FullyEncoded);
}

QString Graph::itemUrl(const QString &path) {
    if (path.isEmpty() || path == "/")
        return kApi;
    QStringList parts;
    for (const QString &part : path.split('/', Qt::SkipEmptyParts))
        parts << QString::fromLatin1(QUrl::toPercentEncoding(part));
    return QString(kApi) + ":/" + parts.join('/') + ":";
}

QString Graph::joinPath(const QString &parent, const QString &name) {
    return parent == "/" || parent.isEmpty() ? "/" + name : parent + "/" + name;
}

QString Graph::parentPath(const QString &path) {
    const int slash = path.lastIndexOf('/');
    return slash <= 0 ? QString("/") : path.left(slash);
}

GraphReply Graph::send(const QByteArray &verb, const QUrl &url, const QByteArray &body,
                       const QList<QPair<QByteArray, QByteArray>> &headers, int timeoutMs) {
    GraphReply out;
    Io &io = ioThread();
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!io.alive) {
            out.error = tr("The app is closing");
            return out;
        }
        QSemaphore done;
        int retryAfter = 0;
        // The request runs on the shared network thread so its connections
        // (and TLS sessions) are reused; this worker just waits for it.
        QMetaObject::invokeMethod(io.context, [&] {
            QNetworkRequest request(url);
            request.setTransferTimeout(timeoutMs);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            for (const auto &header : headers)
                request.setRawHeader(header.first, header.second);
            QNetworkReply *reply = io.manager->sendCustomRequest(request, verb, body);
            QObject::connect(reply, &QNetworkReply::finished, io.context, [&, reply] {
                out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                out.body = reply->readAll();
                out.error.clear();
                if (out.status == 0)
                    out.error = reply->errorString();
                else if (out.status >= 400)
                    out.error = apiError(out);
                retryAfter = reply->rawHeader("Retry-After").toInt();
                reply->deleteLater();
                done.release();
            });
        });
        done.acquire();

        if ((out.status == 429 || out.status == 503) && attempt < 3) {
            QThread::sleep(qBound(1, retryAfter, 30));
            continue;
        }
        break;
    }
    return out;
}

// Caller holds the mutex.
bool Graph::tokenRequest(const QList<QPair<QString, QString>> &form, QString *error) {
    QUrlQuery query;
    query.addQueryItem("client_id", m_clientId);
    query.addQueryItem("scope", kScope);
    for (const auto &item : form)
        query.addQueryItem(item.first, item.second);

    const GraphReply reply = send(
        "POST", QUrl(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/token").arg(m_tenant)),
        query.toString(QUrl::FullyEncoded).toUtf8(), {{"Content-Type", "application/x-www-form-urlencoded"}});

    const QJsonObject json = reply.json();
    if (reply.status != 200 || !json.contains("access_token")) {
        QString message = json.value("error_description").toString().section('\n', 0, 0);
        if (message.isEmpty())
            message = reply.error.isEmpty() ? tr("Sign-in failed") : reply.error;
        // A revoked or expired sign-in cannot be repaired by retrying.
        if (json.value("error").toString() == "invalid_grant") {
            m_refresh.clear();
            forgetToken();
        }
        *error = message;
        return false;
    }

    m_access = json.value("access_token").toString();
    m_expires = QDateTime::currentDateTimeUtc().addSecs(json.value("expires_in").toInt(3600));
    const QString refresh = json.value("refresh_token").toString();
    if (!refresh.isEmpty()) {
        m_refresh = refresh;
        QStringList args = {"store", "--label=Omadrive OneDrive sign-in"};
        args += kKeyAttributes;
        keyring(args, refresh.toUtf8(), nullptr);
    }
    return true;
}

bool Graph::exchangeCode(const QString &code, const QString &verifier, const QString &redirect, QString *error) {
    QMutexLocker lock(&m_mutex);
    return tokenRequest({{"grant_type", "authorization_code"}, {"code", code},
                         {"redirect_uri", redirect}, {"code_verifier", verifier}}, error);
}

bool Graph::restore(QString *error) {
    QMutexLocker lock(&m_mutex);
    QByteArray stored;
    QStringList args = {"lookup"};
    args += kKeyAttributes;
    if (!keyring(args, {}, &stored) || stored.trimmed().isEmpty())
        return false;
    m_refresh = QString::fromUtf8(stored.trimmed());
    return tokenRequest({{"grant_type", "refresh_token"}, {"refresh_token", m_refresh}}, error);
}

void Graph::signOut() {
    QMutexLocker lock(&m_mutex);
    m_access.clear();
    m_refresh.clear();
    forgetToken();
}

bool Graph::signedIn() const {
    QMutexLocker lock(&m_mutex);
    return !m_refresh.isEmpty();
}

QString Graph::accessToken(QString *error) {
    QMutexLocker lock(&m_mutex);
    if (!m_access.isEmpty() && QDateTime::currentDateTimeUtc() < m_expires.addSecs(-60))
        return m_access;
    if (m_refresh.isEmpty()) {
        *error = tr("Not signed in to OneDrive");
        return {};
    }
    if (!tokenRequest({{"grant_type", "refresh_token"}, {"refresh_token", m_refresh}}, error))
        return {};
    return m_access;
}

QString Graph::displayName() {
    const GraphReply reply = call("GET", "https://graph.microsoft.com/v1.0/me?$select=displayName,userPrincipalName");
    const QJsonObject json = reply.json();
    const QString name = json.value("displayName").toString();
    return name.isEmpty() ? json.value("userPrincipalName").toString() : name;
}

GraphReply Graph::call(const QByteArray &verb, const QString &urlOrPath, const QByteArray &body,
                       const QByteArray &contentType) {
    GraphReply out;
    const QString token = accessToken(&out.error);
    if (token.isEmpty())
        return out;
    QList<QPair<QByteArray, QByteArray>> headers = {{"Authorization", "Bearer " + token.toUtf8()}};
    if (!body.isEmpty())
        headers.append({"Content-Type", contentType});
    return send(verb, QUrl(urlOrPath.startsWith("http") ? urlOrPath : QString(kApi) + urlOrPath), body, headers);
}

QList<QJsonObject> Graph::children(const QString &path, QString *error) {
    QList<QJsonObject> items;
    QString url = itemUrl(path) + "/children?$top=200&$select=name,size,folder,file,lastModifiedDateTime";
    while (!url.isEmpty()) {
        const GraphReply reply = call("GET", url);
        if (!reply.ok()) {
            *error = reply.error;
            return {};
        }
        const QJsonObject json = reply.json();
        for (const QJsonValue &value : json.value("value").toArray())
            items.append(value.toObject());
        url = json.value("@odata.nextLink").toString();
    }
    return items;
}

bool Graph::download(const QUrl &url, const QString &file, const std::function<void(qint64)> &progress, QString *error) {
    Io &io = ioThread();
    if (!io.alive) {
        *error = tr("The app is closing");
        return false;
    }
    QSemaphore done;
    bool ok = false;
    QString problem;
    QMetaObject::invokeMethod(io.context, [&] {
        auto *out = new QFile(file);
        if (!out->open(QIODevice::WriteOnly)) {
            problem = tr("Cannot write %1").arg(file);
            delete out;
            done.release();
            return;
        }

        // The download URL is pre-authorised, so no token is sent to it.
        QNetworkRequest request(url);
        request.setTransferTimeout(60000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = io.manager->get(request);
        auto received = std::make_shared<qint64>(0);
        QObject::connect(reply, &QNetworkReply::readyRead, io.context, [=, &progress] {
            const QByteArray data = reply->readAll();
            out->write(data);
            *received += data.size();
            progress(*received);
        });
        QObject::connect(reply, &QNetworkReply::finished, io.context, [=, &ok, &problem, &done] {
            out->write(reply->readAll());
            out->close();
            delete out;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            ok = reply->error() == QNetworkReply::NoError && status == 200;
            if (!ok)
                problem = reply->error() == QNetworkReply::NoError ? tr("Download failed (HTTP %1)").arg(status)
                                                                   : reply->errorString();
            reply->deleteLater();
            done.release();
        });
    });
    done.acquire();
    if (!ok)
        *error = problem;
    return ok;
}

bool Graph::uploadFile(const QString &localFile, const QString &remoteParent,
                       const std::function<void(qint64)> &progress, QString *error) {
    const QFileInfo info(localFile);
    const QString remote = joinPath(remoteParent, info.fileName());
    if (info.size() > kSimpleUploadLimit)
        return uploadLarge(localFile, remote, progress, error);

    QFile file(localFile);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = tr("Cannot read %1").arg(info.fileName());
        return false;
    }
    const GraphReply reply = call("PUT", itemUrl(remote) + "/content?@microsoft.graph.conflictBehavior=rename",
                                  file.readAll(), "application/octet-stream");
    if (!reply.ok()) {
        *error = reply.error;
        return false;
    }
    progress(info.size());
    return true;
}

bool Graph::uploadLarge(const QString &localFile, const QString &remotePath,
                        const std::function<void(qint64)> &progress, QString *error) {
    QFile file(localFile);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = tr("Cannot read %1").arg(QFileInfo(localFile).fileName());
        return false;
    }

    const QJsonObject item{{"@microsoft.graph.conflictBehavior", "rename"}};
    const GraphReply session = call("POST", itemUrl(remotePath) + "/createUploadSession",
                                    QJsonDocument(QJsonObject{{"item", item}}).toJson(QJsonDocument::Compact));
    const QString uploadUrl = session.json().value("uploadUrl").toString();
    if (!session.ok() || uploadUrl.isEmpty()) {
        *error = session.ok() ? tr("OneDrive refused the upload") : session.error;
        return false;
    }

    const qint64 total = file.size();
    qint64 sent = 0;
    while (sent < total) {
        const QByteArray chunk = file.read(kChunk);
        const QByteArray range = QString("bytes %1-%2/%3").arg(sent).arg(sent + chunk.size() - 1).arg(total).toUtf8();

        GraphReply reply;
        for (int attempt = 0; attempt < 3; ++attempt) {
            reply = send("PUT", QUrl(uploadUrl), chunk, {{"Content-Range", range}}, 180000);
            if (reply.ok())
                break;
        }
        if (!reply.ok()) {
            send("DELETE", QUrl(uploadUrl), {}, {});  // drop the unfinished session
            *error = reply.error;
            return false;
        }
        sent += chunk.size();
        progress(sent);
    }
    return true;
}

QString Graph::createFolder(const QString &remoteParent, const QString &name, bool failIfExists, QString *error) {
    const QJsonObject body{{"name", name}, {"folder", QJsonObject()},
                           {"@microsoft.graph.conflictBehavior", failIfExists ? "fail" : "rename"}};
    const GraphReply reply = call("POST", itemUrl(remoteParent) + "/children",
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!reply.ok()) {
        *error = reply.status == 409 ? tr("%1 already exists").arg(name) : reply.error;
        return {};
    }
    return reply.json().value("name").toString(name);
}
