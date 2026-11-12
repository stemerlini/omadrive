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
    // "name", "type", "size" or "modified"; folders always come first.
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
    Q_PROPERTY(bool sortDescending READ sortDescending WRITE setSortDescending NOTIFY sortChanged)
    // Totals for the status line and the size bars.
    Q_PROPERTY(int folderCount READ folderCount NOTIFY listed)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY listed)
    Q_PROPERTY(qint64 totalSize READ totalSize NOTIFY listed)
    Q_PROPERTY(qint64 largestSize READ largestSize NOTIFY listed)

public:
    enum Roles { NameRole = Qt::UserRole + 1, PathRole, IsDirRole, SizeTextRole, ModifiedRole, CloudRole,
                 SizeRole, AgeRole, ModifiedFullRole, KindRole, TypeTextRole, ExtensionRole, ChildCountRole,
                 LabelRole, HiddenRole, LinkRole, LinkTargetRole };

    // What a file is, from its name alone (reading contents would download
    // files in a cloud mount): folder, image, video, audio, archive, pdf,
    // document, spreadsheet, slides, markdown, code, text, executable, file.
    static QString kindOf(const QString &path, bool isDir, bool executable, QString *typeText = nullptr);

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
    QString sortKey() const { return m_sortKey; }
    void setSortKey(const QString &key);
    bool sortDescending() const { return m_sortDescending; }
    void setSortDescending(bool descending);
    int folderCount() const;
    int fileCount() const { return m_entries.size() - folderCount(); }
    qint64 totalSize() const;
    qint64 largestSize() const;
    Q_INVOKABLE QString formatSize(qint64 bytes) const;
    // The total size of the files among these paths in this folder.
    Q_INVOKABLE qint64 sizeOf(const QStringList &paths) const;

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
    void sortChanged();
    void listed();

private:
    struct Entry {
        QString name, path;
        bool isDir;
        qint64 size;
        QDateTime modified;
        QString cloud;
        QString kind, typeText, label, linkTarget;
        bool hidden = false;
        int children = -1;
    };

    void setError(const QString &error);
    void updateCloud();
    void updateFolders();
    void updateChildCounts();
    void updateLabels();
    void sortEntries(QVector<Entry> &entries) const;

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
    QString m_sortKey = "name";
    bool m_sortDescending = false;
};
