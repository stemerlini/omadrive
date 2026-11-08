#include "transfer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QClipboard>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QUrl>
#include <QtConcurrent>

#include "backend.h"
#include "graph.h"
#include "mirror.h"

namespace {

using Say = std::function<void(const QString &)>;

QString tr(const char *text) {
    return QCoreApplication::translate("Transfer", text);
}

QString plural(const char *text, int n) {
    return QCoreApplication::translate("Transfer", text, nullptr, n);
}

QString join(const QStringList &errors) {
    return errors.join('\n');
}

QString size(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1);
}

// Each returns the problems it met, one per line, and carries on past a
// failed file so one bad item does not stop the rest.
// With merge (the mirror) a folder joins an existing one and a file replaces
// an older local copy, whose previous version goes to the trash; an identical
// copy is left alone. Without it nothing is touched and clashes are renamed.
QStringList downloadOne(Graph &graph, const QString &remote, const QString &localDir, const Say &say, bool merge) {
    const GraphReply meta = graph.call("GET", Graph::itemUrl(remote));
    if (!meta.ok())
        return {meta.error};
    const QJsonObject item = meta.json();
    const QString name = item.value("name").toString();

    if (item.contains("folder")) {
        const QString target = merge ? localDir + "/" + name : Backend::uniquePath(localDir, name);
        if (!QDir().mkpath(target))
            return {tr("Cannot create %1").arg(target)};
        QString error;
        const QList<QJsonObject> kids = graph.children(remote, &error);
        if (!error.isEmpty())
            return {error};
        QStringList errors;
        for (const QJsonObject &kid : kids)
            errors += downloadOne(graph, Graph::joinPath(remote, kid.value("name").toString()), target, say, merge);
        return errors;
    }

    const QString url = item.value("@microsoft.graph.downloadUrl").toString();
    if (url.isEmpty())
        return {tr("OneDrive gave no download link for %1").arg(name)};

    const qint64 total = item.value("size").toInteger();
    QString stamp = item.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString();
    if (stamp.isEmpty())
        stamp = item.value("lastModifiedDateTime").toString();
    const QDateTime modified = QDateTime::fromString(stamp, Qt::ISODate);

    const QString target = merge ? localDir + "/" + name : Backend::uniquePath(localDir, name);
    if (merge && QFileInfo::exists(target)) {
        if (Mirror::sameFile(target, total, modified))
            return {};
        // A copy changed on this device after OneDrive's is an edit, not a
        // stale download: keep it.
        if (modified.isValid() && QFileInfo(target).lastModified() > modified.addSecs(2))
            return {tr("%1 is newer on this device and was kept; upload it to update OneDrive").arg(name)};
        if (!QFile::moveToTrash(target))
            return {tr("Cannot move the old %1 to the trash").arg(name)};
    }
    const QString partial = target + ".omadrive-part";
    QFile::remove(partial);
    QString error;
    const bool ok = graph.download(QUrl(url), partial, [&](qint64 got) {
        say(QString("%1: %2 / %3").arg(name, size(got), size(total)));
    }, &error);
    if (!ok) {
        QFile::remove(partial);
        return {QString("%1: %2").arg(name, error)};
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        return {tr("Cannot write %1").arg(target)};
    }
    // Keep OneDrive's time, so the copy is known to match.
    QFile file(target);
    if (modified.isValid() && file.open(QIODevice::Append))
        file.setFileTime(modified, QFileDevice::FileModificationTime);
    return {};
}

QStringList uploadOne(Graph &graph, const QString &local, const QString &remoteParent, const Say &say) {
    const QFileInfo info(local);
    if (info.isSymLink())
        return {tr("%1 is a symbolic link and was skipped").arg(info.fileName())};

    QString error;
    if (info.isDir()) {
        const QString name = graph.createFolder(remoteParent, info.fileName(), false, &error);
        if (name.isEmpty())
            return {error};
        const QString remote = Graph::joinPath(remoteParent, name);
        QStringList errors;
        const auto kids = QDir(local).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &kid : kids)
            errors += uploadOne(graph, kid.absoluteFilePath(), remote, say);
        return errors;
    }

    const qint64 total = info.size();
    if (!graph.uploadFile(local, remoteParent, [&](qint64 sent) {
            say(QString("%1: %2 / %3").arg(info.fileName(), size(sent), size(total)));
        }, &error))
        return {QString("%1: %2").arg(info.fileName(), error)};
    return {};
}

// Removes the mirror's copy of an item where it matches OneDrive, so the
// item is online only again. A copy that differs is kept: it may hold edits.
void freeItem(Graph &graph, const QString &remote, const QJsonObject &item, const QString &root,
              qint64 *freed, int *kept, QStringList *errors) {
    const QString local = Mirror::localPath(root, remote);
    if (item.contains("folder")) {
        if (!QFileInfo(local).isDir())
            return;
        QString error;
        const QList<QJsonObject> kids = graph.children(remote, &error);
        if (!error.isEmpty()) {
            *errors << QString("%1: %2").arg(item.value("name").toString(), error);
            return;
        }
        for (const QJsonObject &kid : kids)
            freeItem(graph, Graph::joinPath(remote, kid.value("name").toString()), kid, root, freed, kept, errors);
        QDir().rmdir(local);  // only when nothing is left in it
        return;
    }
    if (!QFileInfo::exists(local))
        return;
    QString stamp = item.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString();
    if (stamp.isEmpty())
        stamp = item.value("lastModifiedDateTime").toString();
    const qint64 bytes = item.value("size").toInteger();
    if (!Mirror::sameFile(local, bytes, QDateTime::fromString(stamp, Qt::ISODate))) {
        ++*kept;
        return;
    }
    if (QFile::remove(local))
        *freed += bytes;
    else
        *errors << tr("Cannot remove %1").arg(local);
}

}  // namespace

void Transfer::quick(std::function<Result()> job) {
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const Result result = watcher->result();
        watcher->deleteLater();
        if (result.error.isEmpty() && !result.copy.isEmpty())
            QGuiApplication::clipboard()->setText(result.copy);
        if (result.error.isEmpty() && result.open.isValid())
            QDesktopServices::openUrl(result.open);
        emit done(result.error, result.text);
    });
    watcher->setFuture(QtConcurrent::run(job));
}

void Transfer::start(std::function<Result(const Say &)> job) {
    if (m_busy) {
        emit finished(tr("Another OneDrive operation is still running"), {});
        return;
    }
    m_busy = true;
    emit busyChanged();

    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const Result result = watcher->result();
        watcher->deleteLater();
        m_busy = false;
        m_progress.clear();
        emit busyChanged();
        emit progressChanged();
        emit finished(result.error, result.text);
    });
    watcher->setFuture(QtConcurrent::run([this, job] {
        QElapsedTimer clock;
        // Progress arrives per network read; show it a few times a second.
        const Say say = [this, &clock](const QString &text) {
            if (clock.isValid() && clock.elapsed() < 150)
                return;
            clock.restart();
            QMetaObject::invokeMethod(this, [this, text] {
                m_progress = text;
                emit progressChanged();
            });
        };
        return job(say);
    }));
}

void Transfer::download(const QStringList &remotePaths, const QString &localDir, bool removeSource) {
    downloadAll(remotePaths, [localDir](const QString &) { return localDir; }, false, removeSource);
}

void Transfer::downloadToMirror(const QStringList &remotePaths, bool removeSource) {
    const QString root = Mirror::root();
    downloadAll(remotePaths, [root](const QString &remote) { return Mirror::localPath(root, Graph::parentPath(remote)); },
                true, removeSource);
}

void Transfer::downloadAll(const QStringList &remotePaths, const std::function<QString(const QString &)> &dirFor,
                           bool merge, bool removeSource) {
    Graph *account = &Graph::active();
    start([=](const Say &say) {
        Graph &graph = *account;
        QStringList errors;
        int done = 0;
        for (const QString &remote : remotePaths) {
            const QString dir = dirFor(remote);
            if (!QDir().mkpath(dir)) {
                errors << tr("Cannot create %1").arg(dir);
                continue;
            }
            const QStringList problems = downloadOne(graph, remote, dir, say, merge);
            if (!problems.isEmpty()) {
                errors += problems;
                continue;
            }
            ++done;
            if (removeSource) {
                const GraphReply gone = graph.call("DELETE", Graph::itemUrl(remote));
                if (!gone.ok())
                    errors << tr("Downloaded %1 but could not remove it from OneDrive").arg(remote.section('/', -1));
            }
        }
        return Result{join(errors), plural(removeSource ? "Moved %n items from OneDrive" : "Downloaded %n items", done)};
    });
}

void Transfer::upload(const QStringList &localPaths, const QString &remoteDir, bool removeSource) {
    Graph *account = &Graph::active();
    start([=](const Say &say) {
        Graph &graph = *account;
        QStringList errors;
        int done = 0;
        for (const QString &local : localPaths) {
            const QStringList problems = uploadOne(graph, local, remoteDir, say);
            if (!problems.isEmpty()) {
                errors += problems;
                continue;
            }
            ++done;
            if (removeSource && !QFile::moveToTrash(local))
                errors << tr("Uploaded %1 but could not move the original to the trash").arg(QFileInfo(local).fileName());
        }
        return Result{join(errors), plural(removeSource ? "Moved %n items to OneDrive" : "Uploaded %n items", done)};
    });
}

void Transfer::makeFolder(const QString &remoteParent, const QString &name) {
    Graph *graph = &Graph::active();
    start([=](const Say &) {
        QString error;
        if (name.isEmpty() || name.contains('/'))
            return Result{tr("Invalid folder name"), {}};
        graph->createFolder(remoteParent, name, true, &error);
        return Result{error, tr("Folder created")};
    });
}

void Transfer::makeFile(const QString &remoteParent, const QString &name, const QString &content) {
    Graph *graph = &Graph::active();
    start([=](const Say &) {
        if (name.isEmpty() || name.contains('/'))
            return Result{tr("Invalid file name"), {}};
        const GraphReply reply = graph->call(
            "PUT", Graph::itemUrl(Graph::joinPath(remoteParent, name)) + "/content?@microsoft.graph.conflictBehavior=fail",
            content.toUtf8(), "text/plain");
        const QString error = reply.status == 409 ? tr("%1 already exists").arg(name) : reply.error;
        return Result{error, tr("File created")};
    });
}

void Transfer::renameItem(const QString &remotePath, const QString &newName) {
    Graph *graph = &Graph::active();
    start([=](const Say &) {
        if (newName.isEmpty() || newName.contains('/'))
            return Result{tr("Invalid name"), {}};
        const GraphReply reply = graph->call(
            "PATCH", Graph::itemUrl(remotePath), QJsonDocument(QJsonObject{{"name", newName}}).toJson(QJsonDocument::Compact));
        const QString error = reply.status == 409 ? tr("%1 already exists").arg(newName) : reply.error;
        return Result{error, tr("Renamed")};
    });
}

void Transfer::trash(const QStringList &remotePaths) {
    Graph *graph = &Graph::active();
    start([=](const Say &) {
        QStringList errors;
        int done = 0;
        for (const QString &remote : remotePaths) {
            const GraphReply reply = graph->call("DELETE", Graph::itemUrl(remote));
            if (reply.ok())
                ++done;
            else
                errors << QString("%1: %2").arg(remote.section('/', -1), reply.error);
        }
        return Result{join(errors), plural("Moved %n items to the OneDrive recycle bin", done)};
    });
}

void Transfer::freeUp(const QStringList &remotePaths) {
    Graph *graph = &Graph::active();
    const QString root = Mirror::root();
    start([=](const Say &say) {
        qint64 freed = 0;
        int kept = 0;
        QStringList errors;
        for (const QString &remote : remotePaths) {
            say(tr("Freeing up %1").arg(remote.section('/', -1)));
            const GraphReply meta = graph->call("GET", Graph::itemUrl(remote));
            if (!meta.ok()) {
                errors << QString("%1: %2").arg(remote.section('/', -1), meta.error);
                continue;
            }
            freeItem(*graph, remote, meta.json(), root, &freed, &kept, &errors);
        }
        QString text = tr("Freed %1 on this device").arg(size(freed));
        if (kept > 0)
            text += " · " + plural("kept %n changed files", kept);
        return Result{join(errors), text};
    });
}

// Personal OneDrive links are for anyone with the link; work links can be
// for the organisation only ("organization") or for anyone ("anonymous"),
// where its admins allow that.
void Transfer::shareLink(const QString &remotePath, bool edit, const QString &scope) {
    Graph *graph = &Graph::active();
    quick([=] {
        QJsonObject body{{"type", edit ? "edit" : "view"}};
        if (!scope.isEmpty())
            body.insert("scope", scope);
        const GraphReply reply = graph->call("POST", Graph::itemUrl(remotePath) + "/createLink",
                                             QJsonDocument(body).toJson(QJsonDocument::Compact));
        const QString url = reply.json().value("link").toObject().value("webUrl").toString();
        if (!reply.ok() || url.isEmpty())
            return Result{reply.ok() ? tr("OneDrive gave no link") : reply.error, {}};
        Result result{{}, edit ? tr("Link copied: it can edit %1").arg(remotePath.section('/', -1))
                               : tr("Link copied: it can view %1").arg(remotePath.section('/', -1))};
        result.copy = url;
        return result;
    });
}

void Transfer::invite(const QString &remotePath, const QStringList &emails, bool edit, const QString &message) {
    Graph *graph = &Graph::active();
    quick([=] {
        QJsonArray recipients;
        for (const QString &email : emails)
            if (!email.trimmed().isEmpty())
                recipients.append(QJsonObject{{"email", email.trimmed()}});
        if (recipients.isEmpty())
            return Result{tr("Type at least one email address"), {}};
        QJsonObject body{{"recipients", recipients}, {"requireSignIn", true}, {"sendInvitation", true},
                         {"roles", QJsonArray{edit ? "write" : "read"}}};
        if (!message.trimmed().isEmpty())
            body.insert("message", message.trimmed());
        const GraphReply reply = graph->call("POST", Graph::itemUrl(remotePath) + "/invite",
                                             QJsonDocument(body).toJson(QJsonDocument::Compact));
        return Result{reply.error, plural("Shared %1 with %n people", recipients.size()).arg(remotePath.section('/', -1))};
    });
}

void Transfer::openOnline(const QString &remotePath) {
    Graph *graph = &Graph::active();
    quick([=] {
        const GraphReply reply = graph->call("GET", Graph::itemUrl(remotePath) + "?$select=webUrl");
        Result result{reply.error, tr("Opened in the browser")};
        result.open = QUrl(reply.json().value("webUrl").toString());
        if (result.error.isEmpty() && !result.open.isValid())
            result.error = tr("OneDrive gave no web address");
        return result;
    });
}
