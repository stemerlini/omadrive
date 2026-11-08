#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMultiHash>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

// One OneDrive item as the sync folder sees it.
struct RemoteItem {
    QString id, parentId, name, eTag, cTag;
    bool dir = false, root = false, deleted = false;
    bool foreign = false;  // a shortcut to another drive: a shared folder or SharePoint library
    qint64 size = 0;
    int children = -1;  // a folder's child count, when known
    QDateTime modified;
};

// What the sync folder needs from OneDrive. GraphRemote talks only to the
// account's own drive (/me/drive); the tests use a fake.
class Remote {
public:
    // Changed: the item changed there since our eTag (412). Taken: the name
    // is in use (409). Gone: no such item (404).
    enum Result { Ok, Changed, Taken, Gone, Failed };

    virtual ~Remote() = default;
    // The changes since link, or every item when link is empty or expired
    // (then reset is set). next is the link for the following call.
    virtual bool delta(const QString &link, QList<RemoteItem> *items, QString *next, bool *reset, QString *error) = 0;
    virtual Result item(const QString &id, RemoteItem *out, QString *error) = 0;
    virtual Result download(const QString &id, const QString &file, RemoteItem *out, QString *error) = 0;
    // Replaces item id's content if it is still at eTag, or, with no id, adds
    // a new file to parentId without ever replacing one there.
    virtual Result upload(const QString &id, const QString &eTag, const QString &parentId, const QString &name,
                          const QString &file, const QDateTime &modified, RemoteItem *out, QString *error) = 0;
    virtual Result createFolder(const QString &parentId, const QString &name, RemoteItem *out, QString *error) = 0;
    virtual Result move(const QString &id, const QString &eTag, const QString &parentId, const QString &name,
                        RemoteItem *out, QString *error) = 0;
    // To the OneDrive recycle bin, and only if unchanged since eTag.
    virtual Result remove(const QString &id, const QString &eTag, QString *error) = 0;
    virtual bool quota(qint64 *used, qint64 *total) {
        Q_UNUSED(used) Q_UNUSED(total)
        return false;
    }
};

// The state behind a sync folder: the whole OneDrive tree, which files are
// cached, and the local changes still to send. File contents live in
// <cacheDir>/content/<key>, so renames never move them. The tree is saved to
// state.bin and every local change is appended to journal.bin as it happens,
// so nothing waiting to upload is lost if the service stops.
//
// The file system calls take paths from the folder's root ("/a/b.txt") and
// return 0 or -errno. pull() and push() talk to OneDrive and are called by
// one worker thread; the file system calls can come from any thread.
class SyncCore {
public:
    struct Options {
        int settleMs = 5000;   // upload this long after the last write
        qint64 cacheMax = 0;   // bytes of unchanged files to keep; 0 keeps all
        QString host;          // for conflict copies; the host name by default
    };
    struct Stat {
        bool dir = false;
        qint64 size = 0;
        qint64 modified = 0;  // ms since the epoch
        bool cached = false;
    };

    SyncCore(Remote *remote, const QString &cacheDir, const Options &options);
    ~SyncCore();

    static QString cacheDirFor(const QString &account);
    // "report (conflict host 2026-11-08 2250).docx"
    static QString conflictName(const QString &name, const QString &host, const QDateTime &when, int n = 1);
    // Why the account's folder cannot be mounted at mountpoint (inside or over
    // another account's folder, where its files would mix), or empty.
    static QString mountRefusal(const QString &account, const QString &mountpoint, const QStringList &accounts);
    // Files that only matter on this device and are never uploaded: editor
    // locks and swap files, and FUSE's hidden copies of deleted open files.
    static bool transient(const QString &name);

    // Reads the saved tree; false on the first run, when there is none.
    bool load();
    void save();

    bool pull(QString *error);
    // Sends the local changes that are due (all of them when force); returns
    // how many were sent.
    int push(bool force = false);
    bool hasWork() const;
    bool takePullWanted();
    // Sleeps until a local change or ms pass.
    void wait(int ms);
    void wake();

    int stat(const QString &path, Stat *out) const;
    int list(const QString &path, QList<QPair<QString, Stat>> *out) const;
    // Opens the cached copy, downloading it first; fd is the cache file's.
    int open(const QString &path, int flags, int *fd, quint64 *key);
    int create(const QString &path, int flags, int *fd, quint64 *key);
    void wrote(quint64 key, qint64 end);
    void truncated(quint64 key, qint64 size);
    void closed(quint64 key, bool writable);
    int truncate(const QString &path, qint64 size);
    int mkdir(const QString &path);
    int unlink(const QString &path);
    int rmdir(const QString &path);
    int rename(const QString &from, const QString &to, bool noReplace);
    int setModified(const QString &path, qint64 ms);

    // cloud, local, pending or conflict; empty when there is no such item.
    QString state(const QString &path) const;
    QJsonObject info(const QString &path) const;
    // Counts, the items waiting or in conflict, and the last sync and error.
    QJsonObject status() const;
    void quota(qint64 *used, qint64 *total) const;

private:
    struct Node {
        quint64 key = 0, parent = 0;
        QString name;
        bool dir = false;
        QString id, remoteParent, remoteName, eTag, cTag;  // as OneDrive has it
        qint64 size = 0, modified = 0, lastUse = 0;
        bool cached = false, dirty = false, conflict = false;
        // Not saved.
        QMultiHash<QString, quint64> kids;
        quint64 gen = 0;
        qint64 lastWrite = 0, retryAt = 0;
        int open = 0, writers = 0, failures = 0;
        bool downloading = false;
        QString error;
    };
    struct Tombstone {
        QString id, eTag, cTag, parentId, name;
        bool dir = false;
        qint64 due = 0;      // not saved
        bool tried = false;  // not saved: a folder that still held changes
        int failures = 0;
    };
    struct Job;

    qint64 now() const { return m_clock.elapsed(); }
    QString contentPath(quint64 key) const;
    quint64 find(const QString &path) const;
    quint64 findParent(const QString &path, QString *name) const;
    quint64 child(quint64 parent, const QString &name) const;
    quint64 clash(quint64 parent, const QString &name, quint64 except) const;
    QString pathOf(quint64 key) const;
    Stat statOf(const Node &n) const;
    QString stateOf(const Node &n) const;
    bool isTransient(const Node &n) const;
    bool moved(const Node &n) const;
    bool needsPush(const Node &n) const;
    void attach(quint64 key, quint64 parent, const QString &name);
    void detach(quint64 key);
    void erase(quint64 key);
    void bury(const Node &n, bool soon = false);
    void reclaim(quint64 key);
    void dropTombstonesIn(const QString &id);
    bool holdsTombstones(const QString &id) const;
    void forgetRemote(quint64 key);
    void changed(quint64 key);
    void markDirty(Node &n);
    QString uniqueConflict(quint64 parent, const QString &name) const;
    void moveAside(quint64 key);
    void adoptName(quint64 key, const QString &name);
    quint64 splitConflict(quint64 key);
    void applyItems(const QList<RemoteItem> &items, bool reset);
    void updateFile(Node &n, const RemoteItem &item);
    void removeRemote(quint64 key);
    int fetch(QMutexLocker<QMutex> &lock, quint64 key);
    void evict();
    bool nextJob(bool force, Job *job);
    void runJob(Job &job);
    void finishJob(const Job &job);
    void fail(Node *n, const QString &error);
    void record(quint64 key);
    void recordErase(quint64 key);
    void recordTombstones();
    void appendJournal(const QByteArray &record);
    void saveLocked();

    Remote *m_remote;
    QString m_dir;
    Options m_options;
    QElapsedTimer m_clock;
    mutable QMutex m_mutex;
    QWaitCondition m_downloaded, m_wake;
    QHash<quint64, Node> m_nodes;
    QHash<QString, quint64> m_byId;
    QList<Tombstone> m_tombstones;
    QSet<quint64> m_todo;
    QSet<QString> m_refresh;  // item ids to read again
    QSet<QString> m_stale;    // changed there while open for writing here
    quint64 m_nextKey = 2;    // 1 is the root
    QString m_delta;
    QFile m_journal;
    int m_journalRecords = 0;
    bool m_pullWanted = false;
    QDateTime m_lastSync;
    QString m_lastError, m_busy;
    qint64 m_used = -1, m_total = -1;
};
