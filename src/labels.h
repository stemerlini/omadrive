#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

// Colour labels on files and folders, kept by path in a JSON file (labels
// live on this device only; rclone mounts cannot hold extended attributes).
// Renames and moves made in the app carry the labels along.
class Labels : public QObject {
    Q_OBJECT

public:
    explicit Labels(const QString &file, QObject *parent = nullptr);

    static Labels *instance() { return s_instance; }
    static void setInstance(Labels *labels) { s_instance = labels; }
    static const QStringList &names();

    Q_INVOKABLE QString label(const QString &path) const { return m_labels.value(path); }
    // An empty label clears it.
    Q_INVOKABLE void setLabel(const QStringList &paths, const QString &label);
    // `from` and everything under it now lives at `to`.
    void moved(const QString &from, const QString &to);
    void removed(const QString &path);

signals:
    void changed();

private:
    void save() const;

    QString m_file;
    QHash<QString, QString> m_labels;
    static Labels *s_instance;
};
