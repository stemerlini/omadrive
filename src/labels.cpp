#include "labels.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

Labels *Labels::s_instance = nullptr;

const QStringList &Labels::names() {
    static const QStringList list{"red", "orange", "yellow", "green", "blue", "purple", "gray"};
    return list;
}

Labels::Labels(const QString &file, QObject *parent) : QObject(parent), m_file(file) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return;
    const auto object = QJsonDocument::fromJson(in.readAll()).object();
    for (auto it = object.begin(); it != object.end(); ++it)
        if (names().contains(it.value().toString()))
            m_labels.insert(it.key(), it.value().toString());
}

void Labels::setLabel(const QStringList &paths, const QString &label) {
    if (!label.isEmpty() && !names().contains(label))
        return;
    for (const QString &path : paths) {
        if (label.isEmpty())
            m_labels.remove(path);
        else
            m_labels.insert(path, label);
    }
    save();
    emit changed();
}

void Labels::moved(const QString &from, const QString &to) {
    bool any = false;
    for (const QString &key : m_labels.keys()) {
        if (key == from || key.startsWith(from + '/')) {
            m_labels.insert(to + key.mid(from.size()), m_labels.take(key));
            any = true;
        }
    }
    if (any) {
        save();
        emit changed();
    }
}

void Labels::removed(const QString &path) {
    bool any = false;
    for (const QString &key : m_labels.keys())
        if (key == path || key.startsWith(path + '/'))
            any |= m_labels.remove(key) > 0;
    if (any) {
        save();
        emit changed();
    }
}

void Labels::save() const {
    QJsonObject object;
    for (auto it = m_labels.cbegin(); it != m_labels.cend(); ++it)
        object.insert(it.key(), it.value());
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile out(m_file);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        out.commit();
    }
}
