#include "app/trayicon.h"

#include <QCoreApplication>
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <atomic>

namespace {
constexpr auto kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr auto kWatcherPath = "/StatusNotifierWatcher";
constexpr auto kWatcherInterface = "org.kde.StatusNotifierWatcher";
constexpr auto kItemPath = "/StatusNotifierItem";
constexpr auto kMenuPath = "/MenuBar";

std::atomic<int> s_traySeq{0};

void registerTrayMetaTypes() {
    static const bool done = [] {
        qDBusRegisterMetaType<TrayPixmap>();
        qDBusRegisterMetaType<TrayPixmapList>();
        qDBusRegisterMetaType<TrayToolTip>();
        qDBusRegisterMetaType<TrayMenuLayoutItem>();
        qDBusRegisterMetaType<TrayMenuItemProps>();
        qDBusRegisterMetaType<TrayMenuItemPropsList>();
        qDBusRegisterMetaType<TrayMenuUpdatedProps>();
        qDBusRegisterMetaType<TrayMenuUpdatedPropsList>();
        qDBusRegisterMetaType<TrayMenuRemovedProps>();
        qDBusRegisterMetaType<TrayMenuRemovedPropsList>();
        return true;
    }();
    Q_UNUSED(done);
}

TrayPixmap renderSvgPixmap(int size) {
    QIcon icon(QStringLiteral(":/clothbound.svg"));
    QImage img = icon.pixmap(size, size).toImage().convertToFormat(QImage::Format_ARGB32);
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
        return {};
    QByteArray argb(img.width() * img.height() * 4, Qt::Uninitialized);
    auto *dst = reinterpret_cast<uchar *>(argb.data());
    for (int y = 0; y < img.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = src[x];
            *dst++ = uchar(qAlpha(px));
            *dst++ = uchar(qRed(px));
            *dst++ = uchar(qGreen(px));
            *dst++ = uchar(qBlue(px));
        }
    }
    return {img.width(), img.height(), std::move(argb)};
}

class StatusNotifierItemAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
    Q_PROPERTY(QString Category READ category)
    Q_PROPERTY(QString Id READ id)
    Q_PROPERTY(QString Title READ title)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(uint WindowId READ windowId)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(TrayPixmapList IconPixmap READ iconPixmap)
    Q_PROPERTY(QString OverlayIconName READ overlayIconName)
    Q_PROPERTY(TrayPixmapList OverlayIconPixmap READ overlayIconPixmap)
    Q_PROPERTY(QString AttentionIconName READ attentionIconName)
    Q_PROPERTY(TrayPixmapList AttentionIconPixmap READ attentionIconPixmap)
    Q_PROPERTY(QString AttentionMovieName READ attentionMovieName)
    Q_PROPERTY(TrayToolTip ToolTip READ toolTip)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu)
    Q_PROPERTY(QDBusObjectPath Menu READ menu)

public:
    explicit StatusNotifierItemAdaptor(TrayIcon *owner, QObject *parent)
        : QDBusAbstractAdaptor(parent), m_owner(owner) {}

    QString category() const { return QStringLiteral("ApplicationStatus"); }
    QString id() const { return QStringLiteral("clothbound"); }
    QString title() const { return QStringLiteral("Clothbound"); }
    QString status() const { return m_owner->visible() ? QStringLiteral("Active") : QStringLiteral("Passive"); }
    uint windowId() const { return 0; }
    QString iconName() const {
        const QString flatpakId = qEnvironmentVariable("FLATPAK_ID").trimmed();
        return flatpakId.isEmpty() ? QStringLiteral("clothbound") : flatpakId;
    }
    TrayPixmapList iconPixmap() const { return m_owner->iconPixmaps(); }
    QString overlayIconName() const { return {}; }
    TrayPixmapList overlayIconPixmap() const { return {}; }
    QString attentionIconName() const { return {}; }
    TrayPixmapList attentionIconPixmap() const { return {}; }
    QString attentionMovieName() const { return {}; }
    TrayToolTip toolTip() const { return {QStringLiteral("clothbound"), {}, QStringLiteral("Clothbound"), m_owner->tooltip()}; }
    bool itemIsMenu() const { return false; }
    QDBusObjectPath menu() const { return QDBusObjectPath(QString::fromLatin1(kMenuPath)); }

public slots:
    void Activate(int, int) { m_owner->handleActivate(); }
    void SecondaryActivate(int, int) { m_owner->handleSecondaryActivate(); }
    void ContextMenu(int, int) {}
    void Scroll(int, const QString &) {}

signals:
    void NewTitle();
    void NewIcon();
    void NewAttentionIcon();
    void NewOverlayIcon();
    void NewToolTip();
    void NewStatus(const QString &status);

private:
    TrayIcon *m_owner;
};

class DBusMenuAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")
    Q_PROPERTY(uint Version READ version)
    Q_PROPERTY(QString TextDirection READ textDirection)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(QStringList IconThemePath READ iconThemePath)

public:
    explicit DBusMenuAdaptor(TrayIcon *owner, QObject *parent)
        : QDBusAbstractAdaptor(parent), m_owner(owner) {}

    uint version() const { return 3; }
    QString textDirection() const { return QStringLiteral("ltr"); }
    QString status() const { return QStringLiteral("normal"); }
    QStringList iconThemePath() const { return {}; }

public slots:
    uint GetLayout(int parentId, int recursionDepth, const QStringList &propertyNames, TrayMenuLayoutItem &layout) {
        Q_UNUSED(propertyNames);
        layout = m_owner->buildMenuLayout(parentId, recursionDepth);
        return m_owner->menuRevision();
    }

    TrayMenuItemPropsList GetGroupProperties(const QList<int> &ids, const QStringList &propertyNames) {
        Q_UNUSED(propertyNames);
        TrayMenuItemPropsList result;
        const TrayMenuLayoutItem root = m_owner->buildMenuLayout(0, 1);
        for (const QDBusVariant &childVar : root.children) {
            const TrayMenuLayoutItem child = childVar.variant().value<TrayMenuLayoutItem>();
            if (ids.isEmpty() || ids.contains(child.id))
                result.append({child.id, child.properties});
        }
        return result;
    }

    QDBusVariant GetProperty(int id, const QString &name) {
        const TrayMenuLayoutItem item = m_owner->buildMenuLayout(id, 0);
        return QDBusVariant(item.properties.value(name));
    }

    void Event(int id, const QString &eventId, const QDBusVariant &data, uint timestamp) {
        Q_UNUSED(data);
        Q_UNUSED(timestamp);
        m_owner->handleMenuEvent(id, eventId);
    }

    bool AboutToShow(int id) {
        Q_UNUSED(id);
        return false;
    }

signals:
    void ItemsPropertiesUpdated(const TrayMenuUpdatedPropsList &updatedProps,
                                const TrayMenuRemovedPropsList &removedProps);
    void LayoutUpdated(uint revision, int parent);
    void ItemActivationRequested(int id, uint timestamp);

private:
    TrayIcon *m_owner;
};

}  // namespace

QDBusArgument &operator<<(QDBusArgument &arg, const TrayPixmap &p) {
    arg.beginStructure();
    arg << p.width << p.height << p.pixels;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayPixmap &p) {
    arg.beginStructure();
    arg >> p.width >> p.height >> p.pixels;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const TrayToolTip &t) {
    arg.beginStructure();
    arg << t.iconName << t.iconPixmap << t.title << t.description;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayToolTip &t) {
    arg.beginStructure();
    arg >> t.iconName >> t.iconPixmap >> t.title >> t.description;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuLayoutItem &item) {
    arg.beginStructure();
    arg << item.id << item.properties << item.children;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuLayoutItem &item) {
    arg.beginStructure();
    arg >> item.id >> item.properties >> item.children;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuItemProps &item) {
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuItemProps &item) {
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuUpdatedProps &item) {
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuUpdatedProps &item) {
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const TrayMenuRemovedProps &item) {
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayMenuRemovedProps &item) {
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.endStructure();
    return arg;
}
#endif

TrayIcon::TrayIcon(QObject *parent) : QObject(parent) {
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    registerTrayMetaTypes();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;

    m_watcher = new QDBusServiceWatcher(QString::fromLatin1(kWatcherService), bus,
                                        QDBusServiceWatcher::WatchForRegistration |
                                            QDBusServiceWatcher::WatchForUnregistration,
                                        this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { refreshAvailability(); });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        if (m_available) {
            m_available = false;
            emit availableChanged();
        }
    });
    bus.connect(QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
                QString::fromLatin1(kWatcherInterface), QStringLiteral("StatusNotifierHostRegistered"), this,
                SLOT(refreshAvailability()));
    refreshAvailability();
#endif
}

TrayIcon::~TrayIcon() {
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    unregisterItem();
#endif
}

TrayPixmapList TrayIcon::iconPixmaps() const { return m_pixmaps; }

void TrayIcon::setVisible(bool visible) {
    if (visible == m_visible)
        return;
    m_visible = visible;
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    if (m_visible) {
        if (m_pixmaps.isEmpty()) {
            for (const int sz : {22, 32}) {
                const TrayPixmap pm = renderSvgPixmap(sz);
                if (!pm.pixels.isEmpty())
                    m_pixmaps.append(pm);
            }
        }
        registerItem();
    } else {
        unregisterItem();
    }
#endif
    emit visibleChanged();
}

void TrayIcon::setPlaying(bool playing) {
    if (playing == m_playing)
        return;
    m_playing = playing;
    emit playingChanged();
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    notifyMenuChanged();
#else
    ++m_menuRevision;
#endif
}

void TrayIcon::setTooltip(const QString &tooltip) {
    if (tooltip == m_tooltip)
        return;
    m_tooltip = tooltip;
    emit tooltipChanged();
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    if (m_registered)
        emitSniSignal("NewToolTip");
#endif
}

TrayMenuLayoutItem TrayIcon::buildMenuLayout(int parentId, int recursionDepth) const {
    auto leaf = [](int id, const QString &label, bool separator = false) {
        TrayMenuLayoutItem item;
        item.id = id;
        if (separator) {
            item.properties.insert(QStringLiteral("type"), QStringLiteral("separator"));
        } else {
            item.properties.insert(QStringLiteral("label"), label);
            item.properties.insert(QStringLiteral("enabled"), true);
            item.properties.insert(QStringLiteral("visible"), true);
        }
        return item;
    };

    const QList<TrayMenuLayoutItem> entries{
        leaf(1, m_playing ? tr("Pause") : tr("Play")),
        leaf(2, tr("Next paragraph")),
        leaf(3, tr("Previous paragraph")),
        leaf(4, {}, true),
        leaf(5, tr("Show Clothbound")),
        leaf(6, tr("Stop and quit")),
    };

    if (parentId != 0) {
        for (const TrayMenuLayoutItem &entry : entries)
            if (entry.id == parentId)
                return entry;
        return {parentId, {}, {}};
    }

    TrayMenuLayoutItem root;
    root.id = 0;
    root.properties.insert(QStringLiteral("children-display"), QStringLiteral("submenu"));
    if (recursionDepth != 0) {
        for (const TrayMenuLayoutItem &entry : entries)
            root.children.append(TrayMenuVariant(QVariant::fromValue(entry)));
    }
    return root;
}

void TrayIcon::handleMenuEvent(int id, const QString &eventId) {
    if (eventId != QLatin1String("clicked"))
        return;
    switch (id) {
    case 1:
        emit togglePlayRequested();
        break;
    case 2:
        emit nextRequested();
        break;
    case 3:
        emit previousRequested();
        break;
    case 5:
        emit activated();
        break;
    case 6:
        emit quitRequested();
        break;
    default:
        break;
    }
}

void TrayIcon::handleActivate() { emit activated(); }

void TrayIcon::handleSecondaryActivate() { emit togglePlayRequested(); }

void TrayIcon::refreshAvailability() {
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    msg << QString::fromLatin1(kWatcherInterface) << QStringLiteral("IsStatusNotifierHostRegistered");
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        const QDBusPendingReply<QDBusVariant> reply = *call;
        call->deleteLater();
        const bool nowAvailable = reply.isValid() && reply.value().variant().toBool();
        if (nowAvailable != m_available) {
            m_available = nowAvailable;
            emit availableChanged();
        }
        if (m_available && m_visible)
            registerItem();
    });
#endif
}

#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
void TrayIcon::registerItem() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    if (!m_registered) {
        const QString flatpakId = qEnvironmentVariable("FLATPAK_ID").trimmed();
        m_serviceName = flatpakId.isEmpty()
                            ? QStringLiteral("org.kde.StatusNotifierItem-%1-%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(++s_traySeq)
                            : QStringLiteral("%1.StatusNotifierItem_%2_%3")
                                  .arg(flatpakId)
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(++s_traySeq);
        m_itemObject = new QObject(this);
        new StatusNotifierItemAdaptor(this, m_itemObject);
        m_menuObject = new QObject(this);
        new DBusMenuAdaptor(this, m_menuObject);
        const bool serviceOk = bus.registerService(m_serviceName);
        const bool itemOk = serviceOk && bus.registerObject(QString::fromLatin1(kItemPath), m_itemObject);
        const bool menuOk = itemOk && bus.registerObject(QString::fromLatin1(kMenuPath), m_menuObject);
        if (!menuOk) {
            if (itemOk)
                bus.unregisterObject(QString::fromLatin1(kItemPath));
            if (serviceOk)
                bus.unregisterService(m_serviceName);
            m_serviceName.clear();
            unregisterItem();
            return;
        }
        m_registered = true;
    }
    QDBusMessage reg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
        QString::fromLatin1(kWatcherInterface), QStringLiteral("RegisterStatusNotifierItem"));
    reg << m_serviceName;
    bus.asyncCall(reg);
}

void TrayIcon::unregisterItem() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        if (m_registered) {
            bus.unregisterObject(QString::fromLatin1(kMenuPath));
            bus.unregisterObject(QString::fromLatin1(kItemPath));
            if (!m_serviceName.isEmpty())
                bus.unregisterService(m_serviceName);
        } else if (!m_serviceName.isEmpty()) {
            if (m_menuObject && bus.objectRegisteredAt(QString::fromLatin1(kMenuPath)) == m_menuObject)
                bus.unregisterObject(QString::fromLatin1(kMenuPath));
            if (m_itemObject && bus.objectRegisteredAt(QString::fromLatin1(kItemPath)) == m_itemObject)
                bus.unregisterObject(QString::fromLatin1(kItemPath));
            bus.unregisterService(m_serviceName);
        }
    }
    m_registered = false;
    m_serviceName.clear();
    delete m_menuObject;
    m_menuObject = nullptr;
    delete m_itemObject;
    m_itemObject = nullptr;
}

void TrayIcon::notifyMenuChanged() {
    ++m_menuRevision;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!m_registered || !bus.isConnected())
        return;
    QDBusMessage sig = QDBusMessage::createSignal(QString::fromLatin1(kMenuPath),
                                                  QStringLiteral("com.canonical.dbusmenu"),
                                                  QStringLiteral("LayoutUpdated"));
    sig << m_menuRevision << int(0);
    bus.send(sig);
}

void TrayIcon::emitSniSignal(const char *name, const QVariantList &args) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!m_registered || !bus.isConnected())
        return;
    QDBusMessage sig = QDBusMessage::createSignal(QString::fromLatin1(kItemPath),
                                                  QStringLiteral("org.kde.StatusNotifierItem"),
                                                  QString::fromLatin1(name));
    if (!args.isEmpty())
        sig.setArguments(args);
    bus.send(sig);
}

#include "trayicon.moc"
#endif
