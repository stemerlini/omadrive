#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>

// What the Space-bar preview shows for one file or folder, local or on
// OneDrive. Loading runs on a worker thread; only the latest request wins.
class Graph;

class Preview : public QObject {
    Q_OBJECT
    // "", "loading", "image", "text", "pdf", "folder", "info" or "error".
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(QUrl source READ source NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QStringList entries READ entries NOTIFY changed)
    // For OneDrive files: "online" or "local" (a matching mirror copy).
    Q_PROPERTY(QString cloud READ cloud NOTIFY changed)

public:
    explicit Preview(QObject *parent = nullptr);

    struct Result {
        QString kind, name, details, text, cloud;
        QUrl source;
        QStringList entries;
    };

    QString kind() const { return m_result.kind; }
    QString name() const { return m_result.name; }
    QString details() const { return m_result.details; }
    QUrl source() const { return m_result.source; }
    QString text() const { return m_result.text; }
    QStringList entries() const { return m_result.entries; }
    QString cloud() const { return m_result.cloud; }

    // account: whose OneDrive a remote path is in.
    Q_INVOKABLE void show(const QString &path, bool remote, const QString &account = {});
    Q_INVOKABLE void clear();

    static Result local(const QString &path);
    static Result remote(Graph &graph, const QString &mirrorRoot, const QString &path, const QString &cacheDir);

signals:
    void changed();

private:
    void set(const Result &result);

    Result m_result;
    QString m_cacheDir;
    quint64 m_generation = 0;
};
