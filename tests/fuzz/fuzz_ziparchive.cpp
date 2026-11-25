#include "formats/ziparchive.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

namespace {

constexpr size_t kMaxInputBytes = 1024 * 1024;
constexpr quint64 kMaxFuzzInflateBytes = 256ull * 1024;

struct FuzzBackingFile {
    int fd = -1;
    QString path;
    bool isMemfd = false;

    FuzzBackingFile() {
#ifdef __linux__
        fd = memfd_create("clothbound_fuzz_zip", MFD_CLOEXEC);
        if (fd >= 0) {
            isMemfd = true;
            path = QStringLiteral("/proc/self/fd/%1").arg(fd);
            return;
        }
#endif
        path = QStringLiteral("/tmp/clothbound_fuzz_zip_%1.zip").arg(qint64(getpid()));
    }

    ~FuzzBackingFile() {
        if (fd >= 0)
            close(fd);
        else if (!path.isEmpty())
            QFile::remove(path);
    }

    bool writeAll(const uint8_t *data, size_t size) {
        if (isMemfd && fd >= 0) {
            if (ftruncate(fd, 0) != 0)
                return false;
            size_t written = 0;
            while (written < size) {
                const ssize_t n = pwrite(fd, data + written, size - written, off_t(written));
                if (n <= 0)
                    return false;
                written += size_t(n);
            }
            return true;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        if (size > 0 && file.write(reinterpret_cast<const char *>(data), qint64(size)) != qint64(size))
            return false;
        file.close();
        return true;
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > kMaxInputBytes)
        return 0;

    static FuzzBackingFile backing;
    if (!backing.writeAll(data, size))
        return 0;

    ZipArchive zip(backing.path);
    zip.setMaxArchiveInflateBytes(kMaxFuzzInflateBytes);
    if (!zip.open()) {
        (void)zip.errorString();
        return 0;
    }

    const QStringList names = zip.names();
    const qsizetype limit = qMin<qsizetype>(names.size(), 8);
    for (qsizetype i = 0; i < limit; ++i) {
        const QString &name = names[i];
        (void)zip.contains(name);
        (void)zip.resolve(name.toUpper());
        (void)zip.size(name);
        (void)zip.encrypted(name);
        QString err;
        (void)zip.read(name, &err);
    }
    (void)zip.inflatedBytes();
    return 0;
}
