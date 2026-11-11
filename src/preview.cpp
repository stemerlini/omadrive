#include "preview.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QtConcurrent>

namespace {

constexpr qint64 kTextLimit = 256 * 1024;
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

Preview::Preview(QObject *parent) : QObject(parent) {}

void Preview::set(const Result &result) {
    m_result = result;
    emit changed();
}

void Preview::clear() {
    ++m_generation;
    set({});
}

void Preview::show(const QString &path) {
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
    watcher->setFuture(QtConcurrent::run([path] { return Preview::local(path); }));
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
