#pragma once

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QDBusServiceWatcher;

// D-Bus wire types for org.kde.StatusNotifierItem and com.canonical.dbusmenu.
struct TrayPixmap {
    int width = 0;
    int height = 0;
    QByteArray pixels;  // ARGB32 in network byte order (big-endian)
};
using TrayPixmapList = QList<TrayPixmap>;

struct TrayToolTip {
    QString iconName;
    TrayPixmapList iconPixmap;
    QString title;
    QString description;
};

struct TrayMenuLayoutItem {
    int id = 0;
    QVariantMap properties;
    QList<QDBusVariant> children;
};

struct TrayMenuItemProps {
    int id = 0;
    QVariantMap properties;
};
using TrayMenuItemPropsList = QList<TrayMenuItemProps>;

struct TrayMenuUpdatedProps {
    int id = 0;
    QVariantMap properties;
};
using TrayMenuUpdatedPropsList = QList<TrayMenuUpdatedProps>;

struct TrayMenuRemovedProps {
    int id = 0;
    QStringList properties;
};
using TrayMenuRemovedPropsList = QList<TrayMenuRemovedProps>;

Q_DECLARE_METATYPE(TrayPixmap)
Q_DECLARE_METATYPE(TrayPixmapList)
Q_DECLARE_METATYPE(TrayToolTip)
Q_DECLARE_METATYPE(TrayMenuLayoutItem)
Q_DECLARE_METATYPE(TrayMenuItemProps)
Q_DECLARE_METATYPE(TrayMenuItemPropsList)
Q_DECLARE_METATYPE(TrayMenuUpdatedProps)
Q_DECLARE_METATYPE(TrayMenuUpdatedPropsList)
Q_DECLARE_METATYPE(TrayMenuRemovedProps)
Q_DECLARE_METATYPE(TrayMenuRemovedPropsList)

QDBusArgument &operator<<(QDBusArgument &arg, const TrayPixmap &p);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayPixmap &p);
QDBusArgument &operator<<(QDBusArgument &arg, const TrayToolTip &t);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayToolTip &t);
QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuLayoutItem &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuLayoutItem &item);
QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuItemProps &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuItemProps &item);
QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuUpdatedProps &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuUpdatedProps &item);
QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuRemovedProps &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuRemovedProps &item);

// Native Linux system tray icon over D-Bus (org.kde.StatusNotifierItem +
// com.canonical.dbusmenu) for Waybar, Hyprland, KDE, and AppIndicator hosts —
// works directly in QGuiApplication without QtWidgets or Qt.labs.platform.
class TrayIcon : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibleChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(QString tooltip READ tooltip WRITE setTooltip NOTIFY tooltipChanged)

public:
    explicit TrayIcon(QObject *parent = nullptr);
    ~TrayIcon() override;

    bool available() const { return m_available; }
    bool visible() const { return m_visible; }
    void setVisible(bool visible);
    bool playing() const { return m_playing; }
    void setPlaying(bool playing);
    QString tooltip() const { return m_tooltip; }
    void setTooltip(const QString &tooltip);

    // Helper exposed for unit tests and SNI adaptor.
    TrayMenuLayoutItem buildMenuLayout(int parentId, int recursionDepth) const;
    TrayPixmapList iconPixmaps() const;
    uint menuRevision() const { return m_menuRevision; }

    void handleMenuEvent(int id, const QString &eventId);
    void handleActivate();
    void handleSecondaryActivate();

signals:
    void availableChanged();
    void visibleChanged();
    void playingChanged();
    void tooltipChanged();
    void activated();
    void togglePlayRequested();
    void nextRequested();
    void previousRequested();
    void quitRequested();

private slots:
    void refreshAvailability();

private:
    void registerItem();
    void unregisterItem();
    void notifyMenuChanged();
    void emitSniSignal(const char *name, const QVariantList &args = {});

    QDBusServiceWatcher *m_watcher = nullptr;
    QObject *m_itemObject = nullptr;
    QObject *m_menuObject = nullptr;
    QString m_serviceName;
    QString m_tooltip = QStringLiteral("Clothbound");
    TrayPixmapList m_pixmaps;
    uint m_menuRevision = 1;
    bool m_available = false;
    bool m_visible = false;
    bool m_playing = false;
    bool m_registered = false;
};
