#include "mcp/mcpserver.h"

#include "ai/localmodels.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <memory>

#ifndef CLOTHBOUND_VERSION
#define CLOTHBOUND_VERSION "0.5.1"
#endif

using namespace McpProtocol;

namespace {

QString disabledErrorMessage() {
    return QStringLiteral(
        "Local MCP server is disabled. Enable it in Clothbound: Settings → Intelligence → Local agents (MCP).");
}

}  // namespace

McpServer::McpServer(Library *library, LibraryIndex *index, AiServices *ai, const Options &options, QObject *parent)
    : QObject(parent),
      m_query(library, index, ai, {options.indexPath, options.libraryPath}),
      m_settingsPath(options.settingsPath) {}

McpServer::~McpServer() = default;

bool McpServer::openReadOnlyLibrary(QString *errorOut) {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_query.openReadOnlyLibrary(errorOut);
}

void McpServer::installStderrMessageHandler() {
    McpProtocol::installStderrMessageHandler();
}

QString McpServer::settingsPath() const {
    return m_settingsPath.isEmpty() ? LibraryQueryService::defaultSettingsFile() : m_settingsPath;
}

bool McpServer::isEnabled() const {
    const QString path = settingsPath();
    if (!QFileInfo::exists(path))
        return true;
    QSettings settings(path, QSettings::IniFormat);
    return settings.value(QStringLiteral("ai/mcpEnabled"), true).toBool();
}

QString McpServer::extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const {
    return m_query.extractPassageText(bookId, chapter, start, end);
}

QJsonObject McpServer::handleRequest(const QJsonObject &req) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool hasId = req.contains(QStringLiteral("id"));
    const QJsonValue id = hasId ? req.value(QStringLiteral("id")) : QJsonValue(QJsonValue::Null);

    if (req.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0"))
        return jsonRpcError(id, kErrInvalidRequest, QStringLiteral("Invalid Request: jsonrpc must be \"2.0\"."));

    const QJsonValue methodVal = req.value(QStringLiteral("method"));
    if (!methodVal.isString() || methodVal.toString().isEmpty())
        return jsonRpcError(id, kErrInvalidRequest, QStringLiteral("Invalid Request: missing method."));

    const QString method = methodVal.toString();

    // JSON-RPC notifications (no id) must not produce any reply.
    if (method.startsWith(QLatin1String("notifications/")))
        return hasId ? jsonRpcResult(id, QJsonObject{}) : QJsonObject{};
    if (!hasId)
        return QJsonObject{};

    const QJsonObject params = req.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("initialize"))
        return handleInitialize(id, params);
    if (method == QLatin1String("ping"))
        return jsonRpcResult(id, QJsonObject{});
    if (method == QLatin1String("tools/list"))
        return handleToolsList(id);
    if (method == QLatin1String("tools/call"))
        return handleToolsCall(id, params);
    if (method == QLatin1String("resources/list"))
        return handleResourcesList(id);
    if (method == QLatin1String("resources/templates/list"))
        return handleResourceTemplatesList(id);
    if (method == QLatin1String("resources/read"))
        return handleResourcesRead(id, params);

    return jsonRpcError(id, kErrMethodNotFound, QStringLiteral("Method not found: %1").arg(method));
}

QJsonObject McpServer::handleInitialize(const QJsonValue &id, const QJsonObject &params) {
    const QString negotiated = negotiateProtocolVersion(params.value(QStringLiteral("protocolVersion")).toString());
    return jsonRpcResult(id, buildInitializeResult(negotiated, QStringLiteral(CLOTHBOUND_VERSION)));
}

QJsonObject McpServer::handleToolsList(const QJsonValue &id) {
    return jsonRpcResult(id, QJsonObject{{QStringLiteral("tools"), buildToolDefinitions()}});
}

QJsonObject McpServer::handleToolsCall(const QJsonValue &id, const QJsonObject &params) {
    if (!isEnabled())
        return jsonRpcError(id, kErrMcpDisabled, disabledErrorMessage());

    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
        return jsonRpcError(id, kErrInvalidParams, QStringLiteral("Invalid params: missing tool name."));
    if (params.contains(QStringLiteral("arguments")) && !params.value(QStringLiteral("arguments")).isObject() &&
        !params.value(QStringLiteral("arguments")).isNull()) {
        return jsonRpcError(id, kErrInvalidParams, QStringLiteral("Invalid params: arguments must be an object."));
    }
    const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();

    if (name == QLatin1String("list_books"))
        return jsonRpcResult(id, toolListBooks(args));
    if (name == QLatin1String("search_library"))
        return jsonRpcResult(id, toolSearchLibrary(args));
    if (name == QLatin1String("search_passages"))
        return jsonRpcResult(id, toolSearchPassages(args));
    if (name == QLatin1String("get_book_outline"))
        return jsonRpcResult(id, toolGetBookOutline(args));
    if (name == QLatin1String("read_passage"))
        return jsonRpcResult(id, toolReadPassage(args));
    if (name == QLatin1String("list_marks"))
        return jsonRpcResult(id, toolListMarks(args));

    return jsonRpcError(id, kErrInvalidParams, QStringLiteral("Unknown tool: %1").arg(name));
}

QJsonObject McpServer::handleResourcesList(const QJsonValue &id) {
    if (!isEnabled())
        return jsonRpcError(id, kErrMcpDisabled, disabledErrorMessage());

    QJsonArray resources;
    resources.append(QJsonObject{
        {QStringLiteral("uri"), QStringLiteral("clothbound://library/books")},
        {QStringLiteral("name"), QStringLiteral("Library books")},
        {QStringLiteral("description"), QStringLiteral("All visible books in the local Clothbound library.")},
        {QStringLiteral("mimeType"), QStringLiteral("application/json")},
    });

    if (m_query.hasLibraryData()) {
        QList<BookRecord> books = m_query.visibleRecords();
        std::sort(books.begin(), books.end(), [](const BookRecord &a, const BookRecord &b) {
            if (a.openedAt != b.openedAt)
                return a.openedAt > b.openedAt;
            return a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
        });
        constexpr int kMaxListedBooks = 100;
        const int take = qMin(kMaxListedBooks, int(books.size()));
        for (int i = 0; i < take; ++i) {
            const BookRecord &book = books[i];
            resources.append(QJsonObject{
                {QStringLiteral("uri"), QStringLiteral("clothbound://book/%1/outline").arg(book.id)},
                {QStringLiteral("name"), QStringLiteral("%1 — Outline").arg(book.title)},
                {QStringLiteral("description"),
                 QStringLiteral("Metadata, table of contents, and page map for \"%1\".").arg(book.title)},
                {QStringLiteral("mimeType"), QStringLiteral("application/json")},
            });
            resources.append(QJsonObject{
                {QStringLiteral("uri"), QStringLiteral("clothbound://book/%1/marks").arg(book.id)},
                {QStringLiteral("name"), QStringLiteral("%1 — Marks").arg(book.title)},
                {QStringLiteral("description"),
                 QStringLiteral("Highlights, bookmarks, and notes for \"%1\".").arg(book.title)},
                {QStringLiteral("mimeType"), QStringLiteral("application/json")},
            });
        }
    }

    return jsonRpcResult(id, QJsonObject{{QStringLiteral("resources"), resources}});
}

QJsonObject McpServer::handleResourceTemplatesList(const QJsonValue &id) {
    return jsonRpcResult(id, QJsonObject{{QStringLiteral("resourceTemplates"), buildResourceTemplates()}});
}

QJsonObject McpServer::handleResourcesRead(const QJsonValue &id, const QJsonObject &params) {
    if (!isEnabled())
        return jsonRpcError(id, kErrMcpDisabled, disabledErrorMessage());

    const QString uri = params.value(QStringLiteral("uri")).toString().trimmed();
    if (uri.isEmpty())
        return jsonRpcError(id, kErrInvalidParams, QStringLiteral("Invalid params: missing resource uri."));

    auto makeContent = [&id, &uri](const QJsonObject &payload) {
        const QString text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented));
        return jsonRpcResult(
            id, QJsonObject{{QStringLiteral("contents"),
                             QJsonArray{QJsonObject{{QStringLiteral("uri"), uri},
                                                    {QStringLiteral("mimeType"), QStringLiteral("application/json")},
                                                    {QStringLiteral("text"), text}}}}});
    };

    if (uri == QLatin1String("clothbound://library/books"))
        return makeContent(m_query.listBooks({}, {}, {}, 200));

    static const QRegularExpression bookUriRe(QStringLiteral("^clothbound://book/([0-9]+)/(outline|marks)$"));
    const QRegularExpressionMatch m = bookUriRe.match(uri);
    if (!m.hasMatch())
        return jsonRpcError(id, kErrResourceNotFound, QStringLiteral("Unknown resource URI: %1").arg(uri));

    const int bookId = m.captured(1).toInt();
    const QString kind = m.captured(2);
    QString err;
    if (kind == QLatin1String("outline")) {
        const QJsonObject payload = m_query.bookOutline(bookId, &err);
        if (!err.isEmpty())
            return jsonRpcError(id, kErrResourceNotFound, err);
        return makeContent(payload);
    }
    const QJsonObject payload = m_query.listMarks(bookId, {}, 200, &err);
    if (!err.isEmpty())
        return jsonRpcError(id, kErrResourceNotFound, err);
    return makeContent(payload);
}

QJsonObject McpServer::toolListBooks(const QJsonObject &args) {
    int limit = 50;
    if (!readOptionalInt(args, QStringLiteral("limit"), &limit))
        return toolError(QStringLiteral("Invalid 'limit': expected an integer."));
    limit = qBound(1, limit, 200);

    const QString format = args.value(QStringLiteral("format")).toString().trimmed().toLower();
    if (!format.isEmpty() && format != QLatin1String("epub") && format != QLatin1String("pdf"))
        return toolError(QStringLiteral("Invalid 'format': must be 'epub' or 'pdf'."));

    const QString query = args.value(QStringLiteral("query")).toString();
    const QString collection = args.value(QStringLiteral("collection")).toString();
    return toolSuccessJson(m_query.listBooks(query, format, collection, limit));
}

QJsonObject McpServer::toolSearchLibrary(const QJsonObject &args) {
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty())
        return toolError(QStringLiteral("Missing required parameter 'query'."));

    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId))
        return toolError(QStringLiteral("Invalid 'book_id': expected a positive integer."));
    if (hasBookId && bookId <= 0)
        return toolError(QStringLiteral("Book %1 not found in the library.").arg(bookId));

    int limit = 15;
    if (!readOptionalInt(args, QStringLiteral("limit"), &limit))
        return toolError(QStringLiteral("Invalid 'limit': expected an integer."));
    limit = qBound(1, limit, 50);

    QString err;
    const QJsonObject payload = m_query.searchLibrary(query, hasBookId ? bookId : 0, limit, &err);
    if (!err.isEmpty())
        return toolError(err);
    return toolSuccessUntrustedJson(payload);
}

QJsonObject McpServer::toolSearchPassages(const QJsonObject &args) {
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty())
        return toolError(QStringLiteral("Missing required parameter 'query'."));

    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId))
        return toolError(QStringLiteral("Invalid 'book_id': expected a positive integer."));
    if (hasBookId && bookId <= 0)
        return toolError(QStringLiteral("Book %1 not found in the library.").arg(bookId));

    int limit = 8;
    if (!readOptionalInt(args, QStringLiteral("limit"), &limit))
        return toolError(QStringLiteral("Invalid 'limit': expected an integer."));
    limit = qBound(1, limit, 25);

    const QString collection = args.value(QStringLiteral("collection")).toString().trimmed();
    QString err;
    bool hadContent = false;
    const QJsonObject payload =
        m_query.searchPassages(query, hasBookId ? bookId : 0, collection, limit, &err, &hadContent);
    if (!err.isEmpty())
        return toolError(err);
    return hadContent ? toolSuccessUntrustedJson(payload) : toolSuccessJson(payload);
}

QJsonObject McpServer::toolGetBookOutline(const QJsonObject &args) {
    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId) || !hasBookId || bookId <= 0)
        return toolError(QStringLiteral("Missing or invalid required parameter 'book_id'."));

    QString err;
    const QJsonObject outline = m_query.bookOutline(bookId, &err);
    if (!err.isEmpty())
        return toolError(err);
    return toolSuccessJson(outline);
}

QJsonObject McpServer::toolReadPassage(const QJsonObject &args) {
    LibraryQueryService::ReadPassageRequest req;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &req.bookId, &hasBookId) || !hasBookId || req.bookId <= 0)
        return toolError(QStringLiteral("Missing or invalid required parameter 'book_id'."));

    if (!readOptionalInt(args, QStringLiteral("chapter"), &req.chapter, &req.hasChapter))
        return toolError(QStringLiteral("Invalid 'chapter': expected an integer."));
    if (!readOptionalInt(args, QStringLiteral("page"), &req.page, &req.hasPage))
        return toolError(QStringLiteral("Invalid 'page': expected an integer."));
    if (!readOptionalInt(args, QStringLiteral("page_end"), &req.pageEnd, &req.hasPageEnd))
        return toolError(QStringLiteral("Invalid 'page_end': expected an integer."));
    if (!readOptionalInt(args, QStringLiteral("offset"), &req.offset) || req.offset < 0)
        return toolError(QStringLiteral("Invalid 'offset': expected a non-negative integer."));

    req.maxChars = kDefaultReadMaxChars;
    if (!readOptionalInt(args, QStringLiteral("max_chars"), &req.maxChars))
        return toolError(QStringLiteral("Invalid 'max_chars': expected an integer."));

    const LibraryQueryService::ReadPassageResult res = m_query.readPassage(req);
    if (!res.ok)
        return toolError(res.error);

    QString formatted = res.citation + QStringLiteral("\n\n") + wrapUntrustedData(res.text);
    if (res.truncated) {
        formatted += QStringLiteral("\n\n[Truncated at %1 chars (offset %2 of %3). Pass offset=%4 to continue.]")
                         .arg(QString::number(res.text.size()), QString::number(res.offset),
                              QString::number(res.totalChars), QString::number(res.offset + res.text.size()));
    }
    return toolSuccess(formatted, res.toJson());
}

QJsonObject McpServer::toolListMarks(const QJsonObject &args) {
    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId))
        return toolError(QStringLiteral("Invalid 'book_id': expected a positive integer."));
    if (hasBookId && bookId <= 0)
        return toolError(QStringLiteral("Invalid 'book_id': expected a positive integer."));

    int limit = 50;
    if (!readOptionalInt(args, QStringLiteral("limit"), &limit))
        return toolError(QStringLiteral("Invalid 'limit': expected an integer."));
    limit = qBound(1, limit, 200);

    const QString query = args.value(QStringLiteral("query")).toString();
    QString err;
    const QJsonObject payload = m_query.listMarks(hasBookId ? bookId : 0, query, limit, &err);
    if (!err.isEmpty())
        return toolError(err);
    return toolSuccessJson(payload);
}

void McpServer::runStdioLoop(std::FILE *in,
                             std::FILE *out,
                             const std::function<QJsonObject(const QJsonObject &)> &handler,
                             qsizetype maxFrameBytes) {
    McpProtocol::runStdioLoop(in, out, handler, maxFrameBytes);
}

int McpServer::runStdioWithStreams(const QStringList &arguments, std::FILE *in, std::FILE *out) {
    Q_UNUSED(arguments);
    installStderrMessageHandler();

    const QString libraryDbPath = LibraryQueryService::defaultLibraryFile();
    const QString indexDbPath = LibraryQueryService::defaultIndexFile();
    const QString settingsFile = LibraryQueryService::defaultSettingsFile();

    McpServer server(nullptr, nullptr, nullptr, {settingsFile, indexDbPath, libraryDbPath});
    QString openErr;
    if (!server.openReadOnlyLibrary(&openErr)) {
        std::fprintf(stderr, "[clothbound-mcp ERROR] The library can't be opened read-only: %s\n",
                     qPrintable(openErr));
        std::fflush(stderr);
        return 1;
    }

    // Wire strictly local loopback embedder support ONLY when passage_vectors exist.
    // NEVER instantiate GeminiProvider, AiServices, or read-write Library here so
    // GEMINI_API_KEY can never send queries to Google cloud and SQLite is never written.
    std::unique_ptr<LocalModels> localModels;
    if (server.m_query.hasPassageVectors() && QFileInfo::exists(settingsFile)) {
        QSettings s(settingsFile, QSettings::IniFormat);
        const QUrl parsed = LocalModels::normalizeServerUrl(QUrl(s.value(QStringLiteral("local/url")).toString()));
        const QString embModel = s.value(QStringLiteral("local/embedding")).toString().trimmed();
        if (LocalModels::isLoopbackUrl(parsed) && !embModel.isEmpty()) {
            localModels = std::make_unique<LocalModels>();
            LocalModels::Server cfg;
            cfg.kind = LocalModels::kindFromId(s.value(QStringLiteral("local/kind")).toString());
            cfg.url = parsed;
            localModels->configure(cfg, {}, embModel, {});
            server.setLocalEmbedder(localModels.get());
        }
    }

    runStdioLoop(in, out, [&server](const QJsonObject &req) { return server.handleRequest(req); });
    return 0;
}

int McpServer::runStdio(const QStringList &arguments) {
    return runStdioWithStreams(arguments, stdin, stdout);
}
