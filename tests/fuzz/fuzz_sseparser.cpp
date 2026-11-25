#include "ai/sseparser.h"

#include <QByteArray>
#include <cstddef>
#include <cstdint>

namespace {
constexpr size_t kMaxInputBytes = 256 * 1024;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > kMaxInputBytes)
        return 0;

    const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));

    // 1. Single-chunk pass.
    {
        SseParser parser;
        const auto events = parser.feed(raw);
        const auto tail = parser.finish();
        (void)events.size();
        (void)tail.size();
        (void)parser.overflowed();
    }

    // 2. Variable-sized chunked streaming pass (exercises BOM split, CRLF split, partial events).
    if (size >= 2) {
        SseParser parser;
        const size_t stride = 1 + (size_t(data[0]) % 17);
        size_t pos = 1;
        size_t stepIdx = 0;
        while (pos < size) {
            const size_t chunkLen = qMin(size - pos, 1 + ((stride + stepIdx * 7) % 31));
            const auto events = parser.feed(QByteArray(reinterpret_cast<const char *>(data + pos), qsizetype(chunkLen)));
            (void)events.size();
            pos += chunkLen;
            ++stepIdx;
        }
        const auto tail = parser.finish();
        (void)tail.size();
        (void)parser.overflowed();
    }

    // 3. 1-byte drip pass on compact inputs to hit every state transition boundary.
    if (size > 0 && size <= 512) {
        SseParser parser;
        for (size_t i = 0; i < size; ++i)
            (void)parser.feed(QByteArray(reinterpret_cast<const char *>(data + i), 1));
        (void)parser.finish();
    }

    return 0;
}
