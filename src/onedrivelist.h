#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QVector>

class Account;

// The OneDrive pane's listing. It offers the same roles and invokables as
// FolderList; paths are OneDrive paths such as "/Documents/Report.docx".
class OneDriveList : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, PathRole, IsDirRole, SizeTextRole, ModifiedRole, CloudRole };

    explicit OneDriveList(Account *account, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    void setPath(const QString &path);
    QString error() const { return m_error; }
    bool loading() const { return m_loading; }

    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE bool isDirAt(int row) const;
    Q_INVOKABLE int indexOfPath(const QString &path) const;
    // Re-reads the local mirror so the cloud icons follow a download.
    Q_INVOKABLE void recheck();

signals:
    void pathChanged();
    void errorChanged();
    void loadingChanged();

private:
    struct Entry {
        QString name, path;
        bool isDir;
        qint64 size;
        QDateTime modified;
        QString cloud;  // see Mirror::state
    };

    void navigate(const QString &path);
    void fetch(const QString &path);
    void received(const QString &path, const QVector<Entry> &entries, const QString &error);
    void show(const QString &path, QVector<Entry> entries);
    void prefetch();
    void clear();
    void setLoading(bool loading);
    void setError(const QString &error);

    Account *m_account;
    QString m_path = "/", m_target = "/", m_error;
    bool m_loading = false, m_prefetching = false;
    QVector<Entry> m_entries;
    // Listings seen so far: a revisited folder shows at once and is then
    // checked against OneDrive in the background.
    QHash<QString, QVector<Entry>> m_cache;
    QStringList m_prefetchQueue;
};
