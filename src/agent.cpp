#include "agent.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>

#include <sys/xattr.h>

#include "graph.h"
#include "mirror.h"
#include "mount.h"
#include "recent.h"
#include "sync.h"

// Every command prints a single JSON object. Reusing Graph means these share
// the app's sign-in, keyring tokens and work-account safety.

namespace {

QTextStream out(stdout);

int emitJson(const QJsonObject &obj) {
    out << QJsonDocument(obj).toJson(QJsonDocument::Compact) << "\n";
    out.flush();
    return obj.value("ok").toBool() ? 0 : 1;
}

int fail(const QString &message) {
    return emitJson({{"ok", false}, {"error", message}});
}

int okWith(QJsonObject obj) {
    obj.insert("ok", true);
    return emitJson(obj);
}

QString configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive";
}

QJsonObject accountsFile() {
    QFile file(configDir() + "/accounts.json");
    if (file.open(QIODevice::ReadOnly))
        return QJsonDocument::fromJson(file.readAll()).object();
    return {};
}

// Loads the roots and the work-account set so Mirror::workLeak works without
// the GUI, and returns the chosen account entry (the active one by default).
QJsonObject resolveAccounts(const QString &wanted, QString *error) {
    const QJsonObject json = accountsFile();
    const QJsonArray accounts = json.value("accounts").toArray();
    if (accounts.isEmpty()) {
        *error = "No OneDrive account is signed in";
        return {};
    }
    const QString active = json.value("active").toString();
    QHash<QString, QString> roots;
    QSet<QString> work;
    QHash<QString, QPair<QString, QString>> appsById;
    QJsonObject chosen;
    for (const QJsonValue &v : accounts) {
        const QJsonObject a = v.toObject();
        const QString id = a.value("id").toString();
        roots.insert(id, a.value("mirror").toString());
        if (a.value("business").toBool())
            work.insert(id);
        const QString clientId = a.value("clientId").toString();
        if (!clientId.isEmpty())
            appsById.insert(id, {clientId, a.value("tenant").toString()});
        const bool match = wanted.isEmpty() ? id == active
                                            : id == wanted || a.value("email").toString().compare(wanted, Qt::CaseInsensitive) == 0;
        if (match && chosen.isEmpty())
            chosen = a;
    }
    Graph::setApps(appsById);
    Mirror::setRoots(roots, chosen.value("id").toString(active), work);
    if (chosen.isEmpty())
        *error = wanted.isEmpty() ? "No active OneDrive account" : QString("No account matches '%1'").arg(wanted);
    return chosen;
}

// A signed-in Graph for the chosen account, or null with an error set.
std::unique_ptr<Graph> graphFor(const QString &wanted, QJsonObject *account, QString *error) {
    *account = resolveAccounts(wanted, error);
    if (account->isEmpty())
        return nullptr;
    auto graph = std::make_unique<Graph>(account->value("id").toString());
    if (!graph->restore(error)) {
        if (error->isEmpty())
            *error = "That account is not signed in here";
        return nullptr;
    }
    return graph;
}

QJsonObject itemJson(const QJsonObject &item) {
    const bool folder = item.contains("folder");
    QString stamp = item.value("fileSystemInfo").toObject().value("lastModifiedDateTime").toString();
    if (stamp.isEmpty())
        stamp = item.value("lastModifiedDateTime").toString();
    return QJsonObject{
        {"name", item.value("name").toString()},
        {"type", folder ? "folder" : "file"},
        {"size", item.value("size").toVariant().toLongLong()},
        {"changed", stamp},
        {"childCount", folder ? item.value("folder").toObject().value("childCount").toInt() : 0},
        {"webUrl", item.value("webUrl").toString()},
    };
}

// A OneDrive path argument: "" and "/" both mean the drive root.
QString normalisePath(const QString &path) {
    QString p = path.trimmed();
    if (!p.startsWith('/'))
        p = "/" + p;
    while (p.endsWith('/') && p.size() > 1)
        p.chop(1);
    return p == "/" ? QString() : p;
}

// A work (SES) account may write only in the user's own OneDrive, never in a
// SharePoint shared library mounted into it: those appear as items whose drive
// is a SharePoint site, not the user's drive. Returns an error message when the
// target is on another drive, or empty when the write is allowed. Personal
// drives have no shared libraries, so they always pass.
QString sharePointGuard(Graph &graph, const QJsonObject &account, const QString &targetPath) {
    if (!account.value("business").toBool())
        return {};
    const GraphReply mine = graph.call("GET", "https://graph.microsoft.com/v1.0/me/drive?$select=id");
    if (!mine.ok())
        return mine.error;
    const QString ownDrive = mine.json().value("id").toString();
    const QString path = normalisePath(targetPath);
    if (path.isEmpty())
        return {};  // the drive root is always the user's own
    const GraphReply item = graph.call("GET", Graph::itemUrl(path) + "?$select=id,remoteItem,parentReference");
    if (!item.ok())
        return item.error;
    const QJsonObject obj = item.json();
    // A mounted shared folder carries a remoteItem pointing at the site drive;
    // anything already inside one has that site as its parent drive.
    const QJsonObject ref = obj.contains("remoteItem")
        ? obj.value("remoteItem").toObject().value("parentReference").toObject()
        : obj.value("parentReference").toObject();
    const QString driveId = ref.value("driveId").toString();
    if (!ownDrive.isEmpty() && !driveId.isEmpty() && driveId != ownDrive)
        return "That folder is a SharePoint shared library; omadrive only writes to your personal OneDrive, "
               "not the common site";
    return {};
}

int cmdAccounts() {
    const QJsonObject json = accountsFile();
    const QString active = json.value("active").toString();
    QJsonArray list;
    for (const QJsonValue &v : json.value("accounts").toArray()) {
        const QJsonObject a = v.toObject();
        const bool business = a.value("business").toBool();
        list.append(QJsonObject{
            {"id", a.value("id").toString()},
            {"name", a.value("name").toString()},
            {"email", a.value("email").toString()},
            {"business", business},
            {"driveName", QString("OneDrive - %1").arg(business ? a.value("org").toString() : QString("Personal"))},
            {"mirror", a.value("mirror").toString()},
            {"active", a.value("id").toString() == active},
        });
    }
    return okWith({{"accounts", list}});
}

int cmdList(const QString &path, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const QList<QJsonObject> kids = graph->children(normalisePath(path), &error);
    if (!error.isEmpty())
        return fail(error);
    QJsonArray items;
    for (const QJsonObject &k : kids)
        items.append(itemJson(k));
    return okWith({{"account", account.value("id")}, {"path", normalisePath(path).isEmpty() ? "/" : normalisePath(path)},
                   {"items", items}});
}

int cmdStat(const QString &path, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const GraphReply reply = graph->call("GET", Graph::itemUrl(normalisePath(path)));
    if (!reply.ok())
        return fail(reply.error);
    QJsonObject item = itemJson(reply.json());
    item.insert("path", normalisePath(path).isEmpty() ? "/" : normalisePath(path));
    return okWith(item);
}

int cmdSearch(const QString &query, const QString &wanted) {
    if (query.trimmed().isEmpty())
        return fail("Search needs a query");
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const QString url = "https://graph.microsoft.com/v1.0/me/drive/root/search(q='"
        + QString(QUrl::toPercentEncoding(query.trimmed())) + "')?$top=50&$select=name,size,folder,file,"
                                                              "lastModifiedDateTime,fileSystemInfo,webUrl,parentReference";
    const GraphReply reply = graph->call("GET", url);
    if (!reply.ok())
        return fail(reply.error);
    QJsonArray items;
    for (const QJsonValue &v : reply.json().value("value").toArray()) {
        const QJsonObject hit = v.toObject();
        QJsonObject item = itemJson(hit);
        const QString parent = hit.value("parentReference").toObject().value("path").toString();
        const int colon = parent.indexOf("root:");
        if (colon >= 0) {
            const QString folder = QUrl::fromPercentEncoding(parent.mid(colon + 5).toUtf8());
            item.insert("path", Graph::joinPath(folder.isEmpty() ? "/" : folder, hit.value("name").toString()));
        }
        items.append(item);
    }
    return okWith({{"account", account.value("id")}, {"items", items}});
}

// Downloads one file to destDir (a temp dir when empty); returns its local path.
int cmdDownload(const QString &path, const QString &destArg, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const QString remote = normalisePath(path);
    if (remote.isEmpty())
        return fail("Cannot download the drive root; give a file path");

    QString dest = destArg.trimmed();
    if (dest.isEmpty())
        dest = QDir::tempPath() + "/omadrive-agent";
    // Downloading into another account's mirror would move work files across.
    const QString leak = Mirror::workLeak(account.value("id").toString(), {}, {}, dest);
    if (!leak.isEmpty())
        return fail("Files from a work OneDrive stay in that account; nothing was changed");
    if (!QDir().mkpath(dest))
        return fail(QString("Cannot create %1").arg(dest));

    const GraphReply meta = graph->call("GET", Graph::itemUrl(remote));
    if (!meta.ok())
        return fail(meta.error);
    const QJsonObject item = meta.json();
    if (item.contains("folder"))
        return fail("That is a folder; download individual files");
    const QString url = item.value("@microsoft.graph.downloadUrl").toString();
    if (url.isEmpty())
        return fail("OneDrive gave no download link");
    const QString target = dest + "/" + item.value("name").toString();
    if (!graph->download(QUrl(url), target, [](qint64) {}, &error))
        return fail(error);
    return okWith({{"account", account.value("id")}, {"local", target},
                   {"size", item.value("size").toVariant().toLongLong()}});
}

// Prints a text file's contents inline (for agents to read small files).
int cmdCat(const QString &path, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const QString remote = normalisePath(path);
    if (remote.isEmpty())
        return fail("Give a file path");
    const GraphReply meta = graph->call("GET", Graph::itemUrl(remote));
    if (!meta.ok())
        return fail(meta.error);
    const QJsonObject item = meta.json();
    if (item.contains("folder"))
        return fail("That is a folder");
    if (item.value("size").toVariant().toLongLong() > 1024 * 1024)
        return fail("File is larger than 1 MB; download it instead");
    const QString url = item.value("@microsoft.graph.downloadUrl").toString();
    if (url.isEmpty())
        return fail("OneDrive gave no download link");
    // The download URL is pre-authenticated; fetch it like cmdDownload does,
    // since the Graph /content endpoint redirects to a host that rejects the
    // bearer token.
    QTemporaryDir dir;
    const QString file = dir.path() + "/content";
    if (!graph->download(QUrl(url), file, [](qint64) {}, &error))
        return fail(error);
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return fail("Could not read the downloaded file");
    const QByteArray body = f.readAll();
    if (body.contains('\0'))
        return fail("File is not text; download it instead");
    return okWith({{"account", account.value("id")}, {"path", remote}, {"text", QString::fromUtf8(body)}});
}

int cmdUpload(const QString &localFile, const QString &remoteParent, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const QFileInfo info(localFile);
    if (!info.exists() || !info.isFile())
        return fail(QString("No such file: %1").arg(localFile));
    // Uploading a file that lives in another work account's folder would move
    // work files across accounts.
    const QString leak = Mirror::workLeak({}, {info.absoluteFilePath()}, account.value("id").toString(), {});
    if (!leak.isEmpty())
        return fail("Files from a work OneDrive stay in that account; nothing was changed");
    const QString blocked = sharePointGuard(*graph, account, remoteParent);
    if (!blocked.isEmpty())
        return fail(blocked);
    if (!graph->uploadFile(info.absoluteFilePath(), normalisePath(remoteParent), [](qint64) {}, &error))
        return fail(error);
    return okWith({{"account", account.value("id")},
                   {"path", Graph::joinPath(normalisePath(remoteParent).isEmpty() ? "/" : normalisePath(remoteParent),
                                            info.fileName())}});
}

int cmdMkdir(const QString &remoteParent, const QString &name, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    if (name.isEmpty() || name.contains('/'))
        return fail("Invalid folder name");
    const QString blocked = sharePointGuard(*graph, account, remoteParent);
    if (!blocked.isEmpty())
        return fail(blocked);
    const QString made = graph->createFolder(normalisePath(remoteParent), name, true, &error);
    if (!error.isEmpty())
        return fail(error);
    return okWith({{"account", account.value("id")},
                   {"path", Graph::joinPath(normalisePath(remoteParent).isEmpty() ? "/" : normalisePath(remoteParent), made)}});
}

int cmdRename(const QString &path, const QString &newName, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    if (newName.isEmpty() || newName.contains('/'))
        return fail("Invalid name");
    const QString blocked = sharePointGuard(*graph, account, path);
    if (!blocked.isEmpty())
        return fail(blocked);
    const GraphReply reply = graph->call(
        "PATCH", Graph::itemUrl(normalisePath(path)),
        QJsonDocument(QJsonObject{{"name", newName}}).toJson(QJsonDocument::Compact));
    if (!reply.ok())
        return fail(reply.status == 409 ? QString("%1 already exists").arg(newName) : reply.error);
    return okWith({{"account", account.value("id")}, {"name", newName}});
}

int cmdTrash(const QString &path, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    const QString remote = normalisePath(path);
    if (remote.isEmpty())
        return fail("Cannot delete the drive root");
    const QString blocked = sharePointGuard(*graph, account, path);
    if (!blocked.isEmpty())
        return fail(blocked);
    const GraphReply reply = graph->call("DELETE", Graph::itemUrl(remote));
    if (!reply.ok())
        return fail(reply.error);
    return okWith({{"account", account.value("id")}, {"trashed", remote}});
}

int cmdShare(const QString &path, bool edit, const QString &scope, const QString &wanted) {
    QJsonObject account;
    QString error;
    auto graph = graphFor(wanted, &account, &error);
    if (!graph)
        return fail(error);
    QJsonObject body{{"type", edit ? "edit" : "view"}};
    if (!scope.isEmpty())
        body.insert("scope", scope);
    const GraphReply reply = graph->call("POST", Graph::itemUrl(normalisePath(path)) + "/createLink",
                                         QJsonDocument(body).toJson(QJsonDocument::Compact));
    const QString link = reply.json().value("link").toObject().value("webUrl").toString();
    if (!reply.ok() || link.isEmpty())
        return fail(reply.ok() ? QString("OneDrive gave no link") : reply.error);
    return okWith({{"account", account.value("id")}, {"link", link}, {"canEdit", edit}});
}

// The sync state the mount reports for a file (see omadrive mount).
QJsonObject mountInfo(const QString &localPath) {
    const QByteArray path = QFile::encodeName(localPath);
    QByteArray value(64 * 1024, '\0');
    const ssize_t n = ::getxattr(path.constData(), "user.omadrive.info", value.data(), value.size());
    return n > 0 ? QJsonDocument::fromJson(value.left(n)).object() : QJsonObject();
}

// path: a file in a mounted folder, or a OneDrive path of the account.
int cmdStatus(const QString &path, const QString &wanted) {
    if (path.isEmpty())
        return fail("Usage: omadrive agent status <path> [--account <id|email>]");
    if (QFileInfo(path).isAbsolute()) {
        const QJsonObject info = mountInfo(path);
        if (!info.isEmpty())
            return okWith(info);
    }
    QString error;
    const QJsonObject account = resolveAccounts(wanted, &error);
    if (account.isEmpty())
        return fail(error);
    const QString id = account.value("id").toString();
    QString remote = normalisePath(path);
    for (const QString &root : {mountedAt(id), account.value("mirror").toString()}) {
        const QString clean = QDir::cleanPath(root);
        if (!root.isEmpty() && (path == clean || path.startsWith(clean + "/"))) {
            remote = normalisePath(path.mid(clean.size()));
            break;
        }
    }
    QJsonObject info;
    const QString mounted = mountedAt(id);
    if (!mounted.isEmpty()) {
        info = mountInfo(mounted + remote);
    } else {
        // Not mounted: what the last mount saved.
        const QString cache = SyncCore::cacheDirFor(id);
        if (!QFile::exists(cache + "/state.bin"))
            return fail("That account's folder has never been mounted");
        SyncCore core(nullptr, cache, {});
        core.load();
        info = core.info(remote.isEmpty() ? "/" : remote);
        if (!info.isEmpty())
            info.insert("pendingUploads", core.status().value("pending"));
    }
    if (info.isEmpty())
        return fail(QString("No such item: %1").arg(path));
    info.insert("account", id);
    info.insert("mounted", !mounted.isEmpty());
    return okWith(info);
}

// What each mount (or the one asked for) still has to upload, and its conflicts.
int cmdPending(const QString &wanted) {
    QString error;
    QStringList ids = accountIds();
    if (!wanted.isEmpty()) {
        const QJsonObject account = resolveAccounts(wanted, &error);
        if (account.isEmpty())
            return fail(error);
        ids = {account.value("id").toString()};
    }
    QJsonArray accounts;
    for (const QString &id : std::as_const(ids)) {
        QFile file(SyncCore::cacheDirFor(id) + "/status.json");
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        json.insert("account", id);
        json.insert("mounted", !mountedAt(id).isEmpty());
        json.remove("pid");
        accounts.append(json);
    }
    return okWith({{"accounts", accounts}});
}

// Asks a mount to look for OneDrive changes now.
int cmdRefresh(const QString &wanted) {
    QString error;
    const QJsonObject account = resolveAccounts(wanted, &error);
    if (account.isEmpty())
        return fail(error);
    const QString mounted = mountedAt(account.value("id").toString());
    if (mounted.isEmpty())
        return fail("That account's folder is not mounted");
    if (::setxattr(QFile::encodeName(mounted).constData(), "user.omadrive.refresh", "1", 1, 0) != 0)
        return fail(QString("Cannot reach the mount: %1").arg(QString::fromLocal8Bit(strerror(errno))));
    return okWith({{"account", account.value("id")}, {"mountpoint", mounted}});
}

// The answer to a claim that did not win: taken (another device has it, or it
// changed on OneDrive since), pending (changes here not on OneDrive yet), or
// failed.
int claimRefused(const QString &reason, const QString &path, const QString &detail = {}) {
    QString error = detail;
    if (error.isEmpty())
        error = reason == "taken"     ? QString("%1 was claimed elsewhere or changed on OneDrive").arg(path)
              : reason == "pending" ? QString("%1 has changes here that are not on OneDrive yet").arg(path)
                                    : QString("Could not claim %1").arg(path);
    return emitJson({{"ok", false}, {"reason", reason}, {"error", error}, {"path", path}});
}

// Moves an item on OneDrive, only if it is still the version just read, into
// <parent>/.claimed/<host>/ (or where --as says). Of several machines claiming
// the same file, exactly one wins. In a mounted folder the mount does it, so
// its tree follows at once and a file with changes not yet uploaded is refused.
int cmdClaim(const QString &path, const QString &as, const QString &wanted) {
    if (path.isEmpty())
        return fail("Usage: omadrive agent claim <path> [--as <name|path>] [--account <id|email>]");
    QString error;
    QJsonObject account = resolveAccounts(wanted, &error);
    if (account.isEmpty())
        return fail(error);
    QString id = account.value("id").toString();

    // A path in a mounted folder belongs to that folder's account.
    const QString clean = QDir::cleanPath(path);
    QString mounted, remote;
    for (const QString &other : accountIds()) {
        const QString root = mountedAt(other);
        if (root.isEmpty() || !(clean == root || clean.startsWith(root + "/")))
            continue;
        if (other != id) {
            if (!wanted.isEmpty())
                return fail("That path is in another account's folder");
            account = resolveAccounts(other, &error);
            id = other;
        }
        mounted = root;
        remote = normalisePath(clean.mid(root.size()));
        break;
    }
    if (mounted.isEmpty()) {
        const QString mirror = QDir::cleanPath(account.value("mirror").toString());
        const bool inMirror = !account.value("mirror").toString().isEmpty() && clean.startsWith(mirror + "/");
        // A file on this device outside the account's folder is not on OneDrive.
        if (!inMirror && QFileInfo(path).isAbsolute() && QFileInfo::exists(path))
            return fail(QString("%1 is not in a OneDrive folder").arg(path));
        remote = inMirror ? normalisePath(clean.mid(mirror.size())) : normalisePath(path);
        mounted = mountedAt(id);
    }
    if (remote.isEmpty())
        return fail("Cannot claim the drive root");

    const QString name = remote.mid(remote.lastIndexOf('/') + 1);
    QString dest;
    if (!as.contains('/')) {
        const QString claimed = Graph::joinPath(Graph::joinPath(Graph::parentPath(remote), ".claimed"),
                                                QSysInfo::machineHostName());
        dest = Graph::joinPath(claimed, as.isEmpty() ? name : as);
    } else {
        const QString target = QDir::cleanPath(as);
        dest = !mounted.isEmpty() && target.startsWith(mounted + "/") ? normalisePath(target.mid(mounted.size()))
                                                                      : normalisePath(target);
    }
    if (dest.isEmpty() || dest == remote)
        return fail("Invalid destination");

    if (!mounted.isEmpty()) {
        const QByteArray value = dest.toUtf8();
        if (::setxattr(QFile::encodeName(mounted + remote).constData(), "user.omadrive.claim", value.constData(),
                       size_t(value.size()), 0)
            == 0)
            return okWith({{"account", id}, {"path", mounted + dest}, {"remotePath", dest}, {"mounted", true}});
        const int e = errno;
        if (e == ENOENT || e == ESTALE)
            return claimRefused("taken", mounted + remote);
        if (e == EBUSY)
            return claimRefused("pending", mounted + remote);
        return claimRefused(e == EINVAL ? "invalid" : "failed", mounted + remote,
                            QString("Could not claim %1: %2; see %3/mount.log")
                                .arg(mounted + remote, QString::fromLocal8Bit(strerror(e)), SyncCore::cacheDirFor(id)));
    }

    auto graph = graphFor(id, &account, &error);
    if (!graph)
        return fail(error);
    const QString blocked = sharePointGuard(*graph, account, remote);
    if (!blocked.isEmpty())
        return fail(blocked);
    const GraphReply item = graph->call("GET", Graph::itemUrl(remote) + "?$select=id,eTag,name");
    if (item.status == 404)
        return claimRefused("taken", remote);
    if (!item.ok())
        return claimRefused("failed", remote, item.error);
    // The folders on the way, made when missing.
    QString folder;
    for (const QString &part : Graph::parentPath(dest).split('/', Qt::SkipEmptyParts)) {
        const QString next = Graph::joinPath(folder, part);
        const GraphReply there = graph->call("GET", Graph::itemUrl(next) + "?$select=id,folder");
        if (there.status == 404) {
            graph->createFolder(folder, part, true, &error);  // made meanwhile is fine too
        } else if (!there.ok()) {
            return claimRefused("failed", remote, there.error);
        } else if (!there.json().contains("folder")) {
            return claimRefused("invalid", remote, QString("%1 is not a folder").arg(next));
        }
        folder = next;
    }
    const GraphReply parent = graph->call("GET", Graph::itemUrl(Graph::parentPath(dest)) + "?$select=id");
    if (!parent.ok())
        return claimRefused("failed", remote, parent.error);
    const QString blockedDest = sharePointGuard(*graph, account, Graph::parentPath(dest));
    if (!blockedDest.isEmpty())
        return fail(blockedDest);
    const QJsonObject body{{"name", dest.mid(dest.lastIndexOf('/') + 1)},
                           {"parentReference", QJsonObject{{"id", parent.json().value("id").toString()}}}};
    const GraphReply moved = graph->call(
        "PATCH",
        "https://graph.microsoft.com/v1.0/me/drive/items/" + item.json().value("id").toString()
            + "?@microsoft.graph.conflictBehavior=fail",
        QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json",
        {{"If-Match", item.json().value("eTag").toString().toUtf8()}});
    if (moved.status == 404 || moved.status == 409 || moved.status == 412)
        return claimRefused("taken", remote);
    if (!moved.ok())
        return claimRefused("failed", remote, moved.error);
    return okWith({{"account", id}, {"path", dest}, {"remotePath", dest}, {"mounted", false}});
}

// --account/-a <id|email>, --dest <dir>, --scope <s>, --edit; the rest are positionals.
struct Args {
    QStringList positional;
    QString account, dest, scope, as;
    bool edit = false;
};

Args parse(const QStringList &args) {
    Args a;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args[i];
        if ((arg == "--account" || arg == "-a") && i + 1 < args.size())
            a.account = args[++i];
        else if (arg == "--dest" && i + 1 < args.size())
            a.dest = args[++i];
        else if (arg == "--as" && i + 1 < args.size())
            a.as = args[++i];
        else if (arg == "--scope" && i + 1 < args.size())
            a.scope = args[++i];
        else if (arg == "--edit")
            a.edit = true;
        else
            a.positional << arg;
    }
    return a;
}

QString pos(const Args &a, int i) { return i < a.positional.size() ? a.positional[i] : QString(); }

}  // namespace

QJsonObject resolveAccount(const QString &wanted, QString *error) {
    return resolveAccounts(wanted, error);
}

QStringList accountIds() {
    QStringList ids;
    for (const QJsonValue &v : accountsFile().value("accounts").toArray())
        ids << v.toObject().value("id").toString();
    return ids;
}

int runAgent(const QStringList &rawArgs) {
    if (rawArgs.isEmpty())
        return fail("Usage: omadrive agent <accounts|list|stat|search|cat|download|upload|mkdir|rename|trash|share|status|pending|refresh|claim> ...");
    const QString command = rawArgs.first();
    const Args a = parse(rawArgs.mid(1));

    if (command == "accounts")
        return cmdAccounts();
    if (command == "recent")
        return printRecent(pos(a, 0).toInt());
    if (command == "list")
        return cmdList(pos(a, 0), a.account);
    if (command == "stat")
        return cmdStat(pos(a, 0), a.account);
    if (command == "search")
        return cmdSearch(pos(a, 0), a.account);
    if (command == "cat")
        return cmdCat(pos(a, 0), a.account);
    if (command == "download")
        return cmdDownload(pos(a, 0), a.dest, a.account);
    if (command == "upload")
        return cmdUpload(pos(a, 0), pos(a, 1), a.account);
    if (command == "mkdir")
        return cmdMkdir(pos(a, 0), pos(a, 1), a.account);
    if (command == "rename")
        return cmdRename(pos(a, 0), pos(a, 1), a.account);
    if (command == "trash")
        return cmdTrash(pos(a, 0), a.account);
    if (command == "share")
        return cmdShare(pos(a, 0), a.edit, a.scope, a.account);
    if (command == "status")
        return cmdStatus(pos(a, 0), a.account);
    if (command == "pending")
        return cmdPending(a.account);
    if (command == "refresh")
        return cmdRefresh(a.account);
    if (command == "claim")
        return cmdClaim(pos(a, 0), a.as, a.account);
    return fail(QString("Unknown command: %1").arg(command));
}
