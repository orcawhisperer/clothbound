// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>
#include <memory>

namespace {
QString defaultHyprSocketDir() {
    const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature.isEmpty())
        return {};
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty()) {
        const QString path = runtime + QStringLiteral("/hypr/") + signature;
        if (QFileInfo::exists(path))
            return path;
    }
    const QString legacy = QStringLiteral("/tmp/hypr/") + signature;
    return QFileInfo::exists(legacy) ? legacy : QString();
}
}  // namespace

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprSocketDir)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_palette(fallbackPalette()) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding via Hyprland's UNIX sockets
    // directly: listen on .socket2.sock for configreloaded events (zero
    // polling) and query .socket.sock asynchronously.
    m_hyprSocketDir = !hyprSocketDir.isEmpty() ? hyprSocketDir
                    : directory.isEmpty() ? defaultHyprSocketDir() : QString();
    connect(&m_eventSocket, &QLocalSocket::readyRead, this, [this] {
        m_eventBuffer += m_eventSocket.readAll();
        qsizetype newline;
        bool reloaded = false;
        while ((newline = m_eventBuffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_eventBuffer.left(newline).trimmed();
            m_eventBuffer.remove(0, newline + 1);
            if (line.startsWith("configreloaded>>"))
                reloaded = true;
        }
        if (m_eventBuffer.size() > 65536)
            m_eventBuffer.clear();
        if (reloaded)
            refreshRounding();
    });
    if (!m_hyprSocketDir.isEmpty())
        connectHyprEvents();
    reload();
}

Theme::~Theme() {
    m_eventSocket.abort();
    if (m_roundingSocket)
        m_roundingSocket->abort();
}

void Theme::connectHyprEvents() {
    if (m_hyprSocketDir.isEmpty() || m_eventSocket.state() != QLocalSocket::UnconnectedState)
        return;
    const QString eventSocket = m_hyprSocketDir + QStringLiteral("/.socket2.sock");
    if (!QFileInfo::exists(eventSocket))
        return;
    m_eventBuffer.clear();
    m_eventSocket.connectToServer(eventSocket);
}

void Theme::refreshRounding() {
    if (m_hyprSocketDir.isEmpty() || m_roundingSocket)
        return;
    const QString requestSocket = m_hyprSocketDir + QStringLiteral("/.socket.sock");
    if (!QFileInfo::exists(requestSocket))
        return;
    auto *socket = new QLocalSocket(this);
    m_roundingSocket = socket;
    auto buffer = std::make_shared<QByteArray>();
    auto handled = std::make_shared<bool>(false);
    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    timeout->setInterval(500);

    auto finish = [this, socket, buffer, handled, timeout] {
        if (*handled)
            return;
        *handled = true;
        timeout->stop();
        if (m_roundingSocket == socket)
            m_roundingSocket = nullptr;
        const auto value = QJsonDocument::fromJson(*buffer).object().value("int");
        socket->deleteLater();
        if (value.isDouble() && value.toInt(-1) >= 0 && value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    };
    connect(timeout, &QTimer::timeout, socket, [socket, finish] {
        socket->abort();
        finish();
    });
    connect(socket, &QLocalSocket::connected, socket, [socket] {
        socket->write("j/getoption decoration:rounding");
        socket->flush();
    });
    connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer] {
        *buffer += socket->readAll();
    });
    connect(socket, &QLocalSocket::disconnected, this, finish);
    connect(socket, &QLocalSocket::errorOccurred, this, [socket, finish](QLocalSocket::LocalSocketError err) {
        if (err == QLocalSocket::PeerClosedError)
            return;
        socket->abort();
        finish();
    });
    timeout->start();
    socket->connectToServer(requestSocket);
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

QHash<QString, QString> Theme::fallbackLightPalette() {
    return {
        {"mode", "light"},
        {"accent", "#1a5fb4"},
        {"selection", "#d6e4f7"},
        {"muted", "#d4d2cb"},
        {"background", "#f6f5f0"},
        {"dark_background", "#eceae3"},
        {"darker_background", "#e2dfd7"},
        {"lighter_background", "#ffffff"},
        {"foreground", "#1c1b18"},
        {"dark_foreground", "#5c5a54"},
        {"light_foreground", "#45433e"},
        {"bright_foreground", "#000000"},
        {"red", "#c01c28"},
        {"yellow", "#9a6700"},
        {"orange", "#b25000"},
        {"green", "#1f7a3f"},
        {"cyan", "#0b6e7d"},
        {"blue", "#1a5fb4"},
        {"magenta", "#813d9c"},
    };
}

QHash<QString, QString> Theme::readPalette(const QString &path) {
    QHash<QString, QString> palette;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return palette;
    static const QRegularExpression section(R"re(^\s*\[)re");
    static const QRegularExpression entry(R"re(^\s*([a-z_0-9]+)\s*=\s*(["'])([^"']*)\2\s*(?:#.*)?$)re");
    static const QRegularExpression hex("^#[0-9a-fA-F]{6}$");
    const QString text = QString::fromUtf8(file.readAll());
    bool inSection = false;
    for (const QString &rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (section.match(line).hasMatch()) {
            inSection = true;
            continue;
        }
        if (inSection)
            continue;
        const auto match = entry.match(line);
        if (!match.hasMatch())
            continue;
        const QString key = match.captured(1);
        const QString value = match.captured(3).trimmed();
        if (key == "mode" || hex.match(value).hasMatch())
            palette.insert(key, key == "mode" ? value.toLower() : value);
    }
    return palette;
}

QString Theme::readAccent(const QString &path) {
    return readPalette(path).value("accent", "#FFD60A");
}

QString Theme::color(const QString &key) const {
    const auto fallback = m_light ? fallbackLightPalette() : fallbackPalette();
    return m_palette.value(key, fallback.value(key, "#ff00ff"));
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
    connectHyprEvents();
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

    // Keys a theme leaves out keep the fallback matching its mode (light or
    // dark), so partial light themes keep high-contrast secondary text.
    const auto read = readPalette(m_directory + "/theme/colors.toml");
    const bool omarchy = read.contains("background") && read.contains("foreground");
    const bool light = omarchy && read.value("mode") == "light";
    auto palette = light ? fallbackLightPalette() : fallbackPalette();
    if (omarchy) {
        for (auto it = read.cbegin(); it != read.cend(); ++it)
            palette.insert(it.key(), it.value());
    } else {
        // A theme with only an accent (or none) keeps the dark fallback around it.
        palette.insert("accent", read.value("accent", "#FFD60A"));
    }
    if (palette != m_palette || light != m_light || omarchy != m_omarchy) {
        m_palette = palette;
        m_light = light;
        m_omarchy = omarchy;
        emit changed();
    }
}
