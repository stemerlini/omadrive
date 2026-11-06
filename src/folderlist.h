#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVector>

// One pane's directory listing. The pane is a plain local folder today; the
// OneDrive pane will provide the same roles from Microsoft Graph.
class FolderList : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, PathRole, IsDirRole, SizeTextRole, ModifiedRole };

    explicit FolderList(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    void setPath(const QString &path);
    QString error() const { return m_error; }
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);

    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE bool isDirAt(int row) const;
    Q_INVOKABLE int indexOfPath(const QString &path) const;

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
    };

    void setError(const QString &error);

    QString m_path, m_error;
    bool m_showHidden = false;
    QVector<Entry> m_entries;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
