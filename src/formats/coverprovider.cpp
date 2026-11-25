#include "formats/coverprovider.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

QMutex CoverProvider::s_mutex;
// Cap at 24 MiB of pre-rounded cover thumbnails (~80-150 covers depending on DPR).
QCache<QString, QImage> CoverProvider::s_cache(24 * 1024 * 1024);

namespace {

QPainterPath asymmetricCoverPath(qreal w, qreal h, qreal spineR, qreal edgeR) {
    const qreal sr = std::clamp<qreal>(spineR, 0.0, std::min(w, h) * 0.5);
    const qreal er = std::clamp<qreal>(edgeR, 0.0, std::min(w, h) * 0.5);
    QPainterPath path;
    if (sr <= 0.01 && er <= 0.01) {
        path.addRect(0, 0, w, h);
        return path;
    }
    path.moveTo(sr, 0);
    path.lineTo(w - er, 0);
    if (er > 0.01)
        path.arcTo(w - 2 * er, 0, 2 * er, 2 * er, 90, -90);
    path.lineTo(w, h - er);
    if (er > 0.01)
        path.arcTo(w - 2 * er, h - 2 * er, 2 * er, 2 * er, 0, -90);
    path.lineTo(sr, h);
    if (sr > 0.01)
        path.arcTo(0, h - 2 * sr, 2 * sr, 2 * sr, 270, -90);
    path.lineTo(0, sr);
    if (sr > 0.01)
        path.arcTo(0, 0, 2 * sr, 2 * sr, 180, -90);
    path.closeSubpath();
    return path;
}

class CachedCoverResponse : public QQuickImageResponse {
public:
    explicit CachedCoverResponse(QImage image) : m_image(std::move(image)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QImage m_image;
};

}  // namespace

class CoverResponse : public QQuickImageResponse, public QRunnable {
public:
    CoverResponse(QString filePath, QSize requestedSize, qreal radiusPx, qreal spineRadiusPx, qreal boxWidthPx)
        : m_filePath(std::move(filePath)),
          m_requestedSize(requestedSize),
          m_radiusPx(radiusPx),
          m_spineRadiusPx(spineRadiusPx),
          m_boxWidthPx(boxWidthPx) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override { m_cancelled.store(true, std::memory_order_relaxed); }

    void run() override {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished();
            return;
        }
        CoverProvider::maybeScheduleCacheGc();

        const QFileInfo fi(m_filePath);
        if (m_cancelled.load(std::memory_order_relaxed) || !fi.exists() || !fi.isFile()) {
            emit finished();
            return;
        }
        const QString cacheKey = QStringLiteral("%1|%2|%3x%4|r=%5|sr=%6|bw=%7")
                                     .arg(m_filePath)
                                     .arg(fi.lastModified().toSecsSinceEpoch())
                                     .arg(m_requestedSize.width())
                                     .arg(m_requestedSize.height())
                                     .arg(qRound(m_radiusPx))
                                     .arg(qRound(m_spineRadiusPx))
                                     .arg(qRound(m_boxWidthPx));
        if (const QImage hit = CoverProvider::cached(cacheKey); !hit.isNull()) {
            m_image = hit;
            emit finished();
            return;
        }
        QImage out = CoverProvider::renderCoverImage(m_filePath, m_requestedSize, m_radiusPx, m_spineRadiusPx, m_boxWidthPx);
        if (!m_cancelled.load(std::memory_order_relaxed) && !out.isNull()) {
            CoverProvider::storeCached(cacheKey, out);
            m_image = std::move(out);
        }
        emit finished();
    }

private:
    QString m_filePath;
    QSize m_requestedSize;
    qreal m_radiusPx;
    qreal m_spineRadiusPx;
    qreal m_boxWidthPx;
    std::atomic_bool m_cancelled{false};
    QImage m_image;
};

CoverProvider::CoverProvider() = default;
CoverProvider::~CoverProvider() = default;

QImage CoverProvider::cached(const QString &key) {
    QMutexLocker lock(&s_mutex);
    const QImage *img = s_cache.object(key);
    return img ? *img : QImage();
}

void CoverProvider::storeCached(const QString &key, const QImage &image) {
    if (image.isNull())
        return;
    QMutexLocker lock(&s_mutex);
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > s_cache.maxCost())
        return;
    s_cache.insert(key, new QImage(image), int(bytes));
}

QImage CoverProvider::renderCoverImage(const QString &filePath, const QSize &requestedSize, qreal radiusPx,
                                       qreal spineRadiusPx, qreal boxWidthPx) {
    if (filePath.isEmpty())
        return {};
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QSize nativeSize = reader.size();
    if (nativeSize.isValid() && (requestedSize.width() > 0 || requestedSize.height() > 0)) {
        const int maxW = requestedSize.width() > 0 ? requestedSize.width() : 640;
        const int maxH = requestedSize.height() > 0 ? requestedSize.height() : 640;
        if (nativeSize.width() > maxW || nativeSize.height() > maxH) {
            const QSize scaled = nativeSize.scaled(maxW, maxH, Qt::KeepAspectRatio);
            if (scaled.isValid() && !scaled.isEmpty())
                reader.setScaledSize(scaled);
        }
    }
    const QImage decoded = reader.read();
    if (decoded.isNull())
        return {};

    const int w = decoded.width();
    const int h = decoded.height();
    const qreal scale = (boxWidthPx > 1.0) ? (qreal(w) / boxWidthPx) : 1.0;
    const qreal scaledR = radiusPx > 0.0 ? radiusPx * scale : 0.0;
    const qreal scaledSr = spineRadiusPx > 0.0 ? spineRadiusPx * scale : 0.0;

    QImage out(w, h, scaledR > 0.01 || scaledSr > 0.01 ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    if (out.hasAlphaChannel())
        out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, scaledR > 0.01 || scaledSr > 0.01);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (scaledR > 0.01 || scaledSr > 0.01)
        painter.setClipPath(asymmetricCoverPath(w, h, scaledSr, scaledR));
    painter.drawImage(0, 0, decoded);

    // Subtle left-edge spine crease (inset 3px 0 0 rgba(0,0,0,0.22)).
    const qreal creaseW = std::clamp<qreal>(w * 0.04, 2.0 * scale, 3.0 * scale);
    painter.fillRect(QRectF(0.0, 0.0, creaseW, qreal(h)), QColor(0, 0, 0, 56));
    painter.end();
    return out;
}

int CoverProvider::pruneCoverCacheDirectory(const QString &directory, qint64 maxBytes, int maxFiles) {
    if (directory.isEmpty())
        return 0;
    const QDir dir(directory);
    if (!dir.exists())
        return 0;

    const qint64 byteCap = qMax<qint64>(0, maxBytes);
    const qsizetype fileCap = qsizetype(qMax(0, maxFiles));

    struct Entry {
        QString path;
        qint64 size = 0;
        qint64 mtimeMs = 0;
    };
    QList<Entry> entries;
    qint64 totalBytes = 0;

    QDirIterator it(directory, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    while (it.hasNext()) {
        if (QCoreApplication::closingDown())
            return 0;
        const QFileInfo info = it.nextFileInfo();
        if (info.fileName().startsWith(QLatin1Char('.')))
            continue;
        const qint64 sz = qMax<qint64>(0, info.size());
        totalBytes += sz;
        entries.append({info.absoluteFilePath(), sz, info.lastModified().toMSecsSinceEpoch()});
    }

    if (totalBytes <= byteCap && entries.size() <= fileCap)
        return 0;

    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.mtimeMs != b.mtimeMs ? a.mtimeMs < b.mtimeMs : a.path < b.path;
    });

    int removed = 0;
    qsizetype remaining = entries.size();
    for (const Entry &entry : std::as_const(entries)) {
        if (QCoreApplication::closingDown())
            break;
        if (totalBytes <= byteCap && remaining <= fileCap)
            break;
        if (QFile::remove(entry.path)) {
            totalBytes -= entry.size;
            --remaining;
            ++removed;
        }
    }
    return removed;
}

void CoverProvider::maybeScheduleCacheGc() {
    static constexpr qint64 kGcIntervalMs = 5ll * 60 * 1000;
    static std::atomic_bool s_gcRunning{false};
    static std::atomic<qint64> s_lastGcMs{0};

    if (QCoreApplication::closingDown() || s_gcRunning.load(std::memory_order_relaxed))
        return;

    const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    const qint64 lastMs = s_lastGcMs.load(std::memory_order_relaxed);
    if (lastMs != 0 && nowMs - lastMs < kGcIntervalMs)
        return;
    if (s_gcRunning.exchange(true, std::memory_order_acq_rel))
        return;

    // Re-check after winning the single-flight guard to avoid duplicate sweeps.
    const qint64 prevMs = s_lastGcMs.load(std::memory_order_relaxed);
    if (prevMs != 0 && nowMs - prevMs < kGcIntervalMs) {
        s_gcRunning.store(false, std::memory_order_release);
        return;
    }
    s_lastGcMs.store(nowMs, std::memory_order_relaxed);

    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        s_gcRunning.store(false, std::memory_order_release);
        return;
    }
    const QString coversDir = cacheRoot + QStringLiteral("/covers");
    QThreadPool::globalInstance()->start(
        [coversDir] {
            struct Guard {
                ~Guard() { s_gcRunning.store(false, std::memory_order_release); }
            } guard;
            pruneCoverCacheDirectory(coversDir);
        },
        -1);
}

QQuickImageResponse *CoverProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // Format: "<percent-encoded-path-or-file-url>?r=<px>&sr=<px>&bw=<px>"
    const QString encodedPath = id.section(QLatin1Char('?'), 0, 0);
    const QString rawSource = QUrl::fromPercentEncoding(encodedPath.toUtf8());
    QString filePath = rawSource;
    if (rawSource.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        const QUrl u(rawSource);
        if (u.isLocalFile())
            filePath = u.toLocalFile();
    }
    if (filePath.isEmpty())
        return new CachedCoverResponse(QImage());

    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    const qreal radiusPx = query.queryItemValue(QStringLiteral("r")).toDouble();
    const qreal spineRadiusPx = query.queryItemValue(QStringLiteral("sr")).toDouble();
    const qreal boxWidthPx = query.queryItemValue(QStringLiteral("bw")).toDouble();

    // Defer QFileInfo::exists()/stat() and disk decode off the GUI thread.
    auto *response = new CoverResponse(std::move(filePath), requestedSize, radiusPx, spineRadiusPx, boxWidthPx);
    QThreadPool::globalInstance()->start(response);
    return response;
}
