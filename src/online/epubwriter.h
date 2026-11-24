#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

struct EpubImageAsset {
    QString path;       // Relative to OEBPS/, e.g. "images/img-001.jpg"
    QString mediaType;  // e.g. "image/jpeg", "image/png", "image/webp"
    QByteArray data;
};

struct EpubHeading {
    QString id;     // Element id in OEBPS/article.xhtml
    QString title;  // Plain-text heading label
    int level = 2;  // 1..6
};

struct EpubArticle {
    QString title;
    QString author;
    QString publisher;      // Site name or host
    QString publishedDate;  // ISO-8601 date string if known
    QString language;       // BCP-47 tag, defaults to "en"
    QString sourceUrl;      // Canonical or fetched URL
    QString description;
    QStringList subjects;
    int readingMinutes = 0;
    QString bodyXhtml;      // Well-formed XHTML fragment for the article body
    QList<EpubHeading> headings;
    QList<EpubImageAsset> images;
    QString identifier;     // Optional unique package identifier; auto-generated if empty
};

// Pure C++20 + zlib EPUB 3.0 archive writer for offline web articles.
// Writes an OCF-compliant ZIP (`mimetype` first, stored uncompressed with zero
// extra-field bytes) atomically via QSaveFile.
class EpubWriter {
public:
    static bool writeArticle(const QString &outputPath, const EpubArticle &article, QString *error = nullptr);
    static QByteArray buildCoverSvg(const QString &title, const QString &author, const QString &siteName);
    static QString xmlEscape(QStringView text);
};
