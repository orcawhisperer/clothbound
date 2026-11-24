#include "formats/pdfpages.h"

#include "formats/pdfbook.h"

#include <QMetaObject>
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
// Keep up to ~128 MiB of rendered pages in an LRU cache so page turns back
// and forth are instantaneous without re-running PDFium + recolor().
QCache<QString, QImage> PdfPages::s_cache(128 * 1024 * 1024);
// Keep up to ~32 MiB of pre-recolor PDFium renders so switching reading themes
// (Paper/Sepia/Dusk/Night) runs only the ~6 ms LUT pass instead of re-decoding
// slow JPEG2000/JBIG2 scanned pages (~800 ms each).
QCache<QString, QImage> PdfPages::s_rawCache(32 * 1024 * 1024);
QHash<QString, QSet<QString>> PdfPages::s_tokenKeys;
QHash<QString, QSet<QString>> PdfPages::s_tokenRawKeys;
QHash<QString, QList<QPair<int, QString>>> PdfPages::s_idWidths;
QHash<QString, QSet<QString>> PdfPages::s_tokenIds;

namespace {

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
                 QSize size, QColor bg, QColor fg)
        : m_worker(std::move(worker)),
          m_token(std::move(token)),
          m_id(std::move(id)),
          m_cacheKey(std::move(cacheKey)),
          m_page(page),
          m_size(size),
          m_bg(bg),
          m_fg(fg) {
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
        if (const QImage hit = PdfPages::cached(m_cacheKey); !hit.isNull()) {
            m_image = hit;
            emit finished();
            return;
        }
        const QString path = PdfPages::pathFor(m_token);
        if (path.isEmpty()) {
            if (m_worker && m_worker->token == m_token) {
                m_worker->book.reset();
                m_worker->token.clear();
                m_worker->path.clear();
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
            if (m_worker && (m_worker->token != m_token || m_worker->path != path || !m_worker->book)) {
                m_worker->book.reset();
                auto opened = std::make_unique<PdfBook>(path);
                if (opened->open()) {
                    m_worker->token = m_token;
                    m_worker->path = path;
                    m_worker->book = std::move(opened);
                } else {
                    m_worker->token.clear();
                    m_worker->path.clear();
                }
            }
            if (m_worker && m_worker->book && !m_cancelled.load(std::memory_order_relaxed)) {
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
            if (m_worker && (m_worker->token != m_token || m_worker->path != path || !m_worker->book)) {
                m_worker->book.reset();
                auto opened = std::make_unique<PdfBook>(path);
                if (opened->open()) {
                    m_worker->token = m_token;
                    m_worker->path = path;
                    m_worker->book = std::move(opened);
                } else {
                    m_worker->token.clear();
                    m_worker->path.clear();
                    return;
                }
            }
            if (!m_worker || !m_worker->book || m_page < 0 || m_page >= m_worker->book->pageCount())
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
    // Two dedicated single-thread pools (even/odd page parity) so 2-page
    // spreads render left & right in parallel while each QPdfDocument stays
    // strictly affine to its own thread and never touches the GUI thread.
    for (int i = 0; i < kWorkerSlots; ++i) {
        m_workers[i] = std::make_shared<WorkerState>();
        m_pools[i].setMaxThreadCount(1);
    }
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
    for (int i = 0; i < kWorkerSlots; ++i) {
        auto worker = m_workers[i];
        m_pools[i].start([worker] {
            worker->book.reset();
            worker->token.clear();
            worker->path.clear();
        });
    }
    for (int i = 0; i < kWorkerSlots; ++i)
        m_pools[i].waitForDone();
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
    const QSet<QString> rawKeys = s_tokenRawKeys.take(token);
    for (const QString &key : rawKeys)
        s_rawCache.remove(key);
    const QSet<QString> ids = s_tokenIds.take(token);
    for (const QString &id : ids)
        s_idWidths.remove(id);
}

void PdfPages::cancelPrefetch() { s_prefetchGen.fetch_add(1, std::memory_order_relaxed); }

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
        const int slot = page & (kWorkerSlots - 1);
        instance->m_pools[slot].start(
            new PrefetchTask(instance->m_workers[slot], gen, token, id, cacheKey, page, requestedSize, bg, fg), -1);
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
        s_tokenKeys[token].insert(key);
        if (width > 0 && !id.isEmpty()) {
            auto &list = s_idWidths[id];
            bool present = false;
            for (const auto &pair : std::as_const(list))
                if (pair.second == key) {
                    present = true;
                    break;
                }
            if (!present)
                list.append({width, key});
            s_tokenIds[token].insert(id);
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
    if (s_rawCache.insert(rawKey, new QImage(image), int(bytes)))
        s_tokenRawKeys[token].insert(rawKey);
}

QQuickImageResponse *PdfPages::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // "<token>/<page>?bg=%23rrggbb&fg=%23rrggbb" (or without query for original colors)
    const QString cacheKey = makeCacheKey(id, requestedSize);
    if (const QImage hit = cached(cacheKey); !hit.isNull())
        return new CachedPageResponse(hit);
    // When narrowing (e.g. Companion docking beside the page), reuse an already-
    // cached slightly larger render of the exact same page+theme via GPU downscale
    // instead of re-decoding slow JPEG2000/JBIG2 scans from scratch.
    if (const QImage nearHit = cachedDownscaleMatch(id, requestedSize); !nearHit.isNull())
        return new CachedPageResponse(nearHit);

    const QString path = id.section(QLatin1Char('?'), 0, 0);
    const QString token = path.section(QLatin1Char('/'), 0, 0);
    const int page = path.section(QLatin1Char('/'), 1).toInt();
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    const QColor bg(query.queryItemValue(QStringLiteral("bg"), QUrl::FullyDecoded));
    const QColor fg(query.queryItemValue(QStringLiteral("fg"), QUrl::FullyDecoded));
    const int slot = qMax(0, page) & (kWorkerSlots - 1);
    auto *response = new PageResponse(m_workers[slot], token, id, cacheKey, page, requestedSize, bg, fg);
    m_pools[slot].start(response, 1);
    return response;
}

namespace {

struct RecolorLut {
    QRgb bgRgb;
    QRgb gray[256];
    uint8_t tintR[256];
    uint8_t tintG[256];
    uint8_t tintB[256];

    RecolorLut(const QColor &bg, const QColor &fg) {
        const int br = bg.red(), bgG = bg.green(), bb = bg.blue();
        const int fr = fg.red(), fgG = fg.green(), fb = fg.blue();
        bgRgb = qRgb(br, bgG, bb);
        for (int l = 0; l < 256; ++l) {
            gray[l] = qRgb(fr + (br - fr) * l / 255, fgG + (bgG - fgG) * l / 255, fb + (bb - fb) * l / 255);
            tintR[l] = uint8_t((l * 4 + br) / 5);
            tintG[l] = uint8_t((l * 4 + bgG) / 5);
            tintB[l] = uint8_t((l * 4 + bb) / 5);
        }
    }

    inline QRgb mapRgb(int r, int g, int b) const {
        if (r == g && g == b)
            return gray[r];
        const int high = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
        const int low = (r < g) ? (r < b ? r : b) : (g < b ? g : b);
        const int chroma = high - low;
        const int light = (r * 19595 + g * 38470 + b * 7471 + 32768) >> 16;
        const QRgb gpx = gray[light];
        if (chroma <= 16)
            return gpx;
        const int cr = tintR[r];
        const int cg = tintG[g];
        const int cb = tintB[b];
        if (chroma >= 64)
            return qRgb(cr, cg, cb);
        const int w = chroma - 16;  // 1..47 over 48
        const int iw = 48 - w;
        const int gr = qRed(gpx), gg = qGreen(gpx), gb = qBlue(gpx);
        // Exact division by 48 for non-negative values up to 255 * 48 = 12240:
        // floor(n / 48) == (n * 2731) >> 17.
        return qRgb(((gr * iw + cr * w) * 2731) >> 17, ((gg * iw + cg * w) * 2731) >> 17,
                    ((gb * iw + cb * w) * 2731) >> 17);
    }
};

}  // namespace

void PdfPages::recolor(QImage &image, const QColor &bg, const QColor &fg) {
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_RGB32);
    // Pure white paper and black ink stay untouched.
    if (bg == Qt::white && fg == Qt::black)
        return;
    const RecolorLut lut(bg, fg);
    const int w = image.width();
    const int h = image.height();
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pixel = line[x];
            if ((pixel & 0x00FFFFFFu) == 0x00FFFFFFu) {
                line[x] = lut.bgRgb;
                continue;
            }
            const int r = int((pixel >> 16) & 0xFFu);
            const int g = int((pixel >> 8) & 0xFFu);
            const int b = int(pixel & 0xFFu);
            line[x] = lut.mapRgb(r, g, b);
        }
    }
}

void PdfPages::compositeAndRecolor(QImage &image, const QColor &bg, const QColor &fg) {
    if (image.isNull())
        return;
    const bool applyTheme = bg.isValid() && fg.isValid() && !(bg == Qt::white && fg == Qt::black);
    const QImage::Format fmt = image.format();
    if (fmt == QImage::Format_RGB32) {
        if (applyTheme)
            recolor(image, bg, fg);
        return;
    }
    if (fmt != QImage::Format_ARGB32_Premultiplied && fmt != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    const bool premul = (image.format() == QImage::Format_ARGB32_Premultiplied);
    const int w = image.width();
    const int h = image.height();

    if (!applyTheme) {
        for (int y = 0; y < h; ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb px = line[x];
                const uint32_t a = px >> 24;
                if (a == 0) {
                    line[x] = 0xFFFFFFFFu;
                } else if (a == 255) {
                    line[x] = px | 0xFF000000u;
                } else {
                    const uint32_t inv = 255u - a;
                    uint32_t r = (px >> 16) & 0xFFu;
                    uint32_t g = (px >> 8) & 0xFFu;
                    uint32_t b = px & 0xFFu;
                    if (premul) {
                        r = std::min(255u, r + inv);
                        g = std::min(255u, g + inv);
                        b = std::min(255u, b + inv);
                    } else {
                        r = (r * a + 255u * inv + 127u) / 255u;
                        g = (g * a + 255u * inv + 127u) / 255u;
                        b = (b * a + 255u * inv + 127u) / 255u;
                    }
                    line[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
        image.reinterpretAsFormat(QImage::Format_RGB32);
        return;
    }

    const RecolorLut lut(bg, fg);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            const uint32_t a = px >> 24;
            if (a == 0) {
                line[x] = lut.bgRgb;
                continue;
            }
            if (a == 255) {
                if ((px & 0x00FFFFFFu) == 0x00FFFFFFu) {
                    line[x] = lut.bgRgb;
                } else {
                    line[x] = lut.mapRgb(int((px >> 16) & 0xFFu), int((px >> 8) & 0xFFu), int(px & 0xFFu));
                }
                continue;
            }
            const uint32_t inv = 255u - a;
            uint32_t r = (px >> 16) & 0xFFu;
            uint32_t g = (px >> 8) & 0xFFu;
            uint32_t b = px & 0xFFu;
            if (premul) {
                r = std::min(255u, r + inv);
                g = std::min(255u, g + inv);
                b = std::min(255u, b + inv);
            } else {
                r = (r * a + 255u * inv + 127u) / 255u;
                g = (g * a + 255u * inv + 127u) / 255u;
                b = (b * a + 255u * inv + 127u) / 255u;
            }
            line[x] = lut.mapRgb(int(r), int(g), int(b));
        }
    }
    image.reinterpretAsFormat(QImage::Format_RGB32);
}

QRectF PdfPages::contentBox(const QImage &page) {
    if (page.isNull() || page.width() <= 0 || page.height() <= 0)
        return {};
    const int w = page.width();
    const int h = page.height();
    int left = w, right = -1, top = h, bottom = -1;
    const QImage::Format fmt = page.format();
    if (fmt == QImage::Format_RGB32 || fmt == QImage::Format_ARGB32 || fmt == QImage::Format_ARGB32_Premultiplied) {
        const bool checkAlpha = (fmt != QImage::Format_RGB32);
        const bool premul = (fmt == QImage::Format_ARGB32_Premultiplied);
        auto isInk = [checkAlpha, premul](QRgb px) -> bool {
            if ((px & 0x00FFFFFFu) == 0x00FFFFFFu)
                return false;
            if (checkAlpha) {
                const uint32_t a = px >> 24;
                if (a == 0)
                    return false;
                const uint32_t inv = 255u - a;
                uint32_t r = (px >> 16) & 0xFFu;
                uint32_t g = (px >> 8) & 0xFFu;
                uint32_t b = px & 0xFFu;
                if (a < 255) {
                    if (premul) {
                        r = std::min(255u, r + inv);
                        g = std::min(255u, g + inv);
                        b = std::min(255u, b + inv);
                    } else {
                        r = (r * a + 255u * inv) / 255u;
                        g = (g * a + 255u * inv) / 255u;
                        b = (b * a + 255u * inv) / 255u;
                    }
                }
                return (r * 77u + g * 150u + b * 29u) < (200u << 8);
            }
            const uint32_t r = (px >> 16) & 0xFFu;
            const uint32_t g = (px >> 8) & 0xFFu;
            const uint32_t b = px & 0xFFu;
            return (r * 77u + g * 150u + b * 29u) < (200u << 8);
        };
        for (int y = 0; y < h; ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(page.constScanLine(y));
            int rowLeft = -1;
            for (int x = 0; x < w; ++x) {
                if (isInk(line[x])) {
                    rowLeft = x;
                    break;
                }
            }
            if (rowLeft < 0)
                continue;
            if (y < top)
                top = y;
            bottom = y;
            if (rowLeft < left)
                left = rowLeft;
            for (int x = w - 1; x >= right + 1 && x >= rowLeft; --x) {
                if (isInk(line[x])) {
                    right = x;
                    break;
                }
            }
            if (rowLeft > right)
                right = rowLeft;
        }
    } else {
        const QImage gray = page.convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < gray.height(); ++y) {
            const uchar *line = gray.constScanLine(y);
            for (int x = 0; x < gray.width(); ++x)
                if (line[x] < 200) {
                    left = qMin(left, x);
                    right = qMax(right, x);
                    top = qMin(top, y);
                    bottom = qMax(bottom, y);
                }
        }
    }
    if (right < left || bottom < top)
        return {};
    return QRectF(double(left) / w, double(top) / h, double(right - left + 1) / w, double(bottom - top + 1) / h);
}

QRectF PdfPages::bookCrop(PdfBook &book) {
    const int pages = book.pageCount();
    if (pages <= 0)
        return QRectF(0, 0, 1, 1);
    // Pages through the middle of the book: covers and front matter differ.
    QList<double> lefts, tops, rights, bottoms;
    const int from = pages > 8 ? 3 : 0;
    const int step = qMax(1, (pages - from) / 24);
    for (int page = from; page < pages && lefts.size() < 24; page += step) {
        const QSizeF size = book.pageSize(page);
        if (size.width() <= 0 || size.height() <= 0 || !std::isfinite(size.width()) || !std::isfinite(size.height()))
            continue;
        const double ratio = std::clamp(size.height() / size.width(), 0.2, 5.0);
        const int height = qBound(1, qRound(240.0 * ratio), 1200);
        const QRectF box = contentBox(book.renderPageRaw(page, QSize(240, height)));
        if (box.isEmpty() || box.width() < 0.2 || box.height() < 0.2)
            continue;
        lefts.append(box.left());
        tops.append(box.top());
        rights.append(box.right());
        bottoms.append(box.bottom());
    }
    if (lefts.size() < 2)
        return QRectF(0, 0, 1, 1);
    std::sort(lefts.begin(), lefts.end());
    std::sort(tops.begin(), tops.end());
    std::sort(rights.begin(), rights.end());
    std::sort(bottoms.begin(), bottoms.end());
    // The widest the text goes on nearly every page, with a little room.
    const auto near = [](const QList<double> &values, double share) {
        return values[qBound(qsizetype(0), qsizetype(qRound(share * (values.size() - 1))), values.size() - 1)];
    };
    const double margin = 0.025;
    const double left = qMax(0.0, near(lefts, 0.1) - margin);
    const double top = qMax(0.0, near(tops, 0.1) - margin);
    const double right = qMin(1.0, near(rights, 0.9) + margin);
    const double bottom = qMin(1.0, near(bottoms, 0.9) + margin);
    if (right - left < 0.4 || bottom - top < 0.4)
        return QRectF(0, 0, 1, 1);
    return QRectF(left, top, right - left, bottom - top);
}
