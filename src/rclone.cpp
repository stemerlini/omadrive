#include "rclone.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

namespace Rclone {
namespace {

// /proc/self/mounts writes space, tab, newline and backslash as \ooo.
QString unescape(const QByteArray &field) {
    QByteArray out;
    out.reserve(field.size());
    for (int i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            bool ok = false;
            const int c = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out.append(char(c));
                i += 3;
                continue;
            }
        }
        out.append(field[i]);
    }
    return QString::fromUtf8(out);
}

bool inside(const QString &path, const QString &dir) {
    return path == dir || path.startsWith(dir + '/');
}

}  // namespace

QVector<Mount> parseMounts(const QByteArray &mounts, const QString &cacheDir) {
    QVector<Mount> result;
    for (const QByteArray &line : mounts.split('\n')) {
        const QList<QByteArray> f = line.split(' ');
        if (f.size() < 3 || f[2] != "fuse.rclone")
            continue;
        // The source is "remote:" or "remote:sub/folder".
        const QString source = unescape(f[0]);
        const int colon = source.indexOf(':');
        if (colon <= 0)
            continue;
        QString root = source.mid(colon + 1);
        while (root.startsWith('/'))
            root.remove(0, 1);
        QString meta = cacheDir + "/vfsMeta/" + source.left(colon);
        if (!root.isEmpty())
            meta += '/' + root;
        result.append({QDir::cleanPath(unescape(f[1])), QDir::cleanPath(meta)});
    }
    return result;
}

Mount mountFor(const QString &path, const QVector<Mount> &mounts) {
    const QString clean = QDir::cleanPath(path);
    Mount best;
    for (const Mount &m : mounts)
        if (inside(clean, m.mountpoint) && m.mountpoint.size() > best.mountpoint.size())
            best = m;
    return best;
}

Mount mountFor(const QString &path) {
    QFile file("/proc/self/mounts");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/rclone";
    return mountFor(path, parseMounts(file.readAll(), cache));
}

QString state(const Mount &mount, const QString &path, bool isDir) {
    if (isDir || !mount.isValid())
        return {};
    const QString clean = QDir::cleanPath(path);
    if (!inside(clean, mount.mountpoint) || clean == mount.mountpoint)
        return {};
    QFile file(mount.metaRoot + clean.mid(mount.mountpoint.size()));
    if (!file.open(QIODevice::ReadOnly))
        return "online";
    const QJsonObject meta = QJsonDocument::fromJson(file.readAll()).object();
    if (meta.value("Dirty").toBool())
        return "pending";

    // Downloaded when the cached ranges cover the whole file.
    const qint64 size = meta.value("Size").toInteger(-1);
    if (size < 0)
        return "online";
    QVector<QPair<qint64, qint64>> ranges;
    for (const QJsonValue &r : meta.value("Rs").toArray())
        ranges.append({r["Pos"].toInteger(), r["Pos"].toInteger() + r["Size"].toInteger()});
    std::sort(ranges.begin(), ranges.end());
    qint64 covered = 0;
    for (const auto &r : std::as_const(ranges)) {
        if (r.first > covered)
            break;
        covered = std::max(covered, r.second);
    }
    return covered >= size ? "local" : "online";
}

QString folderState(const Mount &mount, const QString &dir) {
    const QString clean = QDir::cleanPath(dir);
    if (!mount.isValid() || !inside(clean, mount.mountpoint))
        return {};
    // Nothing of it in the cache: online, and nothing can be waiting.
    const QString meta = mount.metaRoot + clean.mid(mount.mountpoint.size());
    if (!QFileInfo(meta).isDir())
        return "online";
    for (QDirIterator it(meta, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories); it.hasNext();) {
        QFile file(it.next());
        if (file.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(file.readAll()).object().value("Dirty").toBool())
            return "pending";
    }
    // Local only if every file is; stop at the first one that is not.
    int seen = 0;
    for (QDirIterator it(clean, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories); it.hasNext();) {
        if (++seen > 5000 || state(mount, it.next(), false) != "local")
            return "online";
    }
    return "local";
}

}  // namespace Rclone
