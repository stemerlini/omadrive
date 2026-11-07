#include "onedrivelist.h"

#include <QFutureWatcher>
#include <QJsonObject>
#include <QLocale>
#include <QtConcurrent>

#include <algorithm>

#include "account.h"
#include "graph.h"

namespace {

struct Listing {
    QList<QJsonObject> items;
    QString error;
};

}  // namespace

OneDriveList::OneDriveList(Account *account, QObject *parent) : QAbstractListModel(parent), m_account(account) {
    connect(account, &Account::stateChanged, this, [this] {
        if (m_account->signedIn()) {
            load("/");
        } else if (!m_entries.isEmpty() || m_path != "/") {
            ++m_generation;
            clear();
        }
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
    }
    return {};
}

QHash<int, QByteArray> OneDriveList::roleNames() const {
    return {{NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"},
            {SizeTextRole, "sizeText"}, {ModifiedRole, "modified"}};
}

void OneDriveList::setPath(const QString &path) {
    QString clean = "/" + path.split('/', Qt::SkipEmptyParts).join('/');
    if (clean == m_path && m_error.isEmpty() && !m_entries.isEmpty())
        return;
    load(clean);
}

void OneDriveList::up() {
    if (m_path != "/")
        load(Graph::parentPath(m_path));
}

void OneDriveList::refresh() {
    if (m_account->signedIn())
        load(m_path);
}

// The path only changes once the listing arrives, so a folder that cannot be
// opened leaves the pane where it was.
void OneDriveList::load(const QString &path) {
    const int generation = ++m_generation;
    m_loading = true;
    emit loadingChanged();

    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, path] {
        const Listing listing = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        m_loading = false;
        emit loadingChanged();

        if (!listing.error.isEmpty()) {
            setError(listing.error);
            return;
        }

        QVector<Entry> entries;
        for (const QJsonObject &item : listing.items) {
            const QString name = item.value("name").toString();
            entries.append({name, Graph::joinPath(path, name), item.contains("folder"),
                            item.value("size").toInteger(),
                            QDateTime::fromString(item.value("lastModifiedDateTime").toString(), Qt::ISODate).toLocalTime()});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.isDir != b.isDir)
                return a.isDir;
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });

        const bool moved = path != m_path;
        m_path = path;
        setError({});
        if (moved)
            emit pathChanged();
        beginResetModel();
        m_entries = entries;
        endResetModel();
    });
    watcher->setFuture(QtConcurrent::run([path] {
        Listing listing;
        listing.items = Graph::instance().children(path, &listing.error);
        return listing;
    }));
}

void OneDriveList::clear() {
    m_loading = false;
    emit loadingChanged();
    setError({});
    const bool moved = m_path != "/";
    m_path = "/";
    if (moved)
        emit pathChanged();
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

void OneDriveList::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}
