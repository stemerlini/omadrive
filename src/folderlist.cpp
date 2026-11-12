#include "folderlist.h"
#include "labels.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
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
    if (Labels::instance())
        connect(Labels::instance(), &Labels::changed, this, &FolderList::updateLabels);
}

QString FolderList::kindOf(const QString &path, bool isDir, bool executable, QString *typeText) {
    if (isDir) {
        if (typeText) *typeText = tr("Folder");
        return "folder";
    }
    static const QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    const QString name = mime.name();
    if (typeText)
        *typeText = mime.isDefault() ? (QFileInfo(path).suffix().isEmpty() ? tr("File") : QFileInfo(path).suffix().toUpper() + tr(" file"))
                                     : mime.comment();
    auto any = [&](std::initializer_list<const char *> parts) {
        for (const char *part : parts)
            if (name.contains(QLatin1String(part)))
                return true;
        return false;
    };
    if (name.startsWith("image/")) return "image";
    if (name.startsWith("video/")) return "video";
    if (name.startsWith("audio/")) return "audio";
    if (name == "application/pdf") return "pdf";
    if (any({"zip", "tar", "rar", "7z", "gzip", "bzip", "-xz", "zstd", "compressed", "archive", "iso9660", "lzma"}))
        return "archive";
    if (any({"spreadsheet", "ms-excel", "text/csv"})) return "spreadsheet";
    if (any({"presentation", "powerpoint"})) return "slides";
    if (any({"wordprocessing", "msword", "opendocument.text", "rtf", "epub"})) return "document";
    if (name == "text/markdown") return "markdown";
    if (any({"shellscript"}) || (executable && !mime.inherits("text/plain") && mime.isDefault()))
        return "executable";
    if (name == "text/plain") return "text";
    if (mime.inherits("text/plain") || any({"json", "xml", "javascript", "yaml", "toml", "x-python"}))
        return "code";
    return executable ? "executable" : "file";
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
    case SizeRole: return e.isDir ? -1 : e.size;
    case AgeRole: return e.modified.isValid() ? e.modified.secsTo(QDateTime::currentDateTime()) : -1;
    case ModifiedFullRole: return QLocale().toString(e.modified, QLocale::LongFormat);
    case KindRole: return e.kind;
    case TypeTextRole: return e.typeText;
    case ExtensionRole: return e.isDir ? QString() : QFileInfo(e.name).suffix().toLower();
    case ChildCountRole: return e.children;
    case LabelRole: return e.label;
    case HiddenRole: return e.hidden;
    case LinkRole: return !e.linkTarget.isEmpty();
    case LinkTargetRole: return e.linkTarget;
    }
    return {};
}

QHash<int, QByteArray> FolderList::roleNames() const {
    return {{NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"},
            {SizeTextRole, "sizeText"}, {ModifiedRole, "modified"}, {CloudRole, "cloud"},
            {SizeRole, "size"}, {AgeRole, "age"}, {ModifiedFullRole, "modifiedFull"}, {KindRole, "kind"},
            {TypeTextRole, "typeText"}, {ExtensionRole, "extension"}, {ChildCountRole, "childCount"},
            {LabelRole, "label"}, {HiddenRole, "hidden"}, {LinkRole, "isLink"}, {LinkTargetRole, "linkTarget"}};
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
    const Labels *labels = Labels::instance();
    for (const QFileInfo &info : infos) {
        Entry e{info.fileName(), info.absoluteFilePath(), info.isDir(), info.size(), info.lastModified(),
                Rclone::state(m_mount, info.absoluteFilePath(), info.isDir())};
        e.kind = kindOf(e.path, e.isDir, info.isExecutable(), &e.typeText);
        e.hidden = info.isHidden();
        e.linkTarget = info.isSymLink() ? info.symLinkTarget() : QString();
        e.label = labels ? labels->label(e.path) : QString();
        entries.append(e);
    }
    sortEntries(entries);

    // Folders keep their last state and count until the background pass updates them.
    for (Entry &e : entries)
        if (e.isDir)
            for (const Entry &old : std::as_const(m_entries))
                if (old.path == e.path) {
                    e.cloud = old.cloud;
                    e.children = old.children;
                    break;
                }
    beginResetModel();
    m_entries = entries;
    endResetModel();
    ++m_generation;
    m_folderBusy = false;
    emit listed();
    updateFolders();
    updateChildCounts();
}

void FolderList::sortEntries(QVector<Entry> &entries) const {
    const QString key = m_sortKey;
    const bool descending = m_sortDescending;
    std::stable_sort(entries.begin(), entries.end(), [key, descending](const Entry &a, const Entry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        int order = 0;
        if (key == "size" && !a.isDir)
            order = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
        else if (key == "modified")
            order = a.modified < b.modified ? -1 : a.modified > b.modified ? 1 : 0;
        else if (key == "type")
            order = a.typeText.compare(b.typeText, Qt::CaseInsensitive);
        if (order == 0)
            order = byName;
        return descending ? order > 0 : order < 0;
    });
}

void FolderList::setSortKey(const QString &key) {
    if (key == m_sortKey || !QStringList{"name", "type", "size", "modified"}.contains(key))
        return;
    m_sortKey = key;
    // Newest and largest first, as people mostly want.
    m_sortDescending = key == "size" || key == "modified";
    emit sortChanged();
    refresh();
}

void FolderList::setSortDescending(bool descending) {
    if (descending == m_sortDescending)
        return;
    m_sortDescending = descending;
    emit sortChanged();
    refresh();
}

int FolderList::folderCount() const {
    return std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Entry &e) { return e.isDir; });
}

qint64 FolderList::totalSize() const {
    qint64 total = 0;
    for (const Entry &e : m_entries)
        if (!e.isDir)
            total += e.size;
    return total;
}

qint64 FolderList::largestSize() const {
    qint64 largest = 0;
    for (const Entry &e : m_entries)
        if (!e.isDir)
            largest = std::max(largest, e.size);
    return largest;
}

QString FolderList::formatSize(qint64 bytes) const {
    return QLocale().formattedDataSize(bytes, 1);
}

qint64 FolderList::sizeOf(const QStringList &paths) const {
    qint64 total = 0;
    for (const Entry &e : m_entries)
        if (!e.isDir && paths.contains(e.path))
            total += e.size;
    return total;
}

void FolderList::updateLabels() {
    const Labels *labels = Labels::instance();
    for (int i = 0; i < m_entries.size(); ++i) {
        const QString label = labels ? labels->label(m_entries[i].path) : QString();
        if (label != m_entries[i].label) {
            m_entries[i].label = label;
            emit dataChanged(index(i), index(i), {LabelRole});
        }
    }
}

// How many items each folder holds, listed in the background: in a cloud
// mount every listing can be a request to OneDrive.
void FolderList::updateChildCounts() {
    QStringList dirs;
    for (const Entry &e : std::as_const(m_entries))
        if (e.isDir && dirs.size() < 500)
            dirs << e.path;
    if (dirs.isEmpty())
        return;
    const quint64 generation = m_generation;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (m_showHidden)
        filters |= QDir::Hidden;
    QPointer<FolderList> self(this);
    (void)QtConcurrent::run([self, generation, dirs, filters] {
        for (const QString &dir : dirs) {
            if (!self)
                return;
            const int count = QDir(dir).entryList(filters).size();
            QMetaObject::invokeMethod(self, [self, generation, dir, count] {
                if (!self || generation != self->m_generation)
                    return;
                const int row = self->indexOfPath(dir);
                if (row >= 0 && self->m_entries[row].children != count) {
                    self->m_entries[row].children = count;
                    emit self->dataChanged(self->index(row), self->index(row), {ChildCountRole});
                }
            });
        }
    });
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
