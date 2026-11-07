#include "account.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent>

#include "graph.h"

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

}  // namespace

Account::Account(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(180000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        stopServer();
        setState("signedOut", tr("Sign-in timed out"));
    });
}

void Account::setState(const QString &state, const QString &error) {
    m_state = state;
    m_error = error;
    if (state != "signedIn")
        m_name.clear();
    emit stateChanged();
}

void Account::watch(std::function<Outcome()> job) {
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        m_restoring = false;
        if (outcome.ok) {
            m_name = outcome.name;
            setState("signedIn");
        } else {
            setState("signedOut", outcome.error);
        }
    });
    watcher->setFuture(QtConcurrent::run(job));
}

void Account::restore() {
    if (!Graph::instance().configured())
        return;
    m_restoring = true;
    setState("signingIn");
    watch([] {
        Outcome outcome;
        // No saved sign-in is normal and not an error.
        if (Graph::instance().restore(&outcome.error)) {
            outcome.ok = true;
            outcome.name = Graph::instance().displayName();
        }
        return outcome;
    });
}

void Account::signIn() {
    if (m_state == "signingIn")
        return;
    Graph &graph = Graph::instance();
    if (!graph.configured()) {
        setState("signedOut", tr("Set clientId in %1 first").arg(graph.configPath()));
        return;
    }

    stopServer();
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        stopServer();
        setState("signedOut", tr("Cannot listen for the Microsoft sign-in redirect"));
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &Account::handleConnection);

    m_verifier = randomToken(48);
    m_expectedState = randomToken(16);
    m_redirect = QString("http://localhost:%1").arg(m_server->serverPort());
    const QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    setState("signingIn");
    m_timeout.start();
    QDesktopServices::openUrl(QUrl(graph.authorizeUrl(m_redirect, challenge, m_expectedState)));
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

        if (query.queryItemValue("state") != m_expectedState) {
            setState("signedOut", tr("Sign-in was rejected: the state did not match"));
        } else if (query.hasQueryItem("error")) {
            const QString detail = QUrl::fromPercentEncoding(query.queryItemValue("error_description").replace('+', ' ').toUtf8());
            setState("signedOut", detail.isEmpty() ? query.queryItemValue("error") : detail.section('\n', 0, 0));
        } else {
            finish(query.queryItemValue("code", QUrl::FullyDecoded));
        }
    });
}

void Account::finish(const QString &code) {
    const QString verifier = m_verifier, redirect = m_redirect;
    watch([code, verifier, redirect] {
        Outcome outcome;
        outcome.ok = Graph::instance().exchangeCode(code, verifier, redirect, &outcome.error);
        if (outcome.ok)
            outcome.name = Graph::instance().displayName();
        return outcome;
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

void Account::signOut() {
    stopServer();
    Graph::instance().signOut();
    setState("signedOut");
}
