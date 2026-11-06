#include "folderlist.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>

#include <algorithm>

FolderList::FolderList(QObject *parent) : QAbstractListModel(parent) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &FolderList::refresh);
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
    }
    return {};
}

QHash<int, QByteArray> FolderList::roleNames() const {
    return {{NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"},
            {SizeTextRole, "sizeText"}, {ModifiedRole, "modified"}};
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
        entries.append({info.fileName(), info.absoluteFilePath(), info.isDir(), info.size(), info.lastModified()});

    // Folders first, then names without regard to case.
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    beginResetModel();
    m_entries = entries;
    endResetModel();
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

void FolderList::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}
