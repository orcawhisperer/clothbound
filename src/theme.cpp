// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_palette(fallbackPalette()) {
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

QHash<QString, QString> Theme::fallbackPalette() {
    return {
        {"mode", "dark"},
        {"accent", "#FFD60A"},
        {"selection", "#3a3a42"},
        {"muted", "#2c2c33"},
        {"background", "#0e0e10"},
        {"dark_background", "#09090b"},
        {"darker_background", "#050506"},
        {"lighter_background", "#1c1c20"},
        {"foreground", "#e6e6e9"},
        {"dark_foreground", "#6c6c75"},
        {"light_foreground", "#b4b4bb"},
        {"bright_foreground", "#ffffff"},
        {"red", "#ff6b6b"},
        {"yellow", "#ffd60a"},
        {"orange", "#ff9f43"},
        {"green", "#7bd88f"},
        {"cyan", "#5ad4e6"},
        {"blue", "#6ea8fe"},
        {"magenta", "#d38aea"},
    };
}

QHash<QString, QString> Theme::readPalette(const QString &path) {
    QHash<QString, QString> palette;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return palette;
    static const QRegularExpression line(R"re(^\s*([a-z_0-9]+)\s*=\s*["']([^"']*)["'])re",
                                         QRegularExpression::MultilineOption);
    static const QRegularExpression hex("^#[0-9a-fA-F]{6}$");
    auto matches = line.globalMatch(QString::fromUtf8(file.readAll()));
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString key = match.captured(1), value = match.captured(2).trimmed();
        if (key == "mode" || hex.match(value).hasMatch())
            palette.insert(key, key == "mode" ? value.toLower() : value);
    }
    return palette;
}

QString Theme::readAccent(const QString &path) {
    return readPalette(path).value("accent", "#FFD60A");
}

QString Theme::color(const QString &key) const {
    return m_palette.value(key, fallbackPalette().value(key, "#ff00ff"));
}

QVariantMap Theme::colors() const {
    QVariantMap map;
    for (auto it = m_palette.cbegin(); it != m_palette.cend(); ++it)
        map.insert(it.key(), it.value());
    return map;
}

QString Theme::readableOn(const QString &color) {
    const QColor c(color);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

QString Theme::accentForeground() const { return readableOn(accent()); }

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

    // Keys a theme leaves out keep the fallback's value, so an older or
    // partial colors.toml still yields a complete palette.
    const auto read = readPalette(m_directory + "/theme/colors.toml");
    auto palette = fallbackPalette();
    for (auto it = read.cbegin(); it != read.cend(); ++it)
        palette.insert(it.key(), it.value());
    const bool omarchy = read.contains("background") && read.contains("foreground");
    // A theme with only an accent (or none) keeps the dark fallback around it.
    if (!omarchy) {
        const QString accent = read.value("accent", "#FFD60A");
        palette = fallbackPalette();
        palette.insert("accent", accent);
    }
    const bool light = palette.value("mode") == "light";
    if (palette != m_palette || light != m_light || omarchy != m_omarchy) {
        m_palette = palette;
        m_light = light;
        m_omarchy = omarchy;
        emit changed();
    }
}
