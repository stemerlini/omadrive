#pragma once

#include <QDateTime>
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

    static QString root();
    static void setRoot(const QString &root);
    // ~/OneDrive, or "mirror" from auth.json.
    static QString configuredRoot();
    static QString localPath(const QString &remotePath);
    static QString localPath(const QString &root, const QString &remotePath);
    // "online", "local" or "changed".
    static QString state(const QString &remotePath, bool isDir, qint64 size, const QDateTime &modified);
    static bool sameFile(const QString &localFile, qint64 size, const QDateTime &modified);

    Q_INVOKABLE QString pathFor(const QString &remotePath) const { return localPath(remotePath); }

signals:
    void rootChanged();
};
