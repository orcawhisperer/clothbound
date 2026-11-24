#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QUrl>

struct ExtractedImage {
    QUrl sourceUrl;     // Resolved absolute http(s) URL
    QString localPath;  // Planned relative path inside OEBPS/, e.g. "images/img-001.jpg"
    QString alt;
};

struct ExtractedHeading {
    QString id;     // Unique id emitted in bodyXhtml, e.g. "sec-1"
    QString title;
    int level = 2;  // 1..6
};

struct ExtractedArticle {
    QString title;
    QString author;
    QString siteName;
    QString publishedDate;
    QString description;
    QUrl canonicalUrl;
    QString language;
    QUrl leadImageUrl;
    QString bodyXhtml;
    QString plainText;
    QList<ExtractedImage> images;
    QList<ExtractedHeading> headings;
    int wordCount = 0;
    int readingMinutes = 0;
};

// Pure C++20 Readability-style HTML article extractor.
// Extracts metadata, strips non-content chrome, scores candidate containers,
// sanitizes the primary body into strictly well-formed XHTML, and prepares
// image asset references for offline EPUB packaging.
class ArticleExtractor {
public:
    static ExtractedArticle extract(const QByteArray &htmlBytes, const QUrl &pageUrl);
    static ExtractedArticle extract(const QString &html, const QUrl &pageUrl);

    // Rewrites <img src="..."/> references in sanitized bodyXhtml using the map
    // of planned localPath -> final bundled localPath, cleanly removing any
    // images that failed to download or were skipped.
    static QString finalizeImages(const QString &bodyXhtml, const QHash<QString, QString> &downloadedPathMap);
};
