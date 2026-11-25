#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <memory>

class EpubBook;

// Async EPUB image provider for the reader lightbox:
//   image://bookimg/<token>/<percent-encoded-entry>
// Inflates and decodes EPUB images off the GUI thread (eliminating synchronous
// 20 MB ZIP inflates and 27 MB base64 copies on the main thread) with a bounded
// 32 MiB decoded LRU cache evicted whenever a book closes.
class ReaderImageProvider : public QQuickAsyncImageProvider {
public:
    static constexpr qsizetype kMaxResourceBytes = 20 * 1024 * 1024;
    static constexpr int kMaxDecodedDim = 4096;
    static constexpr qint64 kMaxDecodedPixels = 16LL * 1024 * 1024;

    ReaderImageProvider();
    ~ReaderImageProvider() override;

    static void add(const QString &token, const std::shared_ptr<EpubBook> &book);
    static void remove(const QString &token);
    static bool isRegistered();
    static QString imageUrl(const QString &token, const QString &entry);

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Decodes an image resource from an open EPUB (safe to call off the GUI thread).
    static QImage decodeBookImage(const std::shared_ptr<EpubBook> &book, const QString &entry,
                                  const QSize &requestedSize = {});

private:
    friend class ClothboundTests;
    friend class BookImageResponse;

    static QString makeCacheKey(const QString &token, const QString &entry, const QSize &requestedSize);
    static std::shared_ptr<EpubBook> bookFor(const QString &token);
    static QImage cached(const QString &key);
    static void storeCached(const QString &token, const QString &key, const QImage &image);

    QThreadPool m_pool;

    static QMutex s_mutex;
    static std::atomic<int> s_registeredCount;
    static QHash<QString, std::shared_ptr<EpubBook>> s_books;
    static QCache<QString, QImage> s_cache;
    static QHash<QString, QSet<QString>> s_tokenKeys;
};
