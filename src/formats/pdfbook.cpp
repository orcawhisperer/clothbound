#include "formats/pdfbook.h"

#include <QBuffer>
#include <QJsonObject>
#include <QMutexLocker>
#include <QPainter>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfSelection>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

PdfBook::PdfBook(const QString &path) : m_path(path), m_document(std::make_unique<QPdfDocument>()) {}

PdfBook::~PdfBook() = default;

bool PdfBook::open() {
    QMutexLocker lock(&m_mutex);
    m_peopleSplit = false;
    m_authors.clear();
    m_editors.clear();
    m_idsScanned = false;
    m_detectedDoi.clear();
    m_detectedArxiv.clear();
    switch (m_document->load(m_path)) {
    case QPdfDocument::Error::None:
        return true;
    case QPdfDocument::Error::FileNotFound:
        m_error = QStringLiteral("The file is missing.");
        break;
    case QPdfDocument::Error::IncorrectPassword:
        m_error = QStringLiteral("This PDF is locked with a password.");
        break;
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        m_error = QStringLiteral("This PDF is protected by DRM.");
        break;
    case QPdfDocument::Error::InvalidFileFormat:
        m_error = QStringLiteral("This isn't a readable PDF.");
        break;
    default:
        m_error = QStringLiteral("This PDF can't be opened.");
        break;
    }
    return false;
}

int PdfBook::pageCount() const {
    QMutexLocker lock(&m_mutex);
    return m_document->pageCount();
}

QString PdfBook::title() const {
    QMutexLocker lock(&m_mutex);
    return m_document->metaData(QPdfDocument::MetaDataField::Title).toString().simplified();
}

QString PdfBook::subject() const {
    QMutexLocker lock(&m_mutex);
    return m_document->metaData(QPdfDocument::MetaDataField::Subject).toString().simplified();
}

QString PdfBook::publisher() const { return {}; }

QDateTime PdfBook::created() const {
    QMutexLocker lock(&m_mutex);
    return m_document->metaData(QPdfDocument::MetaDataField::CreationDate).toDateTime();
}

void PdfBook::splitPeople() const {
    QMutexLocker lock(&m_mutex);
    if (m_peopleSplit)
        return;
    m_peopleSplit = true;
    const QString field = m_document->metaData(QPdfDocument::MetaDataField::Author).toString().simplified();
    static const QRegularExpression separators(R"(\s*(?:/|;|\|)\s*)");
    static const QRegularExpression editorMark(R"(\s*[\[(](?:eds?\.?|editor|edited by)[\])]\s*$)",
                                               QRegularExpression::CaseInsensitiveOption);
    for (QString person : field.split(separators, Qt::SkipEmptyParts)) {
        if (person.contains(editorMark)) {
            person.remove(editorMark);
            m_editors.append(person.trimmed());
        } else {
            m_authors.append(person.trimmed());
        }
    }
}

QStringList PdfBook::authors() const {
    QMutexLocker lock(&m_mutex);
    splitPeople();
    return m_authors;
}

QStringList PdfBook::editors() const {
    QMutexLocker lock(&m_mutex);
    splitPeople();
    return m_editors;
}

QList<PdfOutlineEntry> PdfBook::outline() const {
    QMutexLocker lock(&m_mutex);
    QList<PdfOutlineEntry> entries;
    QPdfBookmarkModel model;
    model.setDocument(m_document.get());
    std::function<void(const QModelIndex &, int)> walk = [&](const QModelIndex &parent, int depth) {
        for (int row = 0; row < model.rowCount(parent); ++row) {
            const QModelIndex index = model.index(row, 0, parent);
            PdfOutlineEntry entry;
            entry.title = model.data(index, int(QPdfBookmarkModel::Role::Title)).toString().simplified();
            entry.page = model.data(index, int(QPdfBookmarkModel::Role::Page)).toInt();
            entry.depth = depth;
            if (!entry.title.isEmpty())
                entries.append(entry);
            walk(index, depth + 1);
        }
    };
    walk(QModelIndex(), 0);
    return entries;
}

QImage PdfBook::renderPage(int page, const QSize &size) const {
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= pageCount() || size.width() <= 0 || size.height() <= 0)
        return {};
    constexpr int maxDim = 6000;
    constexpr qint64 maxPixels = 24LL * 1000 * 1000;
    int width = qBound(1, size.width(), maxDim);
    int height = qBound(1, size.height(), maxDim);
    if (qint64(width) * qint64(height) > maxPixels) {
        const double scale = std::sqrt(double(maxPixels) / (double(width) * double(height)));
        width = qMax(1, int(std::floor(width * scale)));
        height = qMax(1, int(std::floor(height * scale)));
    }
    const QSize bounded(width, height);
    // PDFium leaves unpainted areas transparent; books are printed on white.
    const QImage rendered = m_document->render(page, bounded);
    if (rendered.isNull())
        return {};
    QImage image(bounded, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.drawImage(0, 0, rendered);
    return image;
}

QImage PdfBook::renderRegionImage(int pageIndex, double normX, double normY, double normW, double normH,
                                  int targetWidthPx) const {
    QMutexLocker lock(&m_mutex);
    if (!m_document || pageIndex < 0 || pageIndex >= m_document->pageCount())
        return {};
    if (!std::isfinite(normX) || !std::isfinite(normY) || !std::isfinite(normW) || !std::isfinite(normH))
        return {};
    const double x1 = std::clamp(normX, 0.0, 1.0);
    const double y1 = std::clamp(normY, 0.0, 1.0);
    const double x2 = std::clamp(normX + normW, 0.0, 1.0);
    const double y2 = std::clamp(normY + normH, 0.0, 1.0);
    const double rw = x2 - x1;
    const double rh = y2 - y1;
    if (rw <= 0.0005 || rh <= 0.0005)
        return {};

    const QSizeF points = m_document->pagePointSize(pageIndex);
    if (points.width() <= 0.0 || points.height() <= 0.0 || !std::isfinite(points.width()) || !std::isfinite(points.height()))
        return {};

    const double regionPointsW = points.width() * rw;
    const double regionPointsH = points.height() * rh;
    const double regionAspect = std::clamp(regionPointsH / regionPointsW, 0.05, 20.0);

    constexpr int maxCropDim = 2400;
    int cropW = qBound(16, targetWidthPx, maxCropDim);
    int cropH = qMax(1, int(std::lround(double(cropW) * regionAspect)));
    if (cropH > maxCropDim) {
        cropH = maxCropDim;
        cropW = qMax(1, int(std::lround(double(cropH) / regionAspect)));
    }

    int fullW = qMax(cropW, int(std::lround(double(cropW) / rw)));
    int fullH = qMax(cropH, int(std::lround(double(cropH) / rh)));
    constexpr int maxVirtualDim = 12000;
    if (fullW > maxVirtualDim || fullH > maxVirtualDim) {
        const double shrink = std::min(double(maxVirtualDim) / double(fullW), double(maxVirtualDim) / double(fullH));
        fullW = qMax(1, int(std::floor(double(fullW) * shrink)));
        fullH = qMax(1, int(std::floor(double(fullH) * shrink)));
    }

    const int clipX = qBound(0, int(std::floor(x1 * double(fullW))), qMax(0, fullW - 1));
    const int clipY = qBound(0, int(std::floor(y1 * double(fullH))), qMax(0, fullH - 1));
    const int clipW = qBound(1, int(std::ceil(x2 * double(fullW))) - clipX, fullW - clipX);
    const int clipH = qBound(1, int(std::ceil(y2 * double(fullH))) - clipY, fullH - clipY);

    QPdfDocumentRenderOptions opts;
    opts.setScaledSize(QSize(fullW, fullH));
    opts.setScaledClipRect(QRect(clipX, clipY, clipW, clipH));
    QImage rendered = m_document->render(pageIndex, QSize(clipW, clipH), opts);
    if (rendered.isNull()) {
        // Fallback: render bounded full page and crop in memory.
        const QImage full = renderPage(pageIndex, QSize(qMin(fullW, 3200), qMin(fullH, 4200)));
        if (full.isNull())
            return {};
        const int fx = qBound(0, int(std::floor(x1 * full.width())), qMax(0, full.width() - 1));
        const int fy = qBound(0, int(std::floor(y1 * full.height())), qMax(0, full.height() - 1));
        const int fw = qBound(1, int(std::ceil(x2 * full.width())) - fx, full.width() - fx);
        const int fh = qBound(1, int(std::ceil(y2 * full.height())) - fy, full.height() - fy);
        return full.copy(fx, fy, fw, fh);
    }

    QImage image(rendered.size(), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.drawImage(0, 0, rendered);
    return image;
}

QByteArray PdfBook::renderRegionPng(int pageIndex, double normX, double normY, double normW, double normH,
                                    int targetWidthPx) const {
    const QImage region = renderRegionImage(pageIndex, normX, normY, normW, normH, targetWidthPx);
    if (region.isNull())
        return {};
    QByteArray out;
    QBuffer buffer(&out);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (!region.save(&buffer, "PNG"))
        return {};
    return out;
}

QImage PdfBook::renderCover(int height) const {
    QMutexLocker lock(&m_mutex);
    if (pageCount() < 1 || height <= 0)
        return {};
    const QSizeF points = m_document->pagePointSize(0);
    if (points.width() <= 0 || points.height() <= 0 || !std::isfinite(points.width()) || !std::isfinite(points.height()))
        return {};
    const int clampedHeight = qBound(1, height, 2048);
    const double ratio = std::clamp(points.width() / points.height(), 0.2, 5.0);
    const int width = qBound(1, qRound(clampedHeight * ratio), 2048);
    return renderPage(0, QSize(width, clampedHeight));
}

QString PdfBook::pageText(int page) const {
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= pageCount())
        return {};
    return m_document->getAllText(page).text();
}

QString PdfBook::pageTextInBand(int page, double normTop, double normBottom, int maxChars) const {
    QMutexLocker lock(&m_mutex);
    if (page < 0 || page >= pageCount() || maxChars <= 0)
        return {};
    const QSizeF size = pageSize(page);
    if (size.isEmpty())
        return {};
    const double top = std::clamp(normTop, 0.0, 1.0);
    const double bottom = std::clamp(normBottom, top, 1.0);
    const QString text = pageText(page);
    if (text.isEmpty())
        return {};

    QString out;
    qsizetype pos = 0;
    const qsizetype len = text.size();
    while (pos < len && out.size() < maxChars + 40) {
        qsizetype end = pos;
        while (end < len && text[end] != QLatin1Char('\n') && text[end] != QLatin1Char('\r'))
            ++end;
        const QString line = text.mid(pos, end - pos);
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            // Find the first non-whitespace char on this line and inspect its vertical bounds.
            qsizetype probeStart = pos;
            while (probeStart < end && text[probeStart].isSpace())
                ++probeStart;
            const int probeLen = int(qMin<qsizetype>(8, end - probeStart));
            const QPdfSelection sel = m_document->getSelectionAtIndex(page, int(probeStart), probeLen);
            bool overlaps = false;
            for (const QPolygonF &poly : sel.bounds()) {
                const QRectF r = poly.boundingRect();
                const double ry1 = r.top() / size.height();
                const double ry2 = r.bottom() / size.height();
                if (ry2 >= top && ry1 <= bottom) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                if (!out.isEmpty())
                    out.append(QLatin1Char(' '));
                out.append(trimmed);
            }
        }
        if (end >= len)
            break;
        pos = end + (text[end] == QLatin1Char('\r') && end + 1 < len && text[end + 1] == QLatin1Char('\n') ? 2 : 1);
    }
    out.replace(QChar(0xFFFE), QString());
    return out.simplified();
}

QSizeF PdfBook::pageSize(int page) const {
    QMutexLocker lock(&m_mutex);
    return m_document->pagePointSize(page);
}

QString PdfBook::pageLabel(int page) const {
    QMutexLocker lock(&m_mutex);
    return m_document->pageLabel(page);
}

QList<QRectF> PdfBook::mergeLineRects(const QList<QRectF> &raw) {
    // 1. Clean and clamp finite non-empty boxes to [0..1].
    QList<QRectF> boxes;
    boxes.reserve(raw.size());
    for (const QRectF &r : raw) {
        if (!std::isfinite(r.x()) || !std::isfinite(r.y()) || !std::isfinite(r.width()) ||
            !std::isfinite(r.height()))
            continue;
        const qreal x1 = std::clamp<qreal>(r.left(), 0.0, 1.0);
        const qreal y1 = std::clamp<qreal>(r.top(), 0.0, 1.0);
        const qreal x2 = std::clamp<qreal>(r.right(), 0.0, 1.0);
        const qreal y2 = std::clamp<qreal>(r.bottom(), 0.0, 1.0);
        if (x2 - x1 > 1e-6 && y2 - y1 > 1e-6)
            boxes.append(QRectF(x1, y1, x2 - x1, y2 - y1));
    }
    if (boxes.isEmpty())
        return {};

    // Two boxes belong to the same visual line when their vertical centers are close
    // relative to the taller box or their vertical spans overlap strongly.
    const auto sameVisualLine = [](const QRectF &a, const QRectF &b) {
        const qreal maxH = std::max(a.height(), b.height());
        const qreal minH = std::min(a.height(), b.height());
        if (maxH <= 0.0)
            return false;
        if (std::abs(a.center().y() - b.center().y()) <= 0.55 * maxH)
            return true;
        const qreal overlap = std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
        return minH > 0.0 && overlap >= 0.45 * minH;
    };

    // Normal inter-word spacing ceiling: bridges justified gaps while never bridging
    // across a 2-column academic PDF gutter.
    const auto maxWordGap = [](qreal refH) -> qreal {
        return std::clamp<qreal>(3.0 * refH, 0.022, 0.055);
    };

    const auto horizontalGap = [](const QRectF &a, const QRectF &b) -> qreal {
        return std::max(b.left() - a.right(), a.left() - b.right());
    };

    // 2. Traverse in reading order, merging boxes on the same visual line within normal inter-word spacing.
    struct LineBand {
        QRectF rect;
        qreal refHeight = 0.0;
    };
    QList<LineBand> bands;
    bands.reserve(boxes.size());

    for (const QRectF &box : boxes) {
        bool merged = false;
        for (qsizetype i = bands.size() - 1; i >= 0; --i) {
            LineBand &band = bands[i];
            const qreal refH = std::max(band.refHeight, box.height());
            if (sameVisualLine(band.rect, box) && horizontalGap(band.rect, box) <= maxWordGap(refH)) {
                band.rect = band.rect.united(box);
                band.refHeight = refH;
                merged = true;
                break;
            }
        }
        if (!merged)
            bands.append({box, box.height()});
    }

    // Fixed-point merge for any out-of-order tokens belonging to the same visual line segment.
    bool changed = true;
    while (changed) {
        changed = false;
        for (qsizetype i = 0; i < bands.size() && !changed; ++i) {
            for (qsizetype j = i + 1; j < bands.size(); ++j) {
                const qreal refH = std::max(bands[i].refHeight, bands[j].refHeight);
                if (sameVisualLine(bands[i].rect, bands[j].rect) &&
                    horizontalGap(bands[i].rect, bands[j].rect) <= maxWordGap(refH)) {
                    bands[i].rect = bands[i].rect.united(bands[j].rect);
                    bands[i].refHeight = refH;
                    bands.removeAt(j);
                    changed = true;
                    break;
                }
            }
        }
    }

    // 3. Normalize vertical band heights across body lines in the returned range so lines
    //    with only short x-height words match the full line envelope without distorting headings.
    QList<qreal> heights;
    heights.reserve(bands.size());
    for (const LineBand &b : bands)
        heights.append(b.rect.height());
    std::sort(heights.begin(), heights.end());

    qreal refLineH = heights[heights.size() / 2];
    if (heights.size() >= 3) {
        const qreal medianH = heights[heights.size() / 2];
        QList<qreal> bodyHeights;
        bodyHeights.reserve(heights.size());
        for (qreal h : heights) {
            if (h <= medianH * 1.6)
                bodyHeights.append(h);
        }
        if (!bodyHeights.isEmpty())
            refLineH = bodyHeights[bodyHeights.size() * 2 / 3];
    }

    QList<QRectF> out;
    out.reserve(bands.size());
    for (const LineBand &b : bands) {
        const QRectF &r = b.rect;
        qreal h = r.height();
        if (h < refLineH && h >= refLineH * 0.20 && refLineH <= h * 2.1)
            h = refLineH;
        const qreal padY = std::clamp<qreal>(h * 0.06, 0.0004, 0.0020);
        const qreal targetH = h + 2.0 * padY;
        const qreal cy = r.center().y();
        const qreal top = std::clamp<qreal>(cy - targetH * 0.5, 0.0, 1.0);
        const qreal bottom = std::clamp<qreal>(cy + targetH * 0.5, top, 1.0);
        out.append(QRectF(r.left(), top, r.width(), bottom - top));
    }

    // 4. Keep consecutive bands in the same column vertically non-overlapping so translucent
    //    highlight overlays never create dark double-alpha seams between lines.
    constexpr qreal minLineSep = 0.0008;
    for (qsizetype i = 0; i < out.size(); ++i) {
        for (qsizetype j = i + 1; j < out.size(); ++j) {
            QRectF &a = out[i];
            QRectF &b = out[j];
            if (std::min(a.right(), b.right()) <= std::max(a.left(), b.left()))
                continue;
            QRectF *upper = a.center().y() <= b.center().y() ? &a : &b;
            QRectF *lower = upper == &a ? &b : &a;
            if (upper->bottom() > lower->top() - minLineSep) {
                const qreal mid = 0.5 * (upper->bottom() + lower->top());
                const qreal newUpperBottom = std::max(upper->top() + 1e-4, mid - 0.5 * minLineSep);
                const qreal newLowerTop = std::min(lower->bottom() - 1e-4, mid + 0.5 * minLineSep);
                if (newUpperBottom < newLowerTop) {
                    upper->setBottom(newUpperBottom);
                    lower->setTop(newLowerTop);
                }
            }
        }
    }

    return out;
}

QList<QRectF> PdfBook::textRects(int page, qint64 start, qint64 end) const {
    QMutexLocker lock(&m_mutex);
    QList<QRectF> raw;
    const QSizeF size = pageSize(page);
    if (end <= start || size.isEmpty())
        return raw;
    const QPdfSelection selection = m_document->getSelectionAtIndex(page, int(start), int(end - start));
    const QList<QPolygonF> bounds = selection.bounds();
    raw.reserve(bounds.size());
    for (const QPolygonF &polygon : bounds) {
        const QRectF box = polygon.boundingRect();
        raw.append(QRectF(box.x() / size.width(), box.y() / size.height(), box.width() / size.width(),
                          box.height() / size.height()));
    }
    return mergeLineRects(raw);
}

PdfBook::Selection PdfBook::select(int page, const QPointF &from, const QPointF &to) const {
    QMutexLocker lock(&m_mutex);
    Selection out;
    const QSizeF size = pageSize(page);
    if (size.isEmpty())
        return out;
    const QPdfSelection selection = m_document->getSelection(page, QPointF(from.x() * size.width(), from.y() * size.height()),
                                                             QPointF(to.x() * size.width(), to.y() * size.height()));
    if (!selection.isValid() || selection.text().isEmpty())
        return out;
    out.start = selection.startIndex();
    out.end = selection.endIndex();
    out.text = selection.text();
    return out;
}

PdfBook::Selection PdfBook::selectWord(int page, const QPointF &at) const {
    QMutexLocker lock(&m_mutex);
    Selection out;
    const QSizeF size = pageSize(page);
    if (page < 0 || page >= pageCount() || size.isEmpty())
        return out;
    const QPointF pt(at.x() * size.width(), at.y() * size.height());
    QPdfSelection probe = m_document->getSelection(page, pt - QPointF(2.0, 0.0), pt + QPointF(2.0, 0.0));
    if (!probe.isValid() || probe.text().isEmpty())
        probe = m_document->getSelection(page, pt - QPointF(4.0, 1.5), pt + QPointF(4.0, 1.5));
    if (!probe.isValid() || probe.text().isEmpty())
        return out;
    const QString text = pageText(page);
    if (text.isEmpty())
        return out;
    const qsizetype len = text.size();
    const auto isWordChar = [](QChar ch) { return ch.isLetterOrNumber() || ch == QLatin1Char('_'); };
    const auto isApostrophe = [](QChar ch) { return ch == QLatin1Char('\'') || ch == QChar(0x2019); };

    qsizetype anchor = std::clamp<qsizetype>(qsizetype(probe.startIndex() + probe.endIndex()) / 2, 0, len - 1);
    if (!isWordChar(text[anchor])) {
        const qsizetype lo = std::clamp<qsizetype>(qsizetype(probe.startIndex()), 0, len - 1);
        const qsizetype hi = std::clamp<qsizetype>(qsizetype(probe.endIndex()), 0, len);
        bool found = false;
        for (qsizetype i = lo; i < hi; ++i) {
            if (isWordChar(text[i])) {
                anchor = i;
                found = true;
                break;
            }
        }
        if (!found)
            return out;
    }
    qsizetype start = anchor;
    while (start > 0) {
        const QChar prev = text[start - 1];
        if (isWordChar(prev))
            --start;
        else if (isApostrophe(prev) && start >= 2 && isWordChar(text[start - 2]))
            --start;
        else
            break;
    }
    qsizetype end = anchor + 1;
    while (end < len) {
        const QChar next = text[end];
        if (isWordChar(next))
            ++end;
        else if (isApostrophe(next) && end + 1 < len && isWordChar(text[end + 1]))
            ++end;
        else
            break;
    }
    if (end <= start)
        return out;
    out.start = start;
    out.end = end;
    out.text = text.mid(start, end - start);
    return out;
}

QList<PdfLinkEntry> PdfBook::pageLinks(int page) const {
    QMutexLocker lock(&m_mutex);
    QList<PdfLinkEntry> entries;
    const int total = pageCount();
    const QSizeF size = pageSize(page);
    if (page < 0 || page >= total || size.isEmpty())
        return entries;
    QPdfLinkModel model;
    model.setDocument(m_document.get());
    model.setPage(page);
    const int rows = model.rowCount(QModelIndex());
    entries.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex idx = model.index(row, 0);
        const QPdfLink link = model.data(idx, int(QPdfLinkModel::Role::Link)).value<QPdfLink>();
        // Note: Qt6's QPdfLink::isValid() only checks page() >= 0, which is false for
        // external URI links (page == -1). Validate internal targetPage and external url directly.
        const int targetPage = link.page();
        const QUrl url = link.url();
        const bool internal = targetPage >= 0 && targetPage < total;
        const bool external =
            url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https") ||
                              url.scheme() == QLatin1String("mailto"));
        if (!internal && !external)
            continue;
        PdfLinkEntry entry;
        entry.page = internal ? targetPage : -1;
        if (internal) {
            const QPointF loc = link.location();
            const QSizeF destSize = pageSize(targetPage);
            if (destSize.height() > 0.0 && std::isfinite(loc.y()) && loc.y() >= 0.0)
                entry.destY = std::clamp(loc.y() / destSize.height(), 0.0, 1.0);
        }
        if (external)
            entry.url = url;
        for (const QRectF &box : link.rectangles()) {
            const qreal x = qBound<qreal>(0.0, box.x() / size.width(), 1.0);
            const qreal y = qBound<qreal>(0.0, box.y() / size.height(), 1.0);
            const qreal r = qBound<qreal>(x, box.right() / size.width(), 1.0);
            const qreal b = qBound<qreal>(y, box.bottom() / size.height(), 1.0);
            if (r - x > 0.0005 && b - y > 0.0005)
                entry.rects.append(QRectF(x, y, r - x, b - y));
        }
        if (!entry.rects.isEmpty())
            entries.append(std::move(entry));
    }
    return entries;
}

QString PdfBook::extractDoi(const QString &text) {
    if (text.isEmpty())
        return {};
    static const QRegularExpression doiRe(QStringLiteral(R"(\b(10\.\d{4,9}/[-._;()/:A-Za-z0-9]+))"));
    QRegularExpressionMatchIterator it = doiRe.globalMatch(text);
    while (it.hasNext()) {
        QString doi = it.next().captured(1);
        while (!doi.isEmpty()) {
            const QChar last = doi.back();
            if (last == QLatin1Char('.') || last == QLatin1Char(',') || last == QLatin1Char(';') ||
                last == QLatin1Char(':') || last == QLatin1Char(']') || last == QLatin1Char('}') ||
                last == QLatin1Char('>') || last == QLatin1Char('\'') || last == QLatin1Char('"') ||
                last == QLatin1Char('!') || last == QLatin1Char('?')) {
                doi.chop(1);
                continue;
            }
            if (last == QLatin1Char(')') && doi.count(QLatin1Char(')')) > doi.count(QLatin1Char('('))) {
                doi.chop(1);
                continue;
            }
            break;
        }
        const qsizetype slash = doi.indexOf(QLatin1Char('/'));
        if (slash > 0 && slash < doi.size() - 1)
            return doi;
    }
    return {};
}

QString PdfBook::extractArxivId(const QString &text) {
    if (text.isEmpty())
        return {};
    static const QRegularExpression arxivRe(QStringLiteral(R"(\barXiv:\s*(\d{4}\.\d{4,5}(?:v\d+)?)\b)"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = arxivRe.match(text); m.hasMatch())
        return QStringLiteral("arXiv:") + m.captured(1);

    static const QRegularExpression urlRe(QStringLiteral(R"(arxiv\.org/(?:abs|pdf)/(\d{4}\.\d{4,5}(?:v\d+)?)\b)"),
                                          QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = urlRe.match(text); m.hasMatch())
        return QStringLiteral("arXiv:") + m.captured(1);

    static const QRegularExpression doiArxivRe(QStringLiteral(R"(10\.48550/arXiv\.(\d{4}\.\d{4,5}(?:v\d+)?)\b)"),
                                               QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = doiArxivRe.match(text); m.hasMatch())
        return QStringLiteral("arXiv:") + m.captured(1);

    return {};
}

void PdfBook::detectAcademicIds() const {
    QMutexLocker lock(&m_mutex);
    if (m_idsScanned)
        return;
    m_idsScanned = true;
    m_detectedDoi.clear();
    m_detectedArxiv.clear();
    if (!m_document)
        return;

    const int pagesToScan = qMin(2, m_document->pageCount());
    for (int p = 0; p < pagesToScan && (m_detectedDoi.isEmpty() || m_detectedArxiv.isEmpty()); ++p) {
        const QString text = pageText(p);
        if (m_detectedDoi.isEmpty())
            m_detectedDoi = extractDoi(text);
        if (m_detectedArxiv.isEmpty())
            m_detectedArxiv = extractArxivId(text);
    }
    if (m_detectedDoi.isEmpty() || m_detectedArxiv.isEmpty()) {
        const QString meta =
            m_document->metaData(QPdfDocument::MetaDataField::Subject).toString() + QLatin1Char(' ') +
            m_document->metaData(QPdfDocument::MetaDataField::Keywords).toString() + QLatin1Char(' ') +
            m_document->metaData(QPdfDocument::MetaDataField::Title).toString();
        if (m_detectedDoi.isEmpty())
            m_detectedDoi = extractDoi(meta);
        if (m_detectedArxiv.isEmpty())
            m_detectedArxiv = extractArxivId(meta);
    }
}

QString PdfBook::detectedDoi() const {
    QMutexLocker lock(&m_mutex);
    detectAcademicIds();
    return m_detectedDoi;
}

QString PdfBook::detectedArxiv() const {
    QMutexLocker lock(&m_mutex);
    detectAcademicIds();
    return m_detectedArxiv;
}

QJsonArray PdfBook::speakable(const QString &pageText, const QString &bookTitle) {
    // Lines, with where each begins.
    struct Line {
        qint64 start;
        QString text;
    };
    QList<Line> lines;
    qsizetype pos = 0;
    const qsizetype len = pageText.size();
    while (pos <= len) {
        qsizetype end = pos;
        while (end < len && pageText[end] != QLatin1Char('\n') && pageText[end] != QLatin1Char('\r'))
            ++end;
        lines.append({pos, pageText.mid(pos, end - pos)});
        if (end >= len)
            break;
        pos = end + (pageText[end] == QLatin1Char('\r') && end + 1 < len && pageText[end + 1] == QLatin1Char('\n') ? 2 : 1);
    }
    const auto isFurniture = [&bookTitle](const QString &line) {
        const QString t = line.trimmed();
        static const QRegularExpression number(QStringLiteral("^[\\[(]?[0-9ivxlcdmIVXLCDM]{1,6}[\\])]?$"));
        if (number.match(t).hasMatch())
            return true;
        // A running head: short, and the book's title or all capitals —
        // never a chapter or section heading like "CHAPTER ONE" or "PROLOGUE".
        if (t.size() < 60 && !bookTitle.isEmpty() && t.compare(bookTitle, Qt::CaseInsensitive) == 0)
            return true;
        static const QRegularExpression heading(
            QStringLiteral("^(?:CHAPTER|PART|BOOK|SECTION|PROLOGUE|EPILOGUE|INTRODUCTION|PREFACE|FOREWORD|AFTERWORD|APPENDIX|ACT|SCENE|CANTO)\\b"));
        if (heading.match(t).hasMatch())
            return false;
        return t.size() < 50 && t.size() > 3 && t == t.toUpper() && t.contains(QRegularExpression(QStringLiteral("[A-Z]{3}")));
    };
    // Drop a page number or running head at the top and bottom.
    qsizetype first = 0, last = lines.size() - 1;
    while (first <= last && lines[first].text.trimmed().isEmpty())
        ++first;
    while (last >= first && lines[last].text.trimmed().isEmpty())
        --last;
    // (A page number and a running head can share the top.)
    for (int dropped = 0; dropped < 2 && first <= last && isFurniture(lines[first].text); ++dropped)
        ++first;
    for (int dropped = 0; dropped < 2 && last >= first && isFurniture(lines[last].text); ++dropped)
        --last;
    // A paragraph ends at a blank line, or after a short line that ends a
    // sentence.
    int typical = 0;
    {
        QList<int> lengths;
        for (qsizetype i = first; i <= last; ++i)
            if (lines[i].text.trimmed().size() > 20)
                lengths.append(int(lines[i].text.trimmed().size()));
        std::sort(lengths.begin(), lengths.end());
        typical = lengths.isEmpty() ? 60 : lengths[lengths.size() * 3 / 4];
    }
    QJsonArray blocks;
    qint64 blockStart = -1, blockEnd = -1;
    QString text;
    const auto flush = [&] {
        if (blockStart >= 0 && !text.trimmed().isEmpty())
            blocks.append(QJsonObject{{"start", double(blockStart)}, {"end", double(blockEnd)}, {"text", text}, {"kind", "text"}});
        blockStart = -1;
        text.clear();
    };
    for (qsizetype i = first; i <= last; ++i) {
        const QString line = lines[i].text;
        if (line.trimmed().isEmpty()) {
            flush();
            continue;
        }
        if (blockStart < 0) {
            blockStart = lines[i].start;
            text.clear();
        } else {
            // The text stays as long as the page's, so offsets hold: a line
            // break is a space, and a word broken across lines is mended with
            // a soft hyphen and a zero-width space, which aren't spoken.
            const qint64 gap = lines[i].start - blockEnd;
            // PDFium marks a hyphen it found at a line's end as U+FFFE.
            const bool broken = text.endsWith(QChar(0xFFFE)) || text.endsWith(QChar(0x00AD)) ||
                                (text.endsWith(QLatin1Char('-')) && line.trimmed().front().isLower());
            if (broken) {
                text.chop(1);
                text += QChar(0x00AD) + QString(gap, QChar(0x200B));
            } else {
                text += QString(gap, QLatin1Char(' '));
            }
        }
        text += line;
        blockEnd = lines[i].start + line.size();
        const QString trimmed = line.trimmed();
        const bool ends = trimmed.endsWith(QLatin1Char('.')) || trimmed.endsWith(QLatin1Char('?')) ||
                          trimmed.endsWith(QLatin1Char('!')) || trimmed.endsWith(QChar(0x201D)) ||
                          trimmed.endsWith(QLatin1Char('"')) || trimmed.endsWith(QLatin1Char(':'));
        if (ends && trimmed.size() < typical * 0.8)
            flush();
    }
    flush();
    return blocks;
}
