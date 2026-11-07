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

OneDriveList::OneDriveList(Account *account, QObject *parent) : QAbstractListModel(parent), m_account(account) {
    connect(account, &Account::stateChanged, this, [this] {
        if (m_account->signedIn())
            navigate("/");
        else
            clear();
    });
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
    navigate("/" + path.split('/', Qt::SkipEmptyParts).join('/'));
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
    if (m_cache.contains(path))
        show(path, m_cache.value(path));
    fetch(path);
}

void OneDriveList::fetch(const QString &path) {
    if (path == m_target)
        setLoading(!m_cache.contains(path));

    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path] {
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
        received(path, entries, listing.error);
    });
    watcher->setFuture(QtConcurrent::run([path] {
        Listing listing;
        listing.items = Graph::instance().children(path, &listing.error);
        return listing;
    }));
}

void OneDriveList::received(const QString &path, const QVector<Entry> &entries, const QString &error) {
    if (!m_account->signedIn())
        return;
    if (error.isEmpty())
        m_cache.insert(path, entries);

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
            if (e.isDir && !m_cache.contains(e.path) && m_prefetchQueue.size() < kPrefetchFolders)
                m_prefetchQueue << e.path;
    }
    while (!m_prefetchQueue.isEmpty()) {
        const QString next = m_prefetchQueue.takeFirst();
        if (!m_cache.contains(next)) {
            m_prefetching = true;
            fetch(next);
            return;
        }
    }
    m_prefetching = false;
}

void OneDriveList::clear() {
    m_cache.clear();
    m_prefetchQueue.clear();
    m_prefetching = false;
    m_target = "/";
    setLoading(false);
    setError({});
    if (m_path != "/") {
        m_path = "/";
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
