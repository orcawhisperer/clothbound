#pragma once

#include "formats/bookrecord.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QVariantList>

#include <memory>

class AiServices;
class Library;
class LibraryIndex;
class LocalModels;
class PassageIndex;
class QSqlDatabase;

// Strictly read-only library query engine shared by McpServer and the
// WebEngine-free clothbound-mcp headless CLI.
//
// When no live Library* is injected, opens library.sqlite and index.sqlite
// with QSQLITE_OPEN_READONLY + PRAGMA query_only=ON (falling back to SQLite
// URI mode=ro&immutable=1 only for cleanly-closed WAL databases inside
// non-writable directories), never creating files, sidecars, or schema writes.
class LibraryQueryService {
public:
    struct Options {
        QString indexPath;
        QString libraryPath;
    };

    struct ReadPassageRequest {
        int bookId = 0;
        bool hasChapter = false;
        int chapter = 0;
        bool hasPage = false;
        int page = 0;
        bool hasPageEnd = false;
        int pageEnd = 0;
        int offset = 0;
        int maxChars = 8000;
    };

    struct ReadPassageResult {
        bool ok = false;
        QString error;
        BookRecord book;
        int chapter = 0;
        QString chapterTitle;
        int page = 0;
        int pageEnd = 0;
        qsizetype offset = 0;
        qsizetype totalChars = 0;
        bool truncated = false;
        QString citation;
        QString text;

        QJsonObject toJson() const;
    };

    explicit LibraryQueryService(Library *library = nullptr,
                                 LibraryIndex *index = nullptr,
                                 AiServices *ai = nullptr,
                                 const Options &options = {});
    ~LibraryQueryService();

    LibraryQueryService(const LibraryQueryService &) = delete;
    LibraryQueryService &operator=(const LibraryQueryService &) = delete;

    // Default XDG file locations matching Clothbound's QStandardPaths layout.
    static QString defaultSettingsFile();
    static QString defaultIndexFile();
    static QString defaultLibraryFile();

    // Opens an existing SQLite database strictly read-only. Never creates files,
    // executes WAL pragmas, or writes migrations.
    static bool openSqliteReadOnly(const QString &dbPath,
                                   const QString &connName,
                                   QSqlDatabase *outDb,
                                   QString *errorOut = nullptr);

    // Opens options.libraryPath (or defaultLibraryFile()) strictly read-only
    // when no live Library* was injected.
    bool openReadOnlyLibrary(QString *errorOut = nullptr);
    bool isReadOnlyLibraryOpen() const { return m_roLibraryOpen; }
    bool ensureReadOnlyLibraryLoaded() const;
    bool hasLibraryData() const;

    void setLocalEmbedder(LocalModels *local) { m_localEmbedder = local; }
    LocalModels *localEmbedder() const { return m_localEmbedder; }

    QString libraryDatabasePath() const;
    QString resolvedIndexPath() const;
    bool hasPassageVectors() const;

    // Book and collection accessors.
    QList<BookRecord> visibleRecords() const;
    BookRecord bookRecord(int bookId) const;
    QVariantList allCollections() const;
    QVariantList collectionsForBook(int bookId) const;
    bool isBookInCollection(int bookId, int colId) const;
    static int resolveCollectionId(const QVariantList &collections, const QString &collectionArg);

    // Resolves a user-supplied book selector (numeric book id, file path, or
    // title/author substring) against visible books in the library.
    QList<BookRecord> matchBooks(const QString &selector) const;

    // Cached EPUB TOC chapter title lookup (validated against book mtime & path).
    QString chapterTitleFor(const BookRecord &rec, int chapter) const;

    // Passage extraction: chapter_rows B-tree JOIN -> chapter_text fallback ->
    // injected LibraryIndex -> direct EPUB/PDF chapter/page extraction.
    QString extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const;

    // Structured query operations reusable by MCP tools, MCP resources, and CLI.
    QJsonObject listBooks(const QString &query = {},
                          const QString &format = {},
                          const QString &collection = {},
                          int limit = 50) const;

    QJsonObject searchLibrary(const QString &query,
                              int bookId = 0,
                              int limit = 15,
                              QString *errorOut = nullptr) const;

    QJsonObject searchPassages(const QString &query,
                               int bookId = 0,
                               const QString &collection = {},
                               int limit = 8,
                               QString *errorOut = nullptr,
                               bool *hadContentOut = nullptr) const;

    QJsonObject bookOutline(int bookId, QString *errorOut = nullptr) const;

    ReadPassageResult readPassage(const ReadPassageRequest &req) const;

    QJsonObject listMarks(int bookId = 0,
                          const QString &query = {},
                          int limit = 50,
                          QString *errorOut = nullptr) const;

    // Renders highlights, notes, and bookmarks for one book (or all books when
    // bookId == 0) as Markdown without touching writable SQLite connections.
    QString formatMarksMarkdown(int bookId = 0,
                                const QString &query = {},
                                int limit = 200,
                                QString *errorOut = nullptr,
                                int *markCountOut = nullptr) const;

private:
    struct ReadOnlyCollection {
        int id = 0;
        QString name;
    };

    struct CachedChapterTitles {
        qint64 mtime = 0;
        QString path;
        QHash<int, QString> titles;
    };

    QJsonObject listMarksReadOnly(int bookId, const QString &query, int limit, QString *errorOut) const;

    Library *m_library = nullptr;
    LibraryIndex *m_index = nullptr;
    AiServices *m_ai = nullptr;
    LocalModels *m_localEmbedder = nullptr;
    mutable std::unique_ptr<PassageIndex> m_passageIndex;
    QString m_indexPath;
    QString m_libraryPath;

    // Strictly read-only SQLite library backing (used when m_library == nullptr).
    mutable QString m_roLibraryConn;
    mutable bool m_roLibraryOpen = false;
    mutable qint64 m_roDataVersion = -1;
    mutable bool m_roSnapshotLoaded = false;
    mutable QHash<int, BookRecord> m_roBooks;
    mutable QList<ReadOnlyCollection> m_roCollections;
    mutable QHash<int, QString> m_roCollectionNames;
    mutable QHash<int, QSet<int>> m_roBookCollections;

    mutable QHash<int, CachedChapterTitles> m_chapterTitleCache;
};
