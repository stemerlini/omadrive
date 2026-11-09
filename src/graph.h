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

// Who a sign-in belongs to.
struct Profile {
    QString id, name, email, org;  // org: the work or school, empty for a personal account
    bool business = false;
};

// Microsoft Graph client for OneDrive. Every call blocks, so call it from a
// worker thread. Sign-in uses the OAuth code flow with PKCE as a public
// client (no secret). Each account has its own Graph, and its refresh token
// lives in the system keyring under the account's id.
class Graph {
public:
    // The account OneDrive is shown for. Jobs take it when they start, so a
    // switch halfway through never sends their remaining calls elsewhere.
    static Graph &active();
    static void setActive(const QString &id);
    static Graph *forAccount(const QString &id);  // made on first use; lives for the app

    // The app registration comes from ~/.config/omadrive/auth.json.
    static bool configured();
    static QString configPath();
    static QString authorizeUrl(const QString &redirect, const QString &challenge, const QString &state,
                                const QString &loginHint = {}, const QString &clientId = {}, const QString &tenant = {});

    // Which app (client id, tenant) each account signs in and refreshes with.
    // An account with no entry, and a sign-in in progress, use auth.json's
    // default. This lets a work account borrow a different app id, so it can
    // sign in where the tenant blocks the default one, without disturbing the
    // personal account.
    static void setApps(const QHash<QString, QPair<QString, QString>> &apps);
    static QPair<QString, QString> appFor(const QString &id);  // (clientId, tenant), default-filled

    // A Graph made without an id is a sign-in in progress: it keeps its token
    // to itself until adopt() hands it to the account it turned out to be.
    explicit Graph(const QString &id = {});
    QString id() const { return m_id; }
    // The app a sign-in in progress uses, before its account id is known.
    void setApp(const QString &clientId, const QString &tenant);

    bool exchangeCode(const QString &code, const QString &verifier, const QString &redirect, QString *error);
    bool restore(QString *error);  // false without an error means no saved sign-in
    // Signs in with the token saved before accounts had ids; false if there is
    // none. The token stays in place until clearLegacy(), once adopted.
    bool restoreLegacy(QString *error);
    static bool hasLegacy();
    static void clearLegacy();
    void adopt(Graph &signedIn);
    void signOut();
    bool signedIn() const;
    Profile profile(QString *error);
    QByteArray photo();  // the profile picture, empty when there is none

    GraphReply call(const QByteArray &verb, const QString &urlOrPath, const QByteArray &body = {},
                    const QByteArray &contentType = "application/json",
                    const QList<QPair<QByteArray, QByteArray>> &headers = {});
    QList<QJsonObject> children(const QString &path, QString *error);
    bool download(const QUrl &url, const QString &file, const std::function<void(qint64)> &progress, QString *error);
    // Uploads never overwrite: OneDrive renames on a name clash.
    bool uploadFile(const QString &localFile, const QString &remoteParent,
                    const std::function<void(qint64)> &progress, QString *error);
    // Sends localFile through an upload session made at sessionUrl (a
    // ".../createUploadSession" address) with the given item properties and
    // headers (say If-Match); returns the last reply, the item when it worked.
    GraphReply uploadSession(const QString &sessionUrl, const QJsonObject &item,
                             const QList<QPair<QByteArray, QByteArray>> &headers, const QString &localFile,
                             const std::function<void(qint64)> &progress);
    // Reads the drive's change feed from link (from the start when empty),
    // handing each changed item to onItem. When OneDrive has expired the link
    // it calls onReset and reads everything again. Returns the next link, or
    // empty with an error.
    QString delta(const QString &link, const QString &select, const std::function<void(const QJsonObject &)> &onItem,
                  const std::function<void()> &onReset, QString *error);
    // Returns the name OneDrive gave the folder, or empty with an error.
    QString createFolder(const QString &remoteParent, const QString &name, bool failIfExists, QString *error);

    // "/a/b" -> ".../me/drive/root:/a/b:" (the root has no path segment).
    static QString itemUrl(const QString &path);
    static QString joinPath(const QString &parent, const QString &name);
    static QString parentPath(const QString &path);

    static GraphReply send(const QByteArray &verb, const QUrl &url, const QByteArray &body,
                           const QList<QPair<QByteArray, QByteArray>> &headers, int timeoutMs = 60000);

private:
    bool tokenRequest(const QList<QPair<QString, QString>> &form, QString *error);
    QString accessToken(QString *error);
    void stampModified(const QJsonObject &item, const QDateTime &modified);
    bool uploadLarge(const QString &localFile, const QString &remotePath,
                     const std::function<void(qint64)> &progress, QString *error);

    void saveToken();

    QString m_id;
    QString m_clientId, m_tenant;  // set only for a sign-in in progress
    QString m_access, m_refresh;
    QDateTime m_expires;
    mutable QMutex m_mutex;
};
