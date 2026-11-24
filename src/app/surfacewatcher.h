#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QLocalSocket;
class QQuickItem;
class QQuickWindow;

// Watches window exposure, scene-graph lifecycle, activation, and Hyprland
// workspace transitions so reading surfaces (especially QtWebEngine on
// Wayland/Hyprland) reliably acquire a fresh compositor frame on return —
// including when tiled and unfocused.
class SurfaceWatcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool exposed READ isExposed NOTIFY exposedChanged)

public:
    explicit SurfaceWatcher(QObject *parent = nullptr);
    ~SurfaceWatcher() override;

    // Ensures Chromium keeps occluded-workspace surfaces alive without
    // duplicating flags or clobbering user-supplied QTWEBENGINE_CHROMIUM_FLAGS.
    static QByteArray mergeChromiumFlags(const QByteArray &existing);
    static void applyChromiumFlags();

    // True for Hyprland .socket2.sock event lines indicating a workspace or
    // monitor focus switch that can bring a window back on screen.
    static bool isHyprWorkspaceEvent(const QByteArray &line);

    // True for repaint reasons that correspond to an actual surface-recovery
    // event (workspace return, expose, visibility restore, scene-graph rebuild),
    // as opposed to ordinary focus changes within the same workspace.
    static bool isSurfaceNudgeReason(const QString &reason);

    void attach(QQuickWindow *window);
    bool isExposed() const;

    // Coalesced permit for the 1 px Viz LocalSurfaceId recovery nudge: returns
    // true at most once per return episode (or cooldownMs while exposed), and
    // automatically resets when the window becomes unexposed/hidden or its
    // scene graph invalidates.
    Q_INVOKABLE bool takeSurfaceNudge(const QString &reason, int cooldownMs = 500);
    Q_INVOKABLE void resetSurfaceNudgeCooldown();

    // For hermetic tests: point the Hyprland event listener at a mock socket.
    void setHyprEventSocketForTesting(const QString &socketPath);

    // Marks an item, all of its descendant QQuickItems (reaching QtWebEngine's
    // internal delegate item via public QQuickItem APIs), and its window dirty
    // so Qt Quick re-runs updatePaintNode() and re-acquires textures.
    Q_INVOKABLE void forceItemUpdate(QQuickItem *item) const;

signals:
    void exposedChanged();
    void repaintRequested(const QString &reason);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString hyprEventSocketPath() const;
    void connectHyprEvents();
    void disconnectHyprEvents();
    void handleHyprData();
    void emitCoalescedHyprRepaint();
    void scheduleHyprReconnect();
    void handleActiveChange();
    void handleVisibilityChange(int visibility);
    void handleAppStateChange(Qt::ApplicationState state);
    void handleSceneGraphInitialized();
    void handleSceneGraphInvalidated();
    void requestRepaintIfExposed(const QString &reason);
    void emitRepaint(const QString &reason);

    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_appStateConn;
    bool m_wasExposed = false;
    bool m_wasActive = false;
    bool m_wasHidden = false;
    bool m_pendingOnExpose = false;
    QString m_pendingReason;

    QString m_hyprEventSocketOverride;
    QPointer<QLocalSocket> m_hyprSocket;
    QByteArray m_hyprBuffer;
    QTimer m_hyprEmitTimer;
    QTimer m_hyprReconnect;
    int m_hyprReconnectAttempts = 0;
    QElapsedTimer m_lastNudgeTimer;
};
