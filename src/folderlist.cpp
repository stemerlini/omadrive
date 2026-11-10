#include "folderlist.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QtConcurrent>

#include <algorithm>

FolderList::FolderList(QObject *parent) : QAbstractListModel(parent) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &FolderList::refresh);
    // rclone changes its cache without touching the folder, so look again.
    m_cloudPoll.setInterval(2000);
    connect(&m_cloudPoll, &QTimer::timeout, this, &FolderList::updateCloud);
}

int FolderList::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant FolderList::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case IsDirRole: return e.isDir;
    case SizeTextRole: return e.isDir ? QString() : QLocale().formattedDataSize(e.size, 1);
    case ModifiedRole: return QLocale().toString(e.modified, QLocale::ShortFormat);
    case CloudRole: return e.cloud;
    }
    return {};
}

QHash<int, QByteArray> FolderList::roleNames() const {
    return {{NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"},
            {SizeTextRole, "sizeText"}, {ModifiedRole, "modified"}, {CloudRole, "cloud"}};
}

void FolderList::setPath(const QString &path) {
    const QString clean = QDir::cleanPath(QDir(path).absolutePath());
    if (clean == m_path && m_error.isEmpty())
        return;

    const QFileInfo info(clean);
    if (!info.isDir()) {
        setError(tr("%1 is not a folder").arg(clean));
        return;
    }
    if (!info.isReadable() || !info.isExecutable()) {
        setError(tr("No permission to open %1").arg(clean));
        return;
    }

    if (!m_path.isEmpty())
        m_watcher.removePath(m_path);
    m_path = clean;
    m_watcher.addPath(m_path);
    m_mount = Rclone::mountFor(m_path);
    if (m_mount.isValid())
        m_cloudPoll.start();
    else
        m_cloudPoll.stop();
    setError({});
    emit pathChanged();
    refresh();
}

void FolderList::setShowHidden(bool show) {
    if (show == m_showHidden)
        return;
    m_showHidden = show;
    emit showHiddenChanged();
    refresh();
}

void FolderList::up() {
    const QString parent = QFileInfo(m_path).absolutePath();
    if (parent != m_path)
        setPath(parent);
}

void FolderList::refresh() {
    if (m_path.isEmpty())
        return;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (m_showHidden)
        filters |= QDir::Hidden;

    QVector<Entry> entries;
    const auto infos = QDir(m_path).entryInfoList(filters);
    entries.reserve(infos.size());
    for (const QFileInfo &info : infos)
        entries.append({info.fileName(), info.absoluteFilePath(), info.isDir(), info.size(), info.lastModified(),
                        Rclone::state(m_mount, info.absoluteFilePath(), info.isDir())});

    // Folders first, then names without regard to case.
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    // Folders keep their last state until the background pass updates it.
    for (Entry &e : entries)
        if (e.isDir)
            for (const Entry &old : std::as_const(m_entries))
                if (old.path == e.path) {
                    e.cloud = old.cloud;
                    break;
                }
    beginResetModel();
    m_entries = entries;
    endResetModel();
    ++m_generation;
    m_folderBusy = false;
    updateFolders();
}

QString FolderList::pathAt(int row) const {
    return row >= 0 && row < m_entries.size() ? m_entries.at(row).path : QString();
}

bool FolderList::isDirAt(int row) const {
    return row >= 0 && row < m_entries.size() && m_entries.at(row).isDir;
}

int FolderList::indexOfPath(const QString &path) const {
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).path == path)
            return i;
    return -1;
}

void FolderList::updateCloud() {
    if (++m_folderTick % 5 == 0)
        updateFolders();
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.isDir)
            continue;
        const QString cloud = Rclone::state(m_mount, e.path, e.isDir);
        if (cloud != e.cloud) {
            e.cloud = cloud;
            emit dataChanged(index(i), index(i), {CloudRole});
        }
    }
}

bool FolderList::download(const QStringList &paths) {
    if (!m_mount.isValid())
        return false;
    const QString root = m_mount.mountpoint;
    (void)QtConcurrent::run([paths, root] {
        const auto fetch = [](const QString &file) {
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly))
                return;
            while (!f.read(1 << 20).isEmpty()) {}
        };
        for (const QString &path : paths) {
            if (!path.startsWith(root + '/'))
                continue;
            if (!QFileInfo(path).isDir()) {
                fetch(path);
                continue;
            }
            QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (it.hasNext())
                fetch(it.next());
        }
    });
    return true;
}

void FolderList::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

// Folder states walk their trees, so they are worked out in the background
// (every 10 s while shown) and dropped if the listing changed meanwhile.
void FolderList::updateFolders() {
    if (!m_mount.isValid() || m_folderBusy)
        return;
    QStringList dirs;
    for (const Entry &e : std::as_const(m_entries))
        if (e.isDir)
            dirs << e.path;
    if (dirs.isEmpty())
        return;
    m_folderBusy = true;
    const quint64 generation = m_generation;
    const Rclone::Mount mount = m_mount;
    QPointer<FolderList> self(this);
    (void)QtConcurrent::run([self, generation, mount, dirs] {
        for (const QString &dir : dirs) {
            const QString state = Rclone::folderState(mount, dir);
            if (!self)
                return;
            QMetaObject::invokeMethod(self, [self, generation, dir, state] {
                if (!self || generation != self->m_generation)
                    return;
                const int row = self->indexOfPath(dir);
                if (row >= 0 && self->m_entries[row].cloud != state) {
                    self->m_entries[row].cloud = state;
                    emit self->dataChanged(self->index(row), self->index(row), {CloudRole});
                }
            });
        }
        if (self)
            QMetaObject::invokeMethod(self, [self, generation] {
                if (self && generation == self->m_generation)
                    self->m_folderBusy = false;
            });
    });
}
