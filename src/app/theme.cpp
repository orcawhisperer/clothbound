// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "app/theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
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
        if (value.isDouble() && value.toInt(-1) >= 0 && value.toInt() != m_desktopRadius) {
            m_desktopRadius = value.toInt();
            if (m_corners == QLatin1String("match"))
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

// Without Omarchy, the design's neutrals: near-black and near-white greys
// with no tint, and Omarchy's default yellow (or a deep blue on light).
QHash<QString, QString> Theme::fallbackPalette() {
    return {
        {"mode", "dark"},
        {"accent", "#FFD60A"},
        {"selection", "#34343a"},
        {"muted", "#2a2a2e"},
        {"background", "#121214"},
        {"dark_background", "#0e0e10"},
        {"darker_background", "#0a0a0b"},
        {"lighter_background", "#1c1c1f"},
        {"foreground", "#f2f2f3"},
        {"dark_foreground", "#8b8b94"},
        {"light_foreground", "#d4d4d8"},
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
        {"muted", "#dcdce0"},
        {"background", "#fbfbfc"},
        {"dark_background", "#f1f1f3"},
        {"darker_background", "#e8e8eb"},
        {"lighter_background", "#ffffff"},
        {"foreground", "#1c1c1f"},
        {"dark_foreground", "#66666e"},
        {"light_foreground", "#34343a"},
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

QString Theme::accentOn() const {
    return readableOn(accent()) == QLatin1String("black") ? QStringLiteral("#0b0d12") : QStringLiteral("#ffffff");
}

double Theme::contrast(const QString &a, const QString &b) {
    auto luminance = [](const QColor &c) {
        auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    };
    const double x = luminance(QColor(a)), y = luminance(QColor(b));
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}

QString Theme::mix(const QString &a, const QString &b, double amount) {
    const QColor x(a), y(b);
    auto channel = [amount](int from, int to) { return int(std::lround(from + (to - from) * amount)); };
    return QColor(channel(x.red(), y.red()), channel(x.green(), y.green()), channel(x.blue(), y.blue()))
        .name();
}

namespace {
QString withAlpha(const QString &color, double alpha) {
    QColor c(color);
    c.setAlphaF(alpha);
    return c.name(QColor::HexArgb);
}

// `color`, moved step by step toward black or white (whichever is further
// from the ground) until it reads at `ratio` against `ground`.
QString readable(QString color, const QString &ground, double ratio) {
    const QString extreme = Theme::contrast("#000000", ground) > Theme::contrast("#ffffff", ground)
                                ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
    for (int step = 0; step < 24 && Theme::contrast(color, ground) < ratio; ++step)
        color = Theme::mix(color, extreme, .08);
    return color;
}
}  // namespace

QHash<QString, QString> Theme::deriveTokens(const QHash<QString, QString> &palette, bool light, bool omarchy) {
    QHash<QString, QString> t;
    const QString accent = palette.value("accent", light ? "#1a5fb4" : "#FFD60A");
    if (omarchy) {
        // Surfaces step gently away from the theme's own background, and the
        // ink tiers are its foreground made stronger or fainter.
        const QString bg = palette.value("background"), fg = palette.value("foreground");
        const QString extreme = light ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
        t["ground"] = bg;
        t["sidebar"] = mix(bg, fg, light ? .045 : .035);
        t["drawer"] = light ? t["sidebar"] : mix(bg, fg, .025);
        t["raised"] = light ? mix(bg, "#ffffff", .7) : mix(bg, fg, .06);
        t["overlay"] = light ? mix(bg, "#ffffff", .9) : mix(bg, fg, .09);
        t["ink1"] = mix(fg, extreme, .35);
        t["ink2"] = fg;
        t["ink3"] = mix(fg, bg, .28);
        t["ink4"] = mix(fg, bg, .42);
        t["quote"] = mix(fg, bg, .14);
    } else if (light) {
        t = {{"ground", "#fbfbfc"}, {"sidebar", "#f1f1f3"}, {"drawer", "#f1f1f3"},
             {"raised", "#ffffff"}, {"overlay", "#ffffff"},
             {"ink1", "#1c1c1f"}, {"ink2", "#34343a"}, {"ink3", "#5a5a62"}, {"ink4", "#66666e"}, {"quote", "#47474f"}};
    } else {
        t = {{"ground", "#121214"}, {"sidebar", "#18181b"}, {"drawer", "#171716"},
             {"raised", "#1c1c1f"}, {"overlay", "#232327"},
             {"ink1", "#f2f2f3"}, {"ink2", "#d4d4d8"}, {"ink3", "#a1a1aa"}, {"ink4", "#8b8b94"}, {"quote", "#c4c4ca"}};
    }
    // However faint a theme's foreground, body text stays readable (WCAG AA),
    // headings stronger, and only hints may drop to 3:1.
    const QString ground = t["ground"];
    t["ink1"] = readable(t["ink1"], ground, 7.0);
    t["ink2"] = readable(t["ink2"], ground, 4.5);
    t["ink3"] = readable(t["ink3"], ground, 4.5);
    t["ink4"] = readable(t["ink4"], ground, 3.0);
    t["quote"] = readable(t["quote"], ground, 4.5);

    const QString ink = t["ink1"];
    t["hairline"] = withAlpha(ink, light ? .10 : .08);
    t["separator"] = withAlpha(ink, light ? .15 : .12);
    t["fill"] = withAlpha(ink, light ? .05 : .06);
    t["fillHover"] = withAlpha(ink, light ? .08 : .10);
    t["fillPressed"] = withAlpha(ink, light ? .12 : .14);
    // The accent as text or an icon on the ground: on light themes it's
    // darkened until it reads, as the design does with its accents.
    t["accentInk"] = readable(accent, ground, 4.5);
    t["accentSoft"] = withAlpha(accent, light ? .14 : .18);
    t["accentRead"] = withAlpha(accent, .28);
    t["focus"] = withAlpha(accent, .6);
    // Where intelligence runs: green on this computer, amber sent to Google.
    t["positive"] = readable("#5fcb8a", ground, 4.5);
    t["positiveSoft"] = light ? (omarchy ? mix(ground, "#5fcb8a", .14) : QStringLiteral("#e3efe3"))
                              : withAlpha("#5fcb8a", .14);
    t["positiveInk"] = readable(light ? QStringLiteral("#24502c") : QStringLiteral("#8fddae"), ground, 4.5);
    t["cloud"] = readable("#e0af68", ground, 4.5);
    t["cloudSoft"] = light ? (omarchy ? mix(ground, "#e0af68", .16) : QStringLiteral("#f4e6cc"))
                           : withAlpha("#e0af68", .14);
    t["cloudInk"] = readable(light ? QStringLiteral("#6a4410") : QStringLiteral("#ebc48c"), ground, 4.5);
    t["danger"] = readable(palette.value("red", "#ff6b6b"), ground, 4.5);
    t["scrim"] = light ? QStringLiteral("#40060608") : QStringLiteral("#94060608");
    t["shadow"] = light ? QStringLiteral("#26000000") : QStringLiteral("#73000000");
    return t;
}

int Theme::radius() const {
    if (m_corners == QLatin1String("sharp"))
        return 0;
    if (m_corners == QLatin1String("rounded"))
        return 22;
    return m_desktopRadius;
}

void Theme::setCorners(const QString &corners) {
    const QString value = (corners == QLatin1String("rounded") || corners == QLatin1String("sharp"))
                              ? corners
                              : QStringLiteral("match");
    if (value == m_corners)
        return;
    m_corners = value;
    emit changed();
}

void Theme::setAppearance(const QString &appearance) {
    const QString value = appearance == "dark" || appearance == "light" ? appearance : QStringLiteral("match");
    if (value == m_appearance)
        return;
    m_appearance = value;
    reload();
    emit changed();
}

void Theme::setReducedMotion(bool reduced) {
    if (reduced == m_reducedMotion)
        return;
    m_reducedMotion = reduced;
    emit changed();
}

QVariantMap Theme::scale() {
    return {{"caption", 11}, {"small", 12}, {"body", 13}, {"title", 15},
            {"heading", 20}, {"pageTitle", 22}, {"display", 28}};
}

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
    const bool desktop = read.contains("background") && read.contains("foreground");
    const bool desktopLight = desktop && read.value("mode") == "light";
    // Forcing the other mode than the desktop's sets the theme's palette
    // aside for the design's neutrals, keeping only its accent.
    const bool light = m_appearance == "match" ? desktopLight : m_appearance == "light";
    const bool omarchy = desktop && light == desktopLight;
    auto palette = light ? fallbackLightPalette() : fallbackPalette();
    if (omarchy) {
        for (auto it = read.cbegin(); it != read.cend(); ++it)
            palette.insert(it.key(), it.value());
    } else if (read.contains("accent")) {
        // A theme with only an accent (or another mode) keeps the neutrals around it.
        palette.insert("accent", read.value("accent"));
    }
    if (palette != m_palette || light != m_light || omarchy != m_omarchy || m_tokens.isEmpty()) {
        m_tokens = deriveTokens(palette, light, omarchy);
        m_palette = palette;
        m_light = light;
        m_omarchy = omarchy;
        emit changed();
    }
}
