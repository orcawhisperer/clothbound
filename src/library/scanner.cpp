#include "library/scanner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>
#include <QtConcurrentRun>

namespace {
struct Listing {
    QStringList all;
    QStringList toRead;
    QStringList folders;
};

QString canonicalPath(const QFileInfo &info) {
    const QString resolved = info.canonicalFilePath();
    return resolved.isEmpty() ? info.absoluteFilePath() : resolved;
}

// Book files under the folders, skipping hidden files and directories (such as
// caches, trash, and macOS AppleDouble "._*" files) and canonicalizing paths.
Listing list(const QStringList &folders, const QStringList &files, const QHash<QString, QPair<qint64, qint64>> &known,
             const std::shared_ptr<std::atomic<bool>> &cancelled) {
    Listing listing;
    QSet<QString> seen;
    auto consider = [&](const QFileInfo &info) {
        if (info.fileName().startsWith(QLatin1Char('.')))
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
    for (const QString &folder : folders) {
        if (*cancelled)
            break;
        // Skip unmounted or missing folders so unplugged drives don't mark an
        // entire library as missing on startup.
        if (!QFileInfo(folder).isDir())
            continue;
        listing.folders.append(folder);
        // Symlink loops are detected and skipped by the iterator itself.
        QDirIterator it(folder, QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        const QDir root(folder);
        while (it.hasNext() && !*cancelled) {
            const QFileInfo info = it.nextFileInfo();
            if (info.fileName().startsWith(QLatin1Char('.')))
                continue;
            const QString relative = root.relativeFilePath(info.absolutePath());
            if (relative != "." && (relative.startsWith('.') || relative.contains(QStringLiteral("/."))))
                continue;
            consider(info);
        }
    }
    for (const QString &file : files)
        if (!*cancelled && QFileInfo::exists(file))
            consider(QFileInfo(file));
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
    QDir().mkpath(m_coverDirectory);
    // Reading is mostly decompression and parsing; leave a core for the UI.
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
    m_cancelled->store(true);
    m_queue.clear();
    m_running = false;
}

void Scanner::scan(const QStringList &folders, const QStringList &files,
                   const QHash<QString, QPair<qint64, qint64>> &known) {
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
