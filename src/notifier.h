#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <utility>

// A note to the desktop's notifications (Omarchy's shell shows them), for
// what happens out of the window's sight, like reading on in the background.
class Notifier : public QObject {
    Q_OBJECT

public:
    explicit Notifier(QObject *parent = nullptr) : QObject(parent) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(service(), path(), interface(), QStringLiteral("ActionInvoked"), this,
                    SLOT(onActionInvoked(uint, QString)));
        bus.connect(service(), path(), interface(), QStringLiteral("ActivationToken"), this,
                    SLOT(onActivationToken(uint, QString)));
    }

    // A clickable note emits clicked() when it's clicked.
    Q_INVOKABLE void notify(const QString &summary, const QString &body, bool clickable = false) {
        QDBusMessage message = QDBusMessage::createMethodCall(service(), path(), interface(), QStringLiteral("Notify"));
        // "default" is the note's own click; servers that list actions as
        // buttons show its label.
        const QStringList actions =
            clickable ? QStringList{QStringLiteral("default"), tr("Show Dogear")} : QStringList();
        message << QStringLiteral("Dogear") << uint(0) << QStringLiteral("dogear") << summary << body << actions
                << QVariantMap() << int(-1);
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, clickable](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<uint> reply = *call;
            if (clickable && reply.isValid())
                m_clickable.insert(reply.value());
            call->deleteLater();
        });
    }

signals:
    // `token` is the activation token the desktop gave with the click, if any.
    void clicked(const QString &token);

private slots:
    void onActivationToken(uint id, const QString &token) {
        if (m_clickable.contains(id))
            m_token = token;
    }
    void onActionInvoked(uint id, const QString &action) {
        if (m_clickable.remove(id) && action == QLatin1String("default"))
            emit clicked(std::exchange(m_token, QString()));
    }

private:
    static QString service() { return QStringLiteral("org.freedesktop.Notifications"); }
    static QString path() { return QStringLiteral("/org/freedesktop/Notifications"); }
    static QString interface() { return QStringLiteral("org.freedesktop.Notifications"); }

    QSet<uint> m_clickable;
    QString m_token;
};
