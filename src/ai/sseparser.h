#pragma once

#include <QByteArray>
#include <QList>

// Server-sent events, as they arrive in pieces from a streaming response:
// "event: name" and "data: …" lines, a blank line ending each event. Any
// provider that streams over SSE can use it.
class SseParser {
public:
    static constexpr qsizetype kMaxBufferBytes = 4 * 1024 * 1024;

    struct Event {
        QByteArray name;
        QByteArray data;
    };

    // The events completed by this chunk; partial ones wait for the next.
    QList<Event> feed(const QByteArray &chunk);
    // Whatever is left when the stream ends.
    QList<Event> finish();
    bool overflowed() const { return m_overflowed; }

private:
    void appendNormalized(const QByteArray &bytes);
    QList<Event> takeEvents(bool all);

    QByteArray m_buffer;
    QByteArray m_bomPending;
    qsizetype m_pos = 0;
    bool m_bomChecked = false;
    bool m_pendingCr = false;
    bool m_overflowed = false;
};
