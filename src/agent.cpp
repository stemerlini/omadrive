#include "agent.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "cloud.h"
#include "rclone.h"

// Every command prints a single JSON object. Accounts are the rclone OneDrive
// remotes; paths are from the drive root ("/Documents/a.pdf") or, for a
// mounted account, inside its mount folder.

namespace {

using Cloud::Account;

int emitJson(const QJsonObject &obj) {
    QTextStream out(stdout);
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

const QString kWorkLeak = QStringLiteral("Files from a work OneDrive stay in that account; nothing was changed");

bool inside(const QString &path, const QString &dir) {
    return !dir.isEmpty() && (path == dir || path.startsWith(dir + '/'));
}

// The account for a command: the one whose mount folder holds an absolute
// path, else --account, else the default. A path in one account's folder
// with --account naming another is refused.
Account accountFor(const QString &path, const QString &wanted, QString *error) {
    const Account owner = QDir::isAbsolutePath(path) ? Cloud::ownerOf(path) : Account{};
    if (!owner.remote.isEmpty()) {
        if (!wanted.isEmpty() && Cloud::find(wanted).remote != owner.remote) {
            *error = "That path is in another account's folder";
            return {};
        }
        return owner;
    }
    const Account a = Cloud::find(wanted);
    if (a.remote.isEmpty())
        *error = wanted.isEmpty() ? QStringLiteral("No OneDrive account is connected; run omadrive cloud connect")
                                  : QString("No account matches '%1'").arg(wanted);
    return a;
}

QString parentOf(const QString &drivePath) {
    const int slash = drivePath.lastIndexOf('/');
    return slash <= 0 ? QStringLiteral("/") : drivePath.left(slash);
}

QString join(const QString &dir, const QString &name) {
    return dir == "/" || dir.isEmpty() ? "/" + name : dir + "/" + name;
}

bool validName(const QString &name) {
    return !name.isEmpty() && !name.contains('/') && name != "." && name != "..";
}

// rclone lsjson entry -> our item shape.
QJsonObject itemJson(const QJsonObject &e, const QString &drivePath) {
    const bool dir = e.value("IsDir").toBool();
    QString name = e.value("Name").toString();
    if (name.isEmpty())
        name = drivePath.mid(drivePath.lastIndexOf('/') + 1);
    return {{"name", name},
            {"type", dir ? "folder" : "file"},
            {"size", dir ? 0 : e.value("Size").toInteger()},
            {"changed", e.value("ModTime").toString()},
            {"path", drivePath}};
}

// Metadata of one item; empty object when it does not exist.
QJsonObject statItem(const Account &a, const QString &drivePath, QString *error) {
    if (drivePath == "/")
        return {{"IsDir", true}, {"Name", ""}};
    const Cloud::Run run = Cloud::rclone({"lsjson", "--stat", "--no-mimetype", Cloud::remotePath(a, drivePath)}, 60000);
    if (run.code == 0)
        return QJsonDocument::fromJson(run.out).object();
    if (run.code != 3 && run.code != 4 && !run.error().contains("not found", Qt::CaseInsensitive))
        *error = run.error();
    return {};
}

// After changing the drive behind a running mount, let it see the folder now
// rather than at the next poll.
void refreshMount(const Account &a, const QString &drivePath) {
    if (!a.mounted)
        return;
    const QString dir = parentOf(drivePath).mid(1);
    Cloud::rc(a.remote, "vfs/forget", {{"fs", a.fs()}, {"dir", dir}});
    Cloud::rc(a.remote, "vfs/refresh", {{"fs", a.fs()}, {"dir", dir}});
}

// A work account's files must not land in another account's mount folder,
// and files in a work account's folder must not be sent to another account.
bool leaksInto(const Account &from, const QString &localDest) {
    if (!from.work())
        return false;
    const Account owner = Cloud::ownerOf(localDest);
    return !owner.remote.isEmpty() && owner.remote != from.remote;
}

bool leaksFrom(const QString &localSource, const Account &to) {
    const Account owner = Cloud::ownerOf(localSource);
    return owner.work() && owner.remote != to.remote;
}

int cmdAccounts() {
    const Account fallback = Cloud::find({});
    QJsonArray list;
    for (const Account &a : Cloud::accounts())
        list.append(QJsonObject{{"id", a.remote},
                                {"name", a.label()},
                                {"business", a.work()},
                                {"driveName", "OneDrive - " + a.label()},
                                {"mountpoint", a.mountpoint},
                                {"mounted", a.mounted},
                                {"active", a.remote == fallback.remote}});
    return okWith({{"accounts", list}});
}

int cmdList(const QString &path, const QString &wanted) {
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString dir = Cloud::drivePath(a, path);
    const Cloud::Run run = Cloud::rclone({"lsjson", "--no-mimetype", Cloud::remotePath(a, dir)}, 60000);
    if (run.code != 0)
        return fail(run.error());
    QJsonArray items;
    for (const QJsonValue &v : QJsonDocument::fromJson(run.out).array())
        items.append(itemJson(v.toObject(), join(dir, v.toObject().value("Name").toString())));
    return okWith({{"account", a.remote}, {"path", dir}, {"items", items}});
}

int cmdStat(const QString &path, const QString &wanted) {
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString p = Cloud::drivePath(a, path);
    const QJsonObject e = statItem(a, p, &error);
    if (e.isEmpty())
        return fail(error.isEmpty() ? QString("No such item: %1").arg(p) : error);
    return okWith(itemJson(e, p));
}

// OneDrive's own search, with the token rclone keeps for the account; a
// recursive listing of a large drive takes a minute.
QJsonArray graphSearch(const Account &a, const QString &query, QString *error) {
    auto token = [&a]() {
        const Cloud::Run dump = Cloud::rclone({"config", "dump"}, 15000);
        const QJsonObject cfg = QJsonDocument::fromJson(dump.out).object().value(a.remote).toObject();
        return std::make_pair(QJsonDocument::fromJson(cfg.value("token").toString().toUtf8()).object(),
                              cfg.value("drive_id").toString());
    };
    auto [tok, driveId] = token();
    const QDateTime expiry = QDateTime::fromString(tok.value("expiry").toString(), Qt::ISODateWithMs);
    if (!expiry.isValid() || expiry < QDateTime::currentDateTime().addSecs(120)) {
        Cloud::rclone({"about", a.fs()}, 30000);  // makes rclone refresh and save the token
        std::tie(tok, driveId) = token();
    }
    const QString bearer = tok.value("access_token").toString();
    if (bearer.isEmpty() || driveId.isEmpty()) {
        *error = "no token";
        return {};
    }
    QString q = query;
    q.replace('\'', "''");
    QUrl url(QString("https://graph.microsoft.com/v1.0/drives/%1/root/search(q='%2')").arg(driveId, q));
    url.setQuery("$top=200&$select=name,size,folder,file,lastModifiedDateTime,parentReference");

    QNetworkAccessManager net;
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    QNetworkReply *reply = net.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError) {
        *error = reply->isFinished() ? reply->errorString() : QStringLiteral("timed out");
        return {};
    }
    QJsonArray items;
    for (const QJsonValue &v : QJsonDocument::fromJson(reply->readAll()).object().value("value").toArray()) {
        const QJsonObject hit = v.toObject();
        const QString parent = hit.value("parentReference").toObject().value("path").toString();
        const int colon = parent.indexOf("root:");
        if (colon < 0)
            continue;  // outside this drive (shared with me)
        const QString folder = QUrl::fromPercentEncoding(parent.mid(colon + 5).toUtf8());
        const bool dir = hit.contains("folder");
        items.append(QJsonObject{{"name", hit.value("name").toString()},
                                 {"type", dir ? "folder" : "file"},
                                 {"size", dir ? 0 : hit.value("size").toInteger()},
                                 {"changed", hit.value("lastModifiedDateTime").toString()},
                                 {"path", join(folder.isEmpty() ? "/" : folder, hit.value("name").toString())}});
    }
    return items;
}

int cmdSearch(const QString &query, const QString &wanted) {
    if (query.trimmed().isEmpty())
        return fail("Search needs a query");
    QString error;
    const Account a = accountFor({}, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    QJsonArray items = graphSearch(a, query.trimmed(), &error);
    if (!error.isEmpty()) {
        // Slow but always works: walk the drive, matching names.
        QString glob = query.trimmed();
        glob.replace(QRegularExpression(R"([\\*?\[\]{}])"), R"(\\0)");
        const Cloud::Run run = Cloud::rclone(
            {"lsjson", "-R", "--no-mimetype", "--ignore-case", "--include", "*" + glob + "*", a.fs()}, 300000);
        if (run.code != 0)
            return fail(run.error());
        items = {};
        for (const QJsonValue &v : QJsonDocument::fromJson(run.out).array()) {
            const QJsonObject e = v.toObject();
            if (e.value("Name").toString().contains(query.trimmed(), Qt::CaseInsensitive) && items.size() < 200)
                items.append(itemJson(e, "/" + e.value("Path").toString()));
        }
    }
    return okWith({{"account", a.remote}, {"items", items}});
}

// Copies one file to destDir (a temp dir when empty); returns its local path.
int cmdDownload(const QString &path, const QString &destArg, const QString &wanted) {
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString p = Cloud::drivePath(a, path);
    if (p == "/")
        return fail("Cannot download the drive root; give a file path");
    QString dest = destArg.trimmed().isEmpty() ? QDir::tempPath() + "/omadrive-agent" : destArg.trimmed();
    dest = QDir::cleanPath(QDir(dest).absolutePath());
    if (leaksInto(a, dest))
        return fail(kWorkLeak);
    const QJsonObject e = statItem(a, p, &error);
    if (e.isEmpty())
        return fail(error.isEmpty() ? QString("No such item: %1").arg(p) : error);
    if (e.value("IsDir").toBool())
        return fail("That is a folder; download individual files");
    if (!QDir().mkpath(dest))
        return fail(QString("Cannot create %1").arg(dest));
    const QString target = dest + "/" + e.value("Name").toString();
    // Through the mount when there is one: it has changes not yet uploaded.
    if (a.mounted) {
        QFile::remove(target);
        if (!QFile::copy(a.mountpoint + p, target))
            return fail(QString("Could not copy %1").arg(a.mountpoint + p));
    } else {
        const Cloud::Run run = Cloud::rclone({"copyto", Cloud::remotePath(a, p), target}, 3600000);
        if (run.code != 0)
            return fail(run.error());
    }
    return okWith({{"account", a.remote}, {"local", target}, {"size", QFileInfo(target).size()}});
}

int cmdCat(const QString &path, const QString &wanted) {
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString p = Cloud::drivePath(a, path);
    const QJsonObject e = statItem(a, p, &error);
    if (e.isEmpty())
        return fail(error.isEmpty() ? QString("No such item: %1").arg(p) : error);
    if (e.value("IsDir").toBool())
        return fail("That is a folder");
    if (e.value("Size").toInteger() > 1024 * 1024)
        return fail("File is larger than 1 MB; download it instead");
    QByteArray body;
    if (a.mounted) {
        QFile f(a.mountpoint + p);
        if (!f.open(QIODevice::ReadOnly))
            return fail(QString("Could not read %1").arg(f.fileName()));
        body = f.read(1024 * 1024 + 1);
    } else {
        const Cloud::Run run = Cloud::rclone({"cat", Cloud::remotePath(a, p)}, 120000);
        if (run.code != 0)
            return fail(run.error());
        body = run.out;
    }
    if (body.contains('\0'))
        return fail("File is not text; download it instead");
    return okWith({{"account", a.remote}, {"path", p}, {"text", QString::fromUtf8(body)}});
}

int cmdUpload(const QString &localFile, const QString &remoteParent, const QString &wanted) {
    QString error;
    const Account a = accountFor(remoteParent, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QFileInfo info(localFile);
    if (!info.exists() || !info.isFile())
        return fail(QString("No such file: %1").arg(localFile));
    if (leaksFrom(info.absoluteFilePath(), a))
        return fail(kWorkLeak);
    const QString target = join(Cloud::drivePath(a, remoteParent), info.fileName());
    if (!statItem(a, target, &error).isEmpty())
        return fail(QString("%1 already exists").arg(target));
    if (!error.isEmpty())
        return fail(error);
    const Cloud::Run run = Cloud::rclone({"copyto", info.absoluteFilePath(), Cloud::remotePath(a, target)}, 3600000);
    if (run.code != 0)
        return fail(run.error());
    refreshMount(a, target);
    return okWith({{"account", a.remote}, {"path", target}});
}

int cmdMkdir(const QString &remoteParent, const QString &name, const QString &wanted) {
    if (!validName(name))
        return fail("Invalid folder name");
    QString error;
    const Account a = accountFor(remoteParent, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString target = join(Cloud::drivePath(a, remoteParent), name);
    const Cloud::Run run = Cloud::rclone({"mkdir", Cloud::remotePath(a, target)}, 60000);
    if (run.code != 0)
        return fail(run.error());
    refreshMount(a, target);
    return okWith({{"account", a.remote}, {"path", target}});
}

int cmdRename(const QString &path, const QString &newName, const QString &wanted) {
    if (!validName(newName))
        return fail("Invalid name");
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString p = Cloud::drivePath(a, path);
    if (p == "/")
        return fail("Cannot rename the drive root");
    const QJsonObject e = statItem(a, p, &error);
    if (e.isEmpty())
        return fail(error.isEmpty() ? QString("No such item: %1").arg(p) : error);
    const QString target = join(parentOf(p), newName);
    if (!statItem(a, target, &error).isEmpty())
        return fail(QString("%1 already exists").arg(newName));
    const Cloud::Run run = Cloud::rclone(
        {e.value("IsDir").toBool() ? "move" : "moveto", Cloud::remotePath(a, p), Cloud::remotePath(a, target)}, 600000);
    if (run.code != 0)
        return fail(run.error());
    refreshMount(a, p);
    return okWith({{"account", a.remote}, {"name", newName}, {"path", target}});
}

// OneDrive keeps deleted items in its recycle bin (rclone's hard_delete is off).
int cmdTrash(const QString &path, const QString &wanted) {
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    const QString p = Cloud::drivePath(a, path);
    if (p == "/")
        return fail("Cannot delete the drive root");
    const QJsonObject e = statItem(a, p, &error);
    if (e.isEmpty())
        return fail(error.isEmpty() ? QString("No such item: %1").arg(p) : error);
    const Cloud::Run run = Cloud::rclone(
        {e.value("IsDir").toBool() ? "purge" : "deletefile", "--onedrive-hard-delete=false", Cloud::remotePath(a, p)},
        600000);
    if (run.code != 0)
        return fail(run.error());
    refreshMount(a, p);
    return okWith({{"account", a.remote}, {"trashed", p}});
}

int cmdShare(const QString &path, bool edit, const QString &scope, const QString &wanted) {
    if (!scope.isEmpty() && scope != "anonymous" && scope != "organization")
        return fail("Scope is anonymous or organization");
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    if (QDir::isAbsolutePath(path) && QFileInfo::exists(path) && Cloud::ownerOf(path).remote.isEmpty())
        return fail(QString("%1 is not in a OneDrive folder").arg(path));
    const QString p = Cloud::drivePath(a, path);
    const Cloud::Run run = Cloud::rclone({"link", "--onedrive-link-type", edit ? "edit" : "view",
                                          "--onedrive-link-scope",
                                          scope.isEmpty() ? (a.work() ? "organization" : "anonymous") : scope,
                                          Cloud::remotePath(a, p)},
                                         60000);
    const QString link = QString::fromUtf8(run.out).trimmed();
    if (run.code != 0 || link.isEmpty())
        return fail(run.code == 0 ? QStringLiteral("OneDrive gave no link") : run.error());
    return okWith({{"account", a.remote}, {"link", link}, {"canEdit", edit}});
}

int cmdStatus(const QString &path, const QString &wanted) {
    if (path.isEmpty())
        return fail("Usage: omadrive agent status <path> [--account <name>]");
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    if (!a.mounted)
        return fail(QString("%1 is not mounted").arg(a.label()));
    const QString p = Cloud::drivePath(a, path);
    const QFileInfo info(a.mountpoint + p);
    if (!info.exists())
        return fail(QString("No such item: %1").arg(p));
    const QString state = info.isDir() ? QString()
                                       : Rclone::state(Rclone::mountFor(info.absoluteFilePath()), info.absoluteFilePath(), false);
    return okWith({{"account", a.remote},
                   {"path", p},
                   {"local", info.absoluteFilePath()},
                   {"type", info.isDir() ? "folder" : "file"},
                   {"state", state.isEmpty() ? (info.isDir() ? "folder" : "online") : state},
                   {"size", info.isDir() ? 0 : info.size()},
                   {"changed", info.lastModified().toUTC().toString(Qt::ISODate)},
                   {"mounted", true}});
}

int cmdPending(const QString &wanted) {
    QVector<Account> list;
    if (wanted.isEmpty()) {
        list = Cloud::accounts();
    } else {
        QString error;
        const Account a = accountFor({}, wanted, &error);
        if (a.remote.isEmpty())
            return fail(error);
        list = {a};
    }
    QJsonArray accounts;
    for (const Account &a : std::as_const(list)) {
        const QStringList pending = Cloud::pending(a);
        accounts.append(QJsonObject{{"account", a.remote},
                                    {"mounted", a.mounted},
                                    {"mountpoint", a.mountpoint},
                                    {"pending", pending.size()},
                                    {"files", QJsonArray::fromStringList(pending)}});
    }
    return okWith({{"accounts", accounts}});
}

int cmdRefresh(const QString &wanted) {
    QString error;
    const Account a = accountFor({}, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    if (!a.mounted)
        return fail(QString("%1 is not mounted").arg(a.label()));
    const QJsonObject r = Cloud::rc(a.remote, "vfs/refresh", {{"fs", a.fs()}}, 60000);
    if (r.contains("error"))
        return fail(r.value("error").toString());
    return okWith({{"account", a.remote}, {"mountpoint", a.mountpoint}});
}

int claimRefused(const QString &reason, const QString &path, const QString &detail = {}) {
    QString error = detail;
    if (error.isEmpty())
        error = reason == "taken"     ? QString("%1 was claimed elsewhere").arg(path)
              : reason == "pending" ? QString("%1 has changes here that are not on OneDrive yet").arg(path)
                                    : QString("Could not claim %1").arg(path);
    return emitJson({{"ok", false}, {"reason", reason}, {"error", error}, {"path", path}});
}

// Moves an item on OneDrive into <parent>/.claimed/<host>/ (or where --as
// says). OneDrive moves by id, so of several machines claiming the same file
// one move wins and the others find the source gone ("taken").
int cmdClaim(const QString &path, const QString &as, const QString &wanted) {
    if (path.isEmpty())
        return fail("Usage: omadrive agent claim <path> [--as <name|path>] [--account <name>]");
    QString error;
    const Account a = accountFor(path, wanted, &error);
    if (a.remote.isEmpty())
        return fail(error);
    if (QDir::isAbsolutePath(path) && QFileInfo::exists(path) && !inside(QDir::cleanPath(path), a.mountpoint)
        && Cloud::ownerOf(path).remote.isEmpty())
        return fail(QString("%1 is not in a OneDrive folder").arg(path));
    const QString p = Cloud::drivePath(a, path);
    if (p == "/")
        return fail("Cannot claim the drive root");

    const QString name = p.mid(p.lastIndexOf('/') + 1);
    QString dest;
    if (!as.contains('/'))
        dest = join(join(join(parentOf(p), ".claimed"), QSysInfo::machineHostName()), as.isEmpty() ? name : as);
    else
        dest = Cloud::drivePath(a, as);
    if (dest == "/" || dest == p)
        return fail("Invalid destination");

    if (a.mounted && Cloud::pending(a).contains(p))
        return claimRefused("pending", p);
    const QJsonObject e = statItem(a, p, &error);
    if (e.isEmpty())
        return error.isEmpty() ? claimRefused("taken", p) : claimRefused("failed", p, error);
    if (!statItem(a, dest, &error).isEmpty())
        return claimRefused("invalid", p, QString("%1 already exists").arg(dest));

    const bool dir = e.value("IsDir").toBool();
    const Cloud::Run run = Cloud::rclone({dir ? "move" : "moveto", Cloud::remotePath(a, p), Cloud::remotePath(a, dest)},
                                         600000);
    if (run.code != 0) {
        QString again;
        if (statItem(a, p, &again).isEmpty() && again.isEmpty())
            return claimRefused("taken", p);
        return claimRefused("failed", p, run.error());
    }
    refreshMount(a, p);
    refreshMount(a, dest);
    return okWith({{"account", a.remote},
                   {"path", a.mounted ? a.mountpoint + dest : dest},
                   {"remotePath", dest},
                   {"mounted", a.mounted}});
}

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

QString pos(const Args &a, int i) { return a.positional.value(i); }

}  // namespace

int runAgent(const QStringList &rawArgs) {
    if (rawArgs.isEmpty())
        return fail("Usage: omadrive agent <accounts|list|stat|search|cat|download|upload|mkdir|rename|trash|share|status|pending|refresh|claim> ...");
    const QString command = rawArgs.first();
    const Args a = parse(rawArgs.mid(1));

    if (command == "accounts")
        return cmdAccounts();
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
