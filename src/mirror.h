#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

// The local folder that mirrors OneDrive (~/OneDrive unless auth.json sets
// "mirror"). OneDrive "/a/b.txt" lives at "<mirror>/a/b.txt". Downloads land
// there, and each OneDrive item is shown as online only, available locally,
// or changed (the local copy differs in size or modification time).
class Mirror : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString root READ root CONSTANT)

public:
    explicit Mirror(QObject *parent = nullptr) : QObject(parent) {}

    static QString root();
    static QString localPath(const QString &remotePath);
    // "online", "local" or "changed".
    static QString state(const QString &remotePath, bool isDir, qint64 size, const QDateTime &modified);
    static bool sameFile(const QString &localFile, qint64 size, const QDateTime &modified);

    Q_INVOKABLE QString pathFor(const QString &remotePath) const { return localPath(remotePath); }
};
