#include "pdfbook.h"

#include <QJsonObject>
#include <QPainter>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfSelection>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

PdfBook::PdfBook(const QString &path) : m_path(path), m_document(std::make_unique<QPdfDocument>()) {}

PdfBook::~PdfBook() = default;

bool PdfBook::open() {
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

int PdfBook::pageCount() const { return m_document->pageCount(); }

QString PdfBook::title() const {
    return m_document->metaData(QPdfDocument::MetaDataField::Title).toString().simplified();
}

QString PdfBook::subject() const {
    return m_document->metaData(QPdfDocument::MetaDataField::Subject).toString().simplified();
}

QString PdfBook::publisher() const { return {}; }

QDateTime PdfBook::created() const {
    return m_document->metaData(QPdfDocument::MetaDataField::CreationDate).toDateTime();
}

void PdfBook::splitPeople() const {
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
    splitPeople();
    return m_authors;
}

QStringList PdfBook::editors() const {
    splitPeople();
    return m_editors;
}

QList<PdfOutlineEntry> PdfBook::outline() const {
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

QImage PdfBook::renderCover(int height) const {
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
    if (page < 0 || page >= pageCount())
        return {};
    return m_document->getAllText(page).text();
}

QSizeF PdfBook::pageSize(int page) const { return m_document->pagePointSize(page); }

QString PdfBook::pageLabel(int page) const { return m_document->pageLabel(page); }

QList<QRectF> PdfBook::textRects(int page, qint64 start, qint64 end) const {
    QList<QRectF> out;
    const QSizeF size = pageSize(page);
    if (end <= start || size.isEmpty())
        return out;
    const QPdfSelection selection = m_document->getSelectionAtIndex(page, int(start), int(end - start));
    for (const QPolygonF &polygon : selection.bounds()) {
        const QRectF box = polygon.boundingRect();
        out.append(QRectF(box.x() / size.width(), box.y() / size.height(), box.width() / size.width(),
                          box.height() / size.height()));
    }
    return out;
}

PdfBook::Selection PdfBook::select(int page, const QPointF &from, const QPointF &to) const {
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
        if (!link.isValid())
            continue;
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
