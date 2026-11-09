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

// Before accounts had ids the one token was saved without "account".
// secret-tool matches items having at least the given attributes, so the
// accounts' tokens use another "kind" for these never to match them.
const QStringList kLegacyAttributes = {"service", "omadrive", "kind", "refresh-token"};

QStringList keyAttributes(const QString &id) {
    return {"service", "omadrive", "kind", "account-token", "account", id};
}

// Where early multi-account builds kept an account's token.
QStringList oldKeyAttributes(const QString &id) {
    return kLegacyAttributes + QStringList{"account", id};
}

void forgetToken(const QString &id) {
    if (!id.isEmpty())
        keyring(QStringList{"clear"} + keyAttributes(id), {}, nullptr);
}

struct Config {
    QString clientId, tenant = "common";
};

const Config &config() {
    static const Config loaded = [] {
        Config c;
        QFile file(Graph::configPath());
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
            c.clientId = json.value("clientId").toString().trimmed();
            c.tenant = json.value("tenant").toString("common").trimmed();
        }
        if (c.tenant.isEmpty())
            c.tenant = "common";
        return c;
    }();
    return loaded;
}

struct Registry {
    QMutex mutex;
    QHash<QString, Graph *> graphs;
    QString active;
};

Registry &registry() {
    static Registry r;
    return r;
}

// Per-account app (client id, tenant). Empty means use auth.json's default.
struct Apps {
    QMutex mutex;
    QHash<QString, QPair<QString, QString>> byId;
};

Apps &apps() {
    static Apps a;
    return a;
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

Graph *Graph::forAccount(const QString &id) {
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    Graph *&graph = r.graphs[id];
    if (!graph)
        graph = new Graph(id);
    return graph;
}

Graph &Graph::active() {
    QString id;
    {
        QMutexLocker lock(&registry().mutex);
        id = registry().active;
    }
    // With no account this is an empty Graph whose calls say "not signed in".
    return *forAccount(id);
}

void Graph::setActive(const QString &id) {
    QMutexLocker lock(&registry().mutex);
    registry().active = id;
}

Graph::Graph(const QString &id) : m_id(id) {}

void Graph::setApp(const QString &clientId, const QString &tenant) {
    m_clientId = clientId;
    m_tenant = tenant;
}

void Graph::setApps(const QHash<QString, QPair<QString, QString>> &byId) {
    QMutexLocker lock(&apps().mutex);
    apps().byId = byId;
}

QPair<QString, QString> Graph::appFor(const QString &id) {
    {
        QMutexLocker lock(&apps().mutex);
        const auto it = apps().byId.constFind(id);
        if (it != apps().byId.constEnd() && !it->first.isEmpty()) {
            QString tenant = it->second.isEmpty() ? QString("common") : it->second;
            return {it->first, tenant};
        }
    }
    return {config().clientId, config().tenant};
}

bool Graph::configured() {
    return !config().clientId.isEmpty();
}

QString Graph::configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive/auth.json";
}

QString Graph::authorizeUrl(const QString &redirect, const QString &challenge, const QString &state,
                            const QString &loginHint, const QString &clientId, const QString &tenant) {
    const QString app = clientId.isEmpty() ? config().clientId : clientId;
    QString realm = tenant.isEmpty() ? config().tenant : tenant;
    if (realm.isEmpty())
        realm = "common";
    QUrlQuery query;
    query.addQueryItem("client_id", app);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("redirect_uri", redirect);
    query.addQueryItem("response_mode", "query");
    query.addQueryItem("scope", kScope);
    query.addQueryItem("code_challenge", challenge);
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("state", state);
    if (loginHint.isEmpty())
        query.addQueryItem("prompt", "select_account");
    else
        query.addQueryItem("login_hint", loginHint);
    return QString("https://login.microsoftonline.com/%1/oauth2/v2.0/authorize?").arg(realm)
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
    // A sign-in in progress carries its own app; an established account looks
    // its up by id. Either falls back to auth.json's default.
    const QPair<QString, QString> app = m_clientId.isEmpty() ? appFor(m_id)
                                                             : QPair<QString, QString>{m_clientId, m_tenant.isEmpty() ? QString("common") : m_tenant};
    QUrlQuery query;
    query.addQueryItem("client_id", app.first);
    query.addQueryItem("scope", kScope);
    for (const auto &item : form)
        query.addQueryItem(item.first, item.second);

    const GraphReply reply = send(
        "POST", QUrl(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/token").arg(app.second)),
        query.toString(QUrl::FullyEncoded).toUtf8(), {{"Content-Type", "application/x-www-form-urlencoded"}});

    const QJsonObject json = reply.json();
    if (reply.status != 200 || !json.contains("access_token")) {
        QString message = json.value("error_description").toString().section('\n', 0, 0);
        if (message.isEmpty())
            message = reply.error.isEmpty() ? tr("Sign-in failed") : reply.error;
        // A revoked or expired sign-in cannot be repaired by retrying.
        if (json.value("error").toString() == "invalid_grant") {
            m_refresh.clear();
            forgetToken(m_id);
        }
        *error = message;
        return false;
    }

    m_access = json.value("access_token").toString();
    m_expires = QDateTime::currentDateTimeUtc().addSecs(json.value("expires_in").toInt(3600));
    const QString refresh = json.value("refresh_token").toString();
    if (!refresh.isEmpty()) {
        m_refresh = refresh;
        saveToken();
    }
    return true;
}

// Caller holds the mutex.
void Graph::saveToken() {
    if (m_id.isEmpty() || m_refresh.isEmpty())
        return;
    keyring(QStringList{"store", "--label=Omadrive OneDrive sign-in"} + keyAttributes(m_id), m_refresh.toUtf8(), nullptr);
}

bool Graph::exchangeCode(const QString &code, const QString &verifier, const QString &redirect, QString *error) {
    QMutexLocker lock(&m_mutex);
    return tokenRequest({{"grant_type", "authorization_code"}, {"code", code},
                         {"redirect_uri", redirect}, {"code_verifier", verifier}}, error);
}

bool Graph::restore(QString *error) {
    QMutexLocker lock(&m_mutex);
    if (m_id.isEmpty())
        return false;
    QByteArray stored;
    bool moved = false;
    if (!keyring(QStringList{"lookup"} + keyAttributes(m_id), {}, &stored) || stored.trimmed().isEmpty()) {
        if (!keyring(QStringList{"lookup"} + oldKeyAttributes(m_id), {}, &stored) || stored.trimmed().isEmpty())
            return false;
        moved = true;
    }
    m_refresh = QString::fromUtf8(stored.trimmed());
    const bool ok = tokenRequest({{"grant_type", "refresh_token"}, {"refresh_token", m_refresh}}, error);
    if (moved && (ok || m_refresh.isEmpty())) {
        saveToken();
        keyring(QStringList{"clear"} + oldKeyAttributes(m_id), {}, nullptr);
    }
    return ok;
}

bool Graph::restoreLegacy(QString *error) {
    QMutexLocker lock(&m_mutex);
    QByteArray stored;
    if (!keyring(QStringList{"lookup"} + kLegacyAttributes, {}, &stored) || stored.trimmed().isEmpty())
        return false;
    m_refresh = QString::fromUtf8(stored.trimmed());
    const bool ok = tokenRequest({{"grant_type", "refresh_token"}, {"refresh_token", m_refresh}}, error);
    if (!ok && m_refresh.isEmpty())
        clearLegacy();  // rejected for good
    return ok;
}

bool Graph::hasLegacy() {
    QByteArray stored;
    return keyring(QStringList{"lookup"} + kLegacyAttributes, {}, &stored) && !stored.trimmed().isEmpty();
}

void Graph::clearLegacy() {
    keyring(QStringList{"clear"} + kLegacyAttributes, {}, nullptr);
}

void Graph::adopt(Graph &signedIn) {
    QString access, refresh;
    QDateTime expires;
    {
        QMutexLocker lock(&signedIn.m_mutex);
        access = signedIn.m_access;
        refresh = signedIn.m_refresh;
        expires = signedIn.m_expires;
    }
    QMutexLocker lock(&m_mutex);
    m_access = access;
    m_refresh = refresh;
    m_expires = expires;
    saveToken();
}

void Graph::signOut() {
    QMutexLocker lock(&m_mutex);
    m_access.clear();
    m_refresh.clear();
    forgetToken(m_id);
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

Profile Graph::profile(QString *error) {
    Profile p;
    const GraphReply me = call("GET", "https://graph.microsoft.com/v1.0/me?$select=id,displayName,userPrincipalName,mail");
    if (!me.ok()) {
        *error = me.error;
        return p;
    }
    const QJsonObject json = me.json();
    p.id = json.value("id").toString();
    p.email = json.value("mail").toString();
    if (p.email.isEmpty())
        p.email = json.value("userPrincipalName").toString();
    p.name = json.value("displayName").toString();
    if (p.name.isEmpty())
        p.name = p.email;

    const GraphReply drive = call("GET", "https://graph.microsoft.com/v1.0/me/drive?$select=driveType");
    p.business = drive.json().value("driveType").toString() == "business";
    if (p.business) {
        const GraphReply org = call("GET", "https://graph.microsoft.com/v1.0/organization?$select=displayName");
        const QJsonArray orgs = org.json().value("value").toArray();
        if (!orgs.isEmpty())
            p.org = orgs.first().toObject().value("displayName").toString();
        if (p.org.isEmpty())
            p.org = p.email.section('@', 1).section('.', 0, 0).toUpper();
    }
    if (p.id.isEmpty())
        *error = tr("Microsoft did not say who signed in");
    return p;
}

QByteArray Graph::photo() {
    for (const char *url : {"https://graph.microsoft.com/v1.0/me/photos/96x96/$value",
                            "https://graph.microsoft.com/v1.0/me/photo/$value"}) {
        const GraphReply reply = call("GET", url);
        if (reply.status == 200 && !reply.body.isEmpty())
            return reply.body;
    }
    return {};
}

GraphReply Graph::call(const QByteArray &verb, const QString &urlOrPath, const QByteArray &body,
                       const QByteArray &contentType, const QList<QPair<QByteArray, QByteArray>> &extra) {
    GraphReply out;
    const QString token = accessToken(&out.error);
    if (token.isEmpty())
        return out;
    QList<QPair<QByteArray, QByteArray>> headers = {{"Authorization", "Bearer " + token.toUtf8()}};
    if (!body.isEmpty() || verb == "PUT")
        headers.append({"Content-Type", contentType});
    headers += extra;
    return send(verb, QUrl(urlOrPath.startsWith("http") ? urlOrPath : QString(kApi) + urlOrPath), body, headers);
}

QList<QJsonObject> Graph::children(const QString &path, QString *error) {
    QList<QJsonObject> items;
    QString url = itemUrl(path) + "/children?$top=200&$select=name,size,folder,file,lastModifiedDateTime,fileSystemInfo";
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
    stampModified(reply.json(), info.lastModified());
    return true;
}

bool Graph::uploadLarge(const QString &localFile, const QString &remotePath,
                        const std::function<void(qint64)> &progress, QString *error) {
    const GraphReply reply = uploadSession(itemUrl(remotePath) + "/createUploadSession",
                                           {{"@microsoft.graph.conflictBehavior", "rename"}}, {}, localFile, progress);
    if (!reply.ok()) {
        *error = reply.error;
        return false;
    }
    stampModified(reply.json(), QFileInfo(localFile).lastModified());
    return true;
}

GraphReply Graph::uploadSession(const QString &sessionUrl, const QJsonObject &item,
                                const QList<QPair<QByteArray, QByteArray>> &headers, const QString &localFile,
                                const std::function<void(qint64)> &progress) {
    GraphReply out;
    QFile file(localFile);
    if (!file.open(QIODevice::ReadOnly)) {
        out.error = tr("Cannot read %1").arg(QFileInfo(localFile).fileName());
        return out;
    }

    const GraphReply session = call("POST", sessionUrl, QJsonDocument(QJsonObject{{"item", item}}).toJson(QJsonDocument::Compact),
                                    "application/json", headers);
    const QString uploadUrl = session.json().value("uploadUrl").toString();
    if (!session.ok() || uploadUrl.isEmpty()) {
        out = session;
        if (out.ok())
            out.error = tr("OneDrive refused the upload");
        return out;
    }

    const qint64 total = file.size();
    qint64 sent = 0;
    while (sent < total) {
        const QByteArray chunk = file.read(kChunk);
        const QByteArray range = QString("bytes %1-%2/%3").arg(sent).arg(sent + chunk.size() - 1).arg(total).toUtf8();

        for (int attempt = 0; attempt < 3; ++attempt) {
            out = send("PUT", QUrl(uploadUrl), chunk, {{"Content-Range", range}}, 180000);
            if (out.ok() || out.status == 409 || out.status == 412)
                break;
        }
        if (!out.ok()) {
            send("DELETE", QUrl(uploadUrl), {}, {});  // drop the unfinished session
            return out;
        }
        sent += chunk.size();
        progress(sent);
    }
    return out;
}

QString Graph::delta(const QString &link, const QString &select, const std::function<void(const QJsonObject &)> &onItem,
                     const std::function<void()> &onReset, QString *error) {
    const QString fresh = "https://graph.microsoft.com/v1.0/me/drive/root/delta?$top=1000&$select=" + select;
    QString url = link.isEmpty() ? fresh : link;
    QString next;
    while (!url.isEmpty()) {
        const GraphReply reply = call("GET", url);
        if (reply.status == 410 && url != fresh) {  // the link expired: read everything again
            onReset();
            url = fresh;
            continue;
        }
        if (!reply.ok()) {
            *error = reply.error;
            return {};
        }
        const QJsonObject page = reply.json();
        for (const QJsonValue &v : page.value("value").toArray())
            onItem(v.toObject());
        url = page.value("@odata.nextLink").toString();
        next = page.value("@odata.deltaLink").toString(next);
    }
    if (next.isEmpty())
        *error = tr("OneDrive did not finish the change list");
    return next;
}

// Keeps the local file's modification time on OneDrive, like the Windows
// client does, so the mirror copy is recognised as the same file.
void Graph::stampModified(const QJsonObject &item, const QDateTime &modified) {
    const QString id = item.value("id").toString();
    if (id.isEmpty() || !modified.isValid())
        return;
    const QJsonObject info{{"lastModifiedDateTime", modified.toUTC().toString(Qt::ISODate)}};
    call("PATCH", "https://graph.microsoft.com/v1.0/me/drive/items/" + id, QJsonDocument(QJsonObject{{"fileSystemInfo", info}}).toJson(QJsonDocument::Compact));
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
