#include "pdfpages.h"

#include "pdfbook.h"

#include <QMetaObject>
#include <QRunnable>
#include <QUrlQuery>
#include <algorithm>
#include <atomic>
#include <cmath>

QMutex PdfPages::s_mutex;
QHash<QString, QString> PdfPages::s_paths;
// Keep up to ~128 MiB of rendered pages in an LRU cache so page turns back
// and forth are instantaneous without re-running PDFium + recolor().
QCache<QString, QImage> PdfPages::s_cache(128 * 1024 * 1024);
QHash<QString, QSet<QString>> PdfPages::s_tokenKeys;

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
    PageResponse(std::shared_ptr<PdfPages::WorkerState> worker, QString token, QString cacheKey, int page, QSize size,
                 QColor bg, QColor fg)
        : m_worker(std::move(worker)),
          m_token(std::move(token)),
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
            const QSizeF points = m_worker->book->pageSize(m_page);
            QSize size = m_size;
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
            if (!m_cancelled.load(std::memory_order_relaxed)) {
                QImage rendered = m_worker->book->renderPage(m_page, size);
                if (!m_cancelled.load(std::memory_order_relaxed) && !rendered.isNull()) {
                    if (m_bg.isValid() && m_fg.isValid())
                        PdfPages::recolor(rendered, m_bg, m_fg);
                    if (!m_cancelled.load(std::memory_order_relaxed)) {
                        PdfPages::storeCached(m_token, m_cacheKey, rendered);
                        m_image = std::move(rendered);
                    }
                }
            }
        }
        emit finished();
    }

private:
    std::shared_ptr<PdfPages::WorkerState> m_worker;
    QString m_token;
    QString m_cacheKey;
    int m_page;
    QSize m_size;
    QColor m_bg, m_fg;
    std::atomic_bool m_cancelled{false};
    QImage m_image;
};

PdfPages::PdfPages() : m_worker(std::make_shared<WorkerState>()) {
    // Dedicated single render thread: PDFium's QPdfDocument is owned, used,
    // and destroyed solely on this thread, never shared with the GUI thread.
    m_pool.setMaxThreadCount(1);
}

PdfPages::~PdfPages() {
    auto worker = m_worker;
    m_pool.start([worker] {
        worker->book.reset();
        worker->token.clear();
        worker->path.clear();
    });
    m_pool.waitForDone();
}

void PdfPages::add(const QString &token, std::shared_ptr<PdfBook> book) {
    QMutexLocker lock(&s_mutex);
    if (book && !book->path().isEmpty())
        s_paths.insert(token, book->path());
}

void PdfPages::remove(const QString &token) {
    QMutexLocker lock(&s_mutex);
    s_paths.remove(token);
    const QSet<QString> keys = s_tokenKeys.take(token);
    for (const QString &key : keys)
        s_cache.remove(key);
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

void PdfPages::storeCached(const QString &token, const QString &key, const QImage &image) {
    if (image.isNull())
        return;
    QMutexLocker lock(&s_mutex);
    if (!s_paths.contains(token))
        return;
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > s_cache.maxCost())
        return;
    if (s_cache.insert(key, new QImage(image), int(bytes)))
        s_tokenKeys[token].insert(key);
}

QQuickImageResponse *PdfPages::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // "<token>/<page>?bg=%23rrggbb&fg=%23rrggbb" (or without query for original colors)
    const QString cacheKey = id + QStringLiteral("|%1x%2").arg(requestedSize.width()).arg(requestedSize.height());
    if (const QImage hit = cached(cacheKey); !hit.isNull())
        return new CachedPageResponse(hit);

    const QString path = id.section(QLatin1Char('?'), 0, 0);
    const QString token = path.section(QLatin1Char('/'), 0, 0);
    const int page = path.section(QLatin1Char('/'), 1).toInt();
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    const QColor bg(query.queryItemValue(QStringLiteral("bg"), QUrl::FullyDecoded));
    const QColor fg(query.queryItemValue(QStringLiteral("fg"), QUrl::FullyDecoded));
    auto *response = new PageResponse(m_worker, token, cacheKey, page, requestedSize, bg, fg);
    m_pool.start(response);
    return response;
}

void PdfPages::recolor(QImage &image, const QColor &bg, const QColor &fg) {
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_RGB32);
    // Pure white paper and black ink stay untouched.
    if (bg == Qt::white && fg == Qt::black)
        return;
    const int br = bg.red(), bgG = bg.green(), bb = bg.blue();
    const int fr = fg.red(), fgG = fg.green(), fb = fg.blue();
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const int r = qRed(pixel), g = qGreen(pixel), b = qBlue(pixel);
            const int high = qMax(r, qMax(g, b)), low = qMin(r, qMin(g, b));
            const int chroma = high - low;
            const int light = (r * 299 + g * 587 + b * 114) / 1000;
            const int gr = fr + (br - fr) * light / 255;
            const int gg = fgG + (bgG - fgG) * light / 255;
            const int gb = fb + (bb - fb) * light / 255;
            if (chroma <= 16) {
                // Grays: paper to background, ink to text.
                line[x] = qRgb(gr, gg, gb);
            } else {
                const int cr = (r * 4 + br) / 5;
                const int cg = (g * 4 + bgG) / 5;
                const int cb = (b * 4 + bb) / 5;
                if (chroma >= 64) {
                    // Colors keep their hue, set a little toward the background.
                    line[x] = qRgb(cr, cg, cb);
                } else {
                    // Smooth blend across low-saturation anti-aliased edges.
                    const int w = chroma - 16;  // 1..47 over 48
                    line[x] = qRgb((gr * (48 - w) + cr * w) / 48, (gg * (48 - w) + cg * w) / 48,
                                   (gb * (48 - w) + cb * w) / 48);
                }
            }
        }
    }
}

QRectF PdfPages::contentBox(const QImage &page) {
    if (page.isNull() || page.width() <= 0 || page.height() <= 0)
        return {};
    const QImage gray = page.convertToFormat(QImage::Format_Grayscale8);
    int left = gray.width(), right = -1, top = gray.height(), bottom = -1;
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
    if (right < left || bottom < top)
        return {};
    return QRectF(double(left) / gray.width(), double(top) / gray.height(), double(right - left + 1) / gray.width(),
                  double(bottom - top + 1) / gray.height());
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
        const QRectF box = contentBox(book.renderPage(page, QSize(240, height)));
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
