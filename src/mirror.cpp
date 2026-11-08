#include "mirror.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QStandardPaths>

namespace {
QMutex s_mutex;
QString s_root;
QHash<QString, QString> s_roots;  // account id -> its folder
QPointer<Mirror> s_mirror;
}  // namespace

Mirror::Mirror(QObject *parent) : QObject(parent) {
    s_mirror = this;
}

QString Mirror::root() {
    QMutexLocker lock(&s_mutex);
    return s_root.isEmpty() ? personalRoot() : s_root;
}

QString Mirror::rootFor(const QString &account) {
    QMutexLocker lock(&s_mutex);
    return s_roots.value(account);
}

void Mirror::setRoots(const QHash<QString, QString> &roots, const QString &active) {
    {
        QMutexLocker lock(&s_mutex);
        s_roots = roots;
        if (roots.value(active) == s_root)
            return;
        s_root = roots.value(active);
    }
    if (s_mirror)
        emit s_mirror->rootChanged();
}

QString Mirror::personalRoot() {
    const QString configured = configuredRoot();
    return configured.isEmpty() ? QDir::homePath() + "/OneDrive-Personal" : configured;
}

QString Mirror::configuredRoot() {
    static const QString path = [] {
        QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive/auth.json");
        QString configured;
        if (file.open(QIODevice::ReadOnly))
            configured = QJsonDocument::fromJson(file.readAll()).object().value("mirror").toString();
        if (configured.startsWith("~"))
            configured = QDir::homePath() + configured.mid(1);
        return configured.isEmpty() ? QString() : QDir::cleanPath(configured);
    }();
    return path;
}

QString Mirror::localPath(const QString &remotePath) {
    return localPath(root(), remotePath);
}

QString Mirror::localPath(const QString &root, const QString &remotePath) {
    return QDir::cleanPath(root + "/" + remotePath);
}

bool Mirror::sameFile(const QString &localFile, qint64 size, const QDateTime &modified) {
    const QFileInfo info(localFile);
    // OneDrive keeps whole seconds; allow for file systems that round.
    return info.isFile() && info.size() == size && modified.isValid()
        && qAbs(info.lastModified().secsTo(modified)) <= 2;
}

QString Mirror::state(const QString &root, const QString &remotePath, bool isDir, qint64 size,
                      const QDateTime &modified) {
    const QString local = localPath(root, remotePath);
    if (isDir)
        return QFileInfo(local).isDir() ? "local" : "online";
    if (!QFileInfo::exists(local))
        return "online";
    return sameFile(local, size, modified) ? "local" : "changed";
}
