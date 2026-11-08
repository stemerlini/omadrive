#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "account.h"
#include "backend.h"
#include "folderlist.h"
#include "mirror.h"
#include "preview.h"
#include "theme.h"
#include "transfer.h"
#include "translator.h"

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
