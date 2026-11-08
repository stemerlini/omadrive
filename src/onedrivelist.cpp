#include "onedrivelist.h"

#include <QFutureWatcher>
#include <QJsonObject>
#include <QLocale>
#include <QtConcurrent>

#include <algorithm>

#include "account.h"
#include "graph.h"
#include "mirror.h"

namespace {

constexpr int kPrefetchFolders = 8;

struct Listing {
    QList<QJsonObject> items;
    QString error;
};

}  // namespace

namespace {
Account *s_account = nullptr;
}

void OneDriveList::setAccount(Account *account) {
    s_account = account;
}

QHash<QString, QVector<OneDriveList::Entry>> &OneDriveList::cache() const {
    static QHash<QString, QHash<QString, QVector<Entry>>> listings;
    return listings[m_accountId];
}

OneDriveList::OneDriveList(QObject *parent) : QAbstractListModel(parent), m_account(s_account) {
    Q_ASSERT(m_account);
    m_accountId = m_account->activeId();
    connect(m_account, &Account::stateChanged, this, &OneDriveList::accountStateChanged);
    m_signedIn = m_account->signedIn();
    if (m_signedIn)
        navigate(m_target);
}

// Another account keeps its own place: this tab goes back to wherever it was
// in that account, or its root.
void OneDriveList::accountStateChanged() {
    const QString id = m_account->activeId();
    const bool signedIn = m_account->signedIn();
    const bool switched = id != m_accountId;
    if (switched) {
        m_places.insert(m_accountId, m_target);
        m_accountId = id;
        m_target = m_places.value(id, "/");
        emit accountChanged();
        clear();
    }
    if (signedIn && (switched || !m_signedIn))
        navigate(m_target);
    else if (!signedIn && m_signedIn && !switched)
        clear();
    m_signedIn = signedIn;
}

int OneDriveList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant OneDriveList::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case IsDirRole: return e.isDir;
    case SizeTextRole: return e.isDir ? QString() : QLocale().formattedDataSize(e.size, 1);
    case ModifiedRole: return e.modified.isValid() ? QLocale().toString(e.modified, QLocale::ShortFormat) : QString();
    case CloudRole: return e.cloud;
    }
    return {};
}

QHash<int, QByteArray> OneDriveList::roleNames() const {
    return {{NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"},
            {SizeTextRole, "sizeText"}, {ModifiedRole, "modified"}, {CloudRole, "cloud"}};
}

void OneDriveList::setPath(const QString &path) {
    const QString clean = "/" + path.split('/', Qt::SkipEmptyParts).join('/');
    if (m_account->signedIn()) {
        navigate(clean);
        return;
    }
    // Remembered until the sign-in finishes, e.g. a tab restored at start.
    m_target = clean;
    if (m_path != clean) {
        m_path = clean;
        emit pathChanged();
    }
}

void OneDriveList::up() {
    if (m_target != "/")
        navigate(Graph::parentPath(m_target));
}

void OneDriveList::refresh() {
    if (m_account->signedIn())
        fetch(m_target);
}

// A folder seen before appears immediately and is then verified in the
// background. A new one keeps the pane where it is until it arrives, so a
// folder that cannot be opened leaves the pane unchanged.
void OneDriveList::navigate(const QString &path) {
    m_target = path;
    setError({});
    if (cache().contains(path))
        show(path, cache().value(path));
    fetch(path);
}

void OneDriveList::fetch(const QString &path) {
    if (path == m_target)
        setLoading(!cache().contains(path));

    const QString account = m_accountId;
    Graph *graph = Graph::forAccount(account);
    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path, account] {
        const Listing listing = watcher->result();
        watcher->deleteLater();

        QVector<Entry> entries;
        for (const QJsonObject &item : listing.items) {
            const QString name = item.value("name").toString();
            // The time the file was last changed on a device, as other
            // OneDrive clients keep it; it is what the mirror is compared with.
            QString stamp = item.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString();
            if (stamp.isEmpty())
                stamp = item.value("lastModifiedDateTime").toString();
            entries.append({name, Graph::joinPath(path, name), item.contains("folder"),
                            item.value("size").toInteger(),
                            QDateTime::fromString(stamp, Qt::ISODate).toLocalTime(), {}});
        }
        // Folders first, then names without regard to case.
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.isDir != b.isDir)
                return a.isDir;
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        received(account, path, entries, listing.error);
    });
    watcher->setFuture(QtConcurrent::run([path, graph] {
        Listing listing;
        listing.items = graph->children(path, &listing.error);
        return listing;
    }));
}

void OneDriveList::received(const QString &account, const QString &path, const QVector<Entry> &entries,
                            const QString &error) {
    if (account != m_accountId || !m_account->signedIn())
        return;
    if (error.isEmpty())
        cache().insert(path, entries);

    if (path == m_target) {
        setLoading(false);
        if (!error.isEmpty())
            setError(error);
        else
            show(path, entries);
    }
    // Prefetch around the folder being viewed, and keep draining the queue.
    if (m_prefetching || (error.isEmpty() && path == m_target))
        prefetch();
}

void OneDriveList::show(const QString &path, QVector<Entry> entries) {
    for (Entry &e : entries)
        e.cloud = Mirror::state(e.path, e.isDir, e.size, e.modified);
    if (path != m_path) {
        m_path = path;
        emit pathChanged();
    }
    // An unchanged listing must not reset the view and lose the highlight.
    const bool same = entries.size() == m_entries.size()
        && std::equal(entries.begin(), entries.end(), m_entries.begin(), [](const Entry &a, const Entry &b) {
               return a.path == b.path && a.size == b.size && a.modified == b.modified && a.isDir == b.isDir;
           });
    if (same) {
        for (int i = 0; i < entries.size(); ++i) {
            if (entries.at(i).cloud != m_entries.at(i).cloud) {
                m_entries[i].cloud = entries.at(i).cloud;
                emit dataChanged(index(i), index(i), {CloudRole});
            }
        }
        return;
    }
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

void OneDriveList::recheck() {
    show(m_path, m_entries);
}

// Quietly load the folders you are most likely to open next.
void OneDriveList::prefetch() {
    if (!m_prefetching) {
        m_prefetchQueue.clear();
        for (const Entry &e : std::as_const(m_entries))
            if (e.isDir && !cache().contains(e.path) && m_prefetchQueue.size() < kPrefetchFolders)
                m_prefetchQueue << e.path;
    }
    while (!m_prefetchQueue.isEmpty()) {
        const QString next = m_prefetchQueue.takeFirst();
        if (!cache().contains(next)) {
            m_prefetching = true;
            fetch(next);
            return;
        }
    }
    m_prefetching = false;
}

// Empties the view; the target stays, to be shown once signed in.
void OneDriveList::clear() {
    m_prefetchQueue.clear();
    m_prefetching = false;
    setLoading(false);
    setError({});
    if (m_path != m_target) {
        m_path = m_target;
        emit pathChanged();
    }
    beginResetModel();
    m_entries.clear();
    endResetModel();
}

QString OneDriveList::pathAt(int row) const {
    return row >= 0 && row < m_entries.size() ? m_entries.at(row).path : QString();
}

bool OneDriveList::isDirAt(int row) const {
    return row >= 0 && row < m_entries.size() && m_entries.at(row).isDir;
}

int OneDriveList::indexOfPath(const QString &path) const {
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).path == path)
            return i;
    return -1;
}

void OneDriveList::setLoading(bool loading) {
    if (loading == m_loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void OneDriveList::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}
