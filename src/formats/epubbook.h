#pragma once

#include "formats/ziparchive.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

struct EpubMetadata {
    QString title;
    QStringList authors;
    QStringList editors;
    QStringList contributors;
    QString publisher;
    QString date;
    QString language;
    QString description;
    QString identifier;
    QString isbn;
    QString series;
    double seriesIndex = 0;
    QStringList subjects;
};

struct EpubItem {
    QString id;
    QString path;
    QString mediaType;
    QString properties;
};

struct EpubSpineItem {
    QString id;
    QString path;
    QString mediaType;
    bool linear = true;
};

struct EpubTocEntry {
    QString title;
    QString path;
    QString fragment;
    int depth = 0;
    int spine = -1;
};

struct EpubPageTarget {
    QString label;
    QString path;
    QString fragment;
    int spine = -1;
};

// An EPUB 2 or 3 book, read straight from its ZIP: the package's metadata,
// manifest and spine, the table of contents (navigation document or NCX),
// the print page list, and the cover. Read-only and safe to share between
// threads once opened.
class EpubBook {
public:
    explicit EpubBook(const QString &filePath);

    bool open();
    QString errorString() const { return m_error; }
    QString filePath() const { return m_zip.path(); }
    // True when opened from an unzipped `.epub/` directory bundle (e.g. Apple Books).
    bool isDirectoryPackage() const { return m_isDirectory; }
    static bool isUnzippedPackage(const QString &path);
    // Content encrypted by a store (Adobe, Kindle, Kobo) rather than the
    // font obfuscation every reader undoes.
    bool isDrmProtected() const { return m_drm; }
    bool isFixedLayout() const { return m_fixedLayout; }
    bool isRightToLeft() const { return m_rightToLeft; }
    QString version() const { return m_version; }

    const EpubMetadata &metadata() const { return m_metadata; }
    const QList<EpubSpineItem> &spine() const { return m_spine; }
    const QList<EpubTocEntry> &toc() const { return m_toc; }
    const QList<EpubPageTarget> &pageList() const { return m_pageList; }
    const EpubItem *item(const QString &path) const;
    int spineIndex(const QString &path) const { return m_spineIndex.value(path, -1); }
    QString coverPath() const { return m_coverPath; }

    // An entry's bytes, with embedded fonts de-obfuscated, and its type.
    QByteArray resource(const QString &path, QString *mediaType = nullptr, QString *error = nullptr) const;
    QString mediaType(const QString &path) const;
    QByteArray coverImage(QString *mediaType = nullptr) const;
    // The chapter's text exactly as the reader counts it (see extractText).
    QString chapterText(int spineIndex) const;
    // Uncompressed byte length of a spine chapter straight from the ZIP
    // central directory (O(1), zero inflation); 0 for non-text entries.
    qint64 chapterUncompressedBytes(int spineIndex) const;
    // Fast O(1) text-character estimate for a spine chapter derived from ZIP
    // central directory uncompressed byte size (zero chapter inflation).
    int estimatedChapterLength(int spineIndex) const;
    QList<int> estimatedChapterLengths() const;
    quint64 inflatedBytes() const { return m_zip.inflatedBytes(); }

    // The text of a chapter's <body>: every text node in document order,
    // outside script and style, with soft hyphens removed. Positions,
    // search hits, and highlights are offsets into this string, and the
    // reading surface counts the same way in the page's DOM.
    static QString extractText(const QByteArray &document);
    // A link's target inside the book: `href` relative to the file `base`,
    // percent-decoded and normalized. External links resolve to "".
    static QString resolve(const QString &base, const QString &href, QString *fragment = nullptr);
    static QString mediaTypeForPath(const QString &path);

private:
    bool fail(const QString &message);
    bool openDirectory(const QString &dirPath);
    bool containerContains(const QString &name) const;
    QString containerResolve(const QString &name) const;
    QStringList containerNames() const;
    qint64 containerSize(const QString &name) const;
    bool containerEncrypted(const QString &name) const;
    QByteArray containerRead(const QString &name, QString *error = nullptr) const;
    bool readPackage(const QString &opfPath);
    void readEncryption();
    void readNavigation(const QString &navPath);
    void readNcx(const QString &ncxPath);
    void findCover(const QString &coverId, const QString &guideCover);
    QString firstImageIn(const QString &documentPath) const;
    QByteArray deobfuscate(const QString &path, QByteArray data) const;

    ZipArchive m_zip;
    bool m_isDirectory = false;
    QString m_dirRoot;
    QString m_dirRootPrefix;
    QStringList m_dirNames;
    QHash<QString, QString> m_dirFolded;
    QHash<QString, qint64> m_dirSizes;
    QString m_error;
    QString m_opfPath;
    QString m_version;
    QString m_uniqueIdentifier;
    QStringList m_identifiers;
    EpubMetadata m_metadata;
    QHash<QString, EpubItem> m_manifest;  // by path
    QHash<QString, QString> m_idToPath;
    QList<EpubSpineItem> m_spine;
    QHash<QString, int> m_spineIndex;
    QList<EpubTocEntry> m_toc;
    QList<EpubPageTarget> m_pageList;
    QHash<QString, QString> m_encryption;  // path → algorithm
    QString m_coverPath;
    bool m_drm = false;
    bool m_fixedLayout = false;
    bool m_rightToLeft = false;
};
