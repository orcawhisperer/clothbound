#include "ai/sseparser.h"

QList<SseParser::Event> SseParser::feed(const QByteArray &chunk) {
    if (!m_started && !chunk.isEmpty()) {
        m_started = true;
        if (chunk.startsWith("\xEF\xBB\xBF"))
            m_buffer += chunk.mid(3);
        else
            m_buffer += chunk;
    } else {
        m_buffer += chunk;
    }
    return takeEvents(false);
}

QList<SseParser::Event> SseParser::finish() { return takeEvents(true); }

QList<SseParser::Event> SseParser::takeEvents(bool all) {
    if (m_buffer.contains('\r')) {
        m_buffer.replace("\r\n", "\n");
        if (all) {
            m_buffer.replace('\r', '\n');
        } else {
            // Preserve a single trailing '\r' in case the next chunk begins with '\n'
            // (a chunk-split CRLF must not become two newlines / a false event boundary).
            const bool trailingCr = m_buffer.endsWith('\r');
            if (trailingCr)
                m_buffer.chop(1);
            if (m_buffer.contains('\r'))
                m_buffer.replace('\r', '\n');
            if (trailingCr)
                m_buffer.append('\r');
        }
    }
    QList<Event> events;
    while (true) {
        qsizetype end = m_buffer.indexOf("\n\n");
        if (end < 0) {
            if (!all || m_buffer.trimmed().isEmpty())
                break;
            end = m_buffer.size();
        }
        const QByteArray block = m_buffer.left(end);
        m_buffer.remove(0, qMin(end + 2, m_buffer.size()));
        Event event;
        bool named = false, hasData = false;
        for (const QByteArray &line : block.split('\n')) {
            if (line.isEmpty() || line.startsWith(':'))
                continue;
            const qsizetype colon = line.indexOf(':');
            const QByteArray field = colon < 0 ? line : line.left(colon);
            QByteArray value = colon < 0 ? QByteArray() : line.mid(colon + 1);
            if (value.startsWith(' '))
                value.remove(0, 1);
            if (field == "event") {
                event.name = value;
                named = true;
            } else if (field == "data") {
                // Several data lines are one value, joined by newlines.
                event.data = hasData ? event.data + '\n' + value : value;
                hasData = true;
            }
        }
        if (named || hasData)
            events.append(event);
        if (m_buffer.isEmpty())
            break;
    }
    return events;
}
