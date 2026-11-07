#pragma once

#include <QObject>
#include <QStringList>
#include <functional>

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

    Q_INVOKABLE void download(const QStringList &remotePaths, const QString &localDir, bool removeSource);
    Q_INVOKABLE void upload(const QStringList &localPaths, const QString &remoteDir, bool removeSource);
    Q_INVOKABLE void makeFolder(const QString &remoteParent, const QString &name);
    Q_INVOKABLE void renameItem(const QString &remotePath, const QString &newName);
    Q_INVOKABLE void trash(const QStringList &remotePaths);

signals:
    void busyChanged();
    void progressChanged();
    void finished(const QString &error, const QString &text);

private:
    struct Result {
        QString error, text;
    };
    using Say = std::function<void(const QString &)>;

    void start(std::function<Result(const Say &)> job);

    bool m_busy = false;
    QString m_progress;
};
