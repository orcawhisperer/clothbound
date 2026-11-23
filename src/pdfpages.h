#pragma once

#include <QCache>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QRectF>
#include <QSet>
#include <QThreadPool>
#include <atomic>
#include <memory>

class PdfBook;

// A PDF's pages as images for the reading surface, rendered off the main
// thread at the size they're shown: image://pdf/<token>/<page>?bg=…&fg=…
// Pages are recolored to the reading theme (paper to the background, ink
// to the text color) while pictures keep their colors.
class PdfPages : public QQuickAsyncImageProvider {
public:
    PdfPages();
    ~PdfPages() override;

    static void add(const QString &token, std::shared_ptr<PdfBook> book);
    static void remove(const QString &token);
    static void prefetch(const QString &token, const QList<int> &pages, const QSize &requestedSize,
                         const QString &colorQuery);
    static void cancelPrefetch();

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Ink to `fg` and paper to `bg`; colored pixels (pictures) mostly kept.
    static void recolor(QImage &image, const QColor &bg, const QColor &fg);
    // The part of a page with anything on it (0 to 1 across and down).
    static QRectF contentBox(const QImage &page);
    // One crop for a whole book, from some of its pages: margins around the
    // text are cut, the same on every page so nothing jumps.
    static QRectF bookCrop(PdfBook &book);

    struct WorkerState {
        QString token;
        QString path;
        std::unique_ptr<PdfBook> book;
    };

private:
    friend class PageResponse;
    friend class PrefetchTask;
    static QString makeCacheKey(const QString &id, const QSize &requestedSize);
    static QSize renderSizeFor(const QSizeF &points, const QSize &requestedSize);
    static QString pathFor(const QString &token);
    static QImage cached(const QString &key);
    static void storeCached(const QString &token, const QString &key, const QImage &image);

    QThreadPool m_pool;
    std::shared_ptr<WorkerState> m_worker;

    static QMutex s_mutex;
    static PdfPages *s_instance;
    static std::atomic<quint64> s_prefetchGen;
    static QHash<QString, QString> s_paths;
    static QCache<QString, QImage> s_cache;
    static QHash<QString, QSet<QString>> s_tokenKeys;
};
