#pragma once

#include "online/opdsclient.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QString>
#include <QThreadPool>
#include <memory>

// Safe asynchronous QML image provider for OPDS cover thumbnails (`image://opdscover/...`).
// Fetches remote cover URLs through UrlGuard (IP-pinned, manual redirects, no credentials,
// max 4 MiB download cap, max 4096px / 16 MP raster decode cap, respecting each catalog's
// `allowLocalNetwork` setting) off the GUI thread.
//
// Provider registration name: "opdscover"
// URL format: "image://opdscover/<percent-encoded-http-url>?catalog=<percent-encoded-catalog-id>"
//             (or `opds.coverImageUrl(url)` from QML).
class OpdsCoverProvider : public QQuickAsyncImageProvider {
public:
    static constexpr qint64 maxCoverBytes = 4ll * 1024 * 1024;    // 4 MiB
    static constexpr int maxImageDimension = 4096;                // max width/height
    static constexpr qint64 maxImagePixels = 16ll * 1024 * 1024;  // 16 megapixels
    static constexpr int defaultTimeoutMs = 15000;

    explicit OpdsCoverProvider(const OpdsClient *client = nullptr);
    ~OpdsCoverProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    static QString imageUrl(const QString &coverUrl, const QString &catalogId = {});

private:
    struct CoverCache {
        QMutex mutex;
        QCache<QString, QImage> images{16 * 1024 * 1024};  // 16 MiB decoded image cache

        QImage get(const QString &key) {
            QMutexLocker lock(&mutex);
            const QImage *img = images.object(key);
            return img ? *img : QImage();
        }

        void insert(const QString &key, const QImage &img) {
            if (img.isNull())
                return;
            const qsizetype bytes = img.sizeInBytes();
            if (bytes <= 0 || bytes > images.maxCost())
                return;
            QMutexLocker lock(&mutex);
            images.insert(key, new QImage(img), int(bytes));
        }
    };

    std::shared_ptr<OpdsClient::LocalCatalogRegistry> m_localRegistry;
    std::shared_ptr<CoverCache> m_cache;
    QThreadPool m_pool;
};
