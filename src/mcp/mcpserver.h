#pragma once

#include "formats/bookrecord.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdio>
#include <functional>
#include <memory>

class AiServices;
class Library;
class LibraryIndex;
class LocalModels;
class PassageIndex;

// Built-in local stdio Model Context Protocol (MCP) server for Clothbound.
// Exposes strictly read-only, privacy-first tools and resources over JSON-RPC 2.0
// so local coding and research agents can search the library, inspect outlines,
// read verbatim cited passages, and query highlights/notes without data leaving
// the user's computer or mutating SQLite databases.
class McpServer : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype kMaxStdioFrameBytes = 2 * 1024 * 1024;

    struct Options {
        QString settingsPath;
        QString indexPath;
        QString libraryPath;

        Options() = default;
        Options(QString settings, QString index, QString library = {})
            : settingsPath(std::move(settings)), indexPath(std::move(index)), libraryPath(std::move(library)) {}
    };

    explicit McpServer(Library *library,
                       LibraryIndex *index = nullptr,
                       AiServices *ai = nullptr,
                       const Options &options = {},
                       QObject *parent = nullptr);
    ~McpServer() override;

    // Opens `options.libraryPath` strictly read-only (QSQLITE_OPEN_READONLY) when
    // no live Library* was injected. Never runs WAL pragmas or schema migrations.
    bool openReadOnlyLibrary(QString *errorOut = nullptr);
    bool isReadOnlyLibraryOpen() const { return m_roLibraryOpen; }

    // Handles a single JSON-RPC 2.0 request object. Returns an empty QJsonObject
    // for notifications (messages without an "id") that must produce no reply.
    QJsonObject handleRequest(const QJsonObject &req);

    // Checks settings.ini key ai/mcpEnabled (defaults to true).
    bool isEnabled() const;

    void setSettingsPath(const QString &path) { m_settingsPath = path; }
    QString settingsPath() const;
    // Optional strictly loopback local embedder for read-only semantic search
    // without touching AiServices, cloud providers, or ai_usage tables.
    void setLocalEmbedder(LocalModels *local) { m_localEmbedder = local; }

    // Routes all Qt debug/info/warning/critical/fatal logs to stderr so stdout
    // carries exclusively newline-delimited JSON-RPC 2.0 frames.
    static void installStderrMessageHandler();

    // Runs a bounded, NUL-safe newline-delimited JSON-RPC 2.0 stdio loop.
    static void runStdioLoop(std::FILE *in,
                             std::FILE *out,
                             const std::function<QJsonObject(const QJsonObject &)> &handler,
                             qsizetype maxFrameBytes = kMaxStdioFrameBytes);

    // Runs the headless stdio JSON-RPC 2.0 MCP loop using strictly read-only
    // SQLite connections until `in` closes.
    static int runStdioWithStreams(const QStringList &arguments, std::FILE *in, std::FILE *out);

    // Runs the headless stdio JSON-RPC 2.0 MCP loop until stdin closes.
    static int runStdio(const QStringList &arguments);

private:
    friend class ClothboundTests;

    struct ReadOnlyCollection {
        int id = 0;
        QString name;
    };

    bool ensureReadOnlyLibraryLoaded() const;
    bool hasLibraryData() const;
    QList<BookRecord> visibleRecords() const;
    BookRecord bookRecord(int bookId) const;
    QVariantList allCollections() const;
    QVariantList collectionsForBook(int bookId) const;
    bool isBookInCollection(int bookId, int colId) const;
    QString libraryDatabasePath() const;
    bool hasPassageVectors() const;
    QJsonObject buildReadOnlyMarksJson(int bookId, const QString &query, int limit, QString *errorOut) const;

    QJsonObject handleInitialize(const QJsonValue &id, const QJsonObject &params);
    QJsonObject handleToolsList(const QJsonValue &id);
    QJsonObject handleToolsCall(const QJsonValue &id, const QJsonObject &params);
    QJsonObject handleResourcesList(const QJsonValue &id);
    QJsonObject handleResourceTemplatesList(const QJsonValue &id);
    QJsonObject handleResourcesRead(const QJsonValue &id, const QJsonObject &params);

    QJsonObject toolListBooks(const QJsonObject &args);
    QJsonObject toolSearchLibrary(const QJsonObject &args);
    QJsonObject toolSearchPassages(const QJsonObject &args);
    QJsonObject toolGetBookOutline(const QJsonObject &args);
    QJsonObject toolReadPassage(const QJsonObject &args);
    QJsonObject toolListMarks(const QJsonObject &args);

    QJsonObject buildBookOutlineJson(int bookId, QString *errorOut) const;
    QJsonObject buildBookMarksJson(int bookId, const QString &query, int limit, QString *errorOut) const;
    QJsonObject buildBooksListJson(const QString &query, const QString &format, const QString &collection, int limit) const;

    QString resolvedIndexPath() const;
    QString extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const;

    Library *m_library = nullptr;
    LibraryIndex *m_index = nullptr;
    AiServices *m_ai = nullptr;
    LocalModels *m_localEmbedder = nullptr;
    std::unique_ptr<PassageIndex> m_passageIndex;
    QString m_settingsPath;
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
};
