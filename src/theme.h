#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

// Follows the Omarchy theme live: the accent, the whole palette (so the
// library and the "Omarchy" reading theme wear the desktop's colors), light or
// dark mode, and Hyprland's window rounding as `radius`.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(bool omarchy READ omarchy NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString surface READ surface NOTIFY changed)
    Q_PROPERTY(QString sunken READ sunken NOTIFY changed)
    Q_PROPERTY(QString deep READ deep NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QString textSecondary READ textSecondary NOTIFY changed)
    Q_PROPERTY(QString textTertiary READ textTertiary NOTIFY changed)
    Q_PROPERTY(QString border READ border NOTIFY changed)
    Q_PROPERTY(QString selection READ selection NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprSocketDir = {});
    ~Theme() override;

    QString accent() const { return color("accent"); }
    QString accentForeground() const;
    int radius() const { return m_radius; }
    bool light() const { return m_light; }
    bool omarchy() const { return m_omarchy; }
    QString background() const { return color("background"); }
    QString surface() const { return color("lighter_background"); }
    QString sunken() const { return color("dark_background"); }
    QString deep() const { return color("darker_background"); }
    QString text() const { return color("foreground"); }
    QString textSecondary() const { return color("light_foreground"); }
    QString textTertiary() const { return color("dark_foreground"); }
    QString border() const { return color("muted"); }
    QString selection() const { return color("selection"); }
    QVariantMap colors() const;

    QString color(const QString &key) const;

    // Reads `key = "#rrggbb"` lines (and `mode = "light"`) from colors.toml.
    static QHash<QString, QString> readPalette(const QString &path);
    // Kept from the template: just the accent, with the yellow fallback.
    static QString readAccent(const QString &path);
    // The palette used when Omarchy isn't installed (or dark themes omit keys).
    static QHash<QString, QString> fallbackPalette();
    // Fallback values for light themes that omit optional palette keys.
    static QHash<QString, QString> fallbackLightPalette();
    // Black or white, whichever reads on `color` (WCAG relative luminance).
    static QString readableOn(const QString &color);

signals:
    void changed();

private:
    void reload();
    void connectHyprEvents();
    void refreshRounding();

    QString m_directory;
    QHash<QString, QString> m_palette;
    bool m_light = false;
    bool m_omarchy = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprSocketDir;
    int m_radius = 0;
    QLocalSocket m_eventSocket;
    QByteArray m_eventBuffer;
    QPointer<QLocalSocket> m_roundingSocket;
};
