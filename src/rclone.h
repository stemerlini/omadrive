#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Files under an rclone mount (`rclone mount --vfs-cache-mode full`) are
// shown as online only, on this device, or waiting to upload. rclone keeps a
// small JSON file per cached file under <cache>/vfsMeta/<remote>/<path> that
// records which byte ranges are downloaded and whether there are local
// changes; the state is read from there, so no rclone server is needed.
namespace Rclone {

struct Mount {
    QString mountpoint;  // empty when the path is not under an rclone mount
    QString metaRoot;    // <cache>/vfsMeta/<remote>[/<root>]
    bool isValid() const { return !mountpoint.isEmpty(); }
};

// The mounts listed in /proc/self/mounts text; cacheDir is rclone's --cache-dir.
QVector<Mount> parseMounts(const QByteArray &mounts, const QString &cacheDir);
// The rclone mount holding path, from /proc/self/mounts and the default cache.
Mount mountFor(const QString &path);
Mount mountFor(const QString &path, const QVector<Mount> &mounts);
// "online", "local" or "pending" for a file; empty for folders.
QString state(const Mount &mount, const QString &path, bool isDir);
// For a folder: "pending" when anything in it waits to upload, "local" when
// every file in it is on this device, else "online". Lists the folder tree
// through the mount, so call it off the GUI thread.
QString folderState(const Mount &mount, const QString &dir);

}  // namespace Rclone
