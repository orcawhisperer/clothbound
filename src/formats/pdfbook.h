#pragma once

#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>

class QPdfDocument;

struct PdfOutlineEntry {
    QString title;
    int page = 0;
    int depth = 0;
};

struct PdfLinkEntry {
    QList<QRectF> rects;
    int page = -1;
    QUrl url;
};

// A PDF book through QtPdf (PDFium): what the library needs to know about
// it, its outline, and its pages as images.
class PdfBook {
public:
    explicit PdfBook(const QString &path);
    ~PdfBook();

    bool open();
    QString errorString() const { return m_error; }
    QString path() const { return m_path; }
    QPdfDocument *document() const { return m_document.get(); }

    int pageCount() const;
    QString title() const;
    // "H. G. Wells / Martin A. Danahay [editor]" is split into the author
    // and the editor.
    QStringList authors() const;
    QStringList editors() const;
    QString subject() const;
    QString publisher() const;
    QDateTime created() const;
    QList<PdfOutlineEntry> outline() const;

    // The page on white, scaled to `size` (or to `height` keeping its shape).
    QImage renderPage(int page, const QSize &size) const;

    // --- Text, for search, marks, the companion, and listening ----------
    // A page's text, as PDFium orders it; offsets into it are where things
    // are on the page.
    QString pageText(int page) const;
    // The page's size in points.
    QSizeF pageSize(int page) const;
    // Where a stretch of the page's text is, as rectangles in parts of the
    // page (0 to 1 across and down).
    QList<QRectF> textRects(int page, qint64 start, qint64 end) const;
    // The text between two points (parts of the page): {start, end, text}.
    struct Selection {
        qint64 start = -1;
        qint64 end = -1;
        QString text;
    };
    Selection select(int page, const QPointF &from, const QPointF &to) const;
    Selection selectWord(int page, const QPointF &at) const;
    // Internal and external links on a page, with normalized 0..1 rectangles.
    QList<PdfLinkEntry> pageLinks(int page) const;
    // A page's paragraphs to read aloud, as the reading surface gives them
    // ({start, end, text, kind}): lines joined, running heads and page
    // numbers left out, words broken across lines mended.
    static QJsonArray speakable(const QString &pageText, const QString &bookTitle = {});
    // The printed page's own label ("xii", "47"), if the PDF has labels.
    QString pageLabel(int page) const;
    QImage renderCover(int height) const;

private:
    void splitPeople() const;

    QString m_path;
    QString m_error;
    std::unique_ptr<QPdfDocument> m_document;
    mutable bool m_peopleSplit = false;
    mutable QStringList m_authors, m_editors;
};
