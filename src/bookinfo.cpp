#include "bookinfo.h"

#include "epubbook.h"
#include "pdfbook.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>

namespace BookFiles {
QString formatOf(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "epub" || suffix == "kepub")
        return QStringLiteral("epub");
    if (suffix == "pdf")
        return QStringLiteral("pdf");
    return {};
}

bool isBook(const QString &path) { return !formatOf(path).isEmpty(); }

namespace {
QImage scaledCover(QImage image, int height) {
    if (image.isNull() || height <= 0)
        return {};
    if (image.height() > height)
        image = image.scaledToHeight(height, Qt::SmoothTransformation);
    return image.convertToFormat(QImage::Format_RGB32);
}

QImage decodeImage(const QByteArray &bytes, int height) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    // Covers can be enormous; let the decoder shrink them while reading.
    const QSize size = reader.size();
    if (size.isValid() && height > 0 && size.height() > height * 2)
        reader.setScaledSize(size.scaled(QSize(height * 4, height), Qt::KeepAspectRatio));
    return scaledCover(reader.read(), height);
}
}

BookInfo read(const QString &path, int coverHeight) {
    BookInfo info;
    const QFileInfo file(path);
    info.path = file.absoluteFilePath();
    info.format = formatOf(path);
    info.size = file.size();
    info.modified = file.lastModified();
    if (!file.exists()) {
        info.problem = QStringLiteral("The file is missing.");
        return info;
    }

    if (info.format == "epub") {
        EpubBook book(path);
        if (!book.open()) {
            info.problem = book.errorString();
            info.identity = BookName::identify(path, {}, {}, {}, {}, {});
            return info;
        }
        const EpubMetadata &meta = book.metadata();
        info.identity = BookName::identify(path, meta.title, meta.authors, meta.editors, meta.publisher, meta.date);
        info.language = meta.language;
        info.description = meta.description;
        info.isbn = meta.isbn;
        info.series = meta.series;
        info.seriesIndex = meta.seriesIndex;
        info.subjects = meta.subjects;
        info.chapters = int(book.spine().size());
        info.pages = int(book.pageList().size());
        if (book.isDrmProtected())
            info.problem = QStringLiteral("This book is protected by DRM.");
        if (coverHeight > 0)
            info.cover = decodeImage(book.coverImage(), coverHeight);
    } else if (info.format == "pdf") {
        PdfBook book(path);
        if (!book.open()) {
            info.problem = book.errorString();
            info.identity = BookName::identify(path, {}, {}, {}, {}, {});
            return info;
        }
        // A PDF's creation date is when it was made or scanned, not when the
        // book was published, so only the file name can give a year.
        info.identity = BookName::identify(path, book.title(), book.authors(), book.editors(), {}, {});
        info.description = book.subject();
        info.pages = book.pageCount();
        info.chapters = int(book.outline().size());
        if (coverHeight > 0)
            info.cover = scaledCover(book.renderCover(coverHeight), coverHeight);
    } else {
        info.problem = QStringLiteral("Dogear reads EPUB and PDF books.");
    }
    return info;
}
}
