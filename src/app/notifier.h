#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

// A note to the desktop's notifications (Omarchy's shell shows them), for
// what happens out of the window's sight, like reading on in the background.
class Notifier : public QObject {
    Q_OBJECT

public:
    explicit Notifier(QObject *parent = nullptr) : QObject(parent) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            return;
        bus.connect(service(), path(), interface(), QStringLiteral("ActionInvoked"), this,
                    SLOT(onActionInvoked(uint, QString)));
        bus.connect(service(), path(), interface(), QStringLiteral("ActivationToken"), this,
                    SLOT(onActivationToken(uint, QString)));
        bus.connect(service(), path(), interface(), QStringLiteral("NotificationClosed"), this,
                    SLOT(onNotificationClosed(uint, uint)));
    }

    // A clickable note emits clicked() when it's clicked.
    Q_INVOKABLE void notify(const QString &summary, const QString &body, bool clickable = false) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            return;
        QDBusMessage message = QDBusMessage::createMethodCall(service(), path(), interface(), QStringLiteral("Notify"));
        // "default" is the note's own click; servers that list actions as
        // buttons show its label.
        const QStringList actions =
            clickable ? QStringList{QStringLiteral("default"), tr("Show Clothbound")} : QStringList();
        const QVariantMap hints{{QStringLiteral("desktop-entry"), QStringLiteral("clothbound")}};
        message << QStringLiteral("Clothbound") << uint(0) << QStringLiteral("clothbound") << summary << body << actions
                << hints << int(-1);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, clickable](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<uint> reply = *call;
            if (clickable && reply.isValid())
                m_clickable.insert(reply.value());
            call->deleteLater();
        });
    }

    int pendingCount() const { return int(m_clickable.size()); }

signals:
    // `token` is the activation token the desktop gave with the click, if any.
    void clicked(const QString &token);

public slots:
    void onActivationToken(uint id, const QString &token) {
        if (m_clickable.contains(id))
            m_tokens.insert(id, token);
    }
    void onActionInvoked(uint id, const QString &action) {
        const QString token = m_tokens.take(id);
        if (m_clickable.remove(id) && action == QLatin1String("default"))
            emit clicked(token);
    }
    void onNotificationClosed(uint id, uint) {
        m_clickable.remove(id);
        m_tokens.remove(id);
    }

private:
    static QString service() { return QStringLiteral("org.freedesktop.Notifications"); }
    static QString path() { return QStringLiteral("/org/freedesktop/Notifications"); }
    static QString interface() { return QStringLiteral("org.freedesktop.Notifications"); }

    QSet<uint> m_clickable;
    QHash<uint, QString> m_tokens;
};
