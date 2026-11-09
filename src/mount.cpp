#define FUSE_USE_VERSION 31

#include "mount.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fuse.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "agent.h"
#include "graph.h"
#include "sync.h"

namespace {

const QString kItems = "https://graph.microsoft.com/v1.0/me/drive/items/";
const QString kSelect = "id,name,size,eTag,cTag,file,folder,package,root,deleted,parentReference,fileSystemInfo,"
                        "lastModifiedDateTime,remoteItem";
constexpr qint64 kSimpleUpload = 4 * 1024 * 1024;

QTextStream &err() {
    static QTextStream stream(stderr);
    return stream;
}

void say(const QString &message) {
    err() << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss ") << message << "\n";
    err().flush();
}

QByteArray compact(const QJsonObject &json) {
    return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

// The account's own OneDrive through Graph. Every address is an item of
// /me/drive, and items on other drives (shared folders, SharePoint
// libraries) are marked foreign so the sync folder never shows or writes them.
class GraphRemote : public Remote {
public:
    explicit GraphRemote(Graph *graph) : m_graph(graph) {}

    bool delta(const QString &link, QList<RemoteItem> *items, QString *next, bool *reset, QString *error) override {
        if (m_drive.isEmpty()) {
            const GraphReply drive = m_graph->call("GET", "https://graph.microsoft.com/v1.0/me/drive?$select=id");
            if (!drive.ok()) {
                *error = drive.error;
                return false;
            }
            m_drive = drive.json().value("id").toString();
        }
        *next = m_graph->delta(
            link, kSelect, [&](const QJsonObject &item) { items->append(parse(item)); },
            [&] {
                items->clear();
                *reset = true;
            },
            error);
        return !next->isEmpty();
    }

    Result item(const QString &id, RemoteItem *out, QString *error) override {
        const GraphReply reply = m_graph->call("GET", kItems + id + "?$select=" + kSelect);
        if (reply.ok())
            *out = parse(reply.json());
        return result(reply, error);
    }

    Result download(const QString &id, const QString &file, RemoteItem *out, QString *error) override {
        const GraphReply reply = m_graph->call("GET", kItems + id);
        if (!reply.ok())
            return result(reply, error);
        const QJsonObject json = reply.json();
        *out = parse(json);
        if (out->foreign || out->dir) {
            *error = "Not a file in this OneDrive";
            return Failed;
        }
        const QString url = json.value("@microsoft.graph.downloadUrl").toString();
        if (url.isEmpty() || !m_graph->download(QUrl(url), file, [](qint64) {}, error))
            return Failed;
        if (QFileInfo(file).size() != out->size) {
            *error = QString("The download of %1 was cut short").arg(out->name);
            return Failed;
        }
        return Ok;
    }

    Result upload(const QString &id, const QString &eTag, const QString &parentId, const QString &name,
                  const QString &file, const QDateTime &modified, RemoteItem *out, QString *error) override {
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(name));
        // Replacing goes to the item itself, and only while it is still the
        // version we have; a new file goes by name and fails on a clash.
        const QString target = id.isEmpty() ? kItems + parentId + ":/" + encoded + ":" : kItems + id;
        QList<QPair<QByteArray, QByteArray>> headers;
        if (!id.isEmpty())
            headers.append({"If-Match", eTag.toUtf8()});
        const QJsonObject stamp{{"lastModifiedDateTime", modified.toUTC().toString(Qt::ISODate)}};
        GraphReply reply;
        QFile content(file);
        if (content.size() <= kSimpleUpload) {
            if (!content.open(QIODevice::ReadOnly)) {
                *error = QString("Cannot read %1").arg(name);
                return Failed;
            }
            reply = m_graph->call("PUT",
                                  target + "/content" + (id.isEmpty() ? "?@microsoft.graph.conflictBehavior=fail" : ""),
                                  content.readAll(), "application/octet-stream", headers);
            if (reply.ok()) {
                const QString newId = reply.json().value("id").toString();
                const GraphReply stamped = m_graph->call("PATCH", kItems + newId,
                                                         compact({{"fileSystemInfo", stamp}}));
                if (stamped.ok())
                    reply = stamped;
            }
        } else {
            QJsonObject item{{"@microsoft.graph.conflictBehavior", id.isEmpty() ? "fail" : "replace"},
                             {"fileSystemInfo", stamp}};
            if (id.isEmpty())
                item.insert("name", name);
            reply = m_graph->uploadSession(target + "/createUploadSession", item, headers, file, [](qint64) {});
        }
        if (reply.ok())
            *out = parse(reply.json());
        return result(reply, error);
    }

    Result lookup(const QString &parentId, const QString &name, RemoteItem *out, QString *error) override {
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(name));
        const GraphReply reply = m_graph->call("GET", kItems + parentId + ":/" + encoded + ":?$select=" + kSelect);
        if (reply.ok())
            *out = parse(reply.json());
        return result(reply, error);
    }

    Result createFolder(const QString &parentId, const QString &name, RemoteItem *out, QString *error) override {
        const QJsonObject body{{"name", name}, {"folder", QJsonObject()}, {"@microsoft.graph.conflictBehavior", "fail"}};
        const GraphReply reply = m_graph->call("POST", kItems + parentId + "/children", compact(body));
        if (reply.ok())
            *out = parse(reply.json());
        return result(reply, error);
    }

    Result move(const QString &id, const QString &eTag, const QString &parentId, const QString &name,
                RemoteItem *out, QString *error) override {
        const QJsonObject body{{"name", name}, {"parentReference", QJsonObject{{"id", parentId}}}};
        const GraphReply reply = m_graph->call("PATCH", kItems + id, compact(body), "application/json",
                                               {{"If-Match", eTag.toUtf8()}});
        if (reply.ok())
            *out = parse(reply.json());
        return result(reply, error);
    }

    // A plain DELETE puts the item in the OneDrive recycle bin.
    Result remove(const QString &id, const QString &eTag, QString *error) override {
        return result(m_graph->call("DELETE", kItems + id, {}, "application/json", {{"If-Match", eTag.toUtf8()}}),
                      error);
    }

    bool quota(qint64 *used, qint64 *total) override {
        if (m_quotaTimer.isValid() && m_quotaTimer.elapsed() < 600000)
            return false;
        const GraphReply reply = m_graph->call("GET", "https://graph.microsoft.com/v1.0/me/drive?$select=quota");
        if (!reply.ok())
            return false;
        m_quotaTimer.start();
        const QJsonObject quota = reply.json().value("quota").toObject();
        *used = quota.value("used").toVariant().toLongLong();
        *total = quota.value("total").toVariant().toLongLong();
        return *total > 0;
    }

private:
    static Result result(const GraphReply &reply, QString *error) {
        if (reply.ok())
            return Ok;
        *error = reply.error;
        switch (reply.status) {
        case 412:
            return Changed;
        case 409:
            return Taken;
        case 404:
            return Gone;
        default:
            return Failed;
        }
    }

    RemoteItem parse(const QJsonObject &json) const {
        RemoteItem item;
        item.id = json.value("id").toString();
        item.name = json.value("name").toString();
        item.eTag = json.value("eTag").toString();
        item.cTag = json.value("cTag").toString();
        item.size = json.value("size").toVariant().toLongLong();
        item.dir = json.contains("folder") || json.contains("package");
        item.root = json.contains("root");
        item.deleted = json.contains("deleted");
        if (json.contains("folder"))
            item.children = json.value("folder").toObject().value("childCount").toInt(-1);
        const QJsonObject parent = json.value("parentReference").toObject();
        item.parentId = parent.value("id").toString();
        const QString drive = parent.value("driveId").toString();
        item.foreign = json.contains("remoteItem")
            || (!drive.isEmpty() && !m_drive.isEmpty() && drive.compare(m_drive, Qt::CaseInsensitive) != 0);
        QString stamp = json.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString();
        if (stamp.isEmpty())
            stamp = json.value("lastModifiedDateTime").toString();
        item.modified = QDateTime::fromString(stamp, Qt::ISODate);
        return item;
    }

    Graph *m_graph;
    QString m_drive;
    QElapsedTimer m_quotaTimer;
};

// ---- the file system

struct Handle {
    int fd = -1;
    quint64 key = 0;
    bool writable = false;
};

SyncCore *g_core = nullptr;
std::atomic<bool> g_pullWanted{false};

const char kState[] = "user.omadrive.state";
const char kInfo[] = "user.omadrive.info";
const char kRefresh[] = "user.omadrive.refresh";
const char kClaim[] = "user.omadrive.claim";

QString qpath(const char *path) {
    return QString::fromUtf8(path);
}

Handle *handle(fuse_file_info *fi) {
    return reinterpret_cast<Handle *>(fi->fh);
}

void fillStat(const SyncCore::Stat &s, struct stat *st) {
    std::memset(st, 0, sizeof *st);
    st->st_mode = s.dir ? S_IFDIR | 0755 : S_IFREG | 0644;
    st->st_nlink = s.dir ? 2 : 1;
    st->st_uid = ::getuid();
    st->st_gid = ::getgid();
    st->st_size = s.size;
    st->st_blksize = 4096;
    st->st_blocks = s.cached ? (s.size + 511) / 512 : 0;  // online-only files take no room here
    st->st_mtim.tv_sec = s.modified / 1000;
    st->st_mtim.tv_nsec = (s.modified % 1000) * 1000000;
    st->st_atim = st->st_ctim = st->st_mtim;
}

void *fsInit(fuse_conn_info *, fuse_config *cfg) {
    cfg->kernel_cache = 0;
    cfg->attr_timeout = 1;
    cfg->entry_timeout = 1;
    cfg->negative_timeout = 0;
    cfg->use_ino = 0;
    return nullptr;
}

int fsGetattr(const char *path, struct stat *st, fuse_file_info *) {
    SyncCore::Stat s;
    const int result = g_core->stat(qpath(path), &s);
    if (result == 0)
        fillStat(s, st);
    return result;
}

int fsReaddir(const char *path, void *buf, fuse_fill_dir_t filler, off_t, fuse_file_info *, fuse_readdir_flags) {
    QList<QPair<QString, SyncCore::Stat>> entries;
    const int result = g_core->list(qpath(path), &entries);
    if (result != 0)
        return result;
    filler(buf, ".", nullptr, 0, fuse_fill_dir_flags(0));
    filler(buf, "..", nullptr, 0, fuse_fill_dir_flags(0));
    for (const auto &[name, s] : std::as_const(entries)) {
        struct stat st;
        fillStat(s, &st);
        filler(buf, name.toUtf8().constData(), &st, 0, FUSE_FILL_DIR_PLUS);
    }
    return 0;
}

int fsOpen(const char *path, fuse_file_info *fi) {
    auto *h = new Handle;
    h->writable = (fi->flags & O_ACCMODE) != O_RDONLY;
    const int result = g_core->open(qpath(path), fi->flags, &h->fd, &h->key);
    if (result != 0) {
        delete h;
        return result;
    }
    fi->fh = reinterpret_cast<uint64_t>(h);
    return 0;
}

int fsCreate(const char *path, mode_t, fuse_file_info *fi) {
    auto *h = new Handle;
    h->writable = (fi->flags & O_ACCMODE) != O_RDONLY;
    const int result = g_core->create(qpath(path), fi->flags, &h->fd, &h->key);
    if (result != 0) {
        delete h;
        return result;
    }
    fi->fh = reinterpret_cast<uint64_t>(h);
    return 0;
}

int fsRead(const char *, char *buf, size_t size, off_t offset, fuse_file_info *fi) {
    const ssize_t n = ::pread(handle(fi)->fd, buf, size, offset);
    return n < 0 ? -errno : int(n);
}

int fsWrite(const char *, const char *buf, size_t size, off_t offset, fuse_file_info *fi) {
    Handle *h = handle(fi);
    const ssize_t n = ::pwrite(h->fd, buf, size, offset);
    if (n < 0)
        return -errno;
    g_core->wrote(h->key, offset + n);
    return int(n);
}

int fsTruncate(const char *path, off_t size, fuse_file_info *fi) {
    if (fi && fi->fh && handle(fi)->writable) {
        Handle *h = handle(fi);
        if (::ftruncate(h->fd, size) != 0)
            return -errno;
        g_core->truncated(h->key, size);
        return 0;
    }
    return g_core->truncate(qpath(path), size);
}

int fsRelease(const char *, fuse_file_info *fi) {
    Handle *h = handle(fi);
    ::close(h->fd);
    g_core->closed(h->key, h->writable);
    delete h;
    return 0;
}

int fsFsync(const char *, int datasync, fuse_file_info *fi) {
    const int fd = handle(fi)->fd;
    return (datasync ? ::fdatasync(fd) : ::fsync(fd)) == 0 ? 0 : -errno;
}

int fsMkdir(const char *path, mode_t) {
    return g_core->mkdir(qpath(path));
}

int fsUnlink(const char *path) {
    return g_core->unlink(qpath(path));
}

int fsRmdir(const char *path) {
    return g_core->rmdir(qpath(path));
}

int fsRename(const char *from, const char *to, unsigned int flags) {
    if (flags & RENAME_EXCHANGE)
        return -EINVAL;
    return g_core->rename(qpath(from), qpath(to), flags & RENAME_NOREPLACE);
}

int fsUtimens(const char *path, const struct timespec tv[2], fuse_file_info *) {
    if (tv[1].tv_nsec == UTIME_OMIT)
        return 0;
    const qint64 ms = tv[1].tv_nsec == UTIME_NOW ? QDateTime::currentMSecsSinceEpoch()
                                                 : qint64(tv[1].tv_sec) * 1000 + tv[1].tv_nsec / 1000000;
    return g_core->setModified(qpath(path), ms);
}

// OneDrive has no owners or modes; accepting the calls keeps cp -a and
// editors happy.
int fsChmod(const char *, mode_t, fuse_file_info *) {
    return 0;
}

int fsChown(const char *, uid_t, gid_t, fuse_file_info *) {
    return 0;
}

int fsStatfs(const char *, struct statvfs *st) {
    qint64 used = -1, total = -1;
    g_core->quota(&used, &total);
    if (total <= 0) {
        total = 1LL << 40;
        used = 0;
    }
    std::memset(st, 0, sizeof *st);
    st->f_bsize = st->f_frsize = 4096;
    st->f_blocks = total / 4096;
    st->f_bfree = st->f_bavail = qMax<qint64>(0, total - used) / 4096;
    st->f_namemax = 255;
    return 0;
}

int reply(const QByteArray &value, char *out, size_t size) {
    if (size == 0)
        return value.size();
    if (size < size_t(value.size()))
        return -ERANGE;
    std::memcpy(out, value.constData(), value.size());
    return value.size();
}

int fsGetxattr(const char *path, const char *name, char *value, size_t size) {
    if (std::strcmp(name, kState) == 0) {
        const QString state = g_core->state(qpath(path));
        return state.isEmpty() ? -ENOENT : reply(state.toUtf8(), value, size);
    }
    if (std::strcmp(name, kInfo) == 0) {
        QJsonObject info = g_core->info(qpath(path));
        if (info.isEmpty())
            return -ENOENT;
        info.insert("pendingUploads", g_core->status().value("pending"));
        return reply(compact(info), value, size);
    }
    return -ENODATA;
}

int fsListxattr(const char *, char *list, size_t size) {
    QByteArray names(kState);
    names.append('\0').append(kInfo).append('\0');
    return reply(names, list, size);
}

// Setting user.omadrive.claim to a path from the folder's root moves the item
// there on OneDrive straight away (see SyncCore::claim); the result is the
// error: 0 claimed, ESTALE taken by another device, EBUSY changes still
// waiting to upload here.
int fsSetxattr(const char *path, const char *name, const char *value, size_t size, int) {
    if (std::strcmp(name, kClaim) == 0) {
        const QString to = QString::fromUtf8(value, qsizetype(size));
        QString error;
        switch (g_core->claim(qpath(path), to, &error)) {
        case SyncCore::Claimed:
            say(QString("Claimed %1 as %2").arg(qpath(path), to));
            g_core->wake();
            return 0;
        case SyncCore::ClaimTaken:
            return -ESTALE;
        case SyncCore::ClaimPending:
            return -EBUSY;
        case SyncCore::ClaimInvalid:
            return -EINVAL;
        default:
            say(QString("Claiming %1 failed: %2").arg(qpath(path), error));
            return -EIO;
        }
    }
    if (std::strcmp(name, kRefresh) != 0)
        return -ENOTSUP;
    g_pullWanted = true;
    g_core->wake();
    return 0;
}

int fsRemovexattr(const char *, const char *) {
    return -ENOTSUP;
}

fuse_operations operations() {
    fuse_operations ops{};
    ops.init = fsInit;
    ops.getattr = fsGetattr;
    ops.readdir = fsReaddir;
    ops.open = fsOpen;
    ops.create = fsCreate;
    ops.read = fsRead;
    ops.write = fsWrite;
    ops.truncate = fsTruncate;
    ops.release = fsRelease;
    ops.fsync = fsFsync;
    ops.mkdir = fsMkdir;
    ops.unlink = fsUnlink;
    ops.rmdir = fsRmdir;
    ops.rename = fsRename;
    ops.utimens = fsUtimens;
    ops.chmod = fsChmod;
    ops.chown = fsChown;
    ops.statfs = fsStatfs;
    ops.getxattr = fsGetxattr;
    ops.listxattr = fsListxattr;
    ops.setxattr = fsSetxattr;
    ops.removexattr = fsRemovexattr;
    return ops;
}

// ---- the command

struct Options {
    QString account, mountpoint, cacheDir;
    bool foreground = false;
    int settle = 5, poll = 30;
    qint64 cacheMaxMb = 0;
    QStringList rest;
};

Options parse(const QStringList &args) {
    Options o;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args[i];
        const bool more = i + 1 < args.size();
        if ((arg == "--account" || arg == "-a") && more)
            o.account = args[++i];
        else if (arg == "--mountpoint" && more)
            o.mountpoint = args[++i];
        else if (arg == "--cache-dir" && more)
            o.cacheDir = args[++i];
        else if (arg == "--settle" && more)
            o.settle = qMax(0, args[++i].toInt());
        else if (arg == "--poll" && more)
            o.poll = qMax(5, args[++i].toInt());
        else if (arg == "--cache-max" && more)
            o.cacheMaxMb = qMax(0LL, args[++i].toLongLong());
        else if (arg == "--foreground" || arg == "-f")
            o.foreground = true;
        else
            o.rest << arg;
    }
    return o;
}

bool isMounted(const QString &mountpoint) {
    QFile mounts("/proc/self/mounts");
    if (!mounts.open(QIODevice::ReadOnly))
        return false;
    // Spaces in mount points are written as \040.
    const QByteArray wanted = QFile::encodeName(mountpoint).replace(" ", "\\040");
    for (const QByteArray &line : mounts.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() > 2 && fields[1] == wanted)
            return true;
    }
    return false;
}

QString tilde(const QString &path) {
    return path.startsWith(QDir::homePath() + "/") ? "~" + path.mid(QDir::homePath().size()) : path;
}

// The account and its folder, or an error printed and empty.
bool resolve(Options &o, QString *accountId) {
    QString error;
    const QJsonObject account = resolveAccount(o.account, &error);
    if (account.isEmpty()) {
        say(error);
        return false;
    }
    *accountId = account.value("id").toString();
    if (o.mountpoint.isEmpty())
        o.mountpoint = account.value("mirror").toString();
    if (o.mountpoint.isEmpty()) {
        say("The account has no folder; pass --mountpoint");
        return false;
    }
    o.mountpoint = QDir::cleanPath(QFileInfo(o.mountpoint).absoluteFilePath());
    if (o.cacheDir.isEmpty())
        o.cacheDir = SyncCore::cacheDirFor(*accountId);
    return true;
}

QString statusFile(const QString &cacheDir) {
    return cacheDir + "/status.json";
}

void writeStatus(const QString &cacheDir, const QJsonObject &json) {
    QSaveFile file(statusFile(cacheDir));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(json).toJson());
        file.commit();
    }
}

void requestPull(int) {
    g_pullWanted = true;
}

std::atomic<bool> g_stopping{false};

void stopFlush(int) {
    g_stopping = true;
}

// Starts this command again in the background and waits for the mount.
int detach(const Options &o, const QStringList &args) {
    QDir().mkpath(o.cacheDir);
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(QStringList{"mount", "--foreground"} + args);
    process.setStandardOutputFile(o.cacheDir + "/mount.log", QIODevice::Append);
    process.setStandardErrorFile(o.cacheDir + "/mount.log", QIODevice::Append);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        say("Cannot start the mount in the background");
        return 1;
    }
    for (int i = 0; i < 1200; ++i) {
        if (isMounted(o.mountpoint)) {
            say(QString("Mounted OneDrive at %1").arg(o.mountpoint));
            return 0;
        }
        if (::kill(pid_t(pid), 0) != 0)
            break;
        QThread::msleep(100);
    }
    say(QString("The mount did not start; see %1/mount.log").arg(o.cacheDir));
    return 1;
}

}  // namespace

QString mountedAt(const QString &account) {
    QFile file(statusFile(SyncCore::cacheDirFor(account)));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    const QString mountpoint = json.value("mountpoint").toString();
    const pid_t pid = pid_t(json.value("pid").toInteger());
    const bool alive = pid > 0 && ::kill(pid, 0) == 0;
    return json.value("mounted").toBool() && alive && isMounted(mountpoint) ? mountpoint : QString();
}

int runMount(const QStringList &args) {
    Options o = parse(args);
    QString accountId;
    if (!o.rest.isEmpty()) {
        say("Usage: omadrive mount [--account <id|email>] [--foreground] [--mountpoint <dir>]");
        return 2;
    }
    if (!resolve(o, &accountId))
        return 1;
    const QString refusal = SyncCore::mountRefusal(accountId, o.mountpoint, accountIds());
    if (!refusal.isEmpty()) {
        say(refusal);
        return 1;
    }
    if (isMounted(o.mountpoint)) {
        struct stat st;
        if (::stat(QFile::encodeName(o.mountpoint).constData(), &st) == 0) {
            say(QString("%1 is already mounted").arg(o.mountpoint));
            return 1;
        }
        // Left over from a mount that died: clear it.
        QProcess::execute("fusermount3", {"-u", "-z", o.mountpoint});
    }
    QDir dir(o.mountpoint);
    if (!dir.exists() && !QDir().mkpath(o.mountpoint)) {
        say(QString("Cannot make %1").arg(o.mountpoint));
        return 1;
    }
    if (!dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const QString path = tilde(o.mountpoint);
        say(QString("%1 is not empty, and omadrive will not hide files by mounting over them. Move it aside first:\n"
                    "  mv %1 %1.old")
                .arg(path));
        return 1;
    }
    if (!o.foreground) {
        QStringList forward = args;
        forward.removeAll("--foreground");
        forward.removeAll("-f");
        return detach(o, forward);
    }

    Graph graph(accountId);
    GraphRemote remote(&graph);
    SyncCore::Options options;
    options.settleMs = o.settle * 1000;
    options.cacheMax = o.cacheMaxMb * 1024 * 1024;
    SyncCore core(&remote, o.cacheDir, options);
    const bool warm = core.load();

    QString error;
    if (!graph.restore(&error)) {
        if (error.isEmpty()) {
            say("That account is not signed in here; sign in with omadrive first");
            return 1;
        }
        if (!warm) {
            say(error);
            return 1;
        }
        say(QString("Offline (%1); showing the saved tree").arg(error));
    }
    if (!warm) {
        say("Reading the OneDrive tree…");
        if (!core.pull(&error)) {
            say(error);
            return 1;
        }
    }

    g_core = &core;
    QJsonObject base{{"account", accountId}, {"mountpoint", o.mountpoint}, {"pid", qint64(::getpid())}};
    auto status = [&](bool mounted) {
        QJsonObject json = core.status();
        for (auto it = base.cbegin(); it != base.cend(); ++it)
            json.insert(it.key(), it.value());
        json.insert("mounted", mounted);
        return json;
    };
    writeStatus(o.cacheDir, status(true));

    std::signal(SIGUSR1, requestPull);
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        QElapsedTimer sincePull;
        if (warm)
            g_pullWanted = true;
        else
            sincePull.start();
        QJsonObject last;
        while (!stop) {
            if (g_pullWanted.exchange(false) || core.takePullWanted() || !sincePull.isValid()
                || sincePull.elapsed() >= o.poll * 1000) {
                QString problem;
                if (!core.pull(&problem))
                    say(problem);
                sincePull.start();
            }
            if (core.hasWork())
                core.push();
            const QJsonObject now = status(true);
            if (now != last) {
                writeStatus(o.cacheDir, now);
                last = now;
            }
            core.wait(1000);
        }
    });

    const fuse_operations ops = operations();
    const QByteArray mountpoint = QFile::encodeName(o.mountpoint);
    QList<QByteArray> fuseArgs{"omadrive", mountpoint, "-f", "-o", "fsname=omadrive,subtype=omadrive,default_permissions"};
    std::vector<char *> argv;
    for (QByteArray &arg : fuseArgs)
        argv.push_back(arg.data());
    say(QString("Mounting %1 at %2").arg(accountId, o.mountpoint));
    const int result = fuse_main(int(argv.size()), argv.data(), &ops, nullptr);

    say("Unmounting");
    stop = true;
    core.wake();
    worker.join();
    // Send what is still waiting, briefly; the rest stays in the journal.
    std::signal(SIGTERM, stopFlush);
    std::signal(SIGINT, stopFlush);
    QElapsedTimer flush;
    flush.start();
    while (core.hasWork() && flush.elapsed() < 20000 && !g_stopping) {
        if (core.push(true) == 0)
            break;
    }
    core.save();
    writeStatus(o.cacheDir, status(false));
    g_core = nullptr;
    say(core.hasWork() ? "Unmounted; some changes will upload next time" : "Unmounted");
    return result;
}

int runUnmount(const QStringList &args) {
    Options o = parse(args);
    QString accountId;
    const QString explicitMountpoint = o.mountpoint;
    if (!resolve(o, &accountId))
        return 1;
    if (explicitMountpoint.isEmpty()) {
        const QString mounted = mountedAt(accountId);
        if (!mounted.isEmpty())
            o.mountpoint = mounted;
    }
    if (!isMounted(o.mountpoint)) {
        say(QString("%1 is not mounted").arg(o.mountpoint));
        return 0;
    }
    return QProcess::execute("fusermount3", {"-u", o.mountpoint}) == 0 ? 0 : 1;
}
