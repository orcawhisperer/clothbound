#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QString>

// Decodes book cover thumbnails off the main thread and bakes Clothbound's
// asymmetric rounded book corners (3 px spine, 5 px fore-edge; 0 in sharp
// mode) plus the subtle left spine crease directly into the decoded QImage.
// Eliminates 2 GPU FBO layers and a MultiEffect shader pass per visible cover.
class CoverProvider : public QQuickAsyncImageProvider {
public:
    static constexpr qint64 kDefaultMaxCacheBytes = 256ll * 1024 * 1024;
    static constexpr int kDefaultMaxCacheFiles = 5000;

    CoverProvider();
    ~CoverProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Rounds a decoded cover image and draws the left spine crease in-place.
    static QImage renderCoverImage(const QString &filePath, const QSize &requestedSize, qreal radiusPx,
                                   qreal spineRadiusPx, qreal boxWidthPx);

    // Prunes oldest cached cover files when the directory exceeds `maxBytes` or
    // `maxFiles`; returns the number of files removed.
    static int pruneCoverCacheDirectory(const QString &directory,
                                        qint64 maxBytes = kDefaultMaxCacheBytes,
                                        int maxFiles = kDefaultMaxCacheFiles);

private:
    friend class CoverResponse;
    static QImage cached(const QString &key);
    static void storeCached(const QString &key, const QImage &image);
    static void maybeScheduleCacheGc();

    static QMutex s_mutex;
    static QCache<QString, QImage> s_cache;
};
