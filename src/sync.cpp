#include "sync.h"

#include "mirror.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimeZone>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const quint32 kMagic = 0x4f4d4453;  // "OMDS"
const quint32 kVersion = 1;
const quint64 kRoot = 1;
enum Record : quint8 { NodeRecord = 1, EraseRecord = 2, TombstoneRecord = 3 };

QByteArray native(const QString &path) {
    return QFile::encodeName(path);
}

// Names OneDrive refuses.
bool invalidName(const QString &name) {
    if (name.isEmpty() || name == "." || name == "..")
        return true;
    for (const QChar c : name)
        if (QStringLiteral("\"*:<>?\\|").contains(c))
            return true;
    return false;
}

// A bin folder that file managers make at the top of a disk; OneDrive's
// recycle bin is the bin here, so they delete instead.
bool trashFolder(quint64 parent, const QString &name) {
    return parent == kRoot && name.startsWith(".Trash");
}

qint64 epochMs(const QDateTime &time) {
    return time.isValid() ? time.toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch();
}

// Copies a file, sharing its blocks where the file system can (btrfs, xfs).
bool cloneFile(const QString &from, const QString &to) {
    QFile::remove(to);
    const int in = ::open(native(from).constData(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return false;
    const int out = ::open(native(to).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        ::close(in);
        return false;
    }
    bool ok = ::ioctl(out, FICLONE, in) == 0;
    if (!ok) {
        char buffer[1 << 16];
        ssize_t n;
        ok = true;
        while ((n = ::read(in, buffer, sizeof buffer)) > 0)
            if (::write(out, buffer, n) != n) {
                ok = false;
                break;
            }
        ok = ok && n == 0;
    }
    ::close(in);
    ok = ::close(out) == 0 && ok;
    return ok;
}

}  // namespace

struct SyncCore::Job {
    enum Kind { Refresh, Delete, Mkdir, Move, Upload } kind = Refresh;
    quint64 key = 0, gen = 0;
    QString id, eTag, parentId, name, file;
    QDateTime modified;
    bool dir = false;
    Remote::Result result = Remote::Failed;
    RemoteItem out;
    QString error;
};

SyncCore::SyncCore(Remote *remote, const QString &cacheDir, const Options &options)
    : m_remote(remote), m_dir(cacheDir), m_options(options) {
    if (m_options.host.isEmpty())
        m_options.host = QSysInfo::machineHostName();
    m_clock.start();
    Node root;
    root.key = kRoot;
    root.dir = true;
    m_nodes.insert(kRoot, root);
    QDir().mkpath(m_dir + "/content");
    QDir().mkpath(m_dir + "/staging");
}

SyncCore::~SyncCore() = default;

QString SyncCore::cacheDirFor(const QString &account) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omadrive/mount/" + account;
}

QString SyncCore::conflictName(const QString &name, const QString &host, const QDateTime &when, int n) {
    const int dot = name.lastIndexOf('.');
    const QString base = dot <= 0 ? name : name.left(dot);
    const QString ext = dot <= 0 ? QString() : name.mid(dot);
    QString tag = QString("conflict %1 %2").arg(host, when.toString("yyyy-MM-dd HHmm"));
    if (n > 1)
        tag += QString(" %1").arg(n);
    return base + " (" + tag + ")" + ext;
}

QString SyncCore::mountRefusal(const QString &account, const QString &mountpoint, const QStringList &accounts) {
    const QString path = QDir::cleanPath(mountpoint);
    const QString owner = Mirror::ownerOf(path);
    if (!owner.isEmpty() && owner != account)
        return QString("%1 is inside another account's folder").arg(path);
    if (!Mirror::workLeak(account, {}, {}, path).isEmpty())
        return QString("A work account's folder cannot be mounted in another account's folder");
    for (const QString &other : accounts) {
        const QString root = QDir::cleanPath(Mirror::rootFor(other));
        if (other != account && !root.isEmpty() && (root == path || root.startsWith(path + "/")))
            return QString("%1 holds another account's folder").arg(path);
    }
    return {};
}

bool SyncCore::transient(const QString &name) {
    return name.startsWith(".fuse_hidden") || (name.startsWith(".~lock.") && name.endsWith('#'))
        || name.startsWith("~$") || (name.startsWith('.') && (name.endsWith(".swp") || name.endsWith(".swx")));
}

QString SyncCore::contentPath(quint64 key) const {
    return m_dir + "/content/" + QString::number(key);
}

// ---- the tree

quint64 SyncCore::child(quint64 parent, const QString &name) const {
    const auto it = m_nodes.constFind(parent);
    return it == m_nodes.cend() ? 0 : it->kids.value(name);
}

// Another item whose name OneDrive would take for this one (it ignores case).
quint64 SyncCore::clash(quint64 parent, const QString &name, quint64 except) const {
    const auto it = m_nodes.constFind(parent);
    if (it == m_nodes.cend())
        return 0;
    for (auto kid = it->kids.cbegin(); kid != it->kids.cend(); ++kid)
        if (kid.value() != except && kid.key().compare(name, Qt::CaseInsensitive) == 0)
            return kid.value();
    return 0;
}

quint64 SyncCore::find(const QString &path) const {
    quint64 key = kRoot;
    for (const QString &part : path.split('/', Qt::SkipEmptyParts)) {
        key = child(key, part);
        if (!key)
            return 0;
    }
    return key;
}

quint64 SyncCore::findParent(const QString &path, QString *name) const {
    const int slash = path.lastIndexOf('/');
    *name = path.mid(slash + 1);
    const quint64 parent = find(path.left(slash));
    return parent && m_nodes.value(parent).dir ? parent : 0;
}

QString SyncCore::pathOf(quint64 key) const {
    QStringList parts;
    while (key && key != kRoot) {
        const auto it = m_nodes.constFind(key);
        if (it == m_nodes.cend())
            return {};
        parts.prepend(it->name);
        key = it->parent;
    }
    return key == kRoot ? "/" + parts.join('/') : QString();
}

SyncCore::Stat SyncCore::statOf(const Node &n) const {
    Stat s;
    s.dir = n.dir;
    s.size = n.dir ? 0 : n.size;
    s.modified = n.modified;
    s.cached = n.cached;
    return s;
}

bool SyncCore::isTransient(const Node &n) const {
    return !n.dir && transient(n.name);
}

// Renamed or moved here and not yet on OneDrive.
bool SyncCore::moved(const Node &n) const {
    if (n.id.isEmpty() || !n.parent)
        return false;
    return m_nodes.value(n.parent).id != n.remoteParent || n.name != n.remoteName;
}

bool SyncCore::needsPush(const Node &n) const {
    if (n.key == kRoot || !n.parent || isTransient(n))
        return false;
    return n.id.isEmpty() || (!n.dir && n.dirty) || moved(n);
}

QString SyncCore::stateOf(const Node &n) const {
    if (n.conflict)
        return "conflict";
    if (isTransient(n) || n.key == kRoot)
        return "local";
    if (needsPush(n))
        return "pending";
    return n.dir || n.cached ? "local" : "cloud";
}

void SyncCore::attach(quint64 key, quint64 parent, const QString &name) {
    Node &n = m_nodes[key];
    n.parent = parent;
    n.name = name;
    if (parent)
        m_nodes[parent].kids.insert(name, key);
}

void SyncCore::detach(quint64 key) {
    Node &n = m_nodes[key];
    if (n.parent && m_nodes.contains(n.parent))
        m_nodes[n.parent].kids.remove(n.name, key);
    n.parent = 0;
}

void SyncCore::erase(quint64 key) {
    if (key == kRoot || !m_nodes.contains(key))
        return;
    detach(key);
    const Node n = m_nodes.take(key);
    if (!n.id.isEmpty() && m_byId.value(n.id) == key)
        m_byId.remove(n.id);
    QFile::remove(contentPath(key));
    m_todo.remove(key);
    recordErase(key);
}

// Remembers to delete the item on OneDrive (into its recycle bin), after the
// settle time unless soon.
void SyncCore::bury(const Node &n, bool soon) {
    if (n.id.isEmpty())
        return;
    Tombstone t;
    t.id = n.id;
    t.eTag = n.eTag;
    t.cTag = n.cTag;
    t.parentId = n.remoteParent;
    t.name = n.remoteName;
    t.dir = n.dir;
    t.due = soon ? now() : now() + m_options.settleMs;
    m_tombstones.append(t);
    m_byId.remove(n.id);
    recordTombstones();
    m_wake.wakeAll();
}

// Forgets the deletes of what was in folder id, gone with it.
void SyncCore::dropTombstonesIn(const QString &id) {
    QSet<QString> gone{id};
    for (bool more = true; more;) {
        more = false;
        for (int i = m_tombstones.size() - 1; i >= 0; --i)
            if (gone.contains(m_tombstones[i].parentId)) {
                gone.insert(m_tombstones[i].id);
                m_tombstones.removeAt(i);
                more = true;
            }
    }
}

bool SyncCore::holdsTombstones(const QString &id) const {
    for (const Tombstone &t : m_tombstones)
        if (t.parentId == id)
            return true;
    return false;
}

// A new file where one was just deleted (how some editors save) becomes a new
// version of the deleted one instead, keeping its history.
void SyncCore::reclaim(quint64 key) {
    Node &n = m_nodes[key];
    if (n.dir || !n.id.isEmpty() || isTransient(n))
        return;
    const QString parentId = m_nodes.value(n.parent).id;
    if (parentId.isEmpty())
        return;
    for (int i = 0; i < m_tombstones.size(); ++i) {
        const Tombstone &t = m_tombstones[i];
        if (t.dir || t.parentId != parentId || t.name.compare(n.name, Qt::CaseInsensitive) != 0)
            continue;
        n.id = t.id;
        n.eTag = t.eTag;
        n.cTag = t.cTag;
        n.remoteParent = t.parentId;
        n.remoteName = t.name;
        n.dirty = true;
        m_byId.insert(n.id, key);
        m_tombstones.removeAt(i);
        recordTombstones();
        return;
    }
}

// The item is no longer on OneDrive: what is here is sent again as new.
void SyncCore::forgetRemote(quint64 key) {
    Node &n = m_nodes[key];
    if (!n.id.isEmpty() && m_byId.value(n.id) == key)
        m_byId.remove(n.id);
    n.id.clear();
    n.eTag.clear();
    n.cTag.clear();
    n.remoteParent.clear();
    n.remoteName.clear();
    if (!n.dir)
        n.dirty = true;
}

void SyncCore::changed(quint64 key) {
    const auto it = m_nodes.constFind(key);
    if (it == m_nodes.cend())
        return;
    record(key);
    if (needsPush(*it)) {
        m_todo.insert(key);
        m_wake.wakeAll();
    }
}

void SyncCore::markDirty(Node &n) {
    ++n.gen;
    n.lastWrite = now();
    n.modified = QDateTime::currentMSecsSinceEpoch();
    if (!n.dirty) {
        n.dirty = true;
        changed(n.key);
    }
}

QString SyncCore::uniqueConflict(quint64 parent, const QString &name) const {
    const QDateTime when = QDateTime::currentDateTime();
    for (int n = 1;; ++n) {
        const QString candidate = conflictName(name, m_options.host, when, n);
        if (!clash(parent, candidate, 0))
            return candidate;
    }
}

// A local item whose name OneDrive already uses steps aside, keeping both.
void SyncCore::moveAside(quint64 key) {
    const quint64 parent = m_nodes[key].parent;
    const QString name = uniqueConflict(parent, m_nodes[key].name);
    detach(key);
    attach(key, parent, name);
    m_nodes[key].conflict = true;
    changed(key);
}

// Takes the name OneDrive gave an item, when it changed it.
void SyncCore::adoptName(quint64 key, const QString &name) {
    Node &n = m_nodes[key];
    if (name.isEmpty() || name == n.name || clash(n.parent, name, key))
        return;
    const quint64 parent = n.parent;
    detach(key);
    attach(key, parent, name);
}

// Both sides changed the file. OneDrive's version keeps the name (as a new,
// online-only node); ours, under key so open files keep writing to it, gets
// the conflict name and is uploaded beside it. Returns OneDrive's node.
quint64 SyncCore::splitConflict(quint64 key) {
    Node theirs;
    {
        Node &n = m_nodes[key];
        theirs.key = m_nextKey++;
        theirs.id = n.id;
        theirs.remoteParent = n.remoteParent;
        theirs.remoteName = n.remoteName;
        theirs.eTag = n.eTag;
        theirs.cTag = n.cTag;
        theirs.size = n.size;
        theirs.modified = n.modified;
        theirs.lastUse = n.lastUse;
    }
    const quint64 parent = m_nodes[key].parent;
    const QString name = m_nodes[key].name;
    detach(key);
    forgetRemote(key);
    m_nodes[key].conflict = true;
    m_nodes.insert(theirs.key, theirs);
    m_byId.insert(theirs.id, theirs.key);
    if (parent) {
        attach(key, parent, uniqueConflict(parent, name));
        attach(theirs.key, parent, name);
    }
    changed(key);
    changed(theirs.key);
    return theirs.key;
}

// ---- OneDrive's changes

void SyncCore::updateFile(Node &n, const RemoteItem &item) {
    const qint64 modified = epochMs(item.modified);
    const bool contentChanged = item.cTag.isEmpty() ? item.size != n.size || modified != n.modified
                                                    : item.cTag != n.cTag;
    n.eTag = item.eTag;
    if (n.dirty)
        return;  // the caller splits a conflict
    if (contentChanged && n.cached) {
        QFile::remove(contentPath(n.key));
        n.cached = false;
    }
    n.cTag = item.cTag;
    n.size = item.size;
    n.modified = modified;
}

void SyncCore::removeRemote(quint64 key) {
    if (key == kRoot || !m_nodes.contains(key))
        return;
    for (const quint64 kid : m_nodes[key].kids.values())
        removeRemote(kid);
    const Node &n = m_nodes[key];
    const bool keep = n.dir ? !n.kids.isEmpty() : n.cached && (n.dirty || n.writers > 0);
    if (!keep) {
        erase(key);
        return;
    }
    forgetRemote(key);
    changed(key);
}

void SyncCore::applyItems(const QList<RemoteItem> &items, bool reset) {
    QSet<QString> seen, buried;
    for (const Tombstone &t : std::as_const(m_tombstones))
        buried.insert(t.id);
    QList<QPair<quint64, const RemoteItem *>> placed;
    for (const RemoteItem &item : items) {
        if (item.root) {
            Node &root = m_nodes[kRoot];
            if (root.id != item.id) {
                m_byId.remove(root.id);
                root.id = item.id;
                m_byId.insert(item.id, kRoot);
            }
            root.eTag = item.eTag;
            root.modified = epochMs(item.modified);
            seen.insert(item.id);
            record(kRoot);
            continue;
        }
        quint64 key = m_byId.value(item.id);
        // Shared folders and libraries live on other drives: never shown,
        // so never written to.
        if (item.deleted || item.foreign) {
            if (key)
                removeRemote(key);
            continue;
        }
        seen.insert(item.id);
        if (buried.contains(item.id))
            continue;
        if (!key) {
            Node n;
            n.key = key = m_nextKey++;
            n.dir = item.dir;
            n.id = item.id;
            n.eTag = item.eTag;
            n.cTag = item.cTag;
            n.size = item.size;
            n.modified = epochMs(item.modified);
            m_nodes.insert(key, n);
            m_byId.insert(item.id, key);
        } else if (!m_nodes[key].dir) {
            Node &n = m_nodes[key];
            const bool contentChanged = item.cTag.isEmpty() ? item.size != n.size : item.cTag != n.cTag;
            if (n.dirty && contentChanged) {
                key = splitConflict(key);
            } else if (contentChanged && n.writers > 0) {
                m_stale.insert(n.id);  // looked at again once closed
                n.eTag = item.eTag;
                placed.append({key, &item});
                continue;
            }
            updateFile(m_nodes[key], item);
        } else {
            m_nodes[key].eTag = item.eTag;
        }
        placed.append({key, &item});
    }

    const QString rootId = m_nodes[kRoot].id;
    for (const auto &[key, item] : std::as_const(placed)) {
        if (!m_nodes.contains(key))
            continue;
        const bool follow = !m_nodes[key].parent || !moved(m_nodes[key]);
        Node &n = m_nodes[key];
        n.remoteParent = item->parentId;
        n.remoteName = item->name;
        const quint64 parent = item->parentId == rootId ? kRoot : m_byId.value(item->parentId);
        if (follow && (n.parent != parent || n.name != item->name)) {
            detach(key);
            if (parent) {
                const quint64 other = clash(parent, item->name, key);
                if (other && m_nodes[other].id.isEmpty())
                    moveAside(other);
                attach(key, parent, item->name);
            }
        }
        changed(key);
    }

    if (reset) {
        QList<quint64> gone;
        for (const Node &n : std::as_const(m_nodes))
            if (n.key != kRoot && !n.id.isEmpty() && !seen.contains(n.id))
                gone.append(n.key);
        for (const quint64 key : gone)
            removeRemote(key);
    }
}

bool SyncCore::pull(QString *error) {
    QString link;
    {
        QMutexLocker lock(&m_mutex);
        link = m_delta;
        m_pullWanted = false;
    }
    QList<RemoteItem> items;
    QString next;
    bool reset = false;
    const bool ok = m_remote->delta(link, &items, &next, &reset, error);
    qint64 used = -1, total = -1;
    const bool haveQuota = ok && m_remote->quota(&used, &total);

    QMutexLocker lock(&m_mutex);
    if (!ok) {
        m_lastError = *error;
        return false;
    }
    applyItems(items, reset || link.isEmpty());
    m_delta = next;
    if (haveQuota) {
        m_used = used;
        m_total = total;
    }
    m_lastSync = QDateTime::currentDateTimeUtc();
    m_lastError.clear();
    if (!items.isEmpty() || link.isEmpty())
        saveLocked();
    return true;
}

// ---- sending local changes

bool SyncCore::nextJob(bool force, Job *job) {
    const qint64 t = now();
    if (!m_refresh.isEmpty()) {
        job->kind = Job::Refresh;
        job->id = *m_refresh.cbegin();
        m_refresh.remove(job->id);
        return true;
    }
    // A folder goes in one call, with what is in it, unless something in it
    // changed since; then its contents go one by one first.
    for (const bool folders : {true, false})
        for (const Tombstone &stone : std::as_const(m_tombstones)) {
            if ((!force && stone.due > t) || (folders && (!stone.dir || stone.tried)))
                continue;
            if (stone.tried && holdsTombstones(stone.id))
                continue;
            job->kind = Job::Delete;
            job->id = stone.id;
            job->eTag = stone.eTag;
            job->dir = stone.dir;
            job->name = stone.name;
            return true;
        }

    const Node *mkdir = nullptr, *move = nullptr, *upload = nullptr;
    bool moving = false;
    for (const quint64 key : m_todo.values()) {
        const auto it = m_nodes.constFind(key);
        if (it == m_nodes.cend() || !needsPush(*it)) {
            m_todo.remove(key);
            continue;
        }
        const Node &n = *it;
        if (!n.id.isEmpty() && moved(n))
            moving = true;
        if (!force && n.retryAt > t)
            continue;
        if (m_nodes.value(n.parent).id.isEmpty())
            continue;  // its folder is not on OneDrive yet
        if (n.dir && n.id.isEmpty())
            mkdir = &n;
        else if (moved(n))
            move = &n;
        else if (!n.dir && !n.downloading && (force || t - n.lastWrite >= m_options.settleMs))
            upload = &n;
    }
    // Deletes and renames first: a new file may take a name they free.
    if (upload && upload->id.isEmpty() && (moving || !m_tombstones.isEmpty()))
        upload = nullptr;
    const Node *n = mkdir ? mkdir : move ? move : upload;
    if (!n)
        return false;
    job->kind = mkdir ? Job::Mkdir : move ? Job::Move : Job::Upload;
    job->key = n->key;
    job->id = n->id;
    job->eTag = n->eTag;
    job->parentId = m_nodes.value(n->parent).id;
    job->name = n->name;
    job->gen = n->gen;
    job->dir = n->dir;
    job->modified = QDateTime::fromMSecsSinceEpoch(n->modified, QTimeZone::UTC);
    job->file = m_dir + "/staging/" + QString::number(n->key);
    m_busy = pathOf(n->key);
    return true;
}

void SyncCore::runJob(Job &job) {
    switch (job.kind) {
    case Job::Refresh:
        job.result = m_remote->item(job.id, &job.out, &job.error);
        break;
    case Job::Delete:
        job.result = m_remote->remove(job.id, job.eTag, &job.error);
        // Deleting what was in a folder changes the folder's eTag; it can
        // still go if OneDrive has nothing left in it.
        if (job.result == Remote::Changed && job.dir) {
            RemoteItem fresh;
            if (m_remote->item(job.id, &fresh, &job.error) == Remote::Ok && fresh.children == 0)
                job.result = m_remote->remove(job.id, fresh.eTag, &job.error);
        }
        break;
    case Job::Mkdir:
        job.result = m_remote->createFolder(job.parentId, job.name, &job.out, &job.error);
        break;
    case Job::Move:
        job.result = m_remote->move(job.id, job.eTag, job.parentId, job.name, &job.out, &job.error);
        break;
    case Job::Upload:
        // A copy, so writes during the upload cannot tear it.
        if (!cloneFile(contentPath(job.key), job.file)) {
            job.result = Remote::Failed;
            job.error = QString("Cannot read the cached copy of %1").arg(job.name);
            break;
        }
        job.result = m_remote->upload(job.id, job.eTag, job.parentId, job.name, job.file, job.modified, &job.out,
                                      &job.error);
        QFile::remove(job.file);
        break;
    }
}

void SyncCore::fail(Node *n, const QString &error) {
    m_lastError = error;
    if (!n)
        return;
    n->error = error;
    ++n->failures;
    n->retryAt = now() + qMin<qint64>(300000, 2000LL << qMin(n->failures, 7));
}

void SyncCore::finishJob(const Job &job) {
    using R = Remote::Result;
    if (job.kind == Job::Refresh) {
        if (job.result == R::Ok) {
            applyItems({job.out}, false);
        } else if (job.result == R::Gone) {
            RemoteItem gone;
            gone.id = job.id;
            gone.deleted = true;
            applyItems({gone}, false);
        } else {
            m_refresh.insert(job.id);
            m_lastError = job.error;
        }
        return;
    }
    if (job.kind == Job::Delete) {
        for (int i = 0; i < m_tombstones.size(); ++i) {
            Tombstone &t = m_tombstones[i];
            if (t.id != job.id)
                continue;
            if (job.result == R::Failed) {
                m_lastError = job.error;
                t.due = now() + qMin<qint64>(300000, 2000LL << qMin(++t.failures, 7));
                return;
            }
            if (job.result == R::Changed && t.dir && !t.tried && holdsTombstones(t.id)) {
                t.tried = true;
                return;
            }
            // Changed there since: keep OneDrive's newer version and show it again.
            if (job.result == R::Changed || job.result == R::Taken)
                m_refresh.insert(job.id);
            m_tombstones.removeAt(i);
            if (job.result == R::Ok && job.dir)
                dropTombstonesIn(job.id);
            recordTombstones();
            return;
        }
        return;
    }

    auto it = m_nodes.find(job.key);
    if (it == m_nodes.end()) {
        // Deleted here while it was being sent.
        if (job.result == R::Ok && job.id.isEmpty() && !job.out.id.isEmpty()) {
            Node gone;
            gone.id = job.out.id;
            gone.eTag = job.out.eTag;
            gone.dir = job.kind == Job::Mkdir;
            gone.remoteParent = job.out.parentId;
            gone.remoteName = job.out.name;
            bury(gone);
        }
        return;
    }
    Node &n = *it;
    if (job.result == R::Ok) {
        if (n.id.isEmpty() && !job.out.id.isEmpty()) {
            n.id = job.out.id;
            m_byId.insert(n.id, n.key);
        }
        n.eTag = job.out.eTag;
        if (!job.out.cTag.isEmpty())
            n.cTag = job.out.cTag;
        n.remoteParent = job.out.parentId.isEmpty() ? job.parentId : job.out.parentId;
        n.remoteName = job.out.name.isEmpty() ? job.name : job.out.name;
        if (n.name == job.name)
            adoptName(n.key, n.remoteName);
        if (job.kind == Job::Upload && n.gen == job.gen) {
            n.dirty = false;
            n.size = job.out.size;
        }
        n.error.clear();
        n.failures = 0;
        n.retryAt = 0;
        changed(n.key);
        return;
    }
    switch (job.result) {
    case R::Changed:
        if (job.kind == Job::Upload) {
            m_refresh.insert(job.id);
            splitConflict(n.key);
        } else {
            m_refresh.insert(job.id);
            fail(&n, job.error);
        }
        break;
    case R::Taken:
        // Someone else has the name on OneDrive: keep both.
        moveAside(n.key);
        m_pullWanted = true;
        break;
    case R::Gone:
        if (!job.id.isEmpty()) {
            removeRemote(n.key);
        } else {
            m_pullWanted = true;  // its folder went
            fail(&n, job.error);
        }
        break;
    default:
        fail(&n, job.error);
        break;
    }
}

int SyncCore::push(bool force) {
    int sent = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            if (!nextJob(force, &job))
                break;
        }
        runJob(job);
        QMutexLocker lock(&m_mutex);
        finishJob(job);
        m_busy.clear();
        if (job.result == Remote::Failed)
            break;
        ++sent;
    }
    QMutexLocker lock(&m_mutex);
    if (m_journalRecords > 5000)
        saveLocked();
    return sent;
}

bool SyncCore::hasWork() const {
    QMutexLocker lock(&m_mutex);
    return !m_todo.isEmpty() || !m_tombstones.isEmpty() || !m_refresh.isEmpty();
}

bool SyncCore::takePullWanted() {
    QMutexLocker lock(&m_mutex);
    const bool wanted = m_pullWanted;
    m_pullWanted = false;
    return wanted;
}

void SyncCore::wait(int ms) {
    QMutexLocker lock(&m_mutex);
    m_wake.wait(&m_mutex, ms);
}

void SyncCore::wake() {
    m_wake.wakeAll();
}

// ---- the file system

int SyncCore::stat(const QString &path, Stat *out) const {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return -ENOENT;
    *out = statOf(m_nodes[key]);
    return 0;
}

int SyncCore::list(const QString &path, QList<QPair<QString, Stat>> *out) const {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return -ENOENT;
    const Node &dir = m_nodes[key];
    if (!dir.dir)
        return -ENOTDIR;
    QSet<QString> names;
    for (auto it = dir.kids.cbegin(); it != dir.kids.cend(); ++it)
        if (!names.contains(it.key())) {
            names.insert(it.key());
            out->append({it.key(), statOf(m_nodes[it.value()])});
        }
    return 0;
}

// Downloads key's file into the cache, once however many open it at a time.
int SyncCore::fetch(QMutexLocker<QMutex> &lock, quint64 key) {
    for (;;) {
        const auto it = m_nodes.constFind(key);
        if (it == m_nodes.cend())
            return -ENOENT;
        if (it->cached)
            return 0;
        if (!it->downloading)
            break;
        m_downloaded.wait(&m_mutex);
    }
    Node &n = m_nodes[key];
    if (n.id.isEmpty())
        return -EIO;
    if (n.size == 0) {
        QFile file(contentPath(key));
        if (!file.open(QIODevice::WriteOnly))
            return -EIO;
        n.cached = true;
        changed(key);
        return 0;
    }
    n.downloading = true;
    const QString id = n.id, file = contentPath(key), part = file + ".part";
    lock.unlock();
    RemoteItem item;
    QString error;
    const Remote::Result result = m_remote->download(id, part, &item, &error);
    lock.relock();
    m_downloaded.wakeAll();
    const auto it = m_nodes.find(key);
    if (it != m_nodes.end())
        it->downloading = false;
    if (it == m_nodes.end() || it->id != id || result != Remote::Ok) {
        QFile::remove(part);
        if (result != Remote::Ok)
            m_lastError = error;
        return result == Remote::Ok || result == Remote::Gone ? -ENOENT : -EIO;
    }
    if (::rename(native(part).constData(), native(file).constData()) != 0) {
        QFile::remove(part);
        return -EIO;
    }
    it->cached = true;
    it->eTag = item.eTag;
    it->cTag = item.cTag;
    it->size = item.size;
    it->modified = epochMs(item.modified);
    changed(key);
    evict();
    return 0;
}

// Drops the least recently opened unchanged files over the cache limit.
void SyncCore::evict() {
    if (m_options.cacheMax <= 0)
        return;
    qint64 total = 0;
    QList<const Node *> clean;
    for (const Node &n : std::as_const(m_nodes))
        if (n.cached && !n.dir) {
            total += n.size;
            if (!n.dirty && !n.conflict && !n.open && !n.downloading && !n.id.isEmpty() && !isTransient(n))
                clean.append(&n);
        }
    if (total <= m_options.cacheMax)
        return;
    std::sort(clean.begin(), clean.end(), [](const Node *a, const Node *b) { return a->lastUse < b->lastUse; });
    QList<quint64> drop;
    for (const Node *n : std::as_const(clean)) {
        if (total <= m_options.cacheMax * 9 / 10)
            break;
        total -= n->size;
        drop.append(n->key);
    }
    for (const quint64 key : drop) {
        QFile::remove(contentPath(key));
        m_nodes[key].cached = false;
        record(key);
    }
}

int SyncCore::open(const QString &path, int flags, int *fd, quint64 *key) {
    QMutexLocker lock(&m_mutex);
    *key = find(path);
    if (!*key)
        return -ENOENT;
    if (m_nodes[*key].dir)
        return -EISDIR;
    const bool writable = (flags & O_ACCMODE) != O_RDONLY;
    const bool trunc = writable && (flags & O_TRUNC);
    if (!m_nodes[*key].cached) {
        if (trunc) {
            QFile file(contentPath(*key));
            if (!file.open(QIODevice::WriteOnly))
                return -EIO;
            m_nodes[*key].cached = true;
        } else if (const int error = fetch(lock, *key)) {
            return error;
        }
    }
    Node &n = m_nodes[*key];
    *fd = ::open(native(contentPath(*key)).constData(), (flags & O_ACCMODE) | O_CLOEXEC);
    if (*fd < 0)
        return -errno;
    if (trunc && (n.size != 0 || n.id.isEmpty() || !n.dirty)) {
        if (::ftruncate(*fd, 0) != 0) {
            const int error = errno;
            ::close(*fd);
            return -error;
        }
        if (n.size != 0) {
            n.size = 0;
            markDirty(n);
        }
    }
    ++n.open;
    if (writable)
        ++n.writers;
    n.lastUse = QDateTime::currentSecsSinceEpoch();
    return 0;
}

int SyncCore::create(const QString &path, int flags, int *fd, quint64 *key) {
    QMutexLocker lock(&m_mutex);
    QString name;
    const quint64 parent = findParent(path, &name);
    if (!parent)
        return -ENOENT;
    if (invalidName(name))
        return -EINVAL;
    if (trashFolder(parent, name))
        return -EPERM;
    if (clash(parent, name, 0))
        return -EEXIST;
    Node n;
    n.key = m_nextKey++;
    n.cached = n.dirty = true;
    n.modified = QDateTime::currentMSecsSinceEpoch();
    n.lastWrite = now();
    n.lastUse = QDateTime::currentSecsSinceEpoch();
    *fd = ::open(native(contentPath(n.key)).constData(),
                 (flags & O_ACCMODE) | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (*fd < 0)
        return -errno;
    n.open = 1;
    n.writers = (flags & O_ACCMODE) != O_RDONLY ? 1 : 0;
    *key = n.key;
    m_nodes.insert(n.key, n);
    attach(n.key, parent, name);
    reclaim(n.key);
    changed(n.key);
    return 0;
}

void SyncCore::wrote(quint64 key, qint64 end) {
    QMutexLocker lock(&m_mutex);
    const auto it = m_nodes.find(key);
    if (it == m_nodes.end())
        return;
    it->size = qMax(it->size, end);
    markDirty(*it);
}

void SyncCore::truncated(quint64 key, qint64 size) {
    QMutexLocker lock(&m_mutex);
    const auto it = m_nodes.find(key);
    if (it == m_nodes.end())
        return;
    it->size = size;
    markDirty(*it);
}

void SyncCore::closed(quint64 key, bool writable) {
    QMutexLocker lock(&m_mutex);
    const auto it = m_nodes.find(key);
    if (it == m_nodes.end())
        return;
    --it->open;
    if (writable) {
        --it->writers;
        if (it->dirty)
            it->lastWrite = now();
    }
    if (!it->writers && m_stale.remove(it->id))
        m_refresh.insert(it->id);
    m_wake.wakeAll();
}

int SyncCore::truncate(const QString &path, qint64 size) {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return -ENOENT;
    if (m_nodes[key].dir)
        return -EISDIR;
    if (m_nodes[key].cached && m_nodes[key].size == size)
        return 0;
    if (!m_nodes[key].cached) {
        if (size == 0) {
            QFile file(contentPath(key));
            if (!file.open(QIODevice::WriteOnly))
                return -EIO;
            m_nodes[key].cached = true;
        } else if (const int error = fetch(lock, key)) {
            return error;
        }
    }
    if (::truncate(native(contentPath(key)).constData(), size) != 0)
        return -errno;
    Node &n = m_nodes[key];
    n.size = size;
    markDirty(n);
    return 0;
}

int SyncCore::mkdir(const QString &path) {
    QMutexLocker lock(&m_mutex);
    QString name;
    const quint64 parent = findParent(path, &name);
    if (!parent)
        return -ENOENT;
    if (invalidName(name))
        return -EINVAL;
    if (trashFolder(parent, name))
        return -EPERM;
    if (clash(parent, name, 0))
        return -EEXIST;
    Node n;
    n.key = m_nextKey++;
    n.dir = true;
    n.modified = QDateTime::currentMSecsSinceEpoch();
    m_nodes.insert(n.key, n);
    attach(n.key, parent, name);
    changed(n.key);
    return 0;
}

int SyncCore::unlink(const QString &path) {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return -ENOENT;
    if (m_nodes[key].dir)
        return -EISDIR;
    bury(m_nodes[key]);
    erase(key);
    return 0;
}

int SyncCore::rmdir(const QString &path) {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return -ENOENT;
    if (key == kRoot)
        return -EBUSY;
    if (!m_nodes[key].dir)
        return -ENOTDIR;
    if (!m_nodes[key].kids.isEmpty())
        return -ENOTEMPTY;
    bury(m_nodes[key]);
    erase(key);
    return 0;
}

int SyncCore::rename(const QString &from, const QString &to, bool noReplace) {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(from);
    if (!key)
        return -ENOENT;
    if (key == kRoot)
        return -EBUSY;
    QString name;
    const quint64 parent = findParent(to, &name);
    if (!parent)
        return -ENOENT;
    if (invalidName(name))
        return -EINVAL;
    if (trashFolder(parent, name))
        return -EPERM;
    for (quint64 p = parent; p; p = m_nodes.value(p).parent)
        if (p == key)
            return -EINVAL;  // into itself
    const quint64 target = child(parent, name);
    if (target == key)
        return 0;
    if (target) {
        if (noReplace)
            return -EEXIST;
        const Node &t = m_nodes[target];
        Node &s = m_nodes[key];
        if (s.dir && !t.dir)
            return -ENOTDIR;
        if (!s.dir && t.dir)
            return -EISDIR;
        if (t.dir && !t.kids.isEmpty())
            return -ENOTEMPTY;
        if (!s.dir && s.id.isEmpty() && !t.id.isEmpty() && !transient(name)) {
            // A new file saved over an existing one (how editors save):
            // it becomes a new version of that item, keeping its history.
            s.id = t.id;
            s.eTag = t.eTag;
            s.cTag = t.cTag;
            s.remoteParent = t.remoteParent;
            s.remoteName = t.remoteName;
            s.dirty = true;
            m_byId.insert(s.id, key);
            m_nodes[target].id.clear();
        } else {
            bury(t, true);  // before the move takes its name
        }
        erase(target);
    } else if (clash(parent, name, key)) {
        return -EEXIST;
    }
    Node &s = m_nodes[key];
    // Hidden as a FUSE leftover: gone from OneDrive, kept here while open.
    if (!s.dir && transient(name) && !s.id.isEmpty()) {
        bury(s);
        forgetRemote(key);
    }
    detach(key);
    attach(key, parent, name);
    reclaim(key);
    m_nodes[key].conflict = false;  // renaming a conflict copy settles it
    changed(key);
    return 0;
}

int SyncCore::setModified(const QString &path, qint64 ms) {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return -ENOENT;
    Node &n = m_nodes[key];
    if (n.dir || n.modified == ms)
        return 0;
    n.modified = ms;
    if (n.cached) {
        const struct timespec times[2] = {{0, UTIME_OMIT}, {ms / 1000, (ms % 1000) * 1000000}};
        ::utimensat(AT_FDCWD, native(contentPath(key)).constData(), times, 0);
    }
    changed(key);
    return 0;
}

// Something here that OneDrive does not have yet, at key or below it.
bool SyncCore::busy(const Node &n) const {
    if (needsPush(n) || n.conflict || n.writers > 0)
        return true;
    for (const quint64 kid : n.kids)
        if (busy(m_nodes[kid]))
            return true;
    return false;
}

SyncCore::Claim SyncCore::claim(const QString &from, const QString &to, QString *error) {
    QString ignored;
    if (!error)
        error = &ignored;
    const QStringList parts = to.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty() || std::any_of(parts.cbegin(), parts.cend(), invalidName)) {
        *error = QString("Invalid destination %1").arg(to);
        return ClaimInvalid;
    }

    QMutexLocker lock(&m_mutex);
    const quint64 key = find(from);
    if (!key)
        return ClaimTaken;
    if (key == kRoot) {
        *error = "Cannot claim the root";
        return ClaimInvalid;
    }
    if (busy(m_nodes[key]) || m_nodes[key].id.isEmpty())
        return ClaimPending;
    const QString id = m_nodes[key].id, eTag = m_nodes[key].eTag;

    // The folders on the way, made on OneDrive when missing.
    quint64 parent = kRoot;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        if (parent == key) {
            *error = "Cannot move a folder into itself";
            return ClaimInvalid;
        }
        const quint64 existing = child(parent, parts[i]);
        if (existing) {
            if (!m_nodes[existing].dir) {
                *error = QString("%1 is not a folder").arg(parts[i]);
                return ClaimInvalid;
            }
            if (m_nodes[existing].id.isEmpty())
                return ClaimPending;  // made here, not on OneDrive yet
            parent = existing;
            continue;
        }
        const QString parentId = m_nodes[parent].id;
        lock.unlock();
        RemoteItem made;
        Remote::Result result = m_remote->createFolder(parentId, parts[i], &made, error);
        if (result == Remote::Taken)  // made meanwhile, here or elsewhere
            result = m_remote->lookup(parentId, parts[i], &made, error);
        lock.relock();
        if (result != Remote::Ok || !made.dir) {
            if (result == Remote::Ok)
                *error = QString("%1 is not a folder").arg(parts[i]);
            return ClaimFailed;
        }
        applyItems({made}, false);
        parent = m_byId.value(made.id);
        if (!parent)
            return ClaimFailed;
    }
    if (parent == key) {
        *error = "Cannot move a folder into itself";
        return ClaimInvalid;
    }
    const QString parentId = m_nodes[parent].id;
    lock.unlock();
    RemoteItem moved;
    const Remote::Result result = m_remote->move(id, eTag, parentId, parts.last(), &moved, error);
    lock.relock();
    switch (result) {
    case Remote::Ok:
        applyItems({moved}, false);
        return Claimed;
    case Remote::Changed:
    case Remote::Gone:
        // Moved or changed elsewhere: show OneDrive's version soon.
        m_refresh.insert(id);
        m_pullWanted = true;
        m_wake.wakeAll();
        return ClaimTaken;
    case Remote::Taken:
        return ClaimTaken;
    default:
        m_lastError = *error;
        return ClaimFailed;
    }
}

// ---- status

QString SyncCore::state(const QString &path) const {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    return key ? stateOf(m_nodes[key]) : QString();
}

QJsonObject SyncCore::info(const QString &path) const {
    QMutexLocker lock(&m_mutex);
    const quint64 key = find(path);
    if (!key)
        return {};
    const Node &n = m_nodes[key];
    QJsonObject out{
        {"path", pathOf(key)},
        {"state", stateOf(n)},
        {"type", n.dir ? "folder" : "file"},
        {"size", n.dir ? 0 : n.size},
        {"modified", QDateTime::fromMSecsSinceEpoch(n.modified, QTimeZone::UTC).toString(Qt::ISODate)},
        {"eTag", n.eTag},
        {"id", n.id},
        {"cached", n.cached},
    };
    if (!n.error.isEmpty())
        out.insert("error", n.error);
    return out;
}

QJsonObject SyncCore::status() const {
    QMutexLocker lock(&m_mutex);
    int pending = m_tombstones.size(), conflicts = 0;
    QJsonArray items;
    for (const Node &n : std::as_const(m_nodes)) {
        const bool waiting = needsPush(n);
        if (!waiting && !n.conflict)
            continue;
        pending += waiting;
        conflicts += n.conflict;
        if (items.size() < 500) {
            QJsonObject item{{"path", pathOf(n.key)}, {"state", stateOf(n)}};
            if (!n.error.isEmpty())
                item.insert("error", n.error);
            items.append(item);
        }
    }
    for (const Tombstone &t : m_tombstones)
        if (items.size() < 500)
            items.append(QJsonObject{{"name", t.name}, {"state", "pending"}, {"action", "delete"}});
    QJsonObject out{
        {"pending", pending},
        {"conflicts", conflicts},
        {"items", items},
        {"lastSync", m_lastSync.isValid() ? m_lastSync.toString(Qt::ISODate) : QString()},
        {"error", m_lastError},
        {"uploading", m_busy},
    };
    if (m_total > 0) {
        out.insert("used", m_used);
        out.insert("total", m_total);
    }
    return out;
}

void SyncCore::quota(qint64 *used, qint64 *total) const {
    QMutexLocker lock(&m_mutex);
    *used = m_used;
    *total = m_total;
}

// ---- saving

namespace {

void writeNode(QDataStream &out, quint64 key, quint64 parent, const QString &name, bool dir, const QString &id,
               const QString &remoteParent, const QString &remoteName, const QString &eTag, const QString &cTag,
               qint64 size, qint64 modified, qint64 lastUse, bool cached, bool dirty, bool conflict) {
    out << key << parent << name << dir << id << remoteParent << remoteName << eTag << cTag << size << modified
        << lastUse << cached << dirty << conflict;
}

}  // namespace

void SyncCore::record(quint64 key) {
    const auto it = m_nodes.constFind(key);
    if (it == m_nodes.cend())
        return;
    const Node &n = *it;
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << quint8(NodeRecord);
    writeNode(out, n.key, n.parent, n.name, n.dir, n.id, n.remoteParent, n.remoteName, n.eTag, n.cTag, n.size,
              n.modified, n.lastUse, n.cached, n.dirty, n.conflict);
    appendJournal(bytes);
}

void SyncCore::recordErase(quint64 key) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << quint8(EraseRecord) << key;
    appendJournal(bytes);
}

void SyncCore::recordTombstones() {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << quint8(TombstoneRecord) << quint32(m_tombstones.size());
    for (const Tombstone &t : std::as_const(m_tombstones))
        out << t.id << t.eTag << t.cTag << t.parentId << t.name << t.dir;
    appendJournal(bytes);
}

void SyncCore::appendJournal(const QByteArray &record) {
    if (!m_journal.isOpen()) {
        m_journal.setFileName(m_dir + "/journal.bin");
        if (!m_journal.open(QIODevice::WriteOnly | QIODevice::Append))
            return;
    }
    m_journal.write(record);
    m_journal.flush();
    ++m_journalRecords;
}

void SyncCore::save() {
    QMutexLocker lock(&m_mutex);
    saveLocked();
}

void SyncCore::saveLocked() {
    QDir().mkpath(m_dir);
    QSaveFile file(m_dir + "/state.bin");
    if (!file.open(QIODevice::WriteOnly))
        return;
    QDataStream out(&file);
    out << kMagic << kVersion << m_delta << quint32(m_tombstones.size());
    for (const Tombstone &t : std::as_const(m_tombstones))
        out << t.id << t.eTag << t.cTag << t.parentId << t.name << t.dir;
    out << quint32(m_nodes.size());
    for (const Node &n : std::as_const(m_nodes))
        writeNode(out, n.key, n.parent, n.name, n.dir, n.id, n.remoteParent, n.remoteName, n.eTag, n.cTag, n.size,
                  n.modified, n.lastUse, n.cached, n.dirty, n.conflict);
    if (!file.commit())
        return;
    m_journal.close();
    QFile::remove(m_dir + "/journal.bin");
    m_journalRecords = 0;
}

bool SyncCore::load() {
    QMutexLocker lock(&m_mutex);
    auto readNode = [](QDataStream &in, Node *n) {
        in >> n->key >> n->parent >> n->name >> n->dir >> n->id >> n->remoteParent >> n->remoteName >> n->eTag
            >> n->cTag >> n->size >> n->modified >> n->lastUse >> n->cached >> n->dirty >> n->conflict;
    };
    auto readTombstones = [this](QDataStream &in) {
        quint32 count = 0;
        in >> count;
        m_tombstones.clear();
        for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
            Tombstone t;
            in >> t.id >> t.eTag >> t.cTag >> t.parentId >> t.name >> t.dir;
            m_tombstones.append(t);
        }
    };

    bool found = false;
    QFile file(m_dir + "/state.bin");
    if (file.open(QIODevice::ReadOnly)) {
        QDataStream in(&file);
        quint32 magic = 0, version = 0, count = 0;
        in >> magic >> version;
        if (magic == kMagic && version == kVersion) {
            in >> m_delta;
            readTombstones(in);
            in >> count;
            for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
                Node n;
                readNode(in, &n);
                m_nodes.insert(n.key, n);
            }
            found = in.status() == QDataStream::Ok;
        }
    }

    int replayed = 0;
    QFile journal(m_dir + "/journal.bin");
    if (journal.open(QIODevice::ReadOnly)) {
        QDataStream in(&journal);
        while (!in.atEnd()) {
            quint8 kind = 0;
            in >> kind;
            Node n;
            quint64 key = 0;
            if (kind == NodeRecord) {
                readNode(in, &n);
            } else if (kind == EraseRecord) {
                in >> key;
            } else if (kind == TombstoneRecord) {
                readTombstones(in);
            } else {
                break;
            }
            if (in.status() != QDataStream::Ok)
                break;  // cut short by a crash
            if (kind == NodeRecord)
                m_nodes.insert(n.key, n);
            else if (kind == EraseRecord)
                m_nodes.remove(key);
            ++replayed;
        }
    }

    if (!m_nodes.contains(kRoot) || !m_nodes[kRoot].dir) {
        Node root;
        root.key = kRoot;
        root.dir = true;
        m_nodes.insert(kRoot, root);
    }
    m_nodes[kRoot].parent = 0;
    m_byId.clear();
    m_todo.clear();
    QList<quint64> lost;
    for (Node &n : m_nodes) {
        n.kids.clear();
        m_nextKey = qMax(m_nextKey, n.key + 1);
        if (!n.id.isEmpty())
            m_byId.insert(n.id, n.key);
        if (n.cached && !n.dir) {
            const QFileInfo content(contentPath(n.key));
            if (!content.isFile()) {
                n.cached = false;
                if (n.dirty) {
                    n.dirty = false;
                    if (n.id.isEmpty())
                        lost.append(n.key);
                }
            } else if (n.dirty) {
                n.size = content.size();
                n.modified = content.lastModified().toMSecsSinceEpoch();
            }
        }
    }
    for (Node &n : m_nodes)
        if (n.key != kRoot) {
            if (n.parent && m_nodes.contains(n.parent))
                m_nodes[n.parent].kids.insert(n.name, n.key);
            else
                n.parent = 0;
        }
    for (const quint64 key : lost)
        erase(key);
    for (const Node &n : std::as_const(m_nodes))
        if (needsPush(n))
            m_todo.insert(n.key);
    const qint64 t = now();
    for (Tombstone &stone : m_tombstones)
        stone.due = t;
    // Downloads cut short by a crash.
    for (const QString &part : QDir(m_dir + "/content").entryList({"*.part"}, QDir::Files))
        QFile::remove(m_dir + "/content/" + part);
    if (replayed)
        saveLocked();
    return found && !m_nodes[kRoot].id.isEmpty();
}
