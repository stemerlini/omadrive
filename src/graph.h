#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QUrl>
#include <functional>

struct GraphReply {
    int status = 0;
    QByteArray body;
    QString error;  // empty when the call worked

    QJsonObject json() const;
    bool ok() const { return error.isEmpty(); }
};

// Microsoft Graph client for OneDrive. Every call blocks, so call it from a
// worker thread. Sign-in uses the OAuth code flow with PKCE as a public
// client (no secret); the refresh token lives in the system keyring.
class Graph {
public:
    static Graph &instance();

    // The app registration comes from ~/.config/omadrive/auth.json.
    bool configured() const { return !m_clientId.isEmpty(); }
    QString configPath() const;
    QString authorizeUrl(const QString &redirect, const QString &challenge, const QString &state) const;

    bool exchangeCode(const QString &code, const QString &verifier, const QString &redirect, QString *error);
    bool restore(QString *error);  // false without an error means no saved sign-in
    void signOut();
    bool signedIn() const;
    QString displayName();

    GraphReply call(const QByteArray &verb, const QString &urlOrPath, const QByteArray &body = {},
                    const QByteArray &contentType = "application/json");
    QList<QJsonObject> children(const QString &path, QString *error);
    bool download(const QUrl &url, const QString &file, const std::function<void(qint64)> &progress, QString *error);
    // Uploads never overwrite: OneDrive renames on a name clash.
    bool uploadFile(const QString &localFile, const QString &remoteParent,
                    const std::function<void(qint64)> &progress, QString *error);
    // Returns the name OneDrive gave the folder, or empty with an error.
    QString createFolder(const QString &remoteParent, const QString &name, bool failIfExists, QString *error);

    // "/a/b" -> ".../me/drive/root:/a/b:" (the root has no path segment).
    static QString itemUrl(const QString &path);
    static QString joinPath(const QString &parent, const QString &name);
    static QString parentPath(const QString &path);

    static GraphReply send(const QByteArray &verb, const QUrl &url, const QByteArray &body,
                           const QList<QPair<QByteArray, QByteArray>> &headers, int timeoutMs = 60000);

private:
    Graph();

    bool tokenRequest(const QList<QPair<QString, QString>> &form, QString *error);
    QString accessToken(QString *error);
    void stampModified(const QJsonObject &item, const QDateTime &modified);
    bool uploadLarge(const QString &localFile, const QString &remotePath,
                     const std::function<void(qint64)> &progress, QString *error);

    QString m_clientId, m_tenant;
    QString m_access, m_refresh;
    QDateTime m_expires;
    mutable QMutex m_mutex;
};
