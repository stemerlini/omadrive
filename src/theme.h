#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString panel READ panel NOTIFY changed)
    Q_PROPERTY(QString surface READ surface NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QString bright READ bright NOTIFY changed)
    Q_PROPERTY(QString muted READ muted NOTIFY changed)
    Q_PROPERTY(QString secondary READ secondary NOTIFY changed)
    Q_PROPERTY(QString success READ success NOTIFY changed)
    Q_PROPERTY(QString warning READ warning NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return color("accent"); }
    QString accentForeground() const;
    QString background() const { return color("background"); }
    QString panel() const { return color("dark_background"); }
    QString surface() const { return color("lighter_background"); }
    QString foreground() const { return color("foreground"); }
    QString bright() const { return color("bright_foreground"); }
    QString muted() const { return color("dark_foreground"); }
    QString secondary() const { return color("magenta"); }
    QString success() const { return color("green"); }
    QString warning() const { return color("yellow"); }
    QString error() const { return color("red"); }
    bool dark() const { return m_dark; }
    QString fontFamily() const;
    int radius() const { return m_radius; }

    // The colours of an Omarchy colors.toml, with the ones it leaves out
    // taken from Omarchy's default theme (Tokyo Night), as Citadel does.
    static QHash<QString, QString> readColors(const QString &path, bool *dark = nullptr);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QString color(const QString &name) const { return m_colors.value(name); }

    QHash<QString, QString> m_colors;
    bool m_dark = true;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
