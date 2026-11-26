#include "formats/pdfpages.h"

#include "formats/pdfbook.h"
#include "formats/pdfrecolor.h"
#include "formats/tsanhandoff.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QRunnable>
#include <QThread>
#include <QUrlQuery>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>

QMutex PdfPages::s_mutex;
PdfPages *PdfPages::s_instance = nullptr;
std::atomic<quint64> PdfPages::s_prefetchGen{0};
QHash<QString, QString> PdfPages::s_paths;
// Keep up to ~128 MiB of rendered base pages in an LRU cache so page turns back
// and forth are instantaneous without re-running PDFium + recolor().
QCache<QString, QImage> PdfPages::s_cache(128 * 1024 * 1024);
// Dedicated 48 MiB LRU cache for high-zoom viewport tiles (each capped ≤ 16 MiB)
// so zooming/panning never evicts base pages from s_cache or s_rawCache.
QCache<QString, QImage> PdfPages::s_tileCache(48 * 1024 * 1024);
// Keep up to ~32 MiB of pre-recolor PDFium renders so switching reading themes
// (Paper/Sepia/Dusk/Night) runs only the ~6 ms LUT pass instead of re-decoding
// slow JPEG2000/JBIG2 scanned pages (~800 ms each).
QCache<QString, QImage> PdfPages::s_rawCache(32 * 1024 * 1024);
QHash<QString, QSet<QString>> PdfPages::s_tokenKeys;
QHash<QString, QSet<QString>> PdfPages::s_tokenTileKeys;
QHash<QString, QSet<QString>> PdfPages::s_tokenRawKeys;
QHash<QString, QList<QPair<int, QString>>> PdfPages::s_idWidths;
QHash<QString, QSet<QString>> PdfPages::s_tokenIds;

namespace {

void assertWorkerAffinity(PdfPages::WorkerState *worker) {
    Q_ASSERT(worker != nullptr);
    QThread *cur = QThread::currentThread();
    Q_ASSERT(!QCoreApplication::instance() || cur != QCoreApplication::instance()->thread());
    if (worker->book) {
        Q_ASSERT(worker->workerThread == cur);
        if (worker->book->document())
            Q_ASSERT(worker->book->document()->thread() == cur);
    }
}

void resetWorkerBook(PdfPages::WorkerState *worker) {
    assertWorkerAffinity(worker);
    worker->book.reset();
    worker->token.clear();
    worker->path.clear();
    worker->workerThread = nullptr;
}

bool ensureWorkerBook(PdfPages::WorkerState *worker, const QString &token, const QString &path) {
    assertWorkerAffinity(worker);
    if (worker->token != token || worker->path != path || !worker->book) {
        resetWorkerBook(worker);
        auto opened = std::make_unique<PdfBook>(path);
        if (opened->open()) {
            QThread *cur = QThread::currentThread();
            Q_ASSERT(!opened->document() || opened->document()->thread() == cur);
            worker->workerThread = cur;
            worker->token = token;
            worker->path = path;
            worker->book = std::move(opened);
        } else {
            return false;
        }
    }
    return worker->book != nullptr;
}

class CachedPageResponse : public QQuickImageResponse {
public:
    explicit CachedPageResponse(QImage image) : m_image(std::move(image)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QImage m_image;
};

}  // namespace

class PageResponse : public QQuickImageResponse, public QRunnable {
public:
    PageResponse(std::shared_ptr<PdfPages::WorkerState> worker, QString token, QString id, QString cacheKey, int page,
                 QSize size, QColor bg, QColor fg, PdfPages::TileSpec tile = {})
        : m_worker(std::move(worker)),
          m_token(std::move(token)),
          m_id(std::move(id)),
          m_cacheKey(std::move(cacheKey)),
          m_page(page),
          m_size(size),
          m_bg(bg),
          m_fg(fg),
          m_tile(tile) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override { m_cancelled.store(true, std::memory_order_relaxed); }

    void run() override {
        Clothbound::acquirePoolRunnable(this);
        if (m_worker)
            assertWorkerAffinity(m_worker.get());
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished();
            return;
        }
        if (m_tile.active) {
            if (const QImage hit = PdfPages::cachedTile(m_cacheKey); !hit.isNull()) {
                m_image = hit;
                emit finished();
                return;
            }
        } else if (const QImage hit = PdfPages::cached(m_cacheKey); !hit.isNull()) {
            m_image = hit;
            emit finished();
            return;
        }
        const QString path = PdfPages::pathFor(m_token);
        if (path.isEmpty()) {
            if (m_worker && m_worker->token == m_token)
                resetWorkerBook(m_worker.get());
            emit finished();
            return;
        }
        if (m_tile.active) {
            QImage rendered;
            if (m_worker && ensureWorkerBook(m_worker.get(), m_token, path) &&
                !m_cancelled.load(std::memory_order_relaxed)) {
                rendered = PdfPages::renderTileRaw(*m_worker->book, m_page, m_tile, m_size);
            }
            if (!m_cancelled.load(std::memory_order_relaxed) && !rendered.isNull()) {
                PdfPages::compositeAndRecolor(rendered, m_bg, m_fg);
                if (!m_cancelled.load(std::memory_order_relaxed)) {
                    PdfPages::storeTile(m_token, m_cacheKey, rendered);
                    m_image = std::move(rendered);
                }
            }
            emit finished();
            return;
        }
        const QString rawKey = PdfPages::makeRawKey(m_token, m_page, m_size);
        QImage rendered = PdfPages::cachedRaw(rawKey);
        if (!rendered.isNull()) {
            // Detach from the shared raw cache before in-place LUT recoloring.
            rendered = rendered.copy();
        } else {
            if (m_worker && ensureWorkerBook(m_worker.get(), m_token, path) &&
                !m_cancelled.load(std::memory_order_relaxed)) {
                const QSize size = PdfPages::renderSizeFor(m_worker->book->pageSize(m_page), m_size);
                if (!m_cancelled.load(std::memory_order_relaxed)) {
                    rendered = m_worker->book->renderPageRaw(m_page, size);
                    if (!m_cancelled.load(std::memory_order_relaxed) && !rendered.isNull()) {
                        PdfPages::storeRaw(m_token, rawKey, rendered);
                        // Detach before in-place compositeAndRecolor so raw cache stays untouched.
                        rendered = rendered.copy();
                    }
                }
            }
        }
        if (!m_cancelled.load(std::memory_order_relaxed) && !rendered.isNull()) {
            PdfPages::compositeAndRecolor(rendered, m_bg, m_fg);
            if (!m_cancelled.load(std::memory_order_relaxed)) {
                PdfPages::storeCached(m_token, m_id, m_size.width(), m_cacheKey, rendered);
                m_image = std::move(rendered);
            }
        }
        emit finished();
    }

private:
    std::shared_ptr<PdfPages::WorkerState> m_worker;
    QString m_token;
    QString m_id;
    QString m_cacheKey;
    int m_page;
    QSize m_size;
    QColor m_bg, m_fg;
    PdfPages::TileSpec m_tile;
    std::atomic_bool m_cancelled{false};
    QImage m_image;
};

class PrefetchTask : public QRunnable {
public:
    PrefetchTask(std::shared_ptr<PdfPages::WorkerState> worker, quint64 gen, QString token, QString id, QString cacheKey,
                 int page, QSize size, QColor bg, QColor fg)
        : m_worker(std::move(worker)),
          m_gen(gen),
          m_token(std::move(token)),
          m_id(std::move(id)),
          m_cacheKey(std::move(cacheKey)),
          m_page(page),
          m_size(size),
          m_bg(bg),
          m_fg(fg) {
        setAutoDelete(true);
    }

    void run() override {
        Clothbound::acquirePoolRunnable(this);
        if (m_worker)
            assertWorkerAffinity(m_worker.get());
        if (m_gen != PdfPages::s_prefetchGen.load(std::memory_order_relaxed))
            return;
        if (!PdfPages::cached(m_cacheKey).isNull())
            return;
        const QString path = PdfPages::pathFor(m_token);
        if (path.isEmpty())
            return;
        // Keep background JPEG2000/PDFium prefetch below UI/audio scheduling priority.
        struct PriorityGuard {
            QThread *t = QThread::currentThread();
            QThread::Priority prev = (t && t->priority() != QThread::InheritPriority)
                                         ? t->priority()
                                         : QThread::NormalPriority;
            PriorityGuard() {
                if (t)
                    t->setPriority(QThread::LowPriority);
            }
            ~PriorityGuard() {
                if (t)
                    t->setPriority(prev);
            }
        } priorityGuard;

        const QString rawKey = PdfPages::makeRawKey(m_token, m_page, m_size);
        QImage rendered = PdfPages::cachedRaw(rawKey);
        if (!rendered.isNull()) {
            rendered = rendered.copy();
        } else {
            if (!m_worker || !ensureWorkerBook(m_worker.get(), m_token, path))
                return;
            if (m_page < 0 || m_page >= m_worker->book->pageCount())
                return;
            if (m_gen != PdfPages::s_prefetchGen.load(std::memory_order_relaxed))
                return;
            const QSize size = PdfPages::renderSizeFor(m_worker->book->pageSize(m_page), m_size);
            rendered = m_worker->book->renderPageRaw(m_page, size);
            if (rendered.isNull() || m_gen != PdfPages::s_prefetchGen.load(std::memory_order_relaxed))
                return;
            PdfPages::storeRaw(m_token, rawKey, rendered);
            rendered = rendered.copy();
        }
        PdfPages::compositeAndRecolor(rendered, m_bg, m_fg);
        if (m_gen == PdfPages::s_prefetchGen.load(std::memory_order_relaxed))
            PdfPages::storeCached(m_token, m_id, m_size.width(), m_cacheKey, rendered);
    }

private:
    std::shared_ptr<PdfPages::WorkerState> m_worker;
    quint64 m_gen;
    QString m_token;
    QString m_id;
    QString m_cacheKey;
    int m_page;
    QSize m_size;
    QColor m_bg, m_fg;
};

PdfPages::PdfPages() {
    // One dedicated non-expiring worker thread: QtPdf serializes PDFium behind
    // one global QRecursiveMutex, so a single pool halves PDF memory usage with
    // zero loss of PDFium parallelism while keeping QPdfDocument strictly affine
    // to a live thread for its entire lifetime.
    m_worker = std::make_shared<WorkerState>();
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(-1);
    Q_ASSERT(m_pool.maxThreadCount() == 1);
    QMutexLocker lock(&s_mutex);
    s_instance = this;
}

PdfPages::~PdfPages() {
    {
        QMutexLocker lock(&s_mutex);
        if (s_instance == this)
            s_instance = nullptr;
    }
    s_prefetchGen.fetch_add(1, std::memory_order_relaxed);
    auto worker = m_worker;
    Clothbound::startPoolTask(&m_pool, [worker] { resetWorkerBook(worker.get()); });
    m_pool.waitForDone();
}

void PdfPages::add(const QString &token, std::shared_ptr<PdfBook> book) {
    QMutexLocker lock(&s_mutex);
    if (book && !book->path().isEmpty())
        s_paths.insert(token, book->path());
}

void PdfPages::remove(const QString &token) {
    s_prefetchGen.fetch_add(1, std::memory_order_relaxed);
    QMutexLocker lock(&s_mutex);
    s_paths.remove(token);
    const QSet<QString> keys = s_tokenKeys.take(token);
    for (const QString &key : keys)
        s_cache.remove(key);
    const QSet<QString> tileKeys = s_tokenTileKeys.take(token);
    for (const QString &key : tileKeys)
        s_tileCache.remove(key);
    const QSet<QString> rawKeys = s_tokenRawKeys.take(token);
    for (const QString &key : rawKeys)
        s_rawCache.remove(key);
    const QSet<QString> ids = s_tokenIds.take(token);
    for (const QString &id : ids)
        s_idWidths.remove(id);
    if (s_instance) {
        auto worker = s_instance->m_worker;
        // Release the closed QPdfDocument and its file descriptor on its affine
        // worker thread instead of pinning hundreds of MB until another PDF opens.
        Clothbound::startPoolTask(
            &s_instance->m_pool,
            [worker, token] {
                if (worker)
                    assertWorkerAffinity(worker.get());
                if (worker && worker->token == token && PdfPages::pathFor(token).isEmpty())
                    resetWorkerBook(worker.get());
            },
            2);
    }
}

void PdfPages::cancelPrefetch() { s_prefetchGen.fetch_add(1, std::memory_order_relaxed); }

PdfPages::TileSpec PdfPages::parseTileSpec(const QUrlQuery &query) {
    TileSpec spec;
    if (!query.hasQueryItem(QStringLiteral("tx")) && !query.hasQueryItem(QStringLiteral("ty")) &&
        !query.hasQueryItem(QStringLiteral("tw")) && !query.hasQueryItem(QStringLiteral("th")) &&
        !query.hasQueryItem(QStringLiteral("fullw")))
        return spec;

    bool okX = false, okY = false, okW = false, okH = false, okFull = false;
    const double rawX = query.queryItemValue(QStringLiteral("tx")).toDouble(&okX);
    const double rawY = query.queryItemValue(QStringLiteral("ty")).toDouble(&okY);
    const double rawW = query.queryItemValue(QStringLiteral("tw")).toDouble(&okW);
    const double rawH = query.queryItemValue(QStringLiteral("th")).toDouble(&okH);
    const int rawFull = query.queryItemValue(QStringLiteral("fullw")).toInt(&okFull);
    if (!okX || !okY || !okW || !okH || !okFull || rawFull <= 0)
        return spec;
    if (!std::isfinite(rawX) || !std::isfinite(rawY) || !std::isfinite(rawW) || !std::isfinite(rawH))
        return spec;
    if (rawW <= 0.0005 || rawH <= 0.0005)
        return spec;

    const double x1 = std::clamp(rawX, 0.0, 1.0);
    const double y1 = std::clamp(rawY, 0.0, 1.0);
    const double x2 = std::clamp(rawX + rawW, 0.0, 1.0);
    const double y2 = std::clamp(rawY + rawH, 0.0, 1.0);
    if (x2 - x1 <= 0.0005 || y2 - y1 <= 0.0005)
        return spec;

    spec.active = true;
    spec.tx = x1;
    spec.ty = y1;
    spec.tw = x2 - x1;
    spec.th = y2 - y1;
    spec.fullWidth = qBound(64, rawFull, 12000);
    return spec;
}

QPair<QSize, QRect> PdfPages::tileGeometryFor(const QSizeF &points, const TileSpec &spec, const QSize &requestedSize) {
    if (!spec.active || points.width() <= 0.0 || points.height() <= 0.0 || !std::isfinite(points.width()) ||
        !std::isfinite(points.height()))
        return {};

    const double ratio = std::clamp(points.height() / points.width(), 0.1, 10.0);
    constexpr int kMaxVirtualDim = 12000;
    constexpr int kMaxTileDim = 4096;

    double virtW = double(qBound(64, spec.fullWidth, kMaxVirtualDim));
    double virtH = virtW * ratio;
    if (virtH > double(kMaxVirtualDim)) {
        const double shrink = double(kMaxVirtualDim) / virtH;
        virtW = std::max(64.0, std::floor(virtW * shrink));
        virtH = double(kMaxVirtualDim);
    }

    auto computeClip = [&](int fw, int fh) -> QRect {
        const int cx = qBound(0, int(std::floor(spec.tx * double(fw))), qMax(0, fw - 1));
        const int cy = qBound(0, int(std::floor(spec.ty * double(fh))), qMax(0, fh - 1));
        const int cw = qBound(1, int(std::ceil((spec.tx + spec.tw) * double(fw))) - cx, fw - cx);
        const int ch = qBound(1, int(std::ceil((spec.ty + spec.th) * double(fh))) - cy, fh - cy);
        return QRect(cx, cy, cw, ch);
    };

    int fullW = qMax(1, int(std::lround(virtW)));
    int fullH = qMax(1, int(std::lround(virtH)));
    QRect clip = computeClip(fullW, fullH);

    int maxW = kMaxTileDim;
    int maxH = kMaxTileDim;
    if (requestedSize.width() > 0)
        maxW = qMin(maxW, requestedSize.width());
    if (requestedSize.height() > 0)
        maxH = qMin(maxH, requestedSize.height());

    const qint64 pixels = qint64(clip.width()) * qint64(clip.height());
    if (clip.width() > maxW || clip.height() > maxH || pixels > kMaxTilePixels) {
        double scale = 1.0;
        if (clip.width() > maxW)
            scale = std::min(scale, double(maxW) / double(clip.width()));
        if (clip.height() > maxH)
            scale = std::min(scale, double(maxH) / double(clip.height()));
        if (pixels > kMaxTilePixels)
            scale = std::min(scale, std::sqrt(double(kMaxTilePixels) / double(pixels)));
        fullW = qMax(1, int(std::floor(double(fullW) * scale)));
        fullH = qMax(1, int(std::floor(double(fullH) * scale)));
        clip = computeClip(fullW, fullH);
        while ((clip.width() > maxW || clip.height() > maxH ||
                qint64(clip.width()) * qint64(clip.height()) > kMaxTilePixels) &&
               (fullW > 1 || fullH > 1)) {
            fullW = qMax(1, int(std::floor(double(fullW) * 0.96)));
            fullH = qMax(1, int(std::floor(double(fullH) * 0.96)));
            clip = computeClip(fullW, fullH);
        }
        if (clip.width() > maxW)
            clip.setWidth(maxW);
        if (clip.height() > maxH)
            clip.setHeight(maxH);
    }
    return {QSize(fullW, fullH), clip};
}

QImage PdfPages::renderTileRaw(PdfBook &book, int page, const TileSpec &spec, const QSize &requestedSize) {
    if (!spec.active || page < 0 || page >= book.pageCount())
        return {};
    QPdfDocument *doc = book.document();
    if (!doc)
        return {};
    const auto [scaledSize, clipRect] = tileGeometryFor(book.pageSize(page), spec, requestedSize);
    if (!scaledSize.isValid() || scaledSize.isEmpty() || !clipRect.isValid() || clipRect.isEmpty())
        return {};

    QPdfDocumentRenderOptions opts;
    opts.setScaledSize(scaledSize);
    opts.setScaledClipRect(clipRect);
    QImage rendered = doc->render(page, clipRect.size(), opts);
    if (!rendered.isNull())
        return rendered;

    // Fallback: render a bounded full page raw and crop the normalized viewport.
    const QSize fallbackSize = renderSizeFor(book.pageSize(page), QSize(qMin(scaledSize.width(), 3200), 0));
    const QImage full = book.renderPageRaw(page, fallbackSize);
    if (full.isNull())
        return {};
    const int fx = qBound(0, int(std::floor(spec.tx * full.width())), qMax(0, full.width() - 1));
    const int fy = qBound(0, int(std::floor(spec.ty * full.height())), qMax(0, full.height() - 1));
    const int fw = qBound(1, int(std::ceil((spec.tx + spec.tw) * full.width())) - fx, full.width() - fx);
    const int fh = qBound(1, int(std::ceil((spec.ty + spec.th) * full.height())) - fy, full.height() - fy);
    return full.copy(fx, fy, fw, fh);
}

QString PdfPages::makeCacheKey(const QString &id, const QSize &requestedSize) {
    const int w = requestedSize.width() > 0 ? requestedSize.width() : 0;
    const int h = requestedSize.height() > 0 ? requestedSize.height() : 0;
    return id + QStringLiteral("|%1x%2").arg(w).arg(h);
}

QString PdfPages::makeRawKey(const QString &token, int page, const QSize &requestedSize) {
    const int w = requestedSize.width() > 0 ? requestedSize.width() : 0;
    const int h = requestedSize.height() > 0 ? requestedSize.height() : 0;
    return QStringLiteral("%1/%2|%3x%4").arg(token).arg(page).arg(w).arg(h);
}

QSize PdfPages::renderSizeFor(const QSizeF &points, const QSize &requestedSize) {
    QSize size = requestedSize;
    if (points.width() > 0 && points.height() > 0 && std::isfinite(points.width()) && std::isfinite(points.height())) {
        const double ratio = std::clamp(points.height() / points.width(), 0.1, 10.0);
        if (size.width() <= 0 && size.height() <= 0) {
            const int w = qBound(1, qRound(points.width() * 2.0), 6000);
            const int h = qBound(1, qRound(w * ratio), 6000);
            size = QSize(w, h);
        } else if (size.height() <= 0) {
            const int w = qBound(1, size.width(), 6000);
            size = QSize(w, qBound(1, qRound(w * ratio), 6000));
        } else if (size.width() <= 0) {
            const int h = qBound(1, size.height(), 6000);
            size = QSize(qBound(1, qRound(h / ratio), 6000), h);
        }
    }
    constexpr qint64 maxPixels = 24LL * 1000 * 1000;
    size = size.boundedTo(QSize(6000, 6000));
    if (size.width() > 0 && size.height() > 0 && qint64(size.width()) * qint64(size.height()) > maxPixels) {
        const double scale = std::sqrt(double(maxPixels) / (double(size.width()) * double(size.height())));
        size = QSize(qMax(1, int(std::floor(size.width() * scale))), qMax(1, int(std::floor(size.height() * scale))));
    }
    return size;
}

void PdfPages::prefetch(const QString &token, const QList<int> &pages, const QSize &requestedSize,
                        const QString &colorQuery) {
    if (token.isEmpty() || pages.isEmpty())
        return;
    const quint64 gen = s_prefetchGen.fetch_add(1, std::memory_order_relaxed) + 1;
    const QString cleanQuery = colorQuery.startsWith(QLatin1Char('?')) ? colorQuery.mid(1) : colorQuery;
    const QUrlQuery query(cleanQuery);
    const QColor bg(query.queryItemValue(QStringLiteral("bg"), QUrl::FullyDecoded));
    const QColor fg(query.queryItemValue(QStringLiteral("fg"), QUrl::FullyDecoded));

    QMutexLocker lock(&s_mutex);
    if (!s_instance || !s_paths.contains(token))
        return;
    PdfPages *instance = s_instance;
    for (const int page : pages) {
        if (page < 0)
            continue;
        const QString id = token + QLatin1Char('/') + QString::number(page) + colorQuery;
        const QString cacheKey = makeCacheKey(id, requestedSize);
        if (s_cache.contains(cacheKey))
            continue;
        Clothbound::startPoolRunnable(
            &instance->m_pool,
            new PrefetchTask(instance->m_worker, gen, token, id, cacheKey, page, requestedSize, bg, fg), -1);
    }
}

QString PdfPages::pathFor(const QString &token) {
    QMutexLocker lock(&s_mutex);
    return s_paths.value(token);
}

QImage PdfPages::cached(const QString &key) {
    QMutexLocker lock(&s_mutex);
    const QImage *img = s_cache.object(key);
    return img ? *img : QImage();
}

QImage PdfPages::cachedDownscaleMatch(const QString &id, const QSize &requestedSize) {
    const int reqW = requestedSize.width();
    if (reqW <= 0)
        return {};
    const int maxW = reqW * 17 / 10;
    QMutexLocker lock(&s_mutex);
    auto it = s_idWidths.find(id);
    if (it == s_idWidths.end())
        return {};
    auto &entries = it.value();
    int bestW = INT_MAX;
    QImage bestImg;
    for (int i = int(entries.size()) - 1; i >= 0; --i) {
        const auto &[w, key] = entries[i];
        const QImage *img = s_cache.object(key);
        if (!img) {
            entries.removeAt(i);
            continue;
        }
        if (w >= reqW && w <= maxW && w < bestW) {
            bestW = w;
            bestImg = *img;
        }
    }
    return bestImg;
}

void PdfPages::storeCached(const QString &token, const QString &id, int width, const QString &key, const QImage &image) {
    if (image.isNull())
        return;
    QMutexLocker lock(&s_mutex);
    if (!s_paths.contains(token))
        return;
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > s_cache.maxCost())
        return;
    if (s_cache.insert(key, new QImage(image), int(bytes))) {
        auto &tokenKeys = s_tokenKeys[token];
        tokenKeys.insert(key);
        if (tokenKeys.size() > s_cache.count() + 64) {
            for (auto it = tokenKeys.begin(); it != tokenKeys.end();) {
                if (!s_cache.contains(*it))
                    it = tokenKeys.erase(it);
                else
                    ++it;
            }
        }
        if (width > 0 && !id.isEmpty()) {
            auto &list = s_idWidths[id];
            bool present = false;
            for (int i = int(list.size()) - 1; i >= 0; --i) {
                if (list[i].second == key)
                    present = true;
                else if (!s_cache.contains(list[i].second))
                    list.removeAt(i);
            }
            if (!present)
                list.append({width, key});
            s_tokenIds[token].insert(id);
        }
    }
}

QImage PdfPages::cachedTile(const QString &key) {
    QMutexLocker lock(&s_mutex);
    const QImage *img = s_tileCache.object(key);
    return img ? *img : QImage();
}

void PdfPages::storeTile(const QString &token, const QString &key, const QImage &image) {
    if (image.isNull())
        return;
    QMutexLocker lock(&s_mutex);
    if (!s_paths.contains(token))
        return;
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > kMaxTileBytes || bytes > s_tileCache.maxCost())
        return;
    if (s_tileCache.insert(key, new QImage(image), int(bytes))) {
        auto &tileKeys = s_tokenTileKeys[token];
        tileKeys.insert(key);
        if (tileKeys.size() > s_tileCache.count() + 64) {
            for (auto it = tileKeys.begin(); it != tileKeys.end();) {
                if (!s_tileCache.contains(*it))
                    it = tileKeys.erase(it);
                else
                    ++it;
            }
        }
    }
}

QImage PdfPages::cachedRaw(const QString &rawKey) {
    QMutexLocker lock(&s_mutex);
    const QImage *img = s_rawCache.object(rawKey);
    return img ? *img : QImage();
}

void PdfPages::storeRaw(const QString &token, const QString &rawKey, const QImage &image) {
    if (image.isNull())
        return;
    QMutexLocker lock(&s_mutex);
    if (!s_paths.contains(token))
        return;
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > s_rawCache.maxCost())
        return;
    if (s_rawCache.insert(rawKey, new QImage(image), int(bytes))) {
        auto &rawKeys = s_tokenRawKeys[token];
        rawKeys.insert(rawKey);
        if (rawKeys.size() > s_rawCache.count() + 64) {
            for (auto it = rawKeys.begin(); it != rawKeys.end();) {
                if (!s_rawCache.contains(*it))
                    it = rawKeys.erase(it);
                else
                    ++it;
            }
        }
    }
}

QQuickImageResponse *PdfPages::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // "<token>/<page>?bg=%23rrggbb&fg=%23rrggbb[&tx=...&ty=...&tw=...&th=...&fullw=...]"
    const QString cacheKey = makeCacheKey(id, requestedSize);
    const QString path = id.section(QLatin1Char('?'), 0, 0);
    const QString token = path.section(QLatin1Char('/'), 0, 0);
    const int page = path.section(QLatin1Char('/'), 1).toInt();
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    const TileSpec tile = parseTileSpec(query);

    if (tile.active) {
        if (const QImage hit = cachedTile(cacheKey); !hit.isNull())
            return new CachedPageResponse(hit);
    } else {
        if (const QImage hit = cached(cacheKey); !hit.isNull())
            return new CachedPageResponse(hit);
        // When narrowing (e.g. Companion docking beside the page), reuse an already-
        // cached slightly larger render of the exact same page+theme via GPU downscale
        // instead of re-decoding slow JPEG2000/JBIG2 scans from scratch.
        if (const QImage nearHit = cachedDownscaleMatch(id, requestedSize); !nearHit.isNull())
            return new CachedPageResponse(nearHit);
    }

    const QColor bg(query.queryItemValue(QStringLiteral("bg"), QUrl::FullyDecoded));
    const QColor fg(query.queryItemValue(QStringLiteral("fg"), QUrl::FullyDecoded));
    auto *response = new PageResponse(m_worker, token, id, cacheKey, page, requestedSize, bg, fg, tile);
    Clothbound::startPoolRunnable(&m_pool, response, 1);
    return response;
}

void PdfPages::recolor(QImage &image, const QColor &bg, const QColor &fg) {
    PdfRecolor::recolor(image, bg, fg);
}

void PdfPages::compositeAndRecolor(QImage &image, const QColor &bg, const QColor &fg) {
    PdfRecolor::compositeAndRecolor(image, bg, fg);
}

QRectF PdfPages::contentBox(const QImage &page) { return PdfRecolor::contentBox(page); }

QRectF PdfPages::bookCrop(PdfBook &book) { return PdfRecolor::bookCrop(book); }
