#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QWindow>

// One Dogear per library: opening it again (from the launcher, a terminal, or
// a book file) brings back the one already running, hidden in the background
// or not, as it was (the book, the voice, the highlight), and opens the book
// if one was named, rather than starting a second Dogear at the library.
class Instance : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "app.dogear.Reader")

public:
    explicit Instance(QObject *parent = nullptr) : QObject(parent) {}

    // The bus name for this library, so Dogears with different data folders
    // (a test profile, say) stay apart.
    static QString serviceName() {
        const QByteArray folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toUtf8();
        return QStringLiteral("app.dogear.Reader.p") +
               QString::fromLatin1(QCryptographicHash::hash(folder, QCryptographicHash::Sha1).toHex().left(12));
    }

    // Hands `book` (a file, or "") to the Dogear already running for this
    // library, with the launcher's activation token if it gave one; true when
    // there was one to take it.
    static bool passToRunning(const QString &book) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            return false;
        QDBusMessage call = QDBusMessage::createMethodCall(serviceName(), path(), QStringLiteral("app.dogear.Reader"),
                                                           QStringLiteral("Activate"));
        call << book << qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
        return bus.call(call, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
    }

    // Makes this the Dogear to bring back; false when another has the name.
    bool claim() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected() || !bus.registerObject(path(), this, QDBusConnection::ExportScriptableSlots))
            return false;
        if (bus.registerService(serviceName()))
            return true;
        bus.unregisterObject(path());
        return false;
    }

    void setWindow(QWindow *window) { m_window = window; }

    // Shows the window to the user: a hidden one comes back where they are,
    // and one open elsewhere (another workspace) is gone to.
    Q_INVOKABLE void bringBack(const QString &token = {}) {
        if (!m_window)
            return;
        const bool wasShown = m_window->isVisible();
        // The launcher's token lets the compositor hand the window focus.
        if (!token.isEmpty())
            qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
        m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
        // Shown again, the window is new to the compositor, which focuses it;
        // one already open needs Hyprland asked, as Omarchy's own
        // launch-or-focus does.
        if (wasShown)
            focusOnHyprland();
    }

signals:
    // Opened again, with a book to open ("" for none).
    void activated(const QString &book);

public slots:
    // Answers at once, so the Dogear opened again can leave; the work follows.
    Q_SCRIPTABLE void Activate(const QString &book, const QString &token) {
        QMetaObject::invokeMethod(
            this,
            [this, book, token] {
                bringBack(token);
                emit activated(book);
            },
            Qt::QueuedConnection);
    }

private:
    static QString path() { return QStringLiteral("/app/dogear/Reader"); }

    static void focusOnHyprland() {
        if (QGuiApplication::platformName() != QLatin1String("wayland") ||
            !qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE"))
            return;
        const auto hyprctl = [](const QStringList &arguments) {
            QProcess process;
            process.start(QStringLiteral("hyprctl"), arguments);
            if (!process.waitForFinished(1500) || process.exitCode() != 0)
                return QByteArray();
            return process.readAllStandardOutput();
        };
        QString address;
        const QJsonArray clients = QJsonDocument::fromJson(hyprctl({QStringLiteral("clients"), QStringLiteral("-j")})).array();
        for (const QJsonValue &client : clients)
            if (client[QLatin1String("pid")].toInteger() == QCoreApplication::applicationPid()) {
                address = client[QLatin1String("address")].toString();
                break;
            }
        if (address.isEmpty())
            return;
        // Newer Hyprland takes its dispatchers in Lua, older releases in the
        // classic form; Omarchy's launch-or-focus tries them in this order.
        const QByteArray answer = hyprctl(
            {QStringLiteral("dispatch"), QStringLiteral("hl.dsp.focus({ window = \"address:%1\" })").arg(address)});
        if (answer.trimmed() != "ok")
            hyprctl({QStringLiteral("dispatch"), QStringLiteral("focuswindow"), QStringLiteral("address:") + address});
    }

    QPointer<QWindow> m_window;
};
