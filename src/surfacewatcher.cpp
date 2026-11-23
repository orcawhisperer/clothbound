#include "surfacewatcher.h"

#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWindow>

namespace {
constexpr qsizetype kMaxHyprBufferBytes = 64 * 1024;
constexpr int kMaxHyprReconnectAttempts = 5;
const QByteArray kDisableOccludedFlag = QByteArrayLiteral("--disable-backgrounding-occluded-windows");
}  // namespace

SurfaceWatcher::SurfaceWatcher(QObject *parent) : QObject(parent) {
    m_hyprReconnect.setSingleShot(true);
    connect(&m_hyprReconnect, &QTimer::timeout, this, &SurfaceWatcher::connectHyprEvents);
}

SurfaceWatcher::~SurfaceWatcher() {
    disconnectHyprEvents();
    if (m_appStateConn)
        disconnect(m_appStateConn);
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
}

QByteArray SurfaceWatcher::mergeChromiumFlags(const QByteArray &existing) {
    QList<QByteArray> tokens;
    const QByteArray trimmed = existing.trimmed();
    if (!trimmed.isEmpty()) {
        const QList<QByteArray> raw = trimmed.split(' ');
        for (const QByteArray &piece : raw) {
            const QByteArray sub = piece.simplified();
            if (sub.isEmpty())
                continue;
            const QList<QByteArray> parts = sub.split(' ');
            for (const QByteArray &token : parts) {
                if (!token.isEmpty())
                    tokens.append(token);
            }
        }
    }
    if (!tokens.contains(kDisableOccludedFlag))
        tokens.append(kDisableOccludedFlag);
    return tokens.join(' ');
}

void SurfaceWatcher::applyChromiumFlags() {
    const QByteArray merged = mergeChromiumFlags(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", merged);
}

bool SurfaceWatcher::isHyprWorkspaceEvent(const QByteArray &line) {
    const QByteArray trimmed = line.trimmed();
    return trimmed.startsWith("workspace>>") || trimmed.startsWith("workspacev2>>") ||
           trimmed.startsWith("focusedmon>>") || trimmed.startsWith("focusedmonv2>>") ||
           trimmed.startsWith("activespecial>>") || trimmed.startsWith("activespecialv2>>");
}

void SurfaceWatcher::attach(QQuickWindow *window) {
    if (m_window == window) {
        if (m_window && !m_hyprSocket)
            connectHyprEvents();
        return;
    }

    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    if (m_appStateConn) {
        disconnect(m_appStateConn);
        m_appStateConn = {};
    }
    disconnectHyprEvents();

    const bool oldExposed = m_wasExposed;
    m_window = window;
    m_pendingOnExpose = false;
    m_pendingReason.clear();

    if (!m_window) {
        m_wasExposed = false;
        m_wasActive = false;
        m_wasHidden = false;
        if (oldExposed)
            emit exposedChanged();
        return;
    }

    m_wasExposed = m_window->isExposed();
    m_wasActive = m_window->isActive();
    m_wasHidden = !m_window->isVisible() || m_window->visibility() == QWindow::Hidden ||
                  m_window->visibility() == QWindow::Minimized;

    m_window->installEventFilter(this);
    connect(m_window, &QWindow::activeChanged, this, &SurfaceWatcher::handleActiveChange);
    connect(m_window, &QWindow::visibilityChanged, this,
            [this](QWindow::Visibility v) { handleVisibilityChange(int(v)); });
    // sceneGraphInitialized / sceneGraphInvalidated are emitted on the Qt Quick
    // render thread when using the threaded render loop; marshal to GUI thread.
    connect(m_window, &QQuickWindow::sceneGraphInitialized, this, &SurfaceWatcher::handleSceneGraphInitialized,
            Qt::QueuedConnection);
    connect(m_window, &QQuickWindow::sceneGraphInvalidated, this, &SurfaceWatcher::handleSceneGraphInvalidated,
            Qt::QueuedConnection);

    if (qGuiApp)
        m_appStateConn = connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                                 &SurfaceWatcher::handleAppStateChange);

    if (m_wasExposed != oldExposed)
        emit exposedChanged();

    connectHyprEvents();
}

bool SurfaceWatcher::isExposed() const { return m_window && m_window->isExposed(); }

void SurfaceWatcher::setHyprEventSocketForTesting(const QString &socketPath) {
    m_hyprEventSocketOverride = socketPath;
    disconnectHyprEvents();
    if (m_window)
        connectHyprEvents();
}

void SurfaceWatcher::forceItemUpdate(QQuickItem *item) const {
    if (item) {
        QList<QQuickItem *> stack;
        stack.append(item);
        while (!stack.isEmpty()) {
            QQuickItem *current = stack.takeLast();
            if (!current)
                continue;
            current->polish();
            current->update();
            const QList<QQuickItem *> children = current->childItems();
            for (QQuickItem *child : children)
                stack.append(child);
        }
        if (QQuickWindow *win = item->window())
            win->update();
    } else if (m_window) {
        m_window->update();
    }
}

bool SurfaceWatcher::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::Expose) {
        const bool nowExposed = m_window->isExposed();
        const bool becameExposed = !m_wasExposed && nowExposed;
        if (nowExposed != m_wasExposed) {
            m_wasExposed = nowExposed;
            emit exposedChanged();
        }
        if (becameExposed) {
            m_pendingOnExpose = false;
            m_pendingReason.clear();
            // Queue so QQuickWindow::exposeEvent() wakes the render loop first.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    if (m_window && m_window->isVisible() && m_window->isExposed())
                        emitRepaint(QStringLiteral("expose"));
                },
                Qt::QueuedConnection);
        } else if (nowExposed && m_pendingOnExpose) {
            const QString reason = m_pendingReason.isEmpty() ? QStringLiteral("expose") : m_pendingReason;
            m_pendingOnExpose = false;
            m_pendingReason.clear();
            QMetaObject::invokeMethod(
                this,
                [this, reason] {
                    if (m_window && m_window->isVisible() && m_window->isExposed())
                        emitRepaint(reason);
                },
                Qt::QueuedConnection);
        }
    }
    return QObject::eventFilter(watched, event);
}

QString SurfaceWatcher::hyprEventSocketPath() const {
    if (!m_hyprEventSocketOverride.isEmpty())
        return m_hyprEventSocketOverride;
    if (QGuiApplication::platformName() != QLatin1String("wayland"))
        return {};
    const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature.isEmpty())
        return {};
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty()) {
        const QString socket = runtime + QStringLiteral("/hypr/") + signature + QStringLiteral("/.socket2.sock");
        if (QFileInfo::exists(socket))
            return socket;
    }
    const QString legacy = QStringLiteral("/tmp/hypr/") + signature + QStringLiteral("/.socket2.sock");
    return QFileInfo::exists(legacy) ? legacy : QString();
}

void SurfaceWatcher::disconnectHyprEvents() {
    m_hyprReconnect.stop();
    m_hyprReconnectAttempts = 0;
    m_hyprBuffer.clear();
    if (m_hyprSocket) {
        disconnect(m_hyprSocket, nullptr, this, nullptr);
        m_hyprSocket->abort();
        m_hyprSocket->deleteLater();
        m_hyprSocket = nullptr;
    }
}

void SurfaceWatcher::connectHyprEvents() {
    const QString path = hyprEventSocketPath();
    if (path.isEmpty() || !m_window)
        return;
    if (m_hyprSocket) {
        disconnect(m_hyprSocket, nullptr, this, nullptr);
        m_hyprSocket->abort();
        m_hyprSocket->deleteLater();
    }
    m_hyprBuffer.clear();
    auto *socket = new QLocalSocket(this);
    m_hyprSocket = socket;
    connect(socket, &QLocalSocket::connected, this, [this] { m_hyprReconnectAttempts = 0; });
    connect(socket, &QLocalSocket::readyRead, this, &SurfaceWatcher::handleHyprData);
    connect(socket, &QLocalSocket::disconnected, this, &SurfaceWatcher::scheduleHyprReconnect);
    connect(socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        scheduleHyprReconnect();
    });
    socket->connectToServer(path, QIODevice::ReadOnly);
}

void SurfaceWatcher::handleHyprData() {
    if (!m_hyprSocket)
        return;
    m_hyprBuffer += m_hyprSocket->readAll();
    if (m_hyprBuffer.size() > kMaxHyprBufferBytes)
        m_hyprBuffer = m_hyprBuffer.right(kMaxHyprBufferBytes);

    bool workspaceSwitched = false;
    for (;;) {
        const qsizetype nl = m_hyprBuffer.indexOf('\n');
        if (nl < 0)
            break;
        const QByteArray line = m_hyprBuffer.left(nl);
        m_hyprBuffer.remove(0, nl + 1);
        if (isHyprWorkspaceEvent(line))
            workspaceSwitched = true;
    }
    if (workspaceSwitched)
        requestRepaintIfExposed(QStringLiteral("hyprWorkspace"));
}

void SurfaceWatcher::scheduleHyprReconnect() {
    if (!m_window || m_hyprReconnect.isActive() || m_hyprReconnectAttempts >= kMaxHyprReconnectAttempts)
        return;
    const int delayMs = 500 * (1 << m_hyprReconnectAttempts);
    ++m_hyprReconnectAttempts;
    m_hyprReconnect.start(delayMs);
}

void SurfaceWatcher::handleActiveChange() {
    if (!m_window)
        return;
    const bool active = m_window->isActive();
    const bool becameActive = !m_wasActive && active;
    m_wasActive = active;
    if (becameActive)
        requestRepaintIfExposed(QStringLiteral("active"));
}

void SurfaceWatcher::handleVisibilityChange(int visibility) {
    const auto vis = static_cast<QWindow::Visibility>(visibility);
    const bool hidden = vis == QWindow::Hidden || vis == QWindow::Minimized;
    const bool becameVisible = m_wasHidden && !hidden;
    m_wasHidden = hidden;
    if (becameVisible)
        requestRepaintIfExposed(QStringLiteral("visibility"));
}

void SurfaceWatcher::handleAppStateChange(Qt::ApplicationState state) {
    if (state == Qt::ApplicationActive && m_window && m_window->isVisible())
        requestRepaintIfExposed(QStringLiteral("appActive"));
}

void SurfaceWatcher::handleSceneGraphInitialized() {
    if (!m_window)
        return;
    if (m_window->isExposed()) {
        m_pendingOnExpose = false;
        m_pendingReason.clear();
        emitRepaint(QStringLiteral("sceneGraph"));
    } else {
        m_pendingOnExpose = true;
        m_pendingReason = QStringLiteral("sceneGraph");
    }
}

void SurfaceWatcher::handleSceneGraphInvalidated() {
    m_pendingOnExpose = true;
    m_pendingReason = QStringLiteral("sceneGraph");
}

void SurfaceWatcher::requestRepaintIfExposed(const QString &reason) {
    if (!m_window || !m_window->isVisible())
        return;
    if (m_window->isExposed()) {
        m_pendingOnExpose = false;
        m_pendingReason.clear();
        emitRepaint(reason);
    } else {
        m_pendingOnExpose = true;
        m_pendingReason = reason;
    }
}

void SurfaceWatcher::emitRepaint(const QString &reason) {
    if (!m_window)
        return;
    m_window->update();
    emit repaintRequested(reason);
}
