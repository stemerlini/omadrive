#include "account.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent>

#include "mirror.h"

namespace {

QString randomToken(int bytes) {
    QByteArray data(bytes, 0);
    QRandomGenerator::system()->generate(data.begin(), data.end());
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

const char kPage[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><meta charset=utf-8><title>Omadrive</title>"
    "<body style=\"font-family:sans-serif;background:#0e0e10;color:#e6e6e6;text-align:center;padding-top:20vh\">"
    "<h2>Omadrive</h2><p>You can close this tab and go back to the app.</p></body>";

QString accountsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive/accounts.json";
}

QString driveNameOf(const Profile &p) {
    return p.business ? QString("OneDrive - %1").arg(p.org) : QString("OneDrive");
}

// "Stefano Merlini" -> "SM"; "Merlini, Stefano" (as work directories write it) -> "SM".
QString initials(const QString &name) {
    QString text = name;
    if (text.contains(','))
        text = text.section(',', 1).trimmed() + " " + text.section(',', 0, 0).trimmed();
    QString out;
    for (const QString &word : text.split(' ', Qt::SkipEmptyParts)) {
        if (word.at(0).isLetterOrNumber())
            out += word.at(0).toUpper();
        if (out.size() == 2)
            break;
    }
    return out.isEmpty() ? QString("?") : out;
}

// A round picture: the profile photo, or initials on a colour of their own.
QImage roundAvatar(const QByteArray &photo, const Profile &p) {
    constexpr int size = 96;
    QImage out(size, size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    QPainterPath circle;
    circle.addEllipse(0, 0, size, size);
    painter.setClipPath(circle);

    QImage picture;
    if (!photo.isEmpty())
        picture.loadFromData(photo);
    if (!picture.isNull()) {
        picture = picture.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        painter.drawImage((size - picture.width()) / 2, (size - picture.height()) / 2, picture);
        return out;
    }
    static const QList<QColor> palette = {"#7aa2f7", "#bb9af7", "#9ece6a", "#e0af68", "#f7768e", "#7dcfff", "#ff9e64"};
    painter.fillRect(out.rect(), palette.at(qHash(p.id) % palette.size()));
    QFont font;
    font.setBold(true);
    font.setPixelSize(38);
    painter.setFont(font);
    painter.setPen(QColor("#16161e"));
    painter.drawText(out.rect(), Qt::AlignCenter, initials(p.name));
    return out;
}

}  // namespace

Account::Account(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(180000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        stopServer();
        setBusy(false);
        emit notice(tr("Sign-in timed out"), true);
        changed();
    });
}

Account::Entry *Account::entry(const QString &id) {
    for (Entry &e : m_entries)
        if (e.profile.id == id)
            return &e;
    return nullptr;
}

const Account::Entry *Account::activeEntry() const {
    for (const Entry &e : m_entries)
        if (e.profile.id == m_activeId)
            return &e;
    return nullptr;
}

QString Account::state() const {
    const Entry *e = activeEntry();
    if (e && e->status == "ready")
        return "signedIn";
    if ((e && e->status == "connecting") || (m_signingIn && (!e || e->status == "expired")))
        return "signingIn";
    return "signedOut";
}

bool Account::restoring() const {
    const Entry *e = activeEntry();
    // Without a browser sign-in open, signing in means moving the old saved one.
    return (e && e->status == "connecting") || (!e && m_signingIn && !m_server);
}

QString Account::name() const {
    const Entry *e = activeEntry();
    return e ? e->profile.name : QString();
}

QString Account::email() const {
    const Entry *e = activeEntry();
    return e ? e->profile.email : QString();
}

bool Account::business() const {
    const Entry *e = activeEntry();
    return e && e->profile.business;
}

QString Account::driveName() const {
    const Entry *e = activeEntry();
    return e ? driveNameOf(e->profile) : QString("OneDrive");
}

QString Account::avatarFile(const QString &id) const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omadrive/avatars/"
        + QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex() + ".png";
}

QString Account::avatar() const {
    const Entry *e = activeEntry();
    if (!e || !QFile::exists(avatarFile(e->profile.id)))
        return {};
    // The version makes the image reload when the picture changes.
    return QUrl::fromLocalFile(avatarFile(e->profile.id)).toString() + "?v=" + QString::number(e->avatarVersion);
}

QVariantList Account::accounts() const {
    QVariantList list;
    for (const Entry &e : m_entries) {
        const QString file = avatarFile(e.profile.id);
        list.append(QVariantMap{
            {"id", e.profile.id}, {"name", e.profile.name}, {"email", e.profile.email}, {"org", e.profile.org},
            {"business", e.profile.business}, {"driveName", driveNameOf(e.profile)}, {"status", e.status},
            {"active", e.profile.id == m_activeId}, {"mirror", e.mirror},
            {"avatar", QFile::exists(file) ? QUrl::fromLocalFile(file).toString() + "?v=" + QString::number(e.avatarVersion)
                                           : QString()}});
    }
    return list;
}

QString Account::defaultMirror(const Profile &profile, const QStringList &taken) {
    const QString configured = Mirror::configuredRoot();
    if (!profile.business && !taken.contains(configured))
        return configured;
    QString label = profile.business ? profile.org : profile.email.section('@', 0, 0);
    label.replace('/', '-');
    const QString base = QDir::homePath() + "/OneDrive - " + label.trimmed();
    QString path = base;
    for (int n = 2; taken.contains(path); ++n)
        path = QString("%1 (%2)").arg(base).arg(n);
    return path;
}

void Account::load() {
    QFile file(accountsPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &value : json.value("accounts").toArray()) {
        const QJsonObject a = value.toObject();
        Entry e;
        e.profile.id = a.value("id").toString();
        e.profile.name = a.value("name").toString();
        e.profile.email = a.value("email").toString();
        e.profile.org = a.value("org").toString();
        e.profile.business = a.value("business").toBool();
        e.mirror = a.value("mirror").toString();
        if (e.profile.id.isEmpty() || entry(e.profile.id))
            continue;
        if (e.mirror.isEmpty()) {
            QStringList taken;
            for (const Entry &other : std::as_const(m_entries))
                taken << other.mirror;
            e.mirror = defaultMirror(e.profile, taken);
        }
        m_entries.append(e);
    }
    m_activeId = json.value("active").toString();
    if (!activeEntry())
        m_activeId = m_entries.isEmpty() ? QString() : m_entries.first().profile.id;
}

void Account::save() const {
    QJsonArray list;
    for (const Entry &e : m_entries)
        list.append(QJsonObject{{"id", e.profile.id}, {"name", e.profile.name}, {"email", e.profile.email},
                                {"org", e.profile.org}, {"business", e.profile.business}, {"mirror", e.mirror}});
    QDir().mkpath(QFileInfo(accountsPath()).absolutePath());
    QFile file(accountsPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(QJsonObject{{"active", m_activeId}, {"accounts", list}}).toJson());
}

// Every list re-reads on stateChanged, so it only fires when the active
// account or its state really changed.
void Account::changed() {
    const Entry *e = activeEntry();
    Graph::setActive(m_activeId);
    Mirror::setRoot(e ? e->mirror : QString());
    emit accountsChanged();
    const QString key = m_activeId + "|" + state();
    if (key != m_lastKey) {
        m_lastKey = key;
        emit stateChanged();
    }
}

void Account::run(std::function<Outcome()> job, std::function<void(const Outcome &)> done) {
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [watcher, done] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        done(outcome);
    });
    watcher->setFuture(QtConcurrent::run(job));
}

void Account::writeAvatar(Entry &e, const QByteArray &photo) {
    const QString file = avatarFile(e.profile.id);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (roundAvatar(photo, e.profile).save(file, "PNG"))
        ++e.avatarVersion;
}

void Account::restore() {
    if (!Graph::configured())
        return;
    const bool known = QFile::exists(accountsPath());
    load();
    for (const Entry &e : std::as_const(m_entries))
        connectAccount(e.profile.id);
    changed();
    if (known)
        return;

    // The sign-in saved before there could be several accounts.
    setBusy(true);
    changed();
    run([] {
        Outcome outcome;
        outcome.legacy = true;
        Graph pending;
        if (pending.restoreLegacy(&outcome.error))
            outcome.profile = pending.profile(&outcome.error);
        if (!outcome.profile.id.isEmpty() && outcome.error.isEmpty()) {
            Graph *graph = Graph::forAccount(outcome.profile.id);
            graph->adopt(pending);
            Graph::clearLegacy();
            outcome.photo = graph->photo();
            outcome.ok = true;
        } else {
            outcome.retry = Graph::hasLegacy();  // say offline: keep it for next time
        }
        return outcome;
    }, [this](const Outcome &outcome) {
        setBusy(false);
        if (outcome.ok) {
            welcome(outcome);
        } else {
            if (!outcome.retry)
                save();
            if (!outcome.error.isEmpty())
                emit notice(outcome.error, true);
            changed();
        }
    });
}

void Account::connectAccount(const QString &id) {
    if (Entry *e = entry(id))
        e->status = "connecting";
    run([id] {
        Outcome outcome;
        Graph *graph = Graph::forAccount(id);
        if (!graph->restore(&outcome.error))
            return outcome;
        outcome.profile = graph->profile(&outcome.error);
        outcome.ok = outcome.profile.id == id;
        if (outcome.ok)
            outcome.photo = graph->photo();
        return outcome;
    }, [this, id](const Outcome &outcome) {
        Entry *e = entry(id);
        if (!e)
            return;  // signed out meanwhile
        e->status = outcome.ok ? "ready" : "expired";
        if (outcome.ok) {
            e->profile = outcome.profile;
            writeAvatar(*e, outcome.photo);
            save();
        } else if (id == m_activeId && !outcome.error.isEmpty()) {
            emit notice(outcome.error, true);
        }
        if (!QFile::exists(avatarFile(id)))
            writeAvatar(*e, {});
        changed();
    });
}

// A sign-in finished: the account is added (or refreshed) and shown.
void Account::welcome(const Outcome &outcome) {
    Entry *e = entry(outcome.profile.id);
    if (!e) {
        Entry fresh;
        QStringList taken;
        for (const Entry &other : std::as_const(m_entries))
            taken << other.mirror;
        // The account that was signed in before keeps the folder it used.
        fresh.mirror = outcome.legacy ? Mirror::configuredRoot() : defaultMirror(outcome.profile, taken);
        m_entries.append(fresh);
        e = &m_entries.last();
    }
    e->profile = outcome.profile;
    e->status = "ready";
    writeAvatar(*e, outcome.photo);
    m_activeId = e->profile.id;
    save();
    changed();
    emit notice(tr("Signed in as %1 (%2)").arg(e->profile.name, driveNameOf(e->profile)), false);
}

void Account::signIn() {
    const Entry *e = activeEntry();
    interactive(e ? e->profile.email : QString());
}

void Account::addAccount() {
    interactive({});
}

void Account::interactive(const QString &hint) {
    if (m_signingIn)
        return;
    if (!Graph::configured()) {
        emit notice(tr("Set clientId in %1 first").arg(Graph::configPath()), true);
        return;
    }

    stopServer();
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        stopServer();
        emit notice(tr("Cannot listen for the Microsoft sign-in redirect"), true);
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &Account::handleConnection);

    m_verifier = randomToken(48);
    m_expectedState = randomToken(16);
    m_redirect = QString("http://localhost:%1").arg(m_server->serverPort());
    const QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    setBusy(true);
    m_timeout.start();
    changed();
    QDesktopServices::openUrl(QUrl(Graph::authorizeUrl(m_redirect, challenge, m_expectedState, hint)));
}

void Account::handleConnection() {
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        if (!socket->canReadLine())
            return;
        // "GET /?code=...&state=... HTTP/1.1"
        const QList<QByteArray> parts = socket->readLine().split(' ');
        const QString target = parts.size() > 1 ? QString::fromLatin1(parts.at(1)) : QString();
        const QUrlQuery query(QUrl("http://localhost" + target));
        if (!query.hasQueryItem("code") && !query.hasQueryItem("error")) {
            socket->disconnectFromHost();  // e.g. the browser asking for a favicon
            return;
        }

        // The socket is owned by the server, which stopServer() deletes; keep
        // it alive until the page has been fully sent.
        socket->setParent(this);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        socket->write(kPage);
        socket->disconnectFromHost();
        stopServer();

        QString problem;
        if (query.queryItemValue("state") != m_expectedState) {
            problem = tr("Sign-in was rejected: the state did not match");
        } else if (query.hasQueryItem("error")) {
            const QString detail = QUrl::fromPercentEncoding(query.queryItemValue("error_description").replace('+', ' ').toUtf8());
            problem = detail.isEmpty() ? query.queryItemValue("error") : detail.section('\n', 0, 0);
        } else {
            finish(query.queryItemValue("code", QUrl::FullyDecoded));
            return;
        }
        setBusy(false);
        emit notice(problem, true);
        changed();
    });
}

void Account::finish(const QString &code) {
    const QString verifier = m_verifier, redirect = m_redirect;
    run([code, verifier, redirect] {
        Outcome outcome;
        Graph pending;
        if (!pending.exchangeCode(code, verifier, redirect, &outcome.error))
            return outcome;
        outcome.profile = pending.profile(&outcome.error);
        if (outcome.error.isEmpty()) {
            Graph *graph = Graph::forAccount(outcome.profile.id);
            graph->adopt(pending);
            outcome.photo = graph->photo();
            outcome.ok = true;
        }
        return outcome;
    }, [this](const Outcome &outcome) {
        setBusy(false);
        if (outcome.ok) {
            welcome(outcome);
        } else {
            emit notice(outcome.error, true);
            changed();
        }
    });
}

void Account::stopServer() {
    m_timeout.stop();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server.clear();
    }
}

void Account::switchTo(const QString &id) {
    const Entry *e = entry(id);
    if (!e || id == m_activeId)
        return;
    m_activeId = id;
    save();
    changed();
    emit notice(e->status == "expired" ? tr("Sign in again to show %1").arg(e->profile.email)
                                       : tr("%1 · %2").arg(driveNameOf(e->profile), e->profile.name), false);
}

void Account::switchNext() {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).profile.id == m_activeId) {
            switchTo(m_entries.at((i + 1) % m_entries.size()).profile.id);
            return;
        }
    }
}

void Account::signOut() {
    const Entry *e = activeEntry();
    if (!e)
        return;
    const QString id = e->profile.id, label = e->profile.email;
    Graph::forAccount(id)->signOut();
    QFile::remove(avatarFile(id));
    m_entries.removeIf([&](const Entry &x) { return x.profile.id == id; });
    m_activeId = m_entries.isEmpty() ? QString() : m_entries.first().profile.id;
    save();
    changed();
    emit notice(tr("Signed out of %1").arg(label), false);
}

void Account::setBusy(bool busy) {
    if (m_signingIn == busy)
        return;
    m_signingIn = busy;
    emit busyChanged();
}
