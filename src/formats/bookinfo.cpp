#include "formats/bookinfo.h"

#include "formats/epubbook.h"
#include "formats/pdfbook.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QPainter>
#include <mutex>

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
constexpr int maxCoverDimension = 16384;
constexpr qint64 maxCoverPixels = 40ll * 1024 * 1024;

QImage scaledCover(QImage image, int height) {
    if (image.isNull() || height <= 0)
        return {};
    const QSize bounds(qMax(1, height * 4), height);
    if (image.height() > bounds.height() || image.width() > bounds.width())
        image = image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (image.hasAlphaChannel()) {
        // Composite transparent covers on white instead of turning alpha black.
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        return flat;
    }
    return image.convertToFormat(QImage::Format_RGB32);
}

QImage decodeImage(const QByteArray &bytes, int height) {
    if (bytes.isEmpty() || height <= 0)
        return {};
    static std::once_flag limitOnce;
    std::call_once(limitOnce, [] { QImageReader::setAllocationLimit(64); });
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    // Covers can be enormous or have extreme aspect ratios; shrink while
    // decoding and refuse images that would exceed safe pixel limits.
    const QSize size = reader.size();
    if (size.isValid()) {
        if (size.width() <= 0 || size.height() <= 0)
            return {};
        const bool transposed = (int(reader.transformation()) & int(QImageIOHandler::TransformationRotate90)) != 0;
        const QSize bounds = transposed ? QSize(height, qMax(1, height * 4)) : QSize(qMax(1, height * 4), height);
        const qint64 pixels = qint64(size.width()) * qint64(size.height());
        const bool oversized = size.width() > maxCoverDimension || size.height() > maxCoverDimension || pixels > maxCoverPixels;
        const bool needsScale = oversized || size.height() > bounds.height() * 2 || size.width() > bounds.width();
        if (needsScale) {
            if (reader.supportsOption(QImageIOHandler::ScaledSize)) {
                QSize target = size.scaled(bounds, Qt::KeepAspectRatio);
                target.setWidth(qMax(1, target.width()));
                target.setHeight(qMax(1, target.height()));
                reader.setScaledSize(target);
            } else if (oversized) {
                return {};
            }
        }
    }
    return scaledCover(reader.read(), height);
}
}

BookInfo read(const QString &path, int coverHeight, QList<int> *chapterLengths) {
    BookInfo info;
    const QFileInfo file(path);
    const QString resolved = file.canonicalFilePath();
    info.path = resolved.isEmpty() ? file.absoluteFilePath() : resolved;
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
        if (chapterLengths && info.ok()) {
            chapterLengths->clear();
            chapterLengths->reserve(book.spine().size());
            for (int i = 0; i < book.spine().size(); ++i)
                chapterLengths->append(int(book.chapterText(i).size()));
        }
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
