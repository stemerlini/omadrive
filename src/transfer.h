#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>

class Graph;

// Copies, moves and edits between the local disk and OneDrive on a worker
// thread. One operation runs at a time. Nothing is overwritten: on a name
// clash the copy gets a " (2)" style name, and a move removes the original
// only after its copy fully succeeded (OneDrive keeps deleted items in its
// recycle bin).
class Transfer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString progress READ progress NOTIFY progressChanged)

public:
    explicit Transfer(QObject *parent = nullptr) : QObject(parent) {}

    bool busy() const { return m_busy; }
    QString progress() const { return m_progress; }

    // Every OneDrive operation names the account its paths belong to, and
    // works on that account whichever one is shown by then; an id that is
    // not one of the accounts is refused.
    Q_INVOKABLE void download(const QString &account, const QStringList &remotePaths, const QString &localDir,
                              bool removeSource);
    // Into the same path under the account's mirror folder.
    Q_INVOKABLE void downloadToMirror(const QString &account, const QStringList &remotePaths, bool removeSource);
    Q_INVOKABLE void upload(const QString &account, const QStringList &localPaths, const QString &remoteDir,
                            bool removeSource);
    Q_INVOKABLE void makeFolder(const QString &account, const QString &remoteParent, const QString &name);
    Q_INVOKABLE void makeFile(const QString &account, const QString &remoteParent, const QString &name,
                              const QString &content);
    Q_INVOKABLE void renameItem(const QString &account, const QString &remotePath, const QString &newName);
    Q_INVOKABLE void trash(const QString &account, const QStringList &remotePaths);
    // Deletes the mirror's copies that match OneDrive, leaving them online only.
    Q_INVOKABLE void freeUp(const QString &account, const QStringList &remotePaths);

    // These run beside a transfer and report through done(). The link goes
    // to the clipboard; scope is empty (the account's default),
    // "organization" or "anonymous".
    Q_INVOKABLE void shareLink(const QString &account, const QString &remotePath, bool edit, const QString &scope);
    Q_INVOKABLE void invite(const QString &account, const QString &remotePath, const QStringList &emails, bool edit,
                            const QString &message);
    Q_INVOKABLE void openOnline(const QString &account, const QString &remotePath);

signals:
    void busyChanged();
    void progressChanged();
    void finished(const QString &error, const QString &text);
    void done(const QString &error, const QString &text);

private:
    struct Result {
        Result(const QString &error = {}, const QString &text = {}) : error(error), text(text) {}
        QString error, text;
        QString copy;  // put on the clipboard
        QUrl open;     // opened in the browser
    };
    using Say = std::function<void(const QString &)>;

    void start(std::function<Result(const Say &)> job);
    void quick(std::function<Result()> job);
    Graph *graphOf(const QString &account, bool quickJob);
    void downloadAll(Graph *graph, const QStringList &remotePaths, const std::function<QString(const QString &)> &dirFor,
                     bool merge, bool removeSource);

    bool m_busy = false;
    QString m_progress;
};
