#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QString>

// The local folder that mirrors the active account's OneDrive:
// ~/OneDrive-Personal for the first personal account (or "mirror" in
// auth.json), ~/OneDrive-<Org> (say ~/OneDrive-SES) for the others. OneDrive "/a/b.txt" lives at "<mirror>/a/b.txt".
// Downloads land there, and each OneDrive item is shown as online only,
// available locally, or changed (the local copy differs in size or time).
class Mirror : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString root READ root NOTIFY rootChanged)

public:
    explicit Mirror(QObject *parent = nullptr);

    static QString root();  // the active account's
    // Empty for an id that is not one of the accounts.
    static QString rootFor(const QString &account);
    // work: the ids of work/school accounts, whose files must not leave them.
    static void setRoots(const QHash<QString, QString> &roots, const QString &active, const QSet<QString> &work = {});
    // The account whose folder holds localPath, or empty.
    static QString ownerOf(const QString &localPath);
    // The work account whose files a copy would take to another account (its
    // OneDrive or its folder here), or empty when the copy is allowed. Copies
    // into ordinary local folders are allowed. fromAccount/toAccount are empty
    // for local sides; then the paths decide.
    static QString workLeak(const QString &fromAccount, const QStringList &localSources, const QString &toAccount,
                            const QString &localDest);
    // "mirror" from auth.json, or empty.
    static QString configuredRoot();
    // Where a personal account's files go: "mirror", or ~/OneDrive-Personal.
    static QString personalRoot();
    static QString localPath(const QString &remotePath);
    static QString localPath(const QString &root, const QString &remotePath);
    // "online", "local" or "changed".
    static QString state(const QString &root, const QString &remotePath, bool isDir, qint64 size,
                         const QDateTime &modified);
    static bool sameFile(const QString &localFile, qint64 size, const QDateTime &modified);

    Q_INVOKABLE QString leakFrom(const QString &fromAccount, const QStringList &localSources,
                                 const QString &toAccount, const QString &localDest) const {
        return workLeak(fromAccount, localSources, toAccount, localDest);
    }

    Q_INVOKABLE QString pathFor(const QString &account, const QString &remotePath) const {
        const QString r = rootFor(account);
        return r.isEmpty() ? QString() : localPath(r, remotePath);
    }

signals:
    void rootChanged();
};
