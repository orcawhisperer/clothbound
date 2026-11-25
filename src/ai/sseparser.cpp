#include "ai/sseparser.h"

namespace {
constexpr char kUtf8Bom[3] = {'\xEF', '\xBB', '\xBF'};

bool isBomPrefix(const QByteArray &bytes) {
    if (bytes.size() > 3)
        return false;
    for (qsizetype i = 0; i < bytes.size(); ++i)
        if (bytes[i] != kUtf8Bom[i])
            return false;
    return true;
}
}  // namespace

void SseParser::appendNormalized(const QByteArray &bytes) {
    if (bytes.isEmpty() || m_overflowed)
        return;
    const qsizetype unread = m_buffer.size() - m_pos;
    if (unread + bytes.size() > kMaxBufferBytes) {
        m_overflowed = true;
        m_buffer.clear();
        m_pos = 0;
        m_pendingCr = false;
        return;
    }
    m_buffer.reserve(m_buffer.size() + bytes.size());
    for (char c : bytes) {
        if (m_pendingCr) {
            m_pendingCr = false;
            m_buffer.append('\n');
            if (c == '\n')
                continue;
        }
        if (c == '\r') {
            m_pendingCr = true;
        } else {
            m_buffer.append(c);
        }
    }
}

QList<SseParser::Event> SseParser::feed(const QByteArray &chunk) {
    if (m_overflowed || chunk.isEmpty())
        return {};
    if (!m_bomChecked) {
        const qsizetype need = 3 - m_bomPending.size();
        const qsizetype take = qMin(need, chunk.size());
        m_bomPending.append(chunk.constData(), take);
        if (m_bomPending.size() < 3 && isBomPrefix(m_bomPending))
            return {};
        m_bomChecked = true;
        if (m_bomPending.size() == 3 && isBomPrefix(m_bomPending)) {
            m_bomPending.clear();
        } else {
            const QByteArray lead = std::move(m_bomPending);
            m_bomPending.clear();
            appendNormalized(lead);
        }
        if (take < chunk.size())
            appendNormalized(chunk.mid(take));
    } else {
        appendNormalized(chunk);
    }
    return takeEvents(false);
}

QList<SseParser::Event> SseParser::finish() {
    if (m_overflowed)
        return {};
    if (!m_bomChecked) {
        m_bomChecked = true;
        if (!m_bomPending.isEmpty()) {
            const QByteArray lead = std::move(m_bomPending);
            m_bomPending.clear();
            appendNormalized(lead);
        }
    }
    if (m_pendingCr) {
        m_pendingCr = false;
        if (m_buffer.size() - m_pos < kMaxBufferBytes)
            m_buffer.append('\n');
        else
            m_overflowed = true;
    }
    if (m_overflowed)
        return {};
    return takeEvents(true);
}

QList<SseParser::Event> SseParser::takeEvents(bool all) {
    QList<Event> events;
    while (m_pos < m_buffer.size()) {
        qsizetype end = m_buffer.indexOf("\n\n", m_pos);
        qsizetype nextPos = 0;
        if (end < 0) {
            if (!all)
                break;
            end = m_buffer.size();
            nextPos = end;
        } else {
            nextPos = end + 2;
        }
        const QByteArrayView block(m_buffer.constData() + m_pos, end - m_pos);
        m_pos = nextPos;
        if (block.trimmed().isEmpty())
            continue;

        Event event;
        bool named = false;
        bool hasData = false;
        qsizetype lineStart = 0;
        while (lineStart <= block.size()) {
            const qsizetype nl = block.indexOf('\n', lineStart);
            const qsizetype lineEnd = nl < 0 ? block.size() : nl;
            const QByteArrayView line = block.sliced(lineStart, lineEnd - lineStart);
            lineStart = nl < 0 ? block.size() + 1 : nl + 1;
            if (line.isEmpty() || line.startsWith(':'))
                continue;
            const qsizetype colon = line.indexOf(':');
            const QByteArrayView field = colon < 0 ? line : line.sliced(0, colon);
            QByteArrayView value = colon < 0 ? QByteArrayView() : line.sliced(colon + 1);
            if (value.startsWith(' '))
                value = value.sliced(1);
            if (field == "event") {
                event.name = value.toByteArray();
                named = true;
            } else if (field == "data") {
                // Several data lines are one value, joined by newlines.
                if (hasData) {
                    event.data.append('\n');
                    event.data.append(value.constData(), value.size());
                } else {
                    event.data = value.toByteArray();
                    hasData = true;
                }
            }
        }
        if (named || hasData)
            events.append(std::move(event));
    }
    if (m_pos >= m_buffer.size()) {
        m_buffer.clear();
        m_pos = 0;
    } else if (m_pos >= 65536 || (m_pos >= 4096 && m_pos * 2 >= m_buffer.size())) {
        m_buffer.remove(0, m_pos);
        m_pos = 0;
    }
    return events;
}
