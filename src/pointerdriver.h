#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

// A mouse, a wheel and a keyboard for the window, driven from --run while
// debugging (DOGEAR_DEBUG): input arrives as if from the system, through
// shortcuts and focus, so interaction can be checked without a display.
class PointerDriver : public QObject {
    Q_OBJECT

public:
    explicit PointerDriver(QQuickWindow *window, QObject *parent = nullptr) : QObject(parent), m_window(window) {}

    Q_INVOKABLE void press(qreal x, qreal y) { mouse(x, y, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress); }
    Q_INVOKABLE void move(qreal x, qreal y) {
        mouse(x, y, m_down ? Qt::LeftButton : Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
    }
    Q_INVOKABLE void release(qreal x, qreal y) { mouse(x, y, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease); }
    Q_INVOKABLE void click(qreal x, qreal y) {
        press(x, y);
        QTimer::singleShot(60, this, [this, x, y] { release(x, y); });
    }
    Q_INVOKABLE void rightClick(qreal x, qreal y) {
        mouse(x, y, Qt::RightButton, Qt::RightButton, QEvent::MouseButtonPress);
        QTimer::singleShot(60, this, [this, x, y] { mouse(x, y, Qt::NoButton, Qt::RightButton, QEvent::MouseButtonRelease); });
    }
    // Press, move there in steps a frame apart, and let go.
    Q_INVOKABLE void drag(qreal x1, qreal y1, qreal x2, qreal y2, int steps = 16) {
        press(x1, y1);
        for (int i = 1; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            QTimer::singleShot(20 * i, this, [this, x1, y1, x2, y2, t] { move(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); });
        }
        QTimer::singleShot(20 * steps + 40, this, [this, x2, y2] { release(x2, y2); });
    }
    // A wheel turn: negative dy scrolls down (toward later pages); dx
    // sideways, as a touchpad swipe does (negative moves on).
    Q_INVOKABLE void wheel(qreal x, qreal y, int dy, int dx = 0) {
        if (!m_window)
            return;
        const QPointF at(x, y);
        QWindowSystemInterface::handleWheelEvent(m_window, at, m_window->mapToGlobal(at), QPoint(dx, dy), QPoint(dx, dy));
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    // A key, pressed and let go: `key` is a Qt::Key, `text` what it types.
    Q_INVOKABLE void key(int key, const QString &text = {}, int modifiers = 0) {
        if (!m_window)
            return;
        const auto mods = Qt::KeyboardModifiers(modifiers);
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(m_window, QEvent::KeyPress,
                                                                                           key, mods, text);
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(m_window, QEvent::KeyRelease,
                                                                                           key, mods, text);
    }

private:
    void mouse(qreal x, qreal y, Qt::MouseButtons buttons, Qt::MouseButton button, QEvent::Type type) {
        if (!m_window)
            return;
        m_down = buttons & Qt::LeftButton;
        const QPointF at(x, y);
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            m_window, at, m_window->mapToGlobal(at), buttons, button, type);
    }

    QPointer<QQuickWindow> m_window;
    bool m_down = false;
};
