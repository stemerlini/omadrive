#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <unistd.h>

#include "account.h"
#include "backend.h"
#include "folderlist.h"
#include "mirror.h"
#include "preview.h"
#include "sync.h"
#include "theme.h"
#include "transfer.h"
#include "translator.h"

// OneDrive in memory: every change goes to a log that delta() replays.
class FakeRemote : public Remote {
public:
    struct Entry {
        RemoteItem item;
        QByteArray data;
    };
    QMap<QString, Entry> items;
    QList<RemoteItem> log;
    QStringList recycled;
    int next = 0;

    FakeRemote() {
        RemoteItem root;
        root.id = "root";
        root.dir = root.root = true;
        add(root, {});
    }

    QString addFile(const QString &parent, const QString &name, const QByteArray &data) {
        RemoteItem item;
        item.id = QString("i%1").arg(++next);
        item.parentId = parent;
        item.name = name;
        return add(item, data);
    }
    QString addFolder(const QString &parent, const QString &name) {
        RemoteItem item;
        item.id = QString("i%1").arg(++next);
        item.parentId = parent;
        item.name = name;
        item.dir = true;
        return add(item, {});
    }
    // Someone edits the file on another device.
    void edit(const QString &id, const QByteArray &data) {
        items[id].data = data;
        touch(id, true);
    }
    QString find(const QString &parent, const QString &name) const {
        for (const Entry &e : items)
            if (e.item.parentId == parent && e.item.name == name)
                return e.item.id;
        return {};
    }

    bool delta(const QString &link, QList<RemoteItem> *out, QString *nextLink, bool *reset, QString *) override {
        if (link.isEmpty()) {
            *reset = true;
            *out = log;  // creation order puts folders before what is in them
        } else {
            *out = log.mid(link.toInt());
        }
        *nextLink = QString::number(log.size());
        return true;
    }
    Result item(const QString &id, RemoteItem *out, QString *) override {
        if (!items.contains(id))
            return Gone;
        *out = items[id].item;
        return Ok;
    }
    Result download(const QString &id, const QString &file, RemoteItem *out, QString *) override {
        if (!items.contains(id))
            return Gone;
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly))
            return Failed;
        f.write(items[id].data);
        *out = items[id].item;
        return Ok;
    }
    Result upload(const QString &id, const QString &eTag, const QString &parentId, const QString &name,
                  const QString &file, const QDateTime &, RemoteItem *out, QString *) override {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            return Failed;
        QString target = id;
        if (id.isEmpty()) {
            if (taken(parentId, name))
                return Taken;
            target = addFile(parentId, name, f.readAll());
        } else {
            if (!items.contains(id))
                return Gone;
            if (items[id].item.eTag != eTag)
                return Changed;
            edit(id, f.readAll());
        }
        *out = items[target].item;
        return Ok;
    }
    Result createFolder(const QString &parentId, const QString &name, RemoteItem *out, QString *) override {
        if (taken(parentId, name))
            return Taken;
        *out = items[addFolder(parentId, name)].item;
        return Ok;
    }
    Result move(const QString &id, const QString &eTag, const QString &parentId, const QString &name,
                RemoteItem *out, QString *) override {
        if (!items.contains(id))
            return Gone;
        if (items[id].item.eTag != eTag)
            return Changed;
        if (taken(parentId, name))
            return Taken;
        items[id].item.parentId = parentId;
        items[id].item.name = name;
        touch(id, false);
        *out = items[id].item;
        return Ok;
    }
    // Only ever into the recycle bin; there is no other way to delete here.
    Result remove(const QString &id, const QString &eTag, QString *) override {
        if (!items.contains(id))
            return Gone;
        if (items[id].item.eTag != eTag)
            return Changed;
        recycled << items[id].item.name;
        RemoteItem gone = items.take(id).item;
        gone.deleted = true;
        log << gone;
        return Ok;
    }

private:
    QString add(RemoteItem item, const QByteArray &data) {
        items.insert(item.id, {item, data});
        touch(item.id, true);
        return item.id;
    }
    void touch(const QString &id, bool content) {
        RemoteItem &item = items[id].item;
        ++next;
        item.eTag = QString("e%1").arg(next);
        if (content && !item.dir) {
            item.cTag = QString("c%1").arg(next);
            item.size = items[id].data.size();
        }
        log << item;
    }
    bool taken(const QString &parent, const QString &name) const {
        for (const Entry &e : items)
            if (e.item.parentId == parent && e.item.name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    }
};

class OmadriveTests : public QObject {
    Q_OBJECT

private slots:
    void workAccountsGetTheirOwnMirror()
    {
        Profile work;
        work.business = true;
        work.org = "SES S.A.";
        work.email = "stefano.merlini@ses.com";
        const QString base = QDir::homePath() + "/OneDrive-SES";
        QCOMPARE(Account::defaultMirror(work, {}), base);
        QCOMPARE(Account::defaultMirror(work, {base}), base + "-2");
        work.email = "someone@contoso.onmicrosoft.com";
        QCOMPARE(Account::defaultMirror(work, {}), QDir::homePath() + "/OneDrive-Contoso");
    }

    // An operation whose account is gone must not fall back to the one shown.
    void unknownAccountChangesNothing()
    {
        Mirror::setRoots({{"personal", "/tmp/p"}}, "personal");
        Transfer transfer;
        QSignalSpy finished(&transfer, &Transfer::finished);
        QSignalSpy done(&transfer, &Transfer::done);
        transfer.trash("work", {"/Reports"});
        transfer.upload("", {"/tmp/x"}, "/", false);
        transfer.openOnline("work", "/Reports");
        QCOMPARE(finished.count(), 2);
        QCOMPARE(done.count(), 1);
        QVERIFY(!finished.first().first().toString().isEmpty());
        Mirror::setRoots({}, {});
    }

    void workFilesStayInTheirAccount()
    {
        QTemporaryDir dir;
        const QString p = dir.path() + "/OneDrive-Personal", w = dir.path() + "/OneDrive-SES", docs = dir.path() + "/Docs";
        QVERIFY(QDir().mkpath(p) && QDir().mkpath(w) && QDir().mkpath(docs));
        write(w + "/plan.txt", "ses");
        Mirror::setRoots({{"personal", p}, {"work", w}}, "personal", {"work"});

        // Papers go to SES from anywhere.
        QVERIFY(Mirror::workLeak({}, {docs + "/paper.pdf"}, "work", {}).isEmpty());
        QVERIFY(Mirror::workLeak({}, {p + "/paper.pdf"}, "work", {}).isEmpty());
        QVERIFY(Mirror::workLeak("work", {}, {}, docs).isEmpty());
        QVERIFY(Mirror::workLeak("work", {}, {}, w + "/sub").isEmpty());
        // SES files never reach the personal account or its folder.
        QCOMPARE(Mirror::workLeak("work", {}, "personal", {}), QString("work"));
        QCOMPARE(Mirror::workLeak("work", {}, {}, p + "/x"), QString("work"));
        QCOMPARE(Mirror::workLeak({}, {w + "/plan.txt"}, "personal", {}), QString("work"));
        QCOMPARE(Mirror::workLeak({}, {dir.path()}, "personal", {}), QString("work"));

        Backend backend;
        QVERIFY(!backend.copyItems({w + "/plan.txt"}, p).isEmpty());
        QVERIFY(!QFile::exists(p + "/plan.txt"));
        Transfer transfer;
        QSignalSpy finished(&transfer, &Transfer::finished);
        transfer.upload("personal", {w + "/plan.txt"}, "/", false);
        QCOMPARE(finished.count(), 1);
        Mirror::setRoots({}, {});
    }

    void copyNeverOverwrites()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("a") && QDir(dir.path()).mkpath("b"));
        write(dir.path() + "/a/report.txt", "new");
        write(dir.path() + "/b/report.txt", "original");

        Backend backend;
        QCOMPARE(backend.copyItems({dir.path() + "/a/report.txt"}, dir.path() + "/b"), QString());
        QCOMPARE(read(dir.path() + "/b/report.txt"), QByteArray("original"));
        QCOMPARE(read(dir.path() + "/b/report (2).txt"), QByteArray("new"));
        QVERIFY(QDir(dir.path() + "/b").entryList({"*.omadrive-part"}).isEmpty());
    }

    void uniqueNameKeepsFullSuffix()
    {
        QTemporaryDir dir;
        write(dir.path() + "/data.tar.gz", "x");
        QCOMPARE(Backend::uniquePath(dir.path(), "data.tar.gz"), dir.path() + "/data (2).tar.gz");
    }

    void copiesFoldersRecursively()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("src/inner") && QDir(dir.path()).mkpath("dst"));
        write(dir.path() + "/src/inner/f.txt", "deep");

        Backend backend;
        QCOMPARE(backend.copyItems({dir.path() + "/src"}, dir.path() + "/dst"), QString());
        QCOMPARE(read(dir.path() + "/dst/src/inner/f.txt"), QByteArray("deep"));
        QVERIFY(QFile::exists(dir.path() + "/src/inner/f.txt"));
    }

    void refusesToCopyFolderIntoItself()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("a/b"));

        Backend backend;
        QVERIFY(!backend.copyItems({dir.path() + "/a"}, dir.path() + "/a/b").isEmpty());
        QVERIFY(!backend.moveItems({dir.path() + "/a"}, dir.path() + "/a/b").isEmpty());
        QVERIFY(QDir(dir.path() + "/a/b").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void moveRemovesTheOriginal()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("dst"));
        write(dir.path() + "/f.txt", "m");

        Backend backend;
        QCOMPARE(backend.moveItems({dir.path() + "/f.txt"}, dir.path() + "/dst"), QString());
        QVERIFY(!QFile::exists(dir.path() + "/f.txt"));
        QCOMPARE(read(dir.path() + "/dst/f.txt"), QByteArray("m"));
    }

    void renameRejectsPathsAndExistingNames()
    {
        QTemporaryDir dir;
        write(dir.path() + "/a.txt", "a");
        write(dir.path() + "/b.txt", "b");

        Backend backend;
        QVERIFY(!backend.renameItem(dir.path() + "/a.txt", "../x").isEmpty());
        QVERIFY(!backend.renameItem(dir.path() + "/a.txt", "b.txt").isEmpty());
        QCOMPARE(backend.renameItem(dir.path() + "/a.txt", "c.txt"), QString());
        QVERIFY(QFile::exists(dir.path() + "/c.txt"));
    }

    void listsFoldersFirstAndHidesDotfiles()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("zdir"));
        write(dir.path() + "/Apple.txt", "x");
        write(dir.path() + "/.secret", "x");

        FolderList list;
        list.setPath(dir.path());
        QCOMPARE(list.rowCount(), 2);
        QVERIFY(list.isDirAt(0));
        QCOMPARE(list.data(list.index(1), FolderList::NameRole).toString(), QString("Apple.txt"));

        list.setShowHidden(true);
        QCOMPARE(list.rowCount(), 3);
    }

    void unreadableFolderKeepsThePreviousPath()
    {
        QTemporaryDir dir;
        FolderList list;
        list.setPath(dir.path());
        list.setPath(dir.path() + "/missing");
        QCOMPARE(list.path(), QDir::cleanPath(dir.path()));
        QVERIFY(!list.error().isEmpty());
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void mirrorCopyMatchesOnlySameSizeAndTime()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.txt");
        write(path, "hello");
        const QDateTime stamp = QDateTime::fromString("2026-01-02T03:04:05Z", Qt::ISODate);
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
        file.close();

        QVERIFY(Mirror::sameFile(path, 5, stamp));
        QVERIFY(!Mirror::sameFile(path, 6, stamp));
        QVERIFY(!Mirror::sameFile(path, 5, stamp.addSecs(60)));
        QVERIFY(!Mirror::sameFile(dir.filePath("missing"), 5, stamp));
    }

    void previewPicksKindFromContents()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "line one\nline two");
        QImage(4, 4, QImage::Format_RGB32).save(dir.filePath("pic.png"));
        write(dir.filePath("doc.pdf"), "%PDF-1.4\n");
        write(dir.filePath("blob.bin"), QByteArray("\x00\x01\x02\xff", 4));
        QDir(dir.path()).mkdir("sub");

        const Preview::Result text = Preview::local(dir.filePath("notes.txt"));
        QCOMPARE(text.kind, QString("text"));
        QCOMPARE(text.text, QString("line one\nline two"));
        QCOMPARE(Preview::local(dir.filePath("pic.png")).kind, QString("image"));
        QCOMPARE(Preview::local(dir.filePath("doc.pdf")).kind, QString("pdf"));
        QCOMPARE(Preview::local(dir.filePath("blob.bin")).kind, QString("info"));
        const Preview::Result folder = Preview::local(dir.path());
        QCOMPARE(folder.kind, QString("folder"));
        QCOMPARE(folder.entries.first(), QString("sub/"));
    }

    void conflictCopyKeepsBothVersions()
    {
        QCOMPARE(SyncCore::conflictName("report.docx", "laptop", QDateTime(QDate(2026, 11, 8), QTime(22, 50))),
                 QString("report (conflict laptop 2026-11-08 2250).docx"));

        // Edited here and on another device before the upload.
        QTemporaryDir dir;
        FakeRemote remote;
        const QString id = remote.addFile("root", "notes.txt", "v1");
        SyncCore core(&remote, dir.path(), syncOptions());
        QVERIFY(core.pull(nullptr));
        QCOMPARE(core.state("/notes.txt"), QString("cloud"));
        writeFile(core, "/notes.txt", "mine");
        QCOMPARE(core.state("/notes.txt"), QString("pending"));
        remote.edit(id, "theirs");
        QVERIFY(core.pull(nullptr));
        core.push(true);

        // Neither side is overwritten: OneDrive's version keeps the name.
        QCOMPARE(remote.items[id].data, QByteArray("theirs"));
        QCOMPARE(remote.items[id].item.name, QString("notes.txt"));
        QCOMPARE(core.state("/notes.txt"), QString("cloud"));
        QList<QPair<QString, SyncCore::Stat>> entries;
        QCOMPARE(core.list("/", &entries), 0);
        QCOMPARE(entries.size(), 2);
        const QString copy = entries[0].first == "notes.txt" ? entries[1].first : entries[0].first;
        QVERIFY(copy.startsWith("notes (conflict laptop "));
        QCOMPARE(core.state("/" + copy), QString("conflict"));
        QCOMPARE(remote.items[remote.find("root", copy)].data, QByteArray("mine"));
    }

    // The same when the other edit only shows up as the upload's 412.
    void uploadNeverReplacesANewerVersion()
    {
        QTemporaryDir dir;
        FakeRemote remote;
        const QString id = remote.addFile("root", "plan.txt", "v1");
        SyncCore core(&remote, dir.path(), syncOptions());
        QVERIFY(core.pull(nullptr));
        writeFile(core, "/plan.txt", "mine");
        remote.edit(id, "theirs");
        core.push(true);
        core.push(true);
        QCOMPARE(remote.items[id].data, QByteArray("theirs"));
        QCOMPARE(remote.items.size(), 3);  // the root, plan.txt and the conflict copy
        QCOMPARE(core.status().value("conflicts").toInt(), 1);
    }

    void pendingChangesSurviveARestart()
    {
        QTemporaryDir dir;
        FakeRemote remote;
        const QString old = remote.addFile("root", "old.txt", "x");
        {
            SyncCore core(&remote, dir.path(), syncOptions());
            QVERIFY(core.pull(nullptr));
            QCOMPARE(core.mkdir("/Projects"), 0);
            writeFile(core, "/Projects/new.txt", "hello", true);
            QCOMPARE(core.unlink("/old.txt"), 0);
        }  // stopped before uploading anything

        SyncCore core(&remote, dir.path(), syncOptions());
        QVERIFY(core.load());
        QCOMPARE(core.state("/Projects/new.txt"), QString("pending"));
        QCOMPARE(core.status().value("pending").toInt(), 3);
        core.push(true);
        const QString folder = remote.find("root", "Projects");
        QVERIFY(!folder.isEmpty());
        QCOMPARE(remote.items[remote.find(folder, "new.txt")].data, QByteArray("hello"));
        QVERIFY(!remote.items.contains(old));
        QCOMPARE(remote.recycled, QStringList{"old.txt"});
        QCOMPARE(core.state("/Projects/new.txt"), QString("local"));
        QVERIFY(!core.hasWork());
    }

    // Deletes go to the recycle bin, and never take a version edited elsewhere.
    void deleteKeepsAFileChangedElsewhere()
    {
        QTemporaryDir dir;
        FakeRemote remote;
        const QString id = remote.addFile("root", "draft.txt", "v1");
        SyncCore core(&remote, dir.path(), syncOptions());
        QVERIFY(core.pull(nullptr));
        QCOMPARE(core.unlink("/draft.txt"), 0);
        QCOMPARE(core.state("/draft.txt"), QString());
        remote.edit(id, "v2");
        core.push(true);
        QVERIFY(remote.items.contains(id));
        QVERIFY(remote.recycled.isEmpty());
        QCOMPARE(core.state("/draft.txt"), QString("cloud"));
    }

    void workAccountStaysInItsDrive()
    {
        // Shared folders and SharePoint libraries are not shown, so never written.
        QTemporaryDir dir;
        FakeRemote remote;
        const QString library = remote.addFolder("root", "SES Library");
        remote.items[library].item.foreign = true;
        remote.log.last().foreign = true;
        remote.addFile("root", "mine.txt", "x");
        SyncCore core(&remote, dir.path(), syncOptions());
        QVERIFY(core.pull(nullptr));
        QCOMPARE(core.state("/SES Library"), QString());
        QCOMPARE(core.state("/mine.txt"), QString("cloud"));

        const QString p = dir.path() + "/OneDrive-Personal", w = dir.path() + "/OneDrive-SES";
        Mirror::setRoots({{"personal", p}, {"work", w}}, "personal", {"work"});
        const QStringList ids{"personal", "work"};
        QVERIFY(SyncCore::mountRefusal("work", w, ids).isEmpty());
        QVERIFY(!SyncCore::mountRefusal("work", p + "/SES", ids).isEmpty());
        QVERIFY(!SyncCore::mountRefusal("personal", w + "/x", ids).isEmpty());
        QVERIFY(!SyncCore::mountRefusal("personal", dir.path(), ids).isEmpty());
        Mirror::setRoots({}, {});
    }

private:
    static SyncCore::Options syncOptions()
    {
        SyncCore::Options options;
        options.settleMs = 0;
        options.host = "laptop";
        return options;
    }

    // Writes through the sync folder as the FUSE layer does.
    static void writeFile(SyncCore &core, const QString &path, const QByteArray &contents, bool create = false)
    {
        int fd = -1;
        quint64 key = 0;
        const int flags = O_WRONLY | O_TRUNC;
        QCOMPARE(create ? core.create(path, flags, &fd, &key) : core.open(path, flags, &fd, &key), 0);
        QCOMPARE(::write(fd, contents.constData(), contents.size()), contents.size());
        core.wrote(key, contents.size());
        ::close(fd);
        core.closed(key, true);
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
};

QTEST_MAIN(OmadriveTests)
#include "omadrive_tests.moc"
