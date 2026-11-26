#include "library/scanner.h"

#include "formats/epubbook.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QtConcurrentRun>

namespace {
struct Listing {
    QStringList all;
    QStringList toRead;
    QStringList folders;
};

constexpr int maxScanDepth = 32;

QString canonicalPath(const QFileInfo &info) {
    const QString resolved = info.canonicalFilePath();
    return resolved.isEmpty() ? info.absoluteFilePath() : resolved;
}

bool isSkippedDirectoryName(const QString &name) {
    return name.startsWith(QLatin1Char('.')) ||
           name == QLatin1StringView("node_modules") ||
           name == QLatin1StringView("__pycache__") ||
           name == QLatin1StringView("venv");
}

bool isUnzippedEpubBundle(const QFileInfo &info) {
    if (!info.isDir())
        return false;
    const QString name = info.fileName();
    if (name.startsWith(QLatin1Char('.')) || !name.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive))
        return false;
    return EpubBook::isUnzippedPackage(info.absoluteFilePath());
}

bool isVirtualFilesystemPath(const QString &path) {
    return path == QLatin1StringView("/proc") || path.startsWith(QLatin1StringView("/proc/")) ||
           path == QLatin1StringView("/sys") || path.startsWith(QLatin1StringView("/sys/")) ||
           path == QLatin1StringView("/dev") || path.startsWith(QLatin1StringView("/dev/"));
}

bool isPortalDocPath(const QString &path) {
    // Flatpak's xdg-document-portal FUSE mount lives at /run/user/<uid>/doc[/...].
    static constexpr QLatin1StringView prefix("/run/user/");
    if (!path.startsWith(prefix))
        return false;
    const qsizetype uidStart = prefix.size();
    const qsizetype slash = path.indexOf(QLatin1Char('/'), uidStart);
    if (slash <= uidStart)
        return false;
    for (qsizetype i = uidStart; i < slash; ++i) {
        if (!path.at(i).isDigit())
            return false;
    }
    const QStringView tail = QStringView(path).mid(slash + 1);
    return tail == QLatin1StringView("doc") || tail.startsWith(QLatin1StringView("doc/"));
}

// Book files under the folders, pruning hidden directories, dependency trees,
// virtual/portal traps, and symlink cycles before descending, and placing
// explicitly picked loose files first in the scan queue.
Listing list(const QStringList &folders, const QStringList &files, const QHash<QString, QPair<qint64, qint64>> &known,
             const std::shared_ptr<std::atomic<bool>> &cancelled) {
    Listing listing;
    QSet<QString> seen;
    auto consider = [&](const QFileInfo &info) {
        if (info.fileName().startsWith(QLatin1Char('.')))
            return;
        // Skip non-book regular files before canonicalizing to avoid unnecessary
        // realpath() syscalls over FUSE mounts such as xdg-document-portal.
        if (!info.isSymLink() && !BookFiles::isBook(info.fileName()))
            return;
        const QString path = canonicalPath(info);
        if (seen.contains(path) || !BookFiles::isBook(path))
            return;
        seen.insert(path);
        listing.all.append(path);
        const auto stored = known.constFind(path);
        if (stored == known.cend() || stored->first != info.size() ||
            stored->second != info.lastModified().toSecsSinceEpoch())
            listing.toRead.append(path);
    };

    // Process explicit loose files first so single added books appear at the
    // front of listing.toRead instead of waiting behind watched folder crawls.
    for (const QString &file : files) {
        if (*cancelled)
            break;
        const QFileInfo info(file);
        if (info.isFile() || isUnzippedEpubBundle(info))
            consider(info);
    }

    struct WalkFrame {
        QString dirPath;
        int depth = 0;
    };

    for (const QString &folder : folders) {
        if (*cancelled)
            break;
        // Skip unmounted or missing folders so unplugged drives don't mark an
        // entire library as missing on startup.
        const QFileInfo rootInfo(folder);
        if (!rootInfo.isDir())
            continue;
        listing.folders.append(folder);

        const QString rootCanonical = QDir::cleanPath(canonicalPath(rootInfo));
        if (isVirtualFilesystemPath(rootCanonical))
            continue;
        const bool rootInPortalDoc = isPortalDocPath(rootCanonical);

        QSet<QString> visitedDirs;
        visitedDirs.insert(rootCanonical);

        QList<WalkFrame> stack;
        stack.append({rootCanonical, 0});

        while (!stack.isEmpty() && !*cancelled) {
            const WalkFrame frame = stack.takeLast();
            const QDir dir(frame.dirPath);
            // Omitting QDir::Hidden avoids stating dot-entries on Unix; we also
            // check leading '.' explicitly before descending or considering files.
            const QFileInfoList entries =
                dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
            for (const QFileInfo &entry : entries) {
                if (*cancelled)
                    break;
                if (entry.isDir()) {
                    const QString name = entry.fileName();
                    if (isSkippedDirectoryName(name))
                        continue;
                    const QString entryPath = entry.absoluteFilePath();
                    if (isVirtualFilesystemPath(entryPath))
                        continue;
                    const QString subCanonical = entry.canonicalFilePath();
                    if (subCanonical.isEmpty() || isVirtualFilesystemPath(subCanonical))
                        continue;
                    if (!rootInPortalDoc && isPortalDocPath(subCanonical))
                        continue;
                    // Apple Books stores EPUBs as unzipped `.epub/` package directories.
                    // Treat any `.epub` directory containing `META-INF/container.xml` as a
                    // single book leaf without descending into its internal files.
                    if ((name.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive) ||
                         QFileInfo(subCanonical).fileName().endsWith(QLatin1String(".epub"), Qt::CaseInsensitive)) &&
                        EpubBook::isUnzippedPackage(subCanonical)) {
                        consider(QFileInfo(subCanonical));
                        continue;
                    }
                    if (frame.depth >= maxScanDepth)
                        continue;
                    if (visitedDirs.contains(subCanonical))
                        continue;
                    visitedDirs.insert(subCanonical);
                    stack.append({subCanonical, frame.depth + 1});
                } else if (entry.isFile()) {
                    consider(entry);
                }
            }
        }
    }
    return listing;
}

ScannedBook readWithCancel(const QString &path, const QString &coverDirectory,
                           const std::shared_ptr<std::atomic<bool>> &cancelled) {
    if (cancelled && *cancelled)
        return {};
    return Scanner::read(path, coverDirectory);
}

}

QString Scanner::fingerprintFor(const QString &bookPath) {
    const QFileInfo rootInfo(bookPath);
    if (rootInfo.isDir() && EpubBook::isUnzippedPackage(bookPath)) {
        const QString root = rootInfo.canonicalFilePath();
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArrayView("epub-dir\0", 9));
        QFile container(QDir(root).filePath(QStringLiteral("META-INF/container.xml")));
        if (container.open(QIODevice::ReadOnly))
            hash.addData(container.read(64 * 1024));
        QStringList rels;
        QHash<QString, qint64> sizes;
        QString firstOpf;
        QDir rootDir(root);
        QDirIterator it(root, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && rels.size() < 256) {
            const QFileInfo fi = it.nextFileInfo();
            const QString canon = fi.canonicalFilePath();
            if (canon.isEmpty() || !canon.startsWith(prefix))
                continue;
            const QString rel = QDir::fromNativeSeparators(rootDir.relativeFilePath(fi.absoluteFilePath()));
            rels.append(rel);
            sizes.insert(rel, fi.size());
        }
        rels.sort();
        for (const QString &rel : std::as_const(rels)) {
            hash.addData(rel.toUtf8());
            hash.addData(QByteArrayView("\0", 1));
            hash.addData(QByteArray::number(sizes.value(rel)));
            hash.addData(QByteArrayView("\0", 1));
            if (firstOpf.isEmpty() && rel.endsWith(QLatin1String(".opf"), Qt::CaseInsensitive))
                firstOpf = QDir(root).filePath(rel);
        }
        if (!firstOpf.isEmpty()) {
            QFile opf(firstOpf);
            if (opf.open(QIODevice::ReadOnly))
                hash.addData(opf.read(64 * 1024));
        }
        return QString::fromLatin1(hash.result().toHex());
    }
    QFile file(bookPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    constexpr qint64 chunk = 64 * 1024;
    const qint64 size = file.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray sizeBytes = QByteArray::number(size);
    hash.addData(sizeBytes);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(file.read(chunk));
    if (size > chunk * 2 && file.seek(size / 2 - chunk / 2))
        hash.addData(file.read(chunk));
    if (size > chunk && file.seek(size - chunk))
        hash.addData(file.read(chunk));
    return QString::fromLatin1(hash.result().toHex());
}

ScannedBook Scanner::read(const QString &path, const QString &coverDirectory) {
    ScannedBook book;
    // Read metadata, cover, and EPUB chapter lengths from a single archive open.
    book.info = BookFiles::read(path, Scanner::coverHeight, &book.chapterLengths);
    book.fingerprint = Scanner::fingerprintFor(book.info.path.isEmpty() ? path : book.info.path);
    if (!book.info.cover.isNull()) {
        // Saved beside its final name and renamed, so a reader of the cache
        // never sees half a JPEG. Recreate the cache dir if deleted mid-session.
        QDir().mkpath(coverDirectory);
        const QString file = Scanner::coverFileFor(book.info.path.isEmpty() ? path : book.info.path);
        QSaveFile out(QDir(coverDirectory).filePath(file));
        if (out.open(QIODevice::WriteOnly)) {
            QImageWriter writer(&out, "jpeg");
            writer.setQuality(88);
            if (writer.write(book.info.cover) && out.commit())
                book.coverFile = file;
        }
        book.info.cover = QImage();
    }
    if (book.info.format == "epub" && book.info.ok()) {
        for (const int length : std::as_const(book.chapterLengths))
            book.length += length;
    } else if (book.info.format == "pdf") {
        // About as much text as a printed book page holds.
        book.length = qint64(book.info.pages) * 1800;
    }
    return book;
}

Scanner::Scanner(const QString &coverDirectory, QObject *parent)
    : QObject(parent), m_coverDirectory(coverDirectory), m_cancelled(std::make_shared<std::atomic<bool>>(false)) {
    qRegisterMetaType<ScannedBook>();
    // Reading is mostly decompression and parsing; leave a core for the UI.
    // Do not create directories in the constructor so read-only Library opens
    // never mutate the filesystem; read() and Library::rescan() call mkpath().
    m_pool.setMaxThreadCount(qMax(1, QThread::idealThreadCount() - 1));
}

Scanner::~Scanner() {
    cancel();
    m_pool.waitForDone();
}

QString Scanner::coverFileFor(const QString &bookPath) {
    return QString::fromLatin1(QCryptographicHash::hash(bookPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(20)) +
           QStringLiteral(".jpg");
}

void Scanner::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_cancelled->store(true);
    m_queue.clear();
    m_running = false;
}

void Scanner::scan(const QStringList &folders, const QStringList &files,
                   const QHash<QString, QPair<qint64, qint64>> &known) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_running)
        cancel();
    m_cancelled = std::make_shared<std::atomic<bool>>(false);
    m_running = true;
    m_queue.clear();
    m_total = m_done = m_inFlight = 0;
    ++m_generation;
    const auto cancelled = m_cancelled;
    QFuture<Listing> future = QtConcurrent::run(&m_pool, list, folders, files, known, cancelled);
    future.then(this, [this, cancelled](const Listing &listing) {
        if (*cancelled)
            return;
        emit listed(listing.all, listing.folders);
        m_queue = listing.toRead;
        m_total = int(m_queue.size());
        emit progressChanged(0, m_total);
        if (m_queue.isEmpty()) {
            m_running = false;
            emit finished();
            return;
        }
        for (int i = 0; i < m_pool.maxThreadCount(); ++i)
            readNext();
    });
}

void Scanner::readNext() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_queue.isEmpty()) {
        if (m_inFlight == 0 && m_running) {
            m_running = false;
            emit finished();
        }
        return;
    }
    const QString path = m_queue.takeFirst();
    ++m_inFlight;
    const int generation = m_generation;
    const auto cancelled = m_cancelled;
    QtConcurrent::run(&m_pool, readWithCancel, path, m_coverDirectory, cancelled)
        .then(this, [this, generation, cancelled](const ScannedBook &book) {
            // A replaced scan's last workers report nothing and count for
            // nothing; the scan that replaced it finishes on its own.
            if (generation != m_generation || *cancelled)
                return;
            --m_inFlight;
            ++m_done;
            emit bookRead(book);
            emit progressChanged(m_done, m_total);
            readNext();
        });
}
