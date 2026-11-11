#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "folderlist.h"
#include "preview.h"
#include "rclone.h"
#include "theme.h"
#include "translator.h"

class PathfinderTests : public QObject {
    Q_OBJECT

private slots:
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
        QVERIFY(QDir(dir.path() + "/b").entryList({"*.pathfinder-part"}).isEmpty());
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

    void rcloneStateFollowsTheVfsCache()
    {
        QTemporaryDir cache;
        const QByteArray mounts =
            "/dev/sda1 / ext4 rw 0 0\n"
            "onedrive-personal: /home/u/One\\040Drive fuse.rclone rw 0 0\n"
            "work:Shared/Docs /home/u/Docs fuse.rclone rw 0 0\n";
        const auto parsed = Rclone::parseMounts(mounts, cache.path());
        QCOMPARE(parsed.size(), 2);
        QVERIFY(!Rclone::mountFor("/home/u/Other", parsed).isValid());
        QCOMPARE(Rclone::mountFor("/home/u/Docs/a", parsed).metaRoot, cache.path() + "/vfsMeta/work/Shared/Docs");

        const Rclone::Mount m = Rclone::mountFor("/home/u/One Drive/x", parsed);
        QCOMPARE(m.mountpoint, QString("/home/u/One Drive"));
        const QString meta = cache.path() + "/vfsMeta/onedrive-personal/";
        QVERIFY(QDir().mkpath(meta + "d"));
        write(meta + "d/full", R"({"Size":10,"Rs":[{"Pos":4,"Size":6},{"Pos":0,"Size":5}],"Dirty":false})");
        write(meta + "d/part", R"({"Size":10,"Rs":[{"Pos":0,"Size":4},{"Pos":5,"Size":5}],"Dirty":false})");
        write(meta + "d/dirty", R"({"Size":10,"Rs":[],"Dirty":true})");

        QCOMPARE(Rclone::state(m, "/home/u/One Drive/d/full", false), QString("local"));
        QCOMPARE(Rclone::state(m, "/home/u/One Drive/d/part", false), QString("online"));
        QCOMPARE(Rclone::state(m, "/home/u/One Drive/d/dirty", false), QString("pending"));
        QCOMPARE(Rclone::state(m, "/home/u/One Drive/d/never", false), QString("online"));
        QCOMPARE(Rclone::state(m, "/home/u/One Drive/d", true), QString());
    }

    // A folder is pending if anything inside waits to upload, local only if every file is cached.
    void rcloneFolderStateSumsUpItsFiles()
    {
        QTemporaryDir cache, root;
        const Rclone::Mount m{root.path(), cache.path()};
        QVERIFY(QDir().mkpath(root.path() + "/a/sub") && QDir().mkpath(root.path() + "/b"));
        write(root.path() + "/a/sub/f", "x");
        write(root.path() + "/b/f", "x");
        QCOMPARE(Rclone::folderState(m, root.path() + "/a"), QString("online"));

        QVERIFY(QDir().mkpath(cache.path() + "/a/sub"));
        write(cache.path() + "/a/sub/f", R"({"Size":1,"Rs":[{"Pos":0,"Size":1}],"Dirty":false})");
        QCOMPARE(Rclone::folderState(m, root.path() + "/a"), QString("local"));
        write(root.path() + "/a/g", "x");
        QCOMPARE(Rclone::folderState(m, root.path() + "/a"), QString("online"));
        write(cache.path() + "/a/sub/f", R"({"Size":1,"Rs":[],"Dirty":true})");
        QCOMPARE(Rclone::folderState(m, root.path() + "/a"), QString("pending"));
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
        QCOMPARE(theme.accent(), QString("#7aa2f7"));
        QCOMPARE(theme.background(), QString("#1a1b26"));

        // A theme naming only some colours keeps them and derives the rest.
        write(dir.filePath("colors.toml"), "mode = \"light\"\nbackground = \"#ffffff\"\nforeground = \"#000000\"\nblue = \"#0000ff\"\n");
        bool dark = true;
        const auto colors = Theme::readColors(dir.filePath("colors.toml"), &dark);
        QVERIFY(!dark);
        QCOMPARE(colors["accent"], QString("#0000ff"));
        QCOMPARE(colors["background"], QString("#ffffff"));
        QCOMPARE(colors["dark_foreground"], QColor::fromRgbF(0.45, 0.45, 0.45).name());
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
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

private:
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

QTEST_MAIN(PathfinderTests)
#include "pathfinder_tests.moc"