#pragma once

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>

// A mouse, a wheel and a keyboard for the window, driven from --run while
// debugging (DOGEAR_DEBUG): input arrives through public Qt events so release
// binaries carry no Qt private API linkage.
class PointerDriver : public QObject {
    Q_OBJECT

public:
    explicit PointerDriver(QQuickWindow *window, QObject *parent = nullptr) : QObject(parent), m_window(window) {}

    Q_INVOKABLE void press(qreal x, qreal y) { mouse(x, y, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress); }
    Q_INVOKABLE void move(qreal x, qreal y) { mouse(x, y, m_buttons, Qt::NoButton, QEvent::MouseMove); }
    Q_INVOKABLE void release(qreal x, qreal y) { mouse(x, y, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease); }
    Q_INVOKABLE void click(qreal x, qreal y) {
        const int seq = ++m_seq;
        press(x, y);
        QTimer::singleShot(60, this, [this, seq, x, y] {
            if (seq == m_seq)
                release(x, y);
        });
    }
    Q_INVOKABLE void rightClick(qreal x, qreal y) {
        const int seq = ++m_seq;
        mouse(x, y, Qt::RightButton, Qt::RightButton, QEvent::MouseButtonPress);
        QTimer::singleShot(60, this, [this, seq, x, y] {
            if (seq == m_seq)
                mouse(x, y, Qt::NoButton, Qt::RightButton, QEvent::MouseButtonRelease);
        });
    }
    // Press, move there in steps a frame apart, and let go.
    Q_INVOKABLE void drag(qreal x1, qreal y1, qreal x2, qreal y2, int steps = 16) {
        steps = qMax(1, steps);
        const int seq = ++m_seq;
        press(x1, y1);
        for (int i = 1; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            QTimer::singleShot(20 * i, this, [this, seq, x1, y1, x2, y2, t] {
                if (seq == m_seq)
                    move(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t);
            });
        }
        QTimer::singleShot(20 * steps + 40, this, [this, seq, x2, y2] {
            if (seq == m_seq)
                release(x2, y2);
        });
    }
    // A wheel turn: negative dy scrolls down (toward later pages); dx
    // sideways, as a touchpad swipe does (negative moves on).
    Q_INVOKABLE void wheel(qreal x, qreal y, int dy, int dx = 0) {
        if (!m_window)
            return;
        const QPointF at(x, y);
        QWheelEvent event(at, m_window->mapToGlobal(at), QPoint(), QPoint(dx, dy), m_buttons, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(m_window, &event);
    }
    // A key, pressed and let go: `key` is a Qt::Key, `text` what it types.
    Q_INVOKABLE void key(int key, const QString &text = {}, int modifiers = 0) {
        if (!m_window)
            return;
        const auto mods = Qt::KeyboardModifiers(modifiers);
        QKeyEvent down(QEvent::KeyPress, key, mods, text);
        QKeyEvent up(QEvent::KeyRelease, key, mods, text);
        QCoreApplication::sendEvent(m_window, &down);
        QCoreApplication::sendEvent(m_window, &up);
    }

private:
    void mouse(qreal x, qreal y, Qt::MouseButtons buttons, Qt::MouseButton button, QEvent::Type type) {
        if (!m_window)
            return;
        m_buttons = buttons;
        const QPointF at(x, y);
        QMouseEvent event(type, at, at, m_window->mapToGlobal(at), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &event);
    }

    QPointer<QQuickWindow> m_window;
    Qt::MouseButtons m_buttons = Qt::NoButton;
    int m_seq = 0;
};
