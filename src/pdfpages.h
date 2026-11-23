#pragma once

#include <QColor>
#include <QHash>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QRectF>
#include <memory>

class PdfBook;

// A PDF's pages as images for the reading surface, rendered off the main
// thread at the size they're shown: image://pdf/<token>/<page>?bg=…&fg=…
// Pages are recolored to the reading theme (paper to the background, ink
// to the text color) while pictures keep their colors.
class PdfPages : public QQuickAsyncImageProvider {
public:
    static void add(const QString &token, std::shared_ptr<PdfBook> book);
    static void remove(const QString &token);

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Ink to `fg` and paper to `bg`; colored pixels (pictures) mostly kept.
    static void recolor(QImage &image, const QColor &bg, const QColor &fg);
    // The part of a page with anything on it (0 to 1 across and down).
    static QRectF contentBox(const QImage &page);
    // One crop for a whole book, from some of its pages: margins around the
    // text are cut, the same on every page so nothing jumps.
    static QRectF bookCrop(PdfBook &book);

private:
    static std::shared_ptr<PdfBook> book(const QString &token);
    static QMutex s_mutex;
    static QHash<QString, std::shared_ptr<PdfBook>> s_books;
};
