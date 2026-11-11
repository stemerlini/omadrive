#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVector>

#include "rclone.h"

// One pane's directory listing. Inside an rclone mount each file also has a
// "cloud" state (online, local or pending), kept current while shown;
// folders sum up what is inside them.
class FolderList : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(bool cloudMount READ cloudMount NOTIFY pathChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, PathRole, IsDirRole, SizeTextRole, ModifiedRole, CloudRole };

    explicit FolderList(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    void setPath(const QString &path);
    QString error() const { return m_error; }
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    bool cloudMount() const { return m_mount.isValid(); }

    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE bool isDirAt(int row) const;
    Q_INVOKABLE int indexOfPath(const QString &path) const;
    // Reads the files (folders included) through the mount so rclone keeps
    // them on this device. Returns false when not in an rclone mount.
    Q_INVOKABLE bool download(const QStringList &paths);

signals:
    void pathChanged();
    void errorChanged();
    void showHiddenChanged();

private:
    struct Entry {
        QString name, path;
        bool isDir;
        qint64 size;
        QDateTime modified;
        QString cloud;
    };

    void setError(const QString &error);
    void updateCloud();
    void updateFolders();

    QString m_path, m_error;
    bool m_showHidden = false;
    QVector<Entry> m_entries;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_cloudPoll;
    Rclone::Mount m_mount;
    int m_folderTick = 0;
    bool m_folderBusy = false;
    quint64 m_generation = 0;
};
