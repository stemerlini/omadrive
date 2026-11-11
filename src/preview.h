#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>

// What the Space-bar preview shows for one file or folder. Loading runs on a
// worker thread; only the latest request wins.
class Preview : public QObject {
    Q_OBJECT
    // "", "loading", "image", "text", "pdf", "folder", "info" or "error".
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(QUrl source READ source NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QStringList entries READ entries NOTIFY changed)

public:
    explicit Preview(QObject *parent = nullptr);

    struct Result {
        QString kind, name, details, text;
        QUrl source;
        QStringList entries;
    };

    QString kind() const { return m_result.kind; }
    QString name() const { return m_result.name; }
    QString details() const { return m_result.details; }
    QUrl source() const { return m_result.source; }
    QString text() const { return m_result.text; }
    QStringList entries() const { return m_result.entries; }

    Q_INVOKABLE void show(const QString &path);
    Q_INVOKABLE void clear();

    static Result local(const QString &path);

signals:
    void changed();

private:
    void set(const Result &result);

    Result m_result;
    quint64 m_generation = 0;
};
