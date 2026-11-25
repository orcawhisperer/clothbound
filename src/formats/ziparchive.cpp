#include "formats/ziparchive.h"

#include <QMutexLocker>
#include <QStringDecoder>
#include <algorithm>
#include <limits>
#include <zlib.h>

namespace {
constexpr quint64 maxEntrySize = ZipArchive::kMaxEntrySize;
constexpr quint64 maxDirectorySize = ZipArchive::kMaxDirectorySize;
constexpr quint64 maxEntryCount = ZipArchive::kMaxEntryCount;
constexpr quint64 initialInflateCap = 64ull * 1024;
constexpr quint64 maxFileOffset = quint64(std::numeric_limits<qint64>::max());

quint16 u16(const char *p) { return quint16(quint8(p[0]) | quint8(p[1]) << 8); }
quint32 u32(const char *p) { return quint32(u16(p)) | quint32(u16(p + 2)) << 16; }
quint64 u64(const char *p) { return quint64(u32(p)) | quint64(u32(p + 4)) << 32; }

// Names are UTF-8 when flagged, and in practice also when not (EPUB tools
// write UTF-8 regardless); only names that aren't valid UTF-8 fall back.
QString decodeName(const QByteArray &raw) {
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString name = utf8(raw);
    return utf8.hasError() ? QString::fromLatin1(raw) : name;
}
}  // namespace

ZipArchive::ZipArchive(const QString &path) : m_path(path), m_file(path) {}

bool ZipArchive::fail(const QString &message) {
    m_error = message;
    m_open = false;
    m_entries.clear();
    m_folded.clear();
    m_names.clear();
    if (m_file.isOpen())
        m_file.close();
    return false;
}

bool ZipArchive::open() {
    if (m_open)
        return true;
    m_entries.clear();
    m_folded.clear();
    m_names.clear();
    m_inflatedBytes.store(0, std::memory_order_relaxed);
    if (!m_file.open(QIODevice::ReadOnly))
        return fail(m_file.errorString());

    // The end-of-central-directory record sits in the last 64 KiB + 22 bytes,
    // behind an optional comment; search backwards for its signature.
    const qint64 fileSize = m_file.size();
    if (fileSize < 22)
        return fail(QStringLiteral("not a ZIP archive"));
    const qint64 tailSize = qMin<qint64>(fileSize, 65535 + 22 + 20);
    if (!m_file.seek(fileSize - tailSize))
        return fail(QStringLiteral("not a ZIP archive"));
    const QByteArray tail = m_file.read(tailSize);
    qint64 eocd = -1;
    qint64 fallbackEocd = -1;
    for (qint64 i = tail.size() - 22; i >= 0; --i) {
        if (u32(tail.constData() + i) == 0x06054b50) {
            if (fallbackEocd < 0)
                fallbackEocd = i;
            if (qint64(u16(tail.constData() + i + 20)) == tail.size() - i - 22) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd < 0)
        eocd = fallbackEocd;
    if (eocd < 0)
        return fail(QStringLiteral("not a ZIP archive"));

    const char *record = tail.constData() + eocd;
    quint64 count = u16(record + 10);
    quint64 directorySize = u32(record + 12);
    quint64 directoryOffset = u32(record + 16);

    // Zip64 keeps the real values in a second record, found via a locator
    // that sits just before the classic one.
    const quint64 uFileSize = quint64(fileSize);
    if (count == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff) {
        if (eocd < 20 || u32(tail.constData() + eocd - 20) != 0x07064b50)
            return fail(QStringLiteral("damaged zip64 archive"));
        const quint64 recordOffset = u64(tail.constData() + eocd - 20 + 8);
        if (recordOffset > uFileSize || recordOffset > maxFileOffset || uFileSize - recordOffset < 56 ||
            !m_file.seek(qint64(recordOffset)))
            return fail(QStringLiteral("damaged zip64 archive"));
        const QByteArray zip64 = m_file.read(56);
        if (zip64.size() < 56 || u32(zip64.constData()) != 0x06064b50)
            return fail(QStringLiteral("damaged zip64 archive"));
        count = u64(zip64.constData() + 32);
        directorySize = u64(zip64.constData() + 40);
        directoryOffset = u64(zip64.constData() + 48);
    }
    if (count > maxEntryCount)
        return fail(QStringLiteral("ZIP archive has too many entries"));
    if (directorySize > maxDirectorySize || directoryOffset > uFileSize || directorySize > uFileSize - directoryOffset ||
        directoryOffset > maxFileOffset)
        return fail(QStringLiteral("damaged ZIP directory"));

    if (!m_file.seek(qint64(directoryOffset)))
        return fail(QStringLiteral("damaged ZIP directory"));
    const QByteArray directory = m_file.read(qint64(directorySize));
    if (quint64(directory.size()) != directorySize)
        return fail(QStringLiteral("truncated ZIP archive"));
    if (!readCentralDirectory(directory, count, uFileSize, directoryOffset, directorySize))
        return false;
    m_open = true;
    return true;
}

bool ZipArchive::readCentralDirectory(const QByteArray &directory, quint64 count, quint64 fileSize,
                                      quint64 directoryOffset, quint64 directorySize) {
    const char *data = directory.constData();
    const qint64 size = directory.size();
    qint64 pos = 0;

    struct Span {
        quint64 start = 0;
        quint64 end = 0;
    };
    QList<Span> spans;
    spans.reserve(qsizetype(qMin<quint64>(count, maxEntryCount)));
    const quint64 dirEnd = directoryOffset + directorySize;

    for (quint64 i = 0; i < count && pos + 46 <= size; ++i) {
        if (i >= maxEntryCount)
            return fail(QStringLiteral("ZIP archive has too many entries"));
        const char *header = data + pos;
        if (u32(header) != 0x02014b50)
            return fail(QStringLiteral("damaged ZIP directory"));
        Entry entry;
        entry.flags = u16(header + 8);
        entry.method = u16(header + 10);
        entry.crc = u32(header + 16);
        entry.compressed = u32(header + 20);
        entry.uncompressed = u32(header + 24);
        const int nameLength = u16(header + 28), extraLength = u16(header + 30), commentLength = u16(header + 32);
        entry.offset = u32(header + 42);
        if (pos + 46 + nameLength + extraLength + commentLength > size)
            return fail(QStringLiteral("damaged ZIP directory"));
        QString name = decodeName(QByteArray(header + 46, nameLength));
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));

        // The zip64 extra field holds, in this order, only the values the
        // classic header marked as overflowing.
        const char *extra = header + 46 + nameLength;
        for (int e = 0; e + 4 <= extraLength;) {
            const quint16 id = u16(extra + e), length = u16(extra + e + 2);
            if (id == 0x0001) {
                int field = e + 4;
                auto next = [&](quint64 &value) {
                    if (value == 0xffffffff && field + 8 <= e + 4 + length && field + 8 <= extraLength) {
                        value = u64(extra + field);
                        field += 8;
                    }
                };
                next(entry.uncompressed);
                next(entry.compressed);
                next(entry.offset);
            }
            e += 4 + length;
        }

        pos += 46 + nameLength + extraLength + commentLength;
        if (name.isEmpty() || name.endsWith('/'))
            continue;

        // Validate entry bounds and collect compressed byte span (local header
        // is at least 30 bytes followed by entry.compressed bytes) to reject
        // overlapping ZIP bombs that alias one compressed stream across entries.
        if (entry.offset > fileSize || 30ull > fileSize - entry.offset ||
            entry.compressed > fileSize - entry.offset - 30ull)
            return fail(QStringLiteral("damaged ZIP directory"));
        const quint64 spanStart = entry.offset;
        const quint64 spanEnd = entry.offset + 30ull + entry.compressed;
        if (spanStart < dirEnd && spanEnd > directoryOffset)
            return fail(QStringLiteral("overlapping ZIP entries"));
        spans.append({spanStart, spanEnd});

        if (!m_entries.contains(name))
            m_names.append(name);
        m_entries.insert(name, entry);
        m_folded.insert(name.toLower(), name);
    }

    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    for (qsizetype i = 1; i < spans.size(); ++i) {
        if (spans[i].start < spans[i - 1].end)
            return fail(QStringLiteral("overlapping ZIP entries"));
    }
    return true;
}

QString ZipArchive::resolve(const QString &name) const {
    if (m_entries.contains(name))
        return name;
    QString normalized = name;
    if (normalized.contains(QLatin1Char('\\'))) {
        normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (m_entries.contains(normalized))
            return normalized;
    }
    return m_folded.value(normalized.toLower());
}

qint64 ZipArchive::size(const QString &name) const {
    const auto it = m_entries.constFind(resolve(name));
    return it == m_entries.cend() ? -1 : qint64(it->uncompressed);
}

bool ZipArchive::encrypted(const QString &name) const {
    const auto it = m_entries.constFind(resolve(name));
    return it != m_entries.cend() && (it->flags & 1);
}

QByteArray ZipArchive::read(const QString &name, QString *error) const {
    auto setError = [error](const QString &message) {
        if (error)
            *error = message;
        return QByteArray();
    };
    const auto it = m_entries.constFind(resolve(name));
    if (!m_open || it == m_entries.cend())
        return setError(QStringLiteral("%1 is not in the book").arg(name));
    const Entry entry = *it;
    if (entry.flags & 1)
        return setError(QStringLiteral("%1 is encrypted").arg(name));
    if (entry.method != 0 && entry.method != 8)
        return setError(QStringLiteral("%1 uses an unsupported compression method").arg(name));
    if (entry.uncompressed > maxEntrySize || entry.compressed > maxEntrySize)
        return setError(QStringLiteral("%1 is too large").arg(name));

    const quint64 already = m_inflatedBytes.load(std::memory_order_relaxed);
    if (already >= m_maxArchiveInflateBytes)
        return setError(QStringLiteral("%1 is too large (archive budget exceeded)").arg(name));
    const quint64 remainingBudget = m_maxArchiveInflateBytes - already;
    if (entry.uncompressed > remainingBudget)
        return setError(QStringLiteral("%1 is too large (archive budget exceeded)").arg(name));
    const quint64 entryBudget = qMin<quint64>(maxEntrySize, remainingBudget);

    QByteArray compressed;
    {
        // The local header repeats the name and has its own extra field, so
        // the data starts after both; only the lengths are read from it.
        QMutexLocker lock(&m_mutex);
        if (entry.offset > maxFileOffset || !m_file.seek(qint64(entry.offset)))
            return setError(QStringLiteral("%1 could not be read").arg(name));
        const QByteArray local = m_file.read(30);
        if (local.size() < 30 || u32(local.constData()) != 0x04034b50)
            return setError(QStringLiteral("%1 is damaged").arg(name));
        const quint64 skip = quint64(u16(local.constData() + 26)) + quint64(u16(local.constData() + 28));
        if (entry.offset > maxFileOffset - 30 - skip || !m_file.seek(qint64(entry.offset + 30 + skip)))
            return setError(QStringLiteral("%1 could not be read").arg(name));
        compressed = m_file.read(qint64(entry.compressed));
    }
    if (quint64(compressed.size()) != entry.compressed)
        return setError(QStringLiteral("%1 is truncated").arg(name));
    auto validCrc = [&](const QByteArray &bytes) {
        const quint32 actual = quint32(
            crc32(0, bytes.isEmpty() ? nullptr : reinterpret_cast<const Bytef *>(bytes.constData()), uInt(bytes.size())));
        return actual == entry.crc;
    };
    if (entry.method == 0) {
        if (quint64(compressed.size()) > entryBudget)
            return setError(QStringLiteral("%1 is too large (archive budget exceeded)").arg(name));
        if (!validCrc(compressed))
            return setError(QStringLiteral("%1 is damaged").arg(name));
        const quint64 prev = m_inflatedBytes.fetch_add(quint64(compressed.size()), std::memory_order_relaxed);
        if (prev > m_maxArchiveInflateBytes || quint64(compressed.size()) > m_maxArchiveInflateBytes - prev)
            return setError(QStringLiteral("%1 is too large (archive budget exceeded)").arg(name));
        return compressed;
    }

    // Start with a small buffer rather than trusting entry.uncompressed, and
    // grow strictly up to entryBudget (min of maxEntrySize and remaining archive budget).
    const quint64 initial =
        qMax<quint64>(1, qMin<quint64>(qMax<quint64>(entry.uncompressed, 4096), qMin<quint64>(initialInflateCap, entryBudget)));
    QByteArray output;
    output.resize(qsizetype(initial));
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return setError(QStringLiteral("%1 could not be decompressed").arg(name));
    stream.next_in = compressed.isEmpty() ? nullptr : reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_in = uInt(compressed.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = uInt(output.size());
    int result = inflate(&stream, Z_NO_FLUSH);
    while (result == Z_OK && stream.avail_out == 0) {
        const quint64 used = quint64(output.size());
        if (used >= entryBudget) {
            m_inflatedBytes.fetch_add(quint64(stream.total_out), std::memory_order_relaxed);
            inflateEnd(&stream);
            return setError(QStringLiteral("%1 is too large").arg(name));
        }
        const quint64 nextSize = qMin<quint64>(qMax<quint64>(used * 2, 4096), entryBudget);
        output.resize(qsizetype(nextSize));
        stream.next_out = reinterpret_cast<Bytef *>(output.data() + used);
        stream.avail_out = uInt(nextSize - used);
        result = inflate(&stream, Z_NO_FLUSH);
    }
    const qsizetype produced = qsizetype(stream.total_out);
    inflateEnd(&stream);
    const quint64 prev = m_inflatedBytes.fetch_add(quint64(produced), std::memory_order_relaxed);
    if (result != Z_STREAM_END) {
        if (quint64(produced) >= entryBudget)
            return setError(QStringLiteral("%1 is too large").arg(name));
        return setError(QStringLiteral("%1 is damaged").arg(name));
    }
    if (prev > m_maxArchiveInflateBytes || quint64(produced) > m_maxArchiveInflateBytes - prev)
        return setError(QStringLiteral("%1 is too large (archive budget exceeded)").arg(name));
    output.truncate(produced);
    if (!validCrc(output))
        return setError(QStringLiteral("%1 is damaged").arg(name));
    return output;
}
