#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// OneDrive accounts are rclone remotes (type onedrive). Each can be mounted
// by the systemd user unit omadrive-rclone@<remote>.service, which reads
// ~/.config/omadrive/cloud/<remote>.env (REMOTE=, MOUNTPOINT=) and serves
// rclone's remote control on $XDG_RUNTIME_DIR/omadrive-rclone-<remote>.sock.
namespace Cloud {

struct Account {
    QString remote;      // rclone remote name, without the colon
    QString driveType;   // "personal" or "business"
    QString mountpoint;  // empty when no mount is set up
    bool mounted = false;
    bool work() const { return driveType == "business"; }
    QString label() const;
    QString fs() const { return remote + ':'; }
};

struct Run {
    int code = -1;
    QByteArray out, err;
    QString error() const;  // a one-line message for a failed run
};

QString configDir();
QString unitName(const QString &remote);
QString socketPath(const QString &remote);
// "Personal" for onedrive-personal, "SES" for onedrive-ses.
QString labelFor(const QString &remote);
QString defaultMountpoint(const QString &remote);
bool validRemote(const QString &remote);

QVector<Account> accounts();
// By remote name or label (case-insensitive); empty name: the only, or the
// first personal, account. Invalid (empty remote) when not found.
Account find(const QString &name);
// The account whose mount folder holds localPath.
Account ownerOf(const QString &localPath);
// "remote:path" for a drive path ("/a/b") or a path inside the mount folder.
QString remotePath(const Account &account, const QString &path);
// The path from the drive root, "/a/b".
QString drivePath(const Account &account, const QString &path);

Run rclone(const QStringList &args, int timeoutMs = 120000);
// rclone's remote control on the mount; {"error":…} when it is not running.
QJsonObject rc(const QString &remote, const QString &method, const QJsonObject &params = {}, int timeoutMs = 10000);
// Paths from the drive root waiting to upload.
QStringList pending(const Account &account);

// Writes the .env file and enables and starts the unit.
QString mount(const QString &remote, const QString &mountpoint = {});
QString unmount(const QString &remote);

}  // namespace Cloud

// `omadrive cloud status|mount|unmount|connect`.
int runCloud(const QStringList &args);
