#pragma once

#include <QObject>
#include <QStringList>

// File operations between panes. Each returns an empty string on success, or
// a message saying what went wrong. Nothing is ever overwritten: a name that
// already exists in the destination gets " (2)", " (3)", ... appended.
class Backend : public QObject {
    Q_OBJECT

public:
    explicit Backend(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QString copyItems(const QStringList &paths, const QString &destination);
    Q_INVOKABLE QString moveItems(const QStringList &paths, const QString &destination);
    Q_INVOKABLE QString trashItems(const QStringList &paths);
    Q_INVOKABLE QString makeFolder(const QString &parent, const QString &name);
    Q_INVOKABLE QString makeFile(const QString &parent, const QString &name, const QString &content, bool executable);
    Q_INVOKABLE QString renameItem(const QString &path, const QString &newName);
    Q_INVOKABLE QString homePath() const;
    Q_INVOKABLE bool isDir(const QString &path) const;
    Q_INVOKABLE void openExternally(const QString &path);
    // Starts another Omadrive window with fresh tabs.
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE void copyText(const QString &text);

    static QString uniquePath(const QString &directory, const QString &name);

private:
    QString copyOne(const QString &source, const QString &destination);
};
