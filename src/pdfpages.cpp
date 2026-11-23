#include "pdfpages.h"

#include "pdfbook.h"

#include <QRunnable>
#include <QThreadPool>
#include <QUrlQuery>

QMutex PdfPages::s_mutex;
QHash<QString, std::shared_ptr<PdfBook>> PdfPages::s_books;

namespace {

class PageResponse : public QQuickImageResponse, public QRunnable {
public:
    PageResponse(std::shared_ptr<PdfBook> book, int page, QSize size, QColor bg, QColor fg)
        : m_book(std::move(book)), m_page(page), m_size(size), m_bg(bg), m_fg(fg) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }

    void run() override {
        if (m_book) {
            const QSizeF points = m_book->pageSize(m_page);
            QSize size = m_size;
            if (!points.isEmpty()) {
                if (size.width() <= 0 && size.height() <= 0)
                    size = (points * 2).toSize();
                else if (size.height() <= 0)
                    size.setHeight(qRound(size.width() * points.height() / points.width()));
                else if (size.width() <= 0)
                    size.setWidth(qRound(size.height() * points.width() / points.height()));
            }
            m_image = m_book->renderPage(m_page, size.boundedTo(QSize(6000, 8000)));
            if (m_bg.isValid() && m_fg.isValid() && !m_image.isNull())
                PdfPages::recolor(m_image, m_bg, m_fg);
        }
        emit finished();
    }

private:
    std::shared_ptr<PdfBook> m_book;
    int m_page;
    QSize m_size;
    QColor m_bg, m_fg;
    QImage m_image;
};

}  // namespace

void PdfPages::add(const QString &token, std::shared_ptr<PdfBook> book) {
    QMutexLocker lock(&s_mutex);
    s_books.insert(token, std::move(book));
}

void PdfPages::remove(const QString &token) {
    QMutexLocker lock(&s_mutex);
    s_books.remove(token);
}

std::shared_ptr<PdfBook> PdfPages::book(const QString &token) {
    QMutexLocker lock(&s_mutex);
    return s_books.value(token);
}

QQuickImageResponse *PdfPages::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // "<token>/<page>?bg=%23rrggbb&fg=%23rrggbb"
    const QString path = id.section(QLatin1Char('?'), 0, 0);
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    auto *response = new PageResponse(book(path.section(QLatin1Char('/'), 0, 0)), path.section(QLatin1Char('/'), 1).toInt(),
                                      requestedSize, QColor(query.queryItemValue(QStringLiteral("bg"), QUrl::FullyDecoded)),
                                      QColor(query.queryItemValue(QStringLiteral("fg"), QUrl::FullyDecoded)));
    QThreadPool::globalInstance()->start(response);
    return response;
}

void PdfPages::recolor(QImage &image, const QColor &bg, const QColor &fg) {
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_RGB32);
    // White paper and black ink on a white page stay as they are.
    if (bg.lightness() > 245 && fg.lightness() < 40)
        return;
    const int br = bg.red(), bgG = bg.green(), bb = bg.blue();
    const int fr = fg.red(), fgG = fg.green(), fb = fg.blue();
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const int r = qRed(pixel), g = qGreen(pixel), b = qBlue(pixel);
            const int high = qMax(r, qMax(g, b)), low = qMin(r, qMin(g, b));
            const int light = (r * 299 + g * 587 + b * 114) / 1000;
            if (high - low < 48) {
                // Grays: paper to background, ink to text.
                line[x] = qRgb(fr + (br - fr) * light / 255, fgG + (bgG - fgG) * light / 255, fb + (bb - fb) * light / 255);
            } else {
                // Colors keep their hue, set a little toward the background.
                line[x] = qRgb((r * 4 + br) / 5, (g * 4 + bgG) / 5, (b * 4 + bb) / 5);
            }
        }
    }
}

QRectF PdfPages::contentBox(const QImage &page) {
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
        if (size.isEmpty())
            continue;
        const QRectF box = contentBox(book.renderPage(page, QSize(240, qRound(240 * size.height() / size.width()))));
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
    const auto near = [](const QList<double> &values, double share) { return values[qBound(qsizetype(0), qsizetype(qRound(share * (values.size() - 1))), values.size() - 1)]; };
    const double margin = 0.025;
    const double left = qMax(0.0, near(lefts, 0.1) - margin);
    const double top = qMax(0.0, near(tops, 0.1) - margin);
    const double right = qMin(1.0, near(rights, 0.9) + margin);
    const double bottom = qMin(1.0, near(bottoms, 0.9) + margin);
    if (right - left < 0.4 || bottom - top < 0.4)
        return QRectF(0, 0, 1, 1);
    return QRectF(left, top, right - left, bottom - top);
}
