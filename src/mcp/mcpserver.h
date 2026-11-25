#pragma once

#include "mcp/libraryqueryservice.h"
#include "mcp/mcpprotocol.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <functional>

class AiServices;
class Library;
class LibraryIndex;
class LocalModels;

// Built-in local stdio Model Context Protocol (MCP) server for Clothbound.
// Exposes strictly read-only, privacy-first tools and resources over JSON-RPC 2.0
// so local coding and research agents can search the library, inspect outlines,
// read verbatim cited passages, and query highlights/notes without data leaving
// the user's computer or mutating SQLite databases.
class McpServer : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype kMaxStdioFrameBytes = McpProtocol::kMaxStdioFrameBytes;

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
    bool isReadOnlyLibraryOpen() const { return m_query.isReadOnlyLibraryOpen(); }

    // Handles a single JSON-RPC 2.0 request object. Returns an empty QJsonObject
    // for notifications (messages without an "id") that must produce no reply.
    QJsonObject handleRequest(const QJsonObject &req);

    // Checks settings.ini key ai/mcpEnabled (defaults to true).
    bool isEnabled() const;

    void setSettingsPath(const QString &path) { m_settingsPath = path; }
    QString settingsPath() const;
    // Optional strictly loopback local embedder for read-only semantic search
    // without touching AiServices, cloud providers, or ai_usage tables.
    void setLocalEmbedder(LocalModels *local) { m_query.setLocalEmbedder(local); }

    LibraryQueryService &queryService() { return m_query; }
    const LibraryQueryService &queryService() const { return m_query; }

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

    QString extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const;

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

    LibraryQueryService m_query;
    QString m_settingsPath;
};
