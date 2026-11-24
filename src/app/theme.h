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
//
// The interface paints with semantic tokens (ground, sidebar, raised, ink1-4,
// hairline, fill, accentInk...) derived from that palette, so every Omarchy
// theme gets the same calm hierarchy the design asks for, with text kept
// readable however low-contrast a theme is. Without Omarchy they are the
// design's own neutrals. `appearance` can force dark or light over the desktop.
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
    // "match" (follow Omarchy), "dark" or "light".
    Q_PROPERTY(QString appearance READ appearance WRITE setAppearance NOTIFY changed)
    // The type scale, in pixels: caption 11, small 12, body 13, title 15, heading 20, display 28.
    Q_PROPERTY(QVariantMap scale READ scale CONSTANT)
    Q_PROPERTY(QString ground READ ground NOTIFY changed)
    Q_PROPERTY(QString sidebar READ sidebar NOTIFY changed)
    Q_PROPERTY(QString drawer READ drawer NOTIFY changed)
    Q_PROPERTY(QString raised READ raised NOTIFY changed)
    Q_PROPERTY(QString overlay READ overlay NOTIFY changed)
    Q_PROPERTY(QString ink1 READ ink1 NOTIFY changed)
    Q_PROPERTY(QString ink2 READ ink2 NOTIFY changed)
    Q_PROPERTY(QString ink3 READ ink3 NOTIFY changed)
    Q_PROPERTY(QString ink4 READ ink4 NOTIFY changed)
    Q_PROPERTY(QString quote READ quote NOTIFY changed)
    Q_PROPERTY(QString hairline READ hairline NOTIFY changed)
    Q_PROPERTY(QString separator READ separator NOTIFY changed)
    Q_PROPERTY(QString fill READ fill NOTIFY changed)
    Q_PROPERTY(QString fillHover READ fillHover NOTIFY changed)
    Q_PROPERTY(QString fillPressed READ fillPressed NOTIFY changed)
    Q_PROPERTY(QString accentInk READ accentInk NOTIFY changed)
    Q_PROPERTY(QString accentOn READ accentOn NOTIFY changed)
    Q_PROPERTY(QString accentSoft READ accentSoft NOTIFY changed)
    Q_PROPERTY(QString accentRead READ accentRead NOTIFY changed)
    Q_PROPERTY(QString focus READ focus NOTIFY changed)
    Q_PROPERTY(QString positive READ positive NOTIFY changed)
    Q_PROPERTY(QString positiveSoft READ positiveSoft NOTIFY changed)
    Q_PROPERTY(QString positiveInk READ positiveInk NOTIFY changed)
    Q_PROPERTY(QString cloud READ cloud NOTIFY changed)
    Q_PROPERTY(QString cloudSoft READ cloudSoft NOTIFY changed)
    Q_PROPERTY(QString cloudInk READ cloudInk NOTIFY changed)
    Q_PROPERTY(QString danger READ danger NOTIFY changed)
    Q_PROPERTY(QString scrim READ scrim NOTIFY changed)
    Q_PROPERTY(QString shadow READ shadow NOTIFY changed)
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY changed)

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
    QString appearance() const { return m_appearance; }
    void setAppearance(const QString &appearance);
    static QVariantMap scale();
    QString ground() const { return m_tokens.value("ground"); }
    QString sidebar() const { return m_tokens.value("sidebar"); }
    QString drawer() const { return m_tokens.value("drawer"); }
    QString raised() const { return m_tokens.value("raised"); }
    QString overlay() const { return m_tokens.value("overlay"); }
    QString ink1() const { return m_tokens.value("ink1"); }
    QString ink2() const { return m_tokens.value("ink2"); }
    QString ink3() const { return m_tokens.value("ink3"); }
    QString ink4() const { return m_tokens.value("ink4"); }
    QString quote() const { return m_tokens.value("quote"); }
    QString hairline() const { return m_tokens.value("hairline"); }
    QString separator() const { return m_tokens.value("separator"); }
    QString fill() const { return m_tokens.value("fill"); }
    QString fillHover() const { return m_tokens.value("fillHover"); }
    QString fillPressed() const { return m_tokens.value("fillPressed"); }
    QString accentInk() const { return m_tokens.value("accentInk"); }
    QString accentOn() const;
    QString accentSoft() const { return m_tokens.value("accentSoft"); }
    QString accentRead() const { return m_tokens.value("accentRead"); }
    QString focus() const { return m_tokens.value("focus"); }
    QString positive() const { return m_tokens.value("positive"); }
    QString positiveSoft() const { return m_tokens.value("positiveSoft"); }
    QString positiveInk() const { return m_tokens.value("positiveInk"); }
    QString cloud() const { return m_tokens.value("cloud"); }
    QString cloudSoft() const { return m_tokens.value("cloudSoft"); }
    QString cloudInk() const { return m_tokens.value("cloudInk"); }
    QString danger() const { return m_tokens.value("danger"); }
    QString scrim() const { return m_tokens.value("scrim"); }
    QString shadow() const { return m_tokens.value("shadow"); }
    bool reducedMotion() const { return m_reducedMotion; }
    void setReducedMotion(bool reduced);
    QHash<QString, QString> tokens() const { return m_tokens; }

    // The semantic tokens for a palette (see the class comment).
    static QHash<QString, QString> deriveTokens(const QHash<QString, QString> &palette, bool light, bool omarchy);
    // WCAG contrast ratio between two colours, 1 to 21.
    static double contrast(const QString &a, const QString &b);
    // `a` moved toward `b` by `amount` (0 to 1).
    static QString mix(const QString &a, const QString &b, double amount);

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
    QHash<QString, QString> m_tokens;
    QString m_appearance = QStringLiteral("match");
    bool m_light = false;
    bool m_omarchy = false;
    bool m_reducedMotion = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprSocketDir;
    int m_radius = 0;
    QLocalSocket m_eventSocket;
    QByteArray m_eventBuffer;
    QPointer<QLocalSocket> m_roundingSocket;
};
