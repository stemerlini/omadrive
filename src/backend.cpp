#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

namespace {

bool isInside(const QString &child, const QString &parent) {
    return child == parent || child.startsWith(parent + "/");
}

// Names that would escape the folder or are empty are never valid.
bool validName(const QString &name) {
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/');
}

}  // namespace

QString Backend::uniquePath(const QString &directory, const QString &name) {
    QString candidate = directory + "/" + name;
    if (!QFileInfo::exists(candidate) && !QFileInfo(candidate).isSymLink())
        return candidate;

    const QFileInfo info(name);
    // "archive.tar.gz" keeps its full suffix, so the counter goes before ".tar.gz".
    const QString base = info.completeBaseName().isEmpty() ? name : info.baseName();
    const QString suffix = info.completeBaseName().isEmpty() ? QString() : name.mid(base.size());
    for (int i = 2;; ++i) {
        candidate = QString("%1/%2 (%3)%4").arg(directory, base).arg(i).arg(suffix);
        if (!QFileInfo::exists(candidate) && !QFileInfo(candidate).isSymLink())
            return candidate;
    }
}

QString Backend::copyOne(const QString &source, const QString &destination) {
    const QFileInfo info(source);
    const QString target = uniquePath(destination, info.fileName());

    if (info.isSymLink())
        return tr("%1 is a symbolic link and was skipped").arg(info.fileName());

    if (info.isDir()) {
        if (isInside(QFileInfo(destination).absoluteFilePath(), info.absoluteFilePath()))
            return tr("Cannot copy %1 into itself").arg(info.fileName());
        if (!QDir().mkdir(target))
            return tr("Cannot create %1").arg(target);
        QString errors;
        const auto children = QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &child : children) {
            const QString error = copyOne(child.absoluteFilePath(), target);
            if (!error.isEmpty())
                errors += (errors.isEmpty() ? "" : "\n") + error;
        }
        return errors;
    }

    // Copy beside the target and rename, so a failed copy never leaves a
    // half-written file under the final name.
    const QString partial = target + ".omadrive-part";
    QFile::remove(partial);
    if (!QFile::copy(source, partial)) {
        QFile::remove(partial);
        return tr("Cannot copy %1").arg(info.fileName());
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        return tr("Cannot write %1").arg(target);
    }
    return {};
}

QString Backend::copyItems(const QStringList &paths, const QString &destination) {
    if (!QFileInfo(destination).isDir() || !QFileInfo(destination).isWritable())
        return tr("Cannot write to %1").arg(destination);
    QString errors;
    for (const QString &path : paths) {
        const QString error = copyOne(path, destination);
        if (!error.isEmpty())
            errors += (errors.isEmpty() ? "" : "\n") + error;
    }
    return errors;
}

QString Backend::moveItems(const QStringList &paths, const QString &destination) {
    if (!QFileInfo(destination).isDir() || !QFileInfo(destination).isWritable())
        return tr("Cannot write to %1").arg(destination);
    QString errors;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (QFileInfo(info.absolutePath()) == QFileInfo(destination))
            continue;
        if (info.isDir() && isInside(QFileInfo(destination).absoluteFilePath(), info.absoluteFilePath())) {
            errors += tr("Cannot move %1 into itself").arg(info.fileName()) + "\n";
            continue;
        }

        const QString target = uniquePath(destination, info.fileName());
        if (QFile::rename(path, target))
            continue;

        // Across filesystems a rename fails, so copy and remove the original
        // only after the copy fully succeeded.
        const QString error = copyOne(path, destination);
        if (!error.isEmpty()) {
            errors += error + "\n";
            continue;
        }
        const bool removed = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!removed)
            errors += tr("Copied %1 but could not remove the original").arg(info.fileName()) + "\n";
    }
    return errors.trimmed();
}

QString Backend::trashItems(const QStringList &paths) {
    QString errors;
    for (const QString &path : paths)
        if (!QFile::moveToTrash(path))
            errors += (errors.isEmpty() ? "" : "\n") + tr("Cannot move %1 to the trash").arg(QFileInfo(path).fileName());
    return errors;
}

QString Backend::makeFolder(const QString &parent, const QString &name) {
    if (!validName(name))
        return tr("Invalid folder name");
    return QDir().mkdir(uniquePath(parent, name)) ? QString() : tr("Cannot create folder in %1").arg(parent);
}

QString Backend::makeFile(const QString &parent, const QString &name, const QString &content, bool executable) {
    if (!validName(name))
        return tr("Invalid file name");
    QFile file(uniquePath(parent, name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(content.toUtf8()) < 0)
        return tr("Cannot create file in %1").arg(parent);
    if (executable)
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    return {};
}

QString Backend::renameItem(const QString &path, const QString &newName) {
    if (!validName(newName))
        return tr("Invalid name");
    const QFileInfo info(path);
    const QString target = info.absolutePath() + "/" + newName;
    if (target == info.absoluteFilePath())
        return {};
    if (QFileInfo::exists(target))
        return tr("%1 already exists").arg(newName);
    return QFile::rename(path, target) ? QString() : tr("Cannot rename %1").arg(info.fileName());
}

bool Backend::isDir(const QString &path) const {
    return QFileInfo(path).isDir();
}

QString Backend::homePath() const {
    return QDir::homePath();
}

void Backend::openExternally(const QString &path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Backend::newWindow() {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--new-window"});
}

void Backend::copyText(const QString &text) {
    QGuiApplication::clipboard()->setText(text);
}
