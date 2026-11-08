#include "preview.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QtConcurrent>

#include "graph.h"
#include "mirror.h"

namespace {

constexpr qint64 kTextLimit = 256 * 1024;
// Larger OneDrive files are shown by their thumbnail instead of downloaded.
constexpr qint64 kDownloadLimit = 30 * 1024 * 1024;
constexpr int kFolderLimit = 500;

QString trp(const char *text) {
    return QCoreApplication::translate("Preview", text);
}

QString plural(const char *text, int n) {
    return QCoreApplication::translate("Preview", text, nullptr, n);
}

QString size(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1);
}

QString when(const QDateTime &time) {
    return time.isValid() ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat) : QString();
}

bool isImage(const QMimeType &mime) {
    static const QList<QByteArray> supported = QImageReader::supportedMimeTypes();
    if (supported.contains(mime.name().toUtf8()))
        return true;
    for (const QString &alias : mime.aliases())
        if (supported.contains(alias.toUtf8()))
            return true;
    return false;
}

bool isText(const QMimeType &mime) {
    return mime.inherits("text/plain");
}

bool isPdf(const QMimeType &mime) {
    return mime.name() == "application/pdf";
}

QString joinDetails(const QStringList &parts) {
    QStringList kept;
    for (const QString &part : parts)
        if (!part.isEmpty())
            kept << part;
    return kept.join(" · ");
}

}  // namespace

Preview::Preview(QObject *parent) : QObject(parent) {
    // Downloaded previews only live for one run of the app.
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/preview";
    QDir(m_cacheDir).removeRecursively();
    QDir().mkpath(m_cacheDir);
}

void Preview::set(const Result &result) {
    m_result = result;
    emit changed();
}

void Preview::clear() {
    ++m_generation;
    set({});
}

void Preview::show(const QString &path, bool remote) {
    const quint64 generation = ++m_generation;
    if (path.isEmpty()) {
        set({});
        return;
    }
    Result loading;
    loading.kind = "loading";
    loading.name = path.section('/', -1);
    set(loading);

    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        const Result result = watcher->result();
        watcher->deleteLater();
        if (generation == m_generation)
            set(result);
    });
    const QString cacheDir = m_cacheDir;
    Graph *graph = &Graph::active();
    const QString root = Mirror::root();
    watcher->setFuture(QtConcurrent::run([path, remote, cacheDir, graph, root] {
        return remote ? Preview::remote(*graph, root, path, cacheDir) : Preview::local(path);
    }));
}

Preview::Result Preview::local(const QString &path) {
    Result r;
    const QFileInfo info(path);
    r.name = info.fileName().isEmpty() ? path : info.fileName();
    if (!info.exists()) {
        r.kind = "error";
        r.details = trp("The file no longer exists");
        return r;
    }

    if (info.isDir()) {
        const QFileInfoList kids = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &kid : kids.mid(0, kFolderLimit))
            r.entries << (kid.isDir() ? kid.fileName() + "/" : kid.fileName());
        r.kind = "folder";
        r.details = joinDetails({trp("Folder"), plural("%n items", kids.size()), when(info.lastModified())});
        return r;
    }

    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    r.details = joinDetails({mime.comment(), size(info.size()), when(info.lastModified())});
    r.source = QUrl::fromLocalFile(info.absoluteFilePath());
    // Checked first: QtPdf also offers PDF as an image format (page 1 only).
    if (isPdf(mime)) {
        r.kind = "pdf";
    } else if (isImage(mime)) {
        r.kind = "image";
    } else if (isText(mime)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            r.kind = "error";
            r.details = trp("Cannot read %1").arg(r.name);
            return r;
        }
        const QByteArray data = file.read(kTextLimit);
        r.text = QString::fromUtf8(data);
        if (info.size() > kTextLimit)
            r.text += "\n…";
        r.kind = "text";
    } else {
        r.kind = "info";
    }
    return r;
}

Preview::Result Preview::remote(Graph &graph, const QString &mirrorRoot, const QString &path, const QString &cacheDir) {
    Result r;
    r.name = path.section('/', -1);
    const GraphReply meta = graph.call("GET", Graph::itemUrl(path));
    if (!meta.ok()) {
        r.kind = "error";
        r.details = meta.error;
        return r;
    }
    const QJsonObject item = meta.json();

    if (item.contains("folder")) {
        QString error;
        const QList<QJsonObject> kids = graph.children(path, &error);
        if (!error.isEmpty()) {
            r.kind = "error";
            r.details = error;
            return r;
        }
        QStringList dirs, files;
        for (const QJsonObject &kid : kids)
            (kid.contains("folder") ? dirs : files) << kid.value("name").toString();
        const auto byName = [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        for (QString &dir : dirs)
            dir += "/";
        r.entries = (dirs + files).mid(0, kFolderLimit);
        r.kind = "folder";
        r.details = joinDetails({trp("OneDrive folder"), plural("%n items", kids.size())});
        return r;
    }

    const qint64 bytes = item.value("size").toInteger();
    QString stamp = item.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString();
    if (stamp.isEmpty())
        stamp = item.value("lastModifiedDateTime").toString();
    const QDateTime modified = QDateTime::fromString(stamp, Qt::ISODate);

    // A matching copy in the mirror needs no download.
    const QString mirrored = Mirror::localPath(mirrorRoot, path);
    if (Mirror::sameFile(mirrored, bytes, modified)) {
        r = local(mirrored);
        r.details = joinDetails({r.details, trp("on this device")});
        r.cloud = "local";
        return r;
    }

    QMimeDatabase db;
    QMimeType mime = db.mimeTypeForName(item.value("file").toObject().value("mimeType").toString());
    if (!mime.isValid() || mime.isDefault())
        mime = db.mimeTypeForFile(r.name, QMimeDatabase::MatchExtension);
    const QString where = trp("online only");
    r.details = joinDetails({mime.comment(), size(bytes), when(modified), where});
    r.cloud = "online";

    const QString key = QCryptographicHash::hash((path + item.value("eTag").toString()).toUtf8(),
                                                 QCryptographicHash::Sha1).toHex();
    const QString downloadUrl = item.value("@microsoft.graph.downloadUrl").toString();
    if ((isImage(mime) || isPdf(mime) || isText(mime)) && bytes <= kDownloadLimit && !downloadUrl.isEmpty()) {
        const QString suffix = QFileInfo(r.name).suffix();
        const QString file = cacheDir + "/" + key + (suffix.isEmpty() ? QString() : "." + suffix);
        QString error;
        if (QFileInfo::exists(file) || graph.download(QUrl(downloadUrl), file, [](qint64) {}, &error)) {
            Result shown = local(file);
            shown.name = r.name;
            shown.details = r.details;
            shown.cloud = r.cloud;
            return shown;
        }
        QFile::remove(file);
    }

    // Office documents, videos and big files: OneDrive renders a thumbnail.
    const GraphReply thumbs = graph.call("GET", Graph::itemUrl(path) + "/thumbnails?select=large");
    const QJsonArray sets = thumbs.json().value("value").toArray();
    const QString thumbUrl = sets.isEmpty() ? QString()
                                            : sets.at(0).toObject().value("large").toObject().value("url").toString();
    if (thumbs.ok() && !thumbUrl.isEmpty()) {
        const QString file = cacheDir + "/" + key + "-thumb";
        QString error;
        if (QFileInfo::exists(file) || graph.download(QUrl(thumbUrl), file, [](qint64) {}, &error)) {
            r.kind = "image";
            r.source = QUrl::fromLocalFile(file);
            return r;
        }
        QFile::remove(file);
    }
    r.kind = "info";
    return r;
}
