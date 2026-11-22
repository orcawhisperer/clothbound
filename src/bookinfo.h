#pragma once

#include "bookname.h"

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>

// Everything the library keeps about one book file, read without opening
// it for reading. The scanner calls this from worker threads.
struct BookInfo {
    QString path;
    QString format;  // "epub" or "pdf"
    qint64 size = 0;
    QDateTime modified;
    BookIdentity identity;
    QString language;
    QString description;
    QString isbn;
    QString series;
    double seriesIndex = 0;
    QStringList subjects;
    int chapters = 0;
    // Printed pages: a PDF's page count, or an EPUB's page-list length.
    int pages = 0;
    QImage cover;
    // Why the book can't be read ("protected by DRM"), or empty.
    QString problem;

    bool ok() const { return problem.isEmpty(); }
};

namespace BookFiles {
// "epub", "pdf", or "" for files Dogear doesn't read.
QString formatOf(const QString &path);
bool isBook(const QString &path);
// Reads metadata, and the cover scaled to `coverHeight` when it's above 0.
BookInfo read(const QString &path, int coverHeight = 0);
}
