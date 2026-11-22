#pragma once

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>
#include <memory>

class QPdfDocument;

struct PdfOutlineEntry {
    QString title;
    int page = 0;
    int depth = 0;
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
    QImage renderCover(int height) const;

private:
    void splitPeople() const;

    QString m_path;
    QString m_error;
    std::unique_ptr<QPdfDocument> m_document;
    mutable bool m_peopleSplit = false;
    mutable QStringList m_authors, m_editors;
};
