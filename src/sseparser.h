#pragma once

#include <QByteArray>
#include <QList>

// Server-sent events, as they arrive in pieces from a streaming response:
// "event: name" and "data: …" lines, a blank line ending each event. Any
// provider that streams over SSE can use it.
class SseParser {
public:
    struct Event {
        QByteArray name;
        QByteArray data;
    };

    // The events completed by this chunk; partial ones wait for the next.
    QList<Event> feed(const QByteArray &chunk);
    // Whatever is left when the stream ends.
    QList<Event> finish();

private:
    QList<Event> takeEvents(bool all);

    QByteArray m_buffer;
};
