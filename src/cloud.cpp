#include "cloud.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QtConcurrent>

#include <cstdio>
#include <unistd.h>

namespace Cloud {
namespace {

bool inside(const QString &path, const QString &dir) {
    return path == dir || path.startsWith(dir + '/');
}

QString unescapeMount(const QByteArray &field) {
    QByteArray out;
    for (int i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            bool ok = false;
            const int c = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out.append(char(c));
                i += 3;
                continue;
            }
        }
        out.append(field[i]);
    }
    return QString::fromUtf8(out);
}

// mountpoint -> "remote:" for every live rclone mount.
QHash<QString, QString> rcloneMounts() {
    QHash<QString, QString> result;
    QFile file("/proc/self/mounts");
    if (!file.open(QIODevice::ReadOnly))
        return result;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QList<QByteArray> f = line.split(' ');
        if (f.size() >= 3 && f[2] == "fuse.rclone")
            result.insert(QDir::cleanPath(unescapeMount(f[1])), unescapeMount(f[0]));
    }
    return result;
}

QHash<QString, QString> readEnv(const QString &path) {
    QHash<QString, QString> env;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return env;
    for (const QString &line : QString::fromUtf8(file.readAll()).split('\n')) {
        const int eq = line.indexOf('=');
        if (eq > 0 && !line.startsWith('#'))
            env.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return env;
}

void print(const QJsonObject &o) {
    std::fputs(QJsonDocument(o).toJson(QJsonDocument::Compact).constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

}  // namespace

QString Run::error() const {
    const QString text = QString::fromUtf8(err).trimmed();
    if (text.isEmpty())
        return code == -2 ? QStringLiteral("rclone is not installed") : QString("rclone exited with %1").arg(code);
    // rclone prefixes lines with a date and a level; keep the last message.
    QString last = text.split('\n').last();
    last.remove(QRegularExpression(R"(^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d [A-Z]+\s*:\s*)"));
    return last;
}

QString configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omadrive/cloud";
}

QString unitName(const QString &remote) {
    return QString("omadrive-rclone@%1.service").arg(remote);
}

QString socketPath(const QString &remote) {
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QString("/run/user/%1").arg(getuid());
    return runtime + "/omadrive-rclone-" + remote + ".sock";
}

bool validRemote(const QString &remote) {
    static const QRegularExpression re("^[A-Za-z0-9_.-]+$");
    return re.match(remote).hasMatch() && !remote.startsWith('-');
}

QString labelFor(const QString &remote) {
    QString name = remote;
    if (name.startsWith("onedrive-", Qt::CaseInsensitive))
        name = name.mid(9);
    if (name.isEmpty())
        return remote;
    if (name.size() <= 3)
        return name.toUpper();
    return name.left(1).toUpper() + name.mid(1);
}

QString Account::label() const {
    return labelFor(remote);
}

QString defaultMountpoint(const QString &remote) {
    return QDir::homePath() + "/OneDrive-" + labelFor(remote);
}

QVector<Account> accounts() {
    QVector<Account> result;
    const Run dump = rclone({"config", "dump"}, 15000);
    if (dump.code != 0)
        return result;
    const QJsonObject remotes = QJsonDocument::fromJson(dump.out).object();
    const QHash<QString, QString> mounts = rcloneMounts();
    for (auto it = remotes.begin(); it != remotes.end(); ++it) {
        const QJsonObject cfg = it.value().toObject();
        if (cfg.value("type").toString() != "onedrive" || !validRemote(it.key()))
            continue;
        Account a;
        a.remote = it.key();
        a.driveType = cfg.value("drive_type").toString("personal");
        const auto env = readEnv(configDir() + "/" + a.remote + ".env");
        a.mountpoint = env.value("MOUNTPOINT");
        if (!a.mountpoint.isEmpty())
            a.mountpoint = QDir::cleanPath(a.mountpoint);
        a.mounted = !a.mountpoint.isEmpty() && mounts.value(a.mountpoint).startsWith(a.remote + ':');
        result.append(a);
    }
    return result;
}

Account find(const QString &name) {
    const QVector<Account> all = accounts();
    if (name.isEmpty()) {
        for (const Account &a : all)
            if (!a.work())
                return a;
        return all.isEmpty() ? Account{} : all.first();
    }
    for (const Account &a : all)
        if (a.remote.compare(name, Qt::CaseInsensitive) == 0 || a.label().compare(name, Qt::CaseInsensitive) == 0
            || QString("onedrive-" + name).compare(a.remote, Qt::CaseInsensitive) == 0)
            return a;
    return {};
}

Account ownerOf(const QString &localPath) {
    const QString clean = QDir::cleanPath(localPath);
    for (const Account &a : accounts())
        if (!a.mountpoint.isEmpty() && inside(clean, a.mountpoint))
            return a;
    return {};
}

QString drivePath(const Account &account, const QString &path) {
    QString p = QDir::cleanPath(path.isEmpty() ? QStringLiteral("/") : path);
    if (!account.mountpoint.isEmpty() && inside(p, account.mountpoint))
        p = p.mid(account.mountpoint.size());
    if (!p.startsWith('/'))
        p.prepend('/');
    return QDir::cleanPath(p);
}

QString remotePath(const Account &account, const QString &path) {
    return account.fs() + drivePath(account, path).mid(1);
}

Run rclone(const QStringList &args, int timeoutMs) {
    Run run;
    QProcess p;
    p.start("rclone", args);
    if (!p.waitForStarted(5000)) {
        run.code = -2;
        return run;
    }
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        run.code = -3;
        run.err = "rclone timed out";
        return run;
    }
    run.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    run.out = p.readAllStandardOutput();
    run.err = p.readAllStandardError();
    return run;
}

QJsonObject rc(const QString &remote, const QString &method, const QJsonObject &params, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(socketPath(remote));
    if (!socket.waitForConnected(2000))
        return {{"error", "the mount is not running"}};
    const QByteArray body = QJsonDocument(params).toJson(QJsonDocument::Compact);
    socket.write("POST /" + method.toUtf8() + " HTTP/1.0\r\nHost: rclone\r\nContent-Type: application/json\r\n"
                 "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket.flush();
    QByteArray reply;
    QElapsedTimer timer;
    timer.start();
    while (socket.state() == QLocalSocket::ConnectedState && timer.elapsed() < timeoutMs) {
        if (socket.waitForReadyRead(200))
            reply += socket.readAll();
    }
    reply += socket.readAll();
    const int split = reply.indexOf("\r\n\r\n");
    if (split < 0)
        return {{"error", "no reply from rclone"}};
    const QJsonObject obj = QJsonDocument::fromJson(reply.mid(split + 4)).object();
    return obj.isEmpty() ? QJsonObject{{"error", "unreadable reply from rclone"}} : obj;
}

QStringList pending(const Account &account) {
    QStringList result;
    if (!account.mounted)
        return result;
    const QJsonObject q = rc(account.remote, "vfs/queue", {{"fs", account.fs()}});
    for (const QJsonValue &v : q.value("queue").toArray())
        result << '/' + v.toObject().value("name").toString();
    return result;
}

static QString systemctl(const QStringList &args) {
    QProcess p;
    p.start("systemctl", QStringList{"--user"} + args);
    if (!p.waitForFinished(60000) || p.exitCode() != 0)
        return QString::fromUtf8(p.readAllStandardError()).trimmed().split('\n').last();
    return {};
}

QString mount(const QString &remote, const QString &mountpointIn) {
    if (!validRemote(remote))
        return QString("%1 is not a valid account name").arg(remote);
    Account account = find(remote);
    if (account.remote.isEmpty())
        return QString("No OneDrive account %1 in rclone").arg(remote);
    if (account.mounted)
        return {};
    QString mountpoint = !mountpointIn.isEmpty() ? mountpointIn
                       : !account.mountpoint.isEmpty() ? account.mountpoint
                                                       : defaultMountpoint(account.remote);
    if (mountpoint.startsWith('~'))
        mountpoint = QDir::homePath() + mountpoint.mid(1);
    mountpoint = QDir::cleanPath(QDir(mountpoint).absolutePath());
    for (const Account &other : accounts())
        if (other.remote != account.remote && !other.mountpoint.isEmpty()
            && (inside(mountpoint, other.mountpoint) || inside(other.mountpoint, mountpoint)))
            return QString("%1 overlaps %2's folder").arg(mountpoint, other.label());
    if (!QDir().mkpath(mountpoint))
        return QString("Cannot create %1").arg(mountpoint);
    if (!QDir(mountpoint).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        return QString("%1 is not empty; move its files aside first").arg(mountpoint);

    QDir().mkpath(configDir());
    QFile env(configDir() + "/" + account.remote + ".env");
    if (!env.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return QString("Cannot write %1").arg(env.fileName());
    env.write(QString("REMOTE=%1:\nMOUNTPOINT=%2\n").arg(account.remote, mountpoint).toUtf8());
    env.close();

    if (const QString err = systemctl({"enable", "--now", unitName(account.remote)}); !err.isEmpty())
        return err;
    for (int i = 0; i < 60; ++i) {
        if (rcloneMounts().value(mountpoint).startsWith(account.remote + ':'))
            return {};
        QThread::msleep(250);
    }
    return QString("%1 did not mount; see journalctl --user -u %2").arg(account.label(), unitName(account.remote));
}

QString unmount(const QString &remote) {
    if (!validRemote(remote))
        return QString("%1 is not a valid account name").arg(remote);
    return systemctl({"disable", "--now", unitName(remote)});
}

}  // namespace Cloud

namespace {

using namespace Cloud;

int status() {
    const QVector<Account> all = accounts();
    if (all.isEmpty() && rclone({"version"}, 5000).code == -2) {
        print({{"ok", false}, {"error", "rclone is not installed"}});
        return 1;
    }
    // Storage for every account at once; each is a round trip to Microsoft.
    const QList<QJsonObject> rows = QtConcurrent::blockingMapped(all, [](const Account &a) {
        QJsonObject row{{"remote", a.remote}, {"label", a.label()}, {"driveType", a.driveType},
                        {"work", a.work()}, {"mountpoint", a.mountpoint},
                        {"configured", !a.mountpoint.isEmpty()}, {"mounted", a.mounted},
                        {"pending", 0}, {"used", 0}, {"total", 0}, {"error", ""}};
        QJsonObject about;
        if (a.mounted) {
            row["pending"] = pending(a).size();
            about = rc(a.remote, "operations/about", {{"fs", a.fs()}}, 20000);
        } else {
            const Run run = rclone({"about", a.fs(), "--json"}, 20000);
            about = run.code == 0 ? QJsonDocument::fromJson(run.out).object() : QJsonObject{{"error", run.error()}};
        }
        if (about.contains("error"))
            row["error"] = about.value("error").toString();
        row["used"] = about.value("used").toDouble();
        row["total"] = about.value("total").toDouble();
        return row;
    });
    QJsonArray list;
    for (const QJsonObject &r : rows)
        list.append(r);
    print({{"ok", true}, {"accounts", list}});
    return 0;
}

QString ask(const QString &question, const QString &fallback = {}) {
    QTextStream out(stdout), in(stdin);
    out << question;
    if (!fallback.isEmpty())
        out << " [" << fallback << "]";
    out << ": " << Qt::flush;
    const QString answer = in.readLine().trimmed();
    return answer.isEmpty() ? fallback : answer;
}

int connect() {
    QTextStream out(stdout);
    out << "Connect a OneDrive account\n\n" << Qt::flush;
    const bool work = ask("Is it a work or school account? (y/N)", "n").toLower().startsWith('y');
    QString name = ask("A short name for it (e.g. personal, ses)", work ? "work" : "personal").toLower();
    name.replace(QRegularExpression("[^a-z0-9_.-]"), "-");
    const QString remote = "onedrive-" + name;
    if (!validRemote(remote)) {
        out << "That name cannot be used.\n";
        return 1;
    }

    out << "\nA browser window opens for the Microsoft sign-in. When it asks which drive to use,\n"
           "pick the one called \"OneDrive\" (your own files), not a SharePoint library.\n\n" << Qt::flush;
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.setInputChannelMode(QProcess::ForwardedInputChannel);
    if (find(remote).remote.isEmpty())
        p.start("rclone", {"config", "create", remote, "onedrive", "region=global"});
    else
        p.start("rclone", {"config", "reconnect", remote + ":"});
    p.waitForFinished(-1);
    if (p.exitCode() != 0) {
        out << "\nrclone could not finish the sign-in.\n";
        return 1;
    }
    const Run check = rclone({"about", remote + ":"}, 30000);
    if (check.code != 0) {
        out << "\nSigned in, but the drive cannot be read: " << check.error() << "\n"
            << "Run `rclone config` to choose another drive for " << remote << ".\n";
        return 1;
    }

    const QString mountpoint = ask("\nFolder for its files", defaultMountpoint(remote));
    out << "Mounting…\n" << Qt::flush;
    if (const QString err = mount(remote, mountpoint); !err.isEmpty()) {
        out << err << "\n";
        return 1;
    }
    out << "Done: " << labelFor(remote) << " is at " << find(remote).mountpoint << "\n";
    return 0;
}

}  // namespace

int runCloud(const QStringList &args) {
    const QString command = args.value(0);
    if (command == "status")
        return status();
    if (command == "connect")
        return connect();
    if ((command == "mount" || command == "unmount") && args.size() >= 2) {
        const QString err = command == "mount" ? Cloud::mount(args[1], args.value(2)) : Cloud::unmount(args[1]);
        print(err.isEmpty() ? QJsonObject{{"ok", true}} : QJsonObject{{"ok", false}, {"error", err}});
        return err.isEmpty() ? 0 : 1;
    }
    std::fputs("usage: omadrive cloud status | connect | mount <account> [folder] | unmount <account>\n", stderr);
    return 2;
}
