#include "reader/readerimageprovider.h"

#include "formats/epubbook.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <cmath>

QMutex ReaderImageProvider::s_mutex;
std::atomic<int> ReaderImageProvider::s_registeredCount{0};
QHash<QString, std::shared_ptr<EpubBook>> ReaderImageProvider::s_books;
// Bounded 32 MiB LRU cache for decoded lightbox images; evicted on book close.
QCache<QString, QImage> ReaderImageProvider::s_cache(32 * 1024 * 1024);
QHash<QString, QSet<QString>> ReaderImageProvider::s_tokenKeys;

namespace {

class CachedBookImageResponse : public QQuickImageResponse {
public:
    explicit CachedBookImageResponse(QImage image) : m_image(std::move(image)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QImage m_image;
};

}  // namespace

class BookImageResponse : public QQuickImageResponse, public QRunnable {
public:
    BookImageResponse(std::shared_ptr<EpubBook> book, QString token, QString entry, QString cacheKey,
                      QSize requestedSize)
        : m_book(std::move(book)),
          m_token(std::move(token)),
          m_entry(std::move(entry)),
          m_cacheKey(std::move(cacheKey)),
          m_requestedSize(requestedSize) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override { m_cancelled.store(true, std::memory_order_relaxed); }

    void run() override {
        Q_ASSERT(!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread());
        if (m_cancelled.load(std::memory_order_relaxed) || !m_book) {
            emit finished();
            return;
        }
        if (const QImage hit = ReaderImageProvider::cached(m_cacheKey); !hit.isNull()) {
            m_image = hit;
            emit finished();
            return;
        }
        QImage decoded = ReaderImageProvider::decodeBookImage(m_book, m_entry, m_requestedSize);
        if (!m_cancelled.load(std::memory_order_relaxed) && !decoded.isNull()) {
            ReaderImageProvider::storeCached(m_token, m_cacheKey, decoded);
            m_image = std::move(decoded);
        }
        emit finished();
    }

private:
    std::shared_ptr<EpubBook> m_book;
    QString m_token;
    QString m_entry;
    QString m_cacheKey;
    QSize m_requestedSize;
    std::atomic_bool m_cancelled{false};
    QImage m_image;
};

ReaderImageProvider::ReaderImageProvider() {
    m_pool.setMaxThreadCount(2);
    s_registeredCount.fetch_add(1, std::memory_order_relaxed);
}

ReaderImageProvider::~ReaderImageProvider() {
    s_registeredCount.fetch_sub(1, std::memory_order_relaxed);
    m_pool.waitForDone();
}

void ReaderImageProvider::add(const QString &token, const std::shared_ptr<EpubBook> &book) {
    if (token.isEmpty() || !book)
        return;
    QMutexLocker lock(&s_mutex);
    s_books.insert(token, book);
}

void ReaderImageProvider::remove(const QString &token) {
    if (token.isEmpty())
        return;
    QMutexLocker lock(&s_mutex);
    s_books.remove(token);
    const QSet<QString> keys = s_tokenKeys.take(token);
    for (const QString &key : keys)
        s_cache.remove(key);
}

bool ReaderImageProvider::isRegistered() { return s_registeredCount.load(std::memory_order_relaxed) > 0; }

QString ReaderImageProvider::imageUrl(const QString &token, const QString &entry) {
    if (token.isEmpty() || entry.isEmpty())
        return {};
    return QStringLiteral("image://bookimg/%1/%2")
        .arg(token, QString::fromUtf8(QUrl::toPercentEncoding(entry, "/")));
}

QString ReaderImageProvider::makeCacheKey(const QString &token, const QString &entry, const QSize &requestedSize) {
    const int w = requestedSize.width() > 0 ? requestedSize.width() : 0;
    const int h = requestedSize.height() > 0 ? requestedSize.height() : 0;
    return QStringLiteral("%1/%2|%3x%4").arg(token, entry).arg(w).arg(h);
}

std::shared_ptr<EpubBook> ReaderImageProvider::bookFor(const QString &token) {
    QMutexLocker lock(&s_mutex);
    return s_books.value(token);
}

QImage ReaderImageProvider::cached(const QString &key) {
    QMutexLocker lock(&s_mutex);
    const QImage *img = s_cache.object(key);
    return img ? *img : QImage();
}

void ReaderImageProvider::storeCached(const QString &token, const QString &key, const QImage &image) {
    if (image.isNull())
        return;
    QMutexLocker lock(&s_mutex);
    if (!s_books.contains(token))
        return;
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > s_cache.maxCost())
        return;
    if (s_cache.insert(key, new QImage(image), int(bytes))) {
        auto &keys = s_tokenKeys[token];
        keys.insert(key);
        if (keys.size() > s_cache.count() + 32) {
            for (auto it = keys.begin(); it != keys.end();) {
                if (!s_cache.contains(*it))
                    it = keys.erase(it);
                else
                    ++it;
            }
        }
    }
}

QImage ReaderImageProvider::decodeBookImage(const std::shared_ptr<EpubBook> &book, const QString &entry,
                                            const QSize &requestedSize) {
    if (!book || entry.isEmpty())
        return {};
    QString resolvedEntry = entry;
    QString mime, err;
    QByteArray bytes = book->resource(resolvedEntry, &mime, &err);
    if (bytes.isEmpty() && !err.isEmpty()) {
        const QString normalized = EpubBook::resolve(QString(), resolvedEntry);
        if (!normalized.isEmpty() && normalized != resolvedEntry) {
            err.clear();
            bytes = book->resource(normalized, &mime, &err);
            if (!bytes.isEmpty())
                resolvedEntry = normalized;
        }
    }
    if (bytes.isEmpty() || bytes.size() > kMaxResourceBytes)
        return {};
    if (mime.isEmpty())
        mime = EpubBook::mediaTypeForPath(resolvedEntry);
    if (!mime.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
        return {};

    QBuffer buf(&bytes);
    if (!buf.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    const QSize nativeSize = reader.size();
    const bool isSvg = (mime.compare(QLatin1String("image/svg+xml"), Qt::CaseInsensitive) == 0);

    if (nativeSize.isValid() && !nativeSize.isEmpty()) {
        QSize target = nativeSize;
        if (isSvg) {
            const int desiredMax =
                (requestedSize.width() > 0 || requestedSize.height() > 0)
                    ? qBound(512, qMax(requestedSize.width(), requestedSize.height()), 2048)
                    : 2048;
            if (target.width() < desiredMax && target.height() < desiredMax)
                target = target.scaled(desiredMax, desiredMax, Qt::KeepAspectRatio);
        }
        int maxW = kMaxDecodedDim;
        int maxH = kMaxDecodedDim;
        if (requestedSize.width() > 0)
            maxW = qMin(maxW, qMax(2048, requestedSize.width()));
        if (requestedSize.height() > 0)
            maxH = qMin(maxH, qMax(2048, requestedSize.height()));
        if (target.width() > maxW || target.height() > maxH)
            target = target.scaled(maxW, maxH, Qt::KeepAspectRatio);
        if (qint64(target.width()) * qint64(target.height()) > kMaxDecodedPixels) {
            const double scale =
                std::sqrt(double(kMaxDecodedPixels) / (double(target.width()) * double(target.height())));
            target = QSize(qMax(1, int(std::floor(target.width() * scale))),
                           qMax(1, int(std::floor(target.height() * scale))));
        }
        if (target != nativeSize && target.isValid() && !target.isEmpty())
            reader.setScaledSize(target);
    }

    QImage img = reader.read();
    if (img.isNull())
        return {};
    if (img.width() > kMaxDecodedDim || img.height() > kMaxDecodedDim ||
        qint64(img.width()) * qint64(img.height()) > kMaxDecodedPixels) {
        QSize capped = img.size().scaled(kMaxDecodedDim, kMaxDecodedDim, Qt::KeepAspectRatio);
        if (qint64(capped.width()) * qint64(capped.height()) > kMaxDecodedPixels) {
            const double scale =
                std::sqrt(double(kMaxDecodedPixels) / (double(capped.width()) * double(capped.height())));
            capped = QSize(qMax(1, int(std::floor(capped.width() * scale))),
                           qMax(1, int(std::floor(capped.height() * scale))));
        }
        img = img.scaled(capped, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return img;
}

QQuickImageResponse *ReaderImageProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    const QString pathPart = id.section(QLatin1Char('?'), 0, 0);
    const qsizetype slash = pathPart.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash + 1 >= pathPart.size())
        return new CachedBookImageResponse(QImage());

    const QString token = pathPart.left(slash);
    const QString entry = QUrl::fromPercentEncoding(pathPart.mid(slash + 1).toUtf8());
    if (token.isEmpty() || entry.isEmpty())
        return new CachedBookImageResponse(QImage());

    const QString cacheKey = makeCacheKey(token, entry, requestedSize);
    if (const QImage hit = cached(cacheKey); !hit.isNull())
        return new CachedBookImageResponse(hit);

    const std::shared_ptr<EpubBook> book = bookFor(token);
    if (!book)
        return new CachedBookImageResponse(QImage());

    auto *response = new BookImageResponse(book, token, entry, cacheKey, requestedSize);
    m_pool.start(response, 1);
    return response;
}
