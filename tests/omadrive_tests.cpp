#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "folderlist.h"
#include "theme.h"
#include "translator.h"

class OmadriveTests : public QObject {
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

QTEST_MAIN(OmadriveTests)
#include "omadrive_tests.moc"
