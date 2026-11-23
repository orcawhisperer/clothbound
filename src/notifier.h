#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

// A note to the desktop's notifications (Omarchy's shell shows them), for
// what happens out of the window's sight, like reading on in the background.
class Notifier : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE void notify(const QString &summary, const QString &body) {
        QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                              QStringLiteral("/org/freedesktop/Notifications"),
                                                              QStringLiteral("org.freedesktop.Notifications"),
                                                              QStringLiteral("Notify"));
        message << QStringLiteral("Dogear") << uint(0) << QStringLiteral("dogear") << summary << body << QStringList()
                << QVariantMap() << int(-1);
        QDBusConnection::sessionBus().asyncCall(message);
    }
};
