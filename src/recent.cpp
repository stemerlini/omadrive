#include "recent.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>

#include "graph.h"
#include "mirror.h"

// OneDrive's /recent lists files recently *opened*, and search cannot sort,
// so the newest changes come from the drive's delta feed. The first run reads
// the whole drive once (about 30 s for 9000 files); the delta link and the
// newest files are cached, so later runs only read what changed since.

namespace {

const char kDrive[] = "https://graph.microsoft.com/v1.0/me/drive";
const int kKept = 50;

QString cacheFile(const QString &kind, const QString &id) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omadrive/" + kind + "/"
        + QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex()
        + (kind == "avatars" ? ".png" : ".json");
}

// "/drive/root:/Documents/Work" -> "/Documents/Work"; empty when not given.
QString folderOf(const QJsonObject &parent) {
    const QString path = parent.value("path").toString();
    const int colon = path.indexOf("root:");
    if (colon < 0)
        return {};
    const QString folder = QUrl::fromPercentEncoding(path.mid(colon + 5).toUtf8());
    return folder.isEmpty() ? QString("/") : folder;
}

QJsonObject fileOf(const QJsonObject &item) {
    const QString name = item.value("name").toString();
    const QString folder = folderOf(item.value("parentReference").toObject());
    return QJsonObject{
        {"id", item.value("id").toString()},
        {"name", name},
        {"changed", item.value("lastModifiedDateTime").toString()},
        {"stamp", item.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString(
                      item.value("lastModifiedDateTime").toString())},
        {"modifiedBy", item.value("lastModifiedBy").toObject().value("user").toObject().value("displayName").toString()},
        {"size", item.value("size").toVariant().toLongLong()},
        {"webUrl", item.value("webUrl").toString()},
        {"path", folder.isEmpty() ? QString() : Graph::joinPath(folder, name)},
    };
}

// Newest first, keeping kKept.
void trim(QList<QJsonObject> &files) {
    std::sort(files.begin(), files.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("changed").toString() > b.value("changed").toString();
    });
    if (files.size() > kKept)
        files.resize(kKept);
}

// Brings the cached newest files up to date; false with error when OneDrive could not be read.
bool update(Graph &graph, const QString &id, int limit, QList<QJsonObject> &files, QString *error) {
    const QString cache = cacheFile("recent", id);
    QString url;
    {
        QFile file(cache);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
            url = json.value("delta").toString();
            for (const QJsonValue &v : json.value("files").toArray())
                files.append(v.toObject());
        }
    }
    const QString delta = graph.delta(
        url, "id,name,size,webUrl,lastModifiedDateTime,lastModifiedBy,fileSystemInfo,parentReference,file,deleted",
        [&](const QJsonObject &item) {
            // A renamed or moved folder does not list its files again, so every
            // kept path may be out of date: look them up again below.
            if (!item.contains("file") && !item.contains("root"))
                for (QJsonObject &f : files) {
                    f.remove("path");
                    f.remove("webUrl");
                }
            const QString itemId = item.value("id").toString();
            files.erase(std::remove_if(files.begin(), files.end(),
                                       [&](const QJsonObject &f) { return f.value("id").toString() == itemId; }),
                        files.end());
            if (item.contains("file") && !item.contains("deleted"))
                files.append(fileOf(item));
            if (files.size() > 1000)
                trim(files);
        },
        [&] { files.clear(); }, error);
    if (delta.isEmpty())
        return false;
    trim(files);
    // Work drives leave the folder out of delta results, and folder changes
    // clear it above; only the files about to be shown are looked up.
    for (int i = 0; i < files.size() && i < limit; ++i) {
        QJsonObject &f = files[i];
        if (!f.value("path").toString().isEmpty() && !f.value("webUrl").toString().isEmpty())
            continue;
        const GraphReply reply = graph.call("GET", QString(kDrive) + "/items/" + f.value("id").toString()
                                                       + "?$select=name,webUrl,parentReference");
        if (reply.status == 404) {  // gone with its folder
            files.removeAt(i--);
            continue;
        }
        const QJsonObject item = reply.json();
        const QString folder = folderOf(item.value("parentReference").toObject());
        if (reply.ok() && !folder.isEmpty()) {
            f.insert("name", item.value("name").toString(f.value("name").toString()));
            f.insert("path", Graph::joinPath(folder, f.value("name").toString()));
            f.insert("webUrl", item.value("webUrl").toString());
        }
    }
    QJsonArray list;
    for (const QJsonObject &f : std::as_const(files))
        list.append(f);
    QDir().mkpath(QFileInfo(cache).absolutePath());
    QSaveFile out(cache);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(QJsonObject{{"delta", delta}, {"files", list}}).toJson(QJsonDocument::Compact));
        out.commit();
    }
    return true;
}

QJsonObject accountReport(const QJsonObject &a, int limit) {
    const QString id = a.value("id").toString();
    const bool business = a.value("business").toBool();
    const QString mirror = a.value("mirror").toString();
    const QString avatar = cacheFile("avatars", id);
    QJsonObject out{
        {"id", id},
        {"name", a.value("name").toString()},
        {"email", a.value("email").toString()},
        {"business", business},
        {"driveName", QString("OneDrive - %1").arg(business ? a.value("org").toString() : QString("Personal"))},
        {"mirror", mirror},
        {"avatar", QFile::exists(avatar) ? avatar : QString()},
    };
    Graph graph(id);
    QString error;
    if (!graph.restore(&error)) {
        out.insert("error", error.isEmpty() ? QString("Not signed in") : error);
        return out;
    }
    const GraphReply drive = graph.call("GET", QString(kDrive) + "?$select=quota");
    if (drive.ok()) {
        const QJsonObject quota = drive.json().value("quota").toObject();
        out.insert("used", quota.value("used").toVariant().toLongLong());
        out.insert("total", quota.value("total").toVariant().toLongLong());
    }
    QList<QJsonObject> files;
    if (!update(graph, id, limit, files, &error))
        out.insert("error", error);  // the cached list is still shown
    QJsonArray list;
    for (QJsonObject f : std::as_const(files)) {
        if (list.size() >= limit)
            break;
        const QString path = f.value("path").toString();
        if (!path.isEmpty() && !mirror.isEmpty()) {
            const QString local = Mirror::localPath(mirror, path);
            f.insert("local", local);
            f.insert("isLocal", Mirror::sameFile(local, f.value("size").toVariant().toLongLong(),
                                                 QDateTime::fromString(f.value("stamp").toString(), Qt::ISODate)));
        }
        f.remove("stamp");
        list.append(f);
    }
    out.insert("files", list);
    return out;
}

}  // namespace

int printRecent(int limit) {
    if (limit <= 0)
        limit = 10;
    QFile file(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive/accounts.json");
    QJsonObject json;
    if (file.open(QIODevice::ReadOnly))
        json = QJsonDocument::fromJson(file.readAll()).object();
    const QString active = json.value("active").toString();

    // Accounts are read in parallel; Graph calls block.
    QList<QFuture<QJsonObject>> jobs;
    for (const QJsonValue &a : json.value("accounts").toArray())
        jobs.append(QtConcurrent::run([a, limit] { return accountReport(a.toObject(), limit); }));
    QJsonArray out;
    for (auto &job : jobs) {
        QJsonObject report = job.result();
        report.insert("active", report.value("id").toString() == active);
        out.append(report);
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"accounts", out}}).toJson(QJsonDocument::Compact) << "\n";
    return 0;
}
