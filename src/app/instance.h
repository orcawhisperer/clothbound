#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QWindow>
#include <functional>
#include <memory>

// One Clothbound per library: opening it again (from the launcher, a terminal, or
// a book file) brings back the one already running, hidden in the background
// or not, as it was (the book, the voice, the highlight), and opens the book
// if one was named, rather than starting a second Clothbound at the library.
class Instance : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "app.clothbound.Reader")

public:
    explicit Instance(QObject *parent = nullptr) : QObject(parent) {}

    ~Instance() override {
        if (m_dbusRegistered) {
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (bus.isConnected()) {
                bus.unregisterObject(path());
                bus.unregisterService(m_registeredService);
            }
            m_dbusRegistered = false;
            m_registeredService.clear();
        }
        if (m_lock) {
            m_lock->unlock();
            m_lock.reset();
        }
    }

    // The bus name for this library, so Clothbound instances with different data folders
    // (a test profile, say) stay apart.
    static QString serviceName() {
        const QByteArray folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toUtf8();
        return QStringLiteral("app.clothbound.Reader.p") +
               QString::fromLatin1(QCryptographicHash::hash(folder, QCryptographicHash::Sha1).toHex().left(12));
    }

    // Filesystem lock path for this library, providing single-instance mutual
    // exclusion even when session D-Bus is unavailable or the primary is busy.
    static QString lockFilePath() {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!dataDir.isEmpty())
            return dataDir + QStringLiteral("/.instance.lock");
        const QByteArray digest = QCryptographicHash::hash(serviceName().toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
        return QDir::tempPath() + QStringLiteral("/clothbound-instance-") + QString::fromLatin1(digest) +
               QStringLiteral(".lock");
    }

    // Hands `book` (a file, or "") to the Clothbound already running for this
    // library, with the launcher's activation token if it gave one; true when
    // there was one to take it.
    static bool passToRunning(const QString &book) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            return false;
        QDBusMessage call = QDBusMessage::createMethodCall(serviceName(), path(), QStringLiteral("app.clothbound.Reader"),
                                                           QStringLiteral("Activate"));
        call << book << qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
        return bus.call(call, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
    }

    // Makes this the Clothbound to bring back; false when another GUI instance
    // already holds the library lock or D-Bus name.
    bool claim() {
        if (m_lock && m_lock->isLocked())
            return true;

        const QString lockPath = lockFilePath();
        QDir().mkpath(QFileInfo(lockPath).absolutePath());
        auto lock = std::make_unique<QLockFile>(lockPath);
        // Never time out a live GUI session; QLockFile still cleans up stale
        // locks left behind by a crashed process via PID liveness check.
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0))
            return false;

        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.isConnected()) {
            const QString service = serviceName();
            if (!bus.registerObject(path(), this, QDBusConnection::ExportScriptableSlots)) {
                lock->unlock();
                return false;
            }
            if (!bus.registerService(service)) {
                bus.unregisterObject(path());
                lock->unlock();
                return false;
            }
            m_dbusRegistered = true;
            m_registeredService = service;
        }
        m_lock = std::move(lock);
        return true;
    }

    void setWindow(QWindow *window) { m_window = window; }
    void setHyprSocketForTesting(const QString &socketPath) { m_hyprSocketOverride = socketPath; }

    // Shows the window to the user: a hidden one comes back where they are,
    // and one open elsewhere (another workspace) is gone to.
    Q_INVOKABLE void bringBack(const QString &token = {}) {
        if (!m_window)
            return;
        const bool wasShown = m_window->isVisible();
        const bool applyWaylandToken = !token.isEmpty() && token.size() <= 4096 && !token.contains(QChar(0)) &&
                                       QGuiApplication::platformName() == QLatin1String("wayland");
        if (applyWaylandToken)
            qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
        m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
        if (applyWaylandToken)
            qunsetenv("XDG_ACTIVATION_TOKEN");
        // Shown again, the window is new to the compositor, which focuses it;
        // one already open needs Hyprland asked asynchronously over its IPC socket.
        if (wasShown)
            focusOnHyprland();
    }

signals:
    // Opened again, with a book to open ("" for none).
    void activated(const QString &book);

public slots:
    // Answers at once, so the Clothbound opened again can leave; the work follows.
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
    static QString path() { return QStringLiteral("/app/clothbound/Reader"); }

    QString hyprSocketPath() const {
        if (!m_hyprSocketOverride.isEmpty())
            return m_hyprSocketOverride;
        if (QGuiApplication::platformName() != QLatin1String("wayland"))
            return {};
        const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
        if (signature.isEmpty())
            return {};
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!runtime.isEmpty()) {
            const QString socket = runtime + QStringLiteral("/hypr/") + signature + QStringLiteral("/.socket.sock");
            if (QFileInfo::exists(socket))
                return socket;
        }
        const QString legacy = QStringLiteral("/tmp/hypr/") + signature + QStringLiteral("/.socket.sock");
        return QFileInfo::exists(legacy) ? legacy : QString();
    }

    void sendHyprCommand(const QString &socketPath, const QByteArray &command,
                         std::function<void(const QByteArray &)> done) {
        auto *socket = new QLocalSocket(this);
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        timeout->setInterval(1500);
        auto finish = [socket, buffer, handled, timeout, done = std::move(done)]() {
            if (*handled)
                return;
            *handled = true;
            timeout->stop();
            const QByteArray reply = *buffer;
            socket->deleteLater();
            if (done)
                done(reply);
        };
        connect(timeout, &QTimer::timeout, socket, [socket, finish] {
            socket->abort();
            finish();
        });
        connect(socket, &QLocalSocket::connected, socket, [socket, command] {
            socket->write(command);
            socket->flush();
        });
        connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer] { *buffer += socket->readAll(); });
        connect(socket, &QLocalSocket::disconnected, this, finish);
        connect(socket, &QLocalSocket::errorOccurred, this, [socket, finish](QLocalSocket::LocalSocketError err) {
            if (err == QLocalSocket::PeerClosedError)
                return;
            socket->abort();
            finish();
        });
        timeout->start();
        socket->connectToServer(socketPath);
    }

    void focusOnHyprland() {
        const QString socketPath = hyprSocketPath();
        if (socketPath.isEmpty())
            return;
        sendHyprCommand(socketPath, QByteArrayLiteral("j/clients"), [this, socketPath](const QByteArray &reply) {
            QString address;
            const QJsonArray clients = QJsonDocument::fromJson(reply).array();
            for (const QJsonValue &client : clients) {
                if (client[QLatin1String("pid")].toInteger() == QCoreApplication::applicationPid()) {
                    address = client[QLatin1String("address")].toString();
                    break;
                }
            }
            static const QRegularExpression validAddr(QStringLiteral("^0x[0-9a-fA-F]+$"));
            if (!validAddr.match(address).hasMatch())
                return;
            // Newer Hyprland takes its dispatchers in Lua, older releases in the
            // classic form; try Lua first and fall back to focuswindow asynchronously.
            const QByteArray lua =
                QByteArrayLiteral("dispatch hl.dsp.focus({ window = \"address:") + address.toUtf8() + QByteArrayLiteral("\" })");
            sendHyprCommand(socketPath, lua, [this, socketPath, address](const QByteArray &answer) {
                if (answer.trimmed() != "ok")
                    sendHyprCommand(socketPath, QByteArrayLiteral("dispatch focuswindow address:") + address.toUtf8(), {});
            });
        });
    }

    QPointer<QWindow> m_window;
    QString m_hyprSocketOverride;
    std::unique_ptr<QLockFile> m_lock;
    bool m_dbusRegistered = false;
    QString m_registeredService;
};
