#include "ziparchive.h"

#include <QMutexLocker>
#include <QStringDecoder>
#include <zlib.h>

namespace {
// The largest entry we inflate. Books are far smaller; this only stops a
// damaged or hostile archive from claiming gigabytes.
constexpr quint64 maxEntrySize = 512ull * 1024 * 1024;

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
}

ZipArchive::ZipArchive(const QString &path) : m_path(path), m_file(path) {}

bool ZipArchive::fail(const QString &message) {
    m_error = message;
    m_open = false;
    return false;
}

bool ZipArchive::open() {
    if (m_open)
        return true;
    if (!m_file.open(QIODevice::ReadOnly))
        return fail(m_file.errorString());

    // The end-of-central-directory record sits in the last 64 KiB + 22 bytes,
    // behind an optional comment; search backwards for its signature.
    const qint64 fileSize = m_file.size();
    const qint64 tailSize = qMin<qint64>(fileSize, 65535 + 22 + 20);
    m_file.seek(fileSize - tailSize);
    const QByteArray tail = m_file.read(tailSize);
    qint64 eocd = -1;
    for (qint64 i = tail.size() - 22; i >= 0; --i)
        if (u32(tail.constData() + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0)
        return fail(QStringLiteral("not a ZIP archive"));

    const char *record = tail.constData() + eocd;
    quint64 count = u16(record + 10);
    quint64 directorySize = u32(record + 12);
    quint64 directoryOffset = u32(record + 16);

    // Zip64 keeps the real values in a second record, found via a locator
    // that sits just before the classic one.
    if (count == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff) {
        if (eocd < 20 || u32(tail.constData() + eocd - 20) != 0x07064b50)
            return fail(QStringLiteral("damaged zip64 archive"));
        const quint64 recordOffset = u64(tail.constData() + eocd - 20 + 8);
        m_file.seek(qint64(recordOffset));
        const QByteArray zip64 = m_file.read(56);
        if (zip64.size() < 56 || u32(zip64.constData()) != 0x06064b50)
            return fail(QStringLiteral("damaged zip64 archive"));
        count = u64(zip64.constData() + 32);
        directorySize = u64(zip64.constData() + 40);
        directoryOffset = u64(zip64.constData() + 48);
    }
    if (directoryOffset + directorySize > quint64(fileSize) || directorySize > 256ull * 1024 * 1024)
        return fail(QStringLiteral("damaged ZIP directory"));

    m_file.seek(qint64(directoryOffset));
    const QByteArray directory = m_file.read(qint64(directorySize));
    if (quint64(directory.size()) != directorySize)
        return fail(QStringLiteral("truncated ZIP archive"));
    if (!readCentralDirectory(directory, count))
        return false;
    m_open = true;
    return true;
}

bool ZipArchive::readCentralDirectory(const QByteArray &directory, quint64 count) {
    const char *data = directory.constData();
    const qint64 size = directory.size();
    qint64 pos = 0;
    for (quint64 i = 0; i < count && pos + 46 <= size; ++i) {
        const char *header = data + pos;
        if (u32(header) != 0x02014b50)
            return fail(QStringLiteral("damaged ZIP directory"));
        Entry entry;
        entry.flags = u16(header + 8);
        entry.method = u16(header + 10);
        entry.compressed = u32(header + 20);
        entry.uncompressed = u32(header + 24);
        const int nameLength = u16(header + 28), extraLength = u16(header + 30), commentLength = u16(header + 32);
        entry.offset = u32(header + 42);
        if (pos + 46 + nameLength + extraLength + commentLength > size)
            return fail(QStringLiteral("damaged ZIP directory"));
        const QString name = decodeName(QByteArray(header + 46, nameLength));

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
        if (!m_entries.contains(name))
            m_names.append(name);
        m_entries.insert(name, entry);
        m_folded.insert(name.toLower(), name);
    }
    return true;
}

QString ZipArchive::resolve(const QString &name) const {
    if (m_entries.contains(name))
        return name;
    return m_folded.value(name.toLower());
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

    QByteArray compressed;
    {
        // The local header repeats the name and has its own extra field, so
        // the data starts after both; only the lengths are read from it.
        QMutexLocker lock(&m_mutex);
        if (!m_file.seek(qint64(entry.offset)))
            return setError(QStringLiteral("%1 could not be read").arg(name));
        const QByteArray local = m_file.read(30);
        if (local.size() < 30 || u32(local.constData()) != 0x04034b50)
            return setError(QStringLiteral("%1 is damaged").arg(name));
        const qint64 skip = u16(local.constData() + 26) + u16(local.constData() + 28);
        m_file.seek(qint64(entry.offset) + 30 + skip);
        compressed = m_file.read(qint64(entry.compressed));
    }
    if (quint64(compressed.size()) != entry.compressed)
        return setError(QStringLiteral("%1 is truncated").arg(name));
    if (entry.method == 0)
        return compressed;

    QByteArray output;
    output.resize(qsizetype(entry.uncompressed));
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return setError(QStringLiteral("%1 could not be decompressed").arg(name));
    stream.next_in = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_in = uInt(compressed.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = uInt(output.size());
    int result = inflate(&stream, Z_FINISH);
    // A header that understates the size still gets the whole entry, growing
    // the buffer up to the same cap.
    while (result == Z_BUF_ERROR && stream.avail_out == 0 && quint64(output.size()) < maxEntrySize) {
        const qsizetype used = output.size();
        output.resize(qMax<qsizetype>(used * 2, 4096));
        stream.next_out = reinterpret_cast<Bytef *>(output.data() + used);
        stream.avail_out = uInt(output.size() - used);
        result = inflate(&stream, Z_FINISH);
    }
    const qsizetype produced = qsizetype(stream.total_out);
    inflateEnd(&stream);
    if (result != Z_STREAM_END)
        return setError(QStringLiteral("%1 is damaged").arg(name));
    output.truncate(produced);
    return output;
}
