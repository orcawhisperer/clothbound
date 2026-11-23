#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class AiServices;
class Library;
class LibraryIndex;

// Built-in local stdio Model Context Protocol (MCP) server for Clothbound.
// Exposes strictly read-only, privacy-first tools and resources over JSON-RPC 2.0
// so local coding and research agents can search the library, inspect outlines,
// read verbatim cited passages, and query highlights/notes without data leaving
// the user's computer.
class McpServer : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString settingsPath;
        QString indexPath;
    };

    explicit McpServer(Library *library,
                       LibraryIndex *index = nullptr,
                       AiServices *ai = nullptr,
                       const Options &options = {},
                       QObject *parent = nullptr);
    ~McpServer() override;

    // Handles a single JSON-RPC 2.0 request object. Returns an empty QJsonObject
    // for notifications (messages without an "id") that must produce no reply.
    QJsonObject handleRequest(const QJsonObject &req);

    // Checks settings.ini key ai/mcpEnabled (defaults to true).
    bool isEnabled() const;

    void setSettingsPath(const QString &path) { m_settingsPath = path; }
    QString settingsPath() const;

    // Routes all Qt debug/info/warning/critical/fatal logs to stderr so stdout
    // carries exclusively newline-delimited JSON-RPC 2.0 frames.
    static void installStderrMessageHandler();

    // Runs the headless stdio JSON-RPC 2.0 MCP loop until stdin closes.
    static int runStdio(const QStringList &arguments);

private:
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

    LibraryIndex *ensureIndex();
    void ensureIndexedForSearch(int bookId);
    QString extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const;

    Library *m_library = nullptr;
    LibraryIndex *m_index = nullptr;
    bool m_ownsIndex = false;
    AiServices *m_ai = nullptr;
    QString m_settingsPath;
    QString m_indexPath;
};
