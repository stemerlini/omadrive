// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QHash<QString, QString> Theme::readColors(const QString &path, bool *dark) {
    QHash<QString, QString> colors{
        {"accent", "#7aa2f7"},           {"background", "#1a1b26"},
        {"dark_background", "#16161e"},  {"lighter_background", "#24283b"},
        {"foreground", "#a9b1d6"},       {"bright_foreground", "#c0caf5"},
        {"dark_foreground", "#565f89"},  {"magenta", "#bb9af7"},
        {"green", "#9ece6a"},            {"yellow", "#e0af68"},
        {"red", "#f7768e"},
    };
    QString text;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
        text = QString::fromUtf8(file.readAll());
    const QRegularExpression entry(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                   QRegularExpression::MultilineOption);
    QHash<QString, QString> read;
    for (auto it = entry.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        read.insert(match.captured(1), match.captured(2));
    }
    for (auto it = read.cbegin(); it != read.cend(); ++it)
        colors.insert(it.key(), it.value());
    // Older themes name only the basics; derive the rest from them.
    if (!read.contains("accent") && read.contains("blue"))
        colors["accent"] = read["blue"];
    const QColor fg(colors["foreground"]), bg(colors["background"]);
    auto mix = [&](double t) {
        return QColor::fromRgbF(bg.redF() + (fg.redF() - bg.redF()) * t, bg.greenF() + (fg.greenF() - bg.greenF()) * t,
                                bg.blueF() + (fg.blueF() - bg.blueF()) * t).name();
    };
    if (read.contains("background")) {
        if (!read.contains("dark_background")) colors["dark_background"] = QColor(bg).darker(115).name();
        if (!read.contains("lighter_background")) colors["lighter_background"] = mix(0.08);
    }
    if (read.contains("foreground")) {
        if (!read.contains("dark_foreground")) colors["dark_foreground"] = mix(0.55);
        if (!read.contains("bright_foreground")) colors["bright_foreground"] = QColor(fg).lighter(115).name();
    }
    if (dark) {
        static const QRegularExpression mode(R"re(^\s*mode\s*=\s*["'](\w+)["'])re", QRegularExpression::MultilineOption);
        const auto match = mode.match(text);
        *dark = match.hasMatch() ? match.captured(1) != "light" : bg.lightnessF() < 0.5;
    }
    return colors;
}

QString Theme::fontFamily() const {
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(accent());
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    bool dark = true;
    const auto colors = readColors(m_directory + "/theme/colors.toml", &dark);
    if (colors != m_colors || dark != m_dark) {
        m_colors = colors;
        m_dark = dark;
        emit changed();
    }
}
