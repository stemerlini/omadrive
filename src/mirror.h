#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>

// The local folder that mirrors the active account's OneDrive: ~/OneDrive for
// the first personal account (or "mirror" in auth.json), "~/OneDrive - SES"
// style for the others. OneDrive "/a/b.txt" lives at "<mirror>/a/b.txt".
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
    static void setRoots(const QHash<QString, QString> &roots, const QString &active);
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

    Q_INVOKABLE QString pathFor(const QString &account, const QString &remotePath) const {
        const QString r = rootFor(account);
        return r.isEmpty() ? QString() : localPath(r, remotePath);
    }

signals:
    void rootChanged();
};
