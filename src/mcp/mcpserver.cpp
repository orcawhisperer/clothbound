#include "mcp/mcpserver.h"

#include "ai/aiservices.h"
#include "ai/gemini.h"
#include "ai/localmodels.h"
#include "ai/passageindex.h"
#include "app/cli.h"
#include "formats/bookrecord.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "library/librarymodel.h"
#include "library/marks.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef CLOTHBOUND_VERSION
#define CLOTHBOUND_VERSION "0.4.1"
#endif

namespace {

constexpr int kErrParse = -32700;
constexpr int kErrInvalidRequest = -32600;
constexpr int kErrMethodNotFound = -32601;
constexpr int kErrInvalidParams = -32602;
constexpr int kErrMcpDisabled = -32001;
constexpr int kErrResourceNotFound = -32002;

constexpr int kDefaultReadMaxChars = 8000;
constexpr int kMinReadMaxChars = 100;
constexpr int kHardCapReadMaxChars = 24000;

void mcpStderrMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    Q_UNUSED(context);
    const char *level = "INFO";
    switch (type) {
    case QtDebugMsg:
        level = "DEBUG";
        break;
    case QtInfoMsg:
        level = "INFO";
        break;
    case QtWarningMsg:
        level = "WARN";
        break;
    case QtCriticalMsg:
        level = "CRIT";
        break;
    case QtFatalMsg:
        level = "FATAL";
        break;
    }
    const QByteArray utf8 = msg.toUtf8();
    std::fprintf(stderr, "[clothbound-mcp %s] %s\n", level, utf8.constData());
    std::fflush(stderr);
    if (type == QtFatalMsg)
        std::abort();
}

QJsonObject jsonRpcResult(const QJsonValue &id, const QJsonObject &result) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result},
    };
}

QJsonObject jsonRpcError(const QJsonValue &id, int code, const QString &message) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}},
    };
}

QJsonObject toolSuccess(const QString &text, const QJsonObject &structured) {
    return QJsonObject{
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}}},
        {QStringLiteral("structuredContent"), structured},
        {QStringLiteral("isError"), false},
    };
}

QJsonObject toolSuccessJson(const QJsonObject &payload) {
    const QString text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return toolSuccess(text, payload);
}

QJsonObject toolError(const QString &message) {
    return QJsonObject{
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), message}}}},
        {QStringLiteral("isError"), true},
    };
}

QJsonObject readOnlyAnnotations() {
    return QJsonObject{
        {QStringLiteral("readOnlyHint"), true},
        {QStringLiteral("destructiveHint"), false},
        {QStringLiteral("idempotentHint"), true},
        {QStringLiteral("openWorldHint"), false},
    };
}

bool readOptionalInt(const QJsonObject &obj, const QString &key, int *valueOut, bool *presentOut = nullptr) {
    if (!obj.contains(key) || obj.value(key).isNull() || obj.value(key).isUndefined()) {
        if (presentOut)
            *presentOut = false;
        return true;
    }
    if (presentOut)
        *presentOut = true;
    const QJsonValue val = obj.value(key);
    if (!val.isDouble())
        return false;
    const double d = val.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d)
        return false;
    if (d < double(std::numeric_limits<int>::min()) || d > double(std::numeric_limits<int>::max()))
        return false;
    if (valueOut)
        *valueOut = int(d);
    return true;
}

QString cleanSnippet(QString snippet) {
    snippet.remove(QChar(0x2039));  // ‹
    snippet.remove(QChar(0x203A));  // ›
    return snippet.simplified();
}

QString defaultSettingsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
}

QString defaultIndexFile() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/index.sqlite");
}

QStringList significantQueryTerms(const QString &query) {
    static const QSet<QString> stop = {
        QStringLiteral("a"),     QStringLiteral("an"),    QStringLiteral("and"),   QStringLiteral("are"),
        QStringLiteral("as"),    QStringLiteral("at"),    QStringLiteral("be"),    QStringLiteral("but"),
        QStringLiteral("by"),    QStringLiteral("did"),   QStringLiteral("do"),    QStringLiteral("does"),
        QStringLiteral("for"),   QStringLiteral("from"),  QStringLiteral("had"),   QStringLiteral("has"),
        QStringLiteral("have"),  QStringLiteral("he"),    QStringLiteral("her"),   QStringLiteral("him"),
        QStringLiteral("his"),   QStringLiteral("how"),   QStringLiteral("if"),    QStringLiteral("in"),
        QStringLiteral("into"),  QStringLiteral("is"),    QStringLiteral("it"),    QStringLiteral("me"),
        QStringLiteral("my"),    QStringLiteral("no"),    QStringLiteral("not"),   QStringLiteral("of"),
        QStringLiteral("on"),    QStringLiteral("or"),    QStringLiteral("she"),   QStringLiteral("so"),
        QStringLiteral("that"),  QStringLiteral("the"),   QStringLiteral("their"), QStringLiteral("there"),
        QStringLiteral("they"),  QStringLiteral("this"),  QStringLiteral("to"),    QStringLiteral("up"),
        QStringLiteral("us"),    QStringLiteral("was"),   QStringLiteral("we"),    QStringLiteral("were"),
        QStringLiteral("what"),  QStringLiteral("when"),  QStringLiteral("where"), QStringLiteral("which"),
        QStringLiteral("who"),   QStringLiteral("why"),   QStringLiteral("with"),  QStringLiteral("you"),
        QStringLiteral("about"),
    };
    static const QRegularExpression wordRe(QStringLiteral("[\\p{L}\\p{N}][\\p{L}\\p{N}\\p{M}'’]*"));
    QStringList all;
    QStringList kept;
    auto it = wordRe.globalMatch(query.toLower());
    while (it.hasNext()) {
        const QString w = it.next().captured(0);
        if (!all.contains(w))
            all.append(w);
        if (w.size() >= 2 && !stop.contains(w) && !kept.contains(w))
            kept.append(w);
    }
    return kept.isEmpty() ? all : kept;
}

QString epubChapterTitle(const EpubBook &epub, int spineIndex) {
    QString best;
    for (const EpubTocEntry &entry : epub.toc()) {
        if (entry.spine == spineIndex && !entry.title.isEmpty())
            return entry.title;
        if (entry.spine >= 0 && entry.spine < spineIndex && !entry.title.isEmpty())
            best = entry.title;
    }
    return best;
}

QString pdfSectionTitle(const PdfBook &pdf, int pageIndex) {
    QString best;
    for (const PdfOutlineEntry &entry : pdf.outline()) {
        if (entry.page <= pageIndex && !entry.title.isEmpty())
            best = entry.title;
        else if (entry.page > pageIndex)
            break;
    }
    return best;
}

QJsonArray collectionNamesForBook(const Library *library, int bookId) {
    QJsonArray names;
    for (const QVariant &v : library->collectionsForBook(bookId)) {
        const QString name = v.toMap().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            names.append(name);
    }
    return names;
}

int resolveCollectionId(const Library *library, const QString &collectionArg) {
    const QString trimmed = collectionArg.trimmed();
    if (trimmed.isEmpty())
        return 0;
    bool okNum = false;
    const int asId = trimmed.toInt(&okNum);
    for (const QVariant &v : library->collections()) {
        const QVariantMap m = v.toMap();
        const int id = m.value(QStringLiteral("id")).toInt();
        const QString name = m.value(QStringLiteral("name")).toString();
        if (name.compare(trimmed, Qt::CaseInsensitive) == 0)
            return id;
        if (okNum && asId > 0 && id == asId)
            return id;
    }
    return -1;
}

QJsonArray buildToolDefinitions() {
    QJsonArray tools;

    // 1. list_books
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("list_books")},
        {QStringLiteral("description"),
         QStringLiteral("List books in the user's private Clothbound library with metadata, reading progress, collections, and file paths.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{
                  {QStringLiteral("query"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("description"),
                                QStringLiteral("Optional search filter matching title, author, series, publisher, or subjects.")}}},
                  {QStringLiteral("format"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("enum"), QJsonArray{QStringLiteral("epub"), QStringLiteral("pdf")}},
                               {QStringLiteral("description"), QStringLiteral("Optional format filter: 'epub' or 'pdf'.")}}},
                  {QStringLiteral("collection"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("description"),
                                QStringLiteral("Optional collection name (case-insensitive) or collection id.")}}},
                  {QStringLiteral("limit"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("maximum"), 200},
                               {QStringLiteral("default"), 50},
                               {QStringLiteral("description"), QStringLiteral("Maximum number of books to return (default 50, max 200).")}}},
              }},
             {QStringLiteral("additionalProperties"), false},
         }},
        {QStringLiteral("annotations"), readOnlyAnnotations()},
    });

    // 2. search_library
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("search_library")},
        {QStringLiteral("description"),
         QStringLiteral("Fast SQLite FTS5 full-text keyword search across the Clothbound library, returning book titles, chapter/page locations, and verbatim snippets.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{
                  {QStringLiteral("query"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("description"), QStringLiteral("Keywords or phrase to search for.")}}},
                  {QStringLiteral("book_id"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"), QStringLiteral("Optional book id to scope search to a single book.")}}},
                  {QStringLiteral("limit"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("maximum"), 50},
                               {QStringLiteral("default"), 15},
                               {QStringLiteral("description"), QStringLiteral("Maximum results to return (default 15, max 50).")}}},
              }},
             {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}},
             {QStringLiteral("additionalProperties"), false},
         }},
        {QStringLiteral("annotations"), readOnlyAnnotations()},
    });

    // 3. search_passages
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("search_passages")},
        {QStringLiteral("description"),
         QStringLiteral("Hybrid semantic + lexical passage search across the Clothbound library (automatically falls back to FTS5 + BM25 when embeddings are not built), returning verbatim cited passages.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{
                  {QStringLiteral("query"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("description"), QStringLiteral("Natural-language question or topic to search passages for.")}}},
                  {QStringLiteral("book_id"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"), QStringLiteral("Optional book id to scope search to one book.")}}},
                  {QStringLiteral("collection"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("description"),
                                QStringLiteral("Optional collection name or id to scope search to a shelf.")}}},
                  {QStringLiteral("limit"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("maximum"), 25},
                               {QStringLiteral("default"), 8},
                               {QStringLiteral("description"), QStringLiteral("Maximum passages to return (default 8, max 25).")}}},
              }},
             {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}},
             {QStringLiteral("additionalProperties"), false},
         }},
        {QStringLiteral("annotations"), readOnlyAnnotations()},
    });

    // 4. get_book_outline
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("get_book_outline")},
        {QStringLiteral("description"),
         QStringLiteral("Return a book's metadata, full table of contents / chapters, and print/PDF page map.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{
                  {QStringLiteral("book_id"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"), QStringLiteral("The numeric id of the book in the library.")}}},
              }},
             {QStringLiteral("required"), QJsonArray{QStringLiteral("book_id")}},
             {QStringLiteral("additionalProperties"), false},
         }},
        {QStringLiteral("annotations"), readOnlyAnnotations()},
    });

    // 5. read_passage
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("read_passage")},
        {QStringLiteral("description"),
         QStringLiteral("Read exact verbatim text from an EPUB chapter or PDF page range with an accurate citation header.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{
                  {QStringLiteral("book_id"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"), QStringLiteral("The numeric id of the book in the library.")}}},
                  {QStringLiteral("chapter"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 0},
                               {QStringLiteral("description"),
                                QStringLiteral("0-based chapter index (EPUB spine index, or 0-based PDF page index).")}}},
                  {QStringLiteral("page"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"),
                                QStringLiteral("1-based page number (PDF page, or EPUB print page / 1-based chapter).")}}},
                  {QStringLiteral("page_end"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"), QStringLiteral("Optional inclusive 1-based end page for multi-page PDF reads.")}}},
                  {QStringLiteral("offset"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 0},
                               {QStringLiteral("default"), 0},
                               {QStringLiteral("description"), QStringLiteral("Optional character offset within the chapter/page to start reading from.")}}},
                  {QStringLiteral("max_chars"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), kMinReadMaxChars},
                               {QStringLiteral("maximum"), kHardCapReadMaxChars},
                               {QStringLiteral("default"), kDefaultReadMaxChars},
                               {QStringLiteral("description"), QStringLiteral("Maximum characters to return (default 8000, hard cap 24000).")}}},
              }},
             {QStringLiteral("required"), QJsonArray{QStringLiteral("book_id")}},
             {QStringLiteral("additionalProperties"), false},
         }},
        {QStringLiteral("annotations"), readOnlyAnnotations()},
    });

    // 6. list_marks
    tools.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("list_marks")},
        {QStringLiteral("description"),
         QStringLiteral("List the user's highlights, bookmarks, and notes with quoted text, chapter/page location, highlight color, and note text.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{
                  {QStringLiteral("book_id"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("description"), QStringLiteral("Optional book id to list marks for one book.")}}},
                  {QStringLiteral("query"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("description"), QStringLiteral("Optional filter matching quote, note, label, book title, or author.")}}},
                  {QStringLiteral("limit"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                               {QStringLiteral("minimum"), 1},
                               {QStringLiteral("maximum"), 200},
                               {QStringLiteral("default"), 50},
                               {QStringLiteral("description"), QStringLiteral("Maximum marks to return (default 50, max 200).")}}},
              }},
             {QStringLiteral("additionalProperties"), false},
         }},
        {QStringLiteral("annotations"), readOnlyAnnotations()},
    });

    return tools;
}

bool databaseHasPassageVectors(const QSqlDatabase &db) {
    if (!db.isOpen())
        return false;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='passage_vectors'")))
        return false;
    if (!q.next())
        return false;
    QSqlQuery c(db);
    return c.exec(QStringLiteral("SELECT 1 FROM passage_vectors LIMIT 1")) && c.next();
}

// Register McpServer::runStdio with the CLI dispatcher whenever this module is linked.
struct McpRunnerRegistrar {
    McpRunnerRegistrar() { registerMcpRunner(&McpServer::runStdio); }
} s_mcpRunnerRegistrar;

}  // namespace

McpServer::McpServer(Library *library, LibraryIndex *index, AiServices *ai, const Options &options, QObject *parent)
    : QObject(parent),
      m_library(library),
      m_index(index),
      m_ownsIndex(index == nullptr),
      m_ai(ai),
      m_settingsPath(options.settingsPath),
      m_indexPath(options.indexPath) {}

McpServer::~McpServer() = default;

void McpServer::installStderrMessageHandler() {
    qInstallMessageHandler(&mcpStderrMessageHandler);
}

QString McpServer::settingsPath() const {
    return m_settingsPath.isEmpty() ? defaultSettingsFile() : m_settingsPath;
}

bool McpServer::isEnabled() const {
    QSettings settings(settingsPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("ai/mcpEnabled"), true).toBool();
}

LibraryIndex *McpServer::ensureIndex() {
    if (!m_index) {
        if (m_indexPath.isEmpty())
            m_indexPath = defaultIndexFile();
        m_index = new LibraryIndex(m_indexPath, this);
        m_ownsIndex = true;
    }
    return m_index;
}

void McpServer::ensureIndexedForSearch(int bookId) {
    LibraryIndex *idx = ensureIndex();
    if (!idx || !idx->available() || !m_library)
        return;

    // Only touch the SQLite file directly if we know the backing path (owned index or explicit indexPath).
    const QString dbPath = m_indexPath;
    if (dbPath.isEmpty())
        return;

    QHash<int, QPair<qint64, QString>> indexed;
    {
        const QString connName = QStringLiteral("mcp-idx-check-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
            db.setDatabaseName(dbPath);
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec(QStringLiteral("SELECT book, mtime, path FROM indexed"))) {
                    while (q.next())
                        indexed.insert(q.value(0).toInt(), {q.value(1).toLongLong(), q.value(2).toString()});
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connName);
    }

    auto needsIndex = [&indexed](const BookRecord &r) {
        if (!r.visible() || r.path.isEmpty() || !QFileInfo::exists(r.path))
            return false;
        const auto it = indexed.constFind(r.id);
        return it == indexed.cend() || it->first != r.mtime || it->second != r.path;
    };

    if (bookId > 0) {
        const BookRecord rec = m_library->record(bookId);
        if (needsIndex(rec))
            LibraryIndex::indexBook(dbPath, {rec.id, rec.path, rec.format, rec.mtime});
        return;
    }

    QElapsedTimer budget;
    budget.start();
    int done = 0;
    constexpr int kMaxSyncCatchupBooks = 25;
    constexpr qint64 kMaxSyncCatchupMs = 3000;
    for (const BookRecord &rec : m_library->visibleRecords()) {
        if (!needsIndex(rec))
            continue;
        LibraryIndex::indexBook(dbPath, {rec.id, rec.path, rec.format, rec.mtime});
        ++done;
        if (done >= kMaxSyncCatchupBooks || budget.elapsed() >= kMaxSyncCatchupMs)
            break;
    }
}

QString McpServer::extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const {
    if (m_index && m_index->available() && end > start) {
        const QString fromIndex = m_index->passage(bookId, chapter, start, end);
        if (!fromIndex.isEmpty())
            return fromIndex;
    }
    if (!m_library)
        return {};
    const BookRecord rec = m_library->record(bookId);
    if (!rec.visible() || rec.path.isEmpty())
        return {};
    QString full;
    if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(rec.path);
        if (pdf.open())
            full = pdf.pageText(chapter);
    } else {
        EpubBook epub(rec.path);
        if (epub.open() && !epub.isDrmProtected())
            full = epub.chapterText(chapter);
    }
    if (full.isEmpty())
        return {};
    const qsizetype s = std::clamp<qsizetype>(qsizetype(start), 0, full.size());
    const qsizetype len = end > start ? qMin<qsizetype>(qsizetype(end - start), full.size() - s) : qMin<qsizetype>(1100, full.size() - s);
    return full.mid(s, len).trimmed();
}

QJsonObject McpServer::handleRequest(const QJsonObject &req) {
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
    const QString requested = params.value(QStringLiteral("protocolVersion")).toString().trimmed();
    static const QStringList supported = {
        QStringLiteral("2024-11-05"),
        QStringLiteral("2025-03-26"),
        QStringLiteral("2025-06-18"),
    };
    const QString negotiated = supported.contains(requested) ? requested : QStringLiteral("2024-11-05");

    return jsonRpcResult(
        id,
        QJsonObject{
            {QStringLiteral("protocolVersion"), negotiated},
            {QStringLiteral("capabilities"),
             QJsonObject{
                 {QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}},
                 {QStringLiteral("resources"),
                  QJsonObject{{QStringLiteral("subscribe"), false}, {QStringLiteral("listChanged"), false}}},
             }},
            {QStringLiteral("serverInfo"),
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("clothbound")},
                 {QStringLiteral("version"), QStringLiteral(CLOTHBOUND_VERSION)},
             }},
            {QStringLiteral("instructions"),
             QStringLiteral("Clothbound local read-only library MCP server. Use list_books to discover book IDs, "
                            "search_library or search_passages to find cited passages, get_book_outline to inspect "
                            "structure, read_passage to read verbatim chapters or pages, and list_marks for highlights "
                            "and notes.")},
        });
}

QJsonObject McpServer::handleToolsList(const QJsonValue &id) {
    return jsonRpcResult(id, QJsonObject{{QStringLiteral("tools"), buildToolDefinitions()}});
}

QJsonObject McpServer::handleToolsCall(const QJsonValue &id, const QJsonObject &params) {
    if (!isEnabled()) {
        return jsonRpcError(
            id, kErrMcpDisabled,
            QStringLiteral("Local MCP server is disabled. Enable it in Clothbound: Settings → Intelligence → Local agents (MCP)."));
    }

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
    QJsonArray resources;
    resources.append(QJsonObject{
        {QStringLiteral("uri"), QStringLiteral("clothbound://library/books")},
        {QStringLiteral("name"), QStringLiteral("Library books")},
        {QStringLiteral("description"), QStringLiteral("All visible books in the local Clothbound library.")},
        {QStringLiteral("mimeType"), QStringLiteral("application/json")},
    });

    if (m_library) {
        QList<BookRecord> books = m_library->visibleRecords();
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
    QJsonArray templates{
        QJsonObject{
            {QStringLiteral("uriTemplate"), QStringLiteral("clothbound://book/{id}/outline")},
            {QStringLiteral("name"), QStringLiteral("Book outline")},
            {QStringLiteral("description"), QStringLiteral("Metadata, table of contents, and page map for a book id.")},
            {QStringLiteral("mimeType"), QStringLiteral("application/json")},
        },
        QJsonObject{
            {QStringLiteral("uriTemplate"), QStringLiteral("clothbound://book/{id}/marks")},
            {QStringLiteral("name"), QStringLiteral("Book marks")},
            {QStringLiteral("description"), QStringLiteral("Highlights, bookmarks, and notes for a book id.")},
            {QStringLiteral("mimeType"), QStringLiteral("application/json")},
        },
    };
    return jsonRpcResult(id, QJsonObject{{QStringLiteral("resourceTemplates"), templates}});
}

QJsonObject McpServer::handleResourcesRead(const QJsonValue &id, const QJsonObject &params) {
    if (!isEnabled()) {
        return jsonRpcError(
            id, kErrMcpDisabled,
            QStringLiteral("Local MCP server is disabled. Enable it in Clothbound: Settings → Intelligence → Local agents (MCP)."));
    }

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
        return makeContent(buildBooksListJson({}, {}, {}, 200));

    static const QRegularExpression bookUriRe(QStringLiteral("^clothbound://book/([0-9]+)/(outline|marks)$"));
    const QRegularExpressionMatch m = bookUriRe.match(uri);
    if (!m.hasMatch())
        return jsonRpcError(id, kErrResourceNotFound, QStringLiteral("Unknown resource URI: %1").arg(uri));

    const int bookId = m.captured(1).toInt();
    const QString kind = m.captured(2);
    QString err;
    if (kind == QLatin1String("outline")) {
        const QJsonObject payload = buildBookOutlineJson(bookId, &err);
        if (!err.isEmpty())
            return jsonRpcError(id, kErrResourceNotFound, err);
        return makeContent(payload);
    }
    const QJsonObject payload = buildBookMarksJson(bookId, {}, 200, &err);
    if (!err.isEmpty())
        return jsonRpcError(id, kErrResourceNotFound, err);
    return makeContent(payload);
}

QJsonObject McpServer::buildBooksListJson(const QString &query, const QString &format, const QString &collection,
                                          int limit) const {
    QJsonArray out;
    int totalMatched = 0;
    if (!m_library)
        return QJsonObject{{QStringLiteral("books"), out}, {QStringLiteral("count"), 0}, {QStringLiteral("total"), 0}};

    const int colId = collection.trimmed().isEmpty() ? 0 : resolveCollectionId(m_library, collection);
    const QString fmt = format.trimmed().toLower();
    const QStringList tokens = WorksModel::searchable(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QList<BookRecord> books = m_library->visibleRecords();
    std::sort(books.begin(), books.end(), [](const BookRecord &a, const BookRecord &b) {
        if (a.openedAt != b.openedAt)
            return a.openedAt > b.openedAt;
        if (a.addedAt != b.addedAt)
            return a.addedAt > b.addedAt;
        return a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
    });

    if (colId >= 0) {
        for (const BookRecord &book : std::as_const(books)) {
            if (!fmt.isEmpty() && book.format.compare(fmt, Qt::CaseInsensitive) != 0)
                continue;
            if (colId > 0 && !m_library->isInCollection(book.id, colId))
                continue;
            if (!tokens.isEmpty()) {
                const QString hay = WorksModel::searchable(
                    book.title + QLatin1Char(' ') + book.author + QLatin1Char(' ') + book.series + QLatin1Char(' ') +
                    book.subjects + QLatin1Char(' ') + book.publisher + QLatin1Char(' ') + book.isbn);
                bool matchesAll = true;
                for (const QString &tok : tokens) {
                    if (!hay.contains(tok)) {
                        matchesAll = false;
                        break;
                    }
                }
                if (!matchesAll)
                    continue;
            }
            ++totalMatched;
            if (out.size() >= limit)
                continue;

            QJsonObject item{
                {QStringLiteral("id"), book.id},
                {QStringLiteral("title"), book.title},
                {QStringLiteral("author"), book.author},
                {QStringLiteral("format"), book.format},
                {QStringLiteral("progress"), book.progress},
                {QStringLiteral("pages"), book.pages},
                {QStringLiteral("chapters"), book.chapters},
                {QStringLiteral("collections"), collectionNamesForBook(m_library, book.id)},
                {QStringLiteral("filePath"), book.path},
                {QStringLiteral("finished"), book.finishedAt > 0},
            };
            if (!book.series.isEmpty())
                item.insert(QStringLiteral("series"), book.series);
            if (book.year > 0)
                item.insert(QStringLiteral("year"), book.year);
            out.append(item);
        }
    }

    return QJsonObject{
        {QStringLiteral("books"), out},
        {QStringLiteral("count"), int(out.size())},
        {QStringLiteral("total"), totalMatched},
        {QStringLiteral("truncated"), totalMatched > int(out.size())},
    };
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
    return toolSuccessJson(buildBooksListJson(query, format, collection, limit));
}

QJsonObject McpServer::toolSearchLibrary(const QJsonObject &args) {
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty())
        return toolError(QStringLiteral("Missing required parameter 'query'."));

    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId))
        return toolError(QStringLiteral("Invalid 'book_id': expected a positive integer."));
    if (hasBookId) {
        if (bookId <= 0 || !m_library || !m_library->record(bookId).visible())
            return toolError(QStringLiteral("Book %1 not found in the library.").arg(bookId));
    }

    int limit = 15;
    if (!readOptionalInt(args, QStringLiteral("limit"), &limit))
        return toolError(QStringLiteral("Invalid 'limit': expected an integer."));
    limit = qBound(1, limit, 50);

    ensureIndexedForSearch(bookId);
    LibraryIndex *idx = ensureIndex();
    if (!idx || !idx->available())
        return toolError(QStringLiteral("Full-text search index (SQLite FTS5) is not available."));

    const QVariantList hits = idx->search(query, limit, bookId);
    QJsonArray results;
    QHash<int, QHash<int, QString>> chapterTitleCache;

    auto chapterTitleFor = [&](const BookRecord &rec, int chapter) -> QString {
        auto it = chapterTitleCache.constFind(rec.id);
        if (it == chapterTitleCache.cend()) {
            QHash<int, QString> map;
            if (rec.format.compare(QLatin1String("epub"), Qt::CaseInsensitive) == 0) {
                EpubBook epub(rec.path);
                if (epub.open() && !epub.isDrmProtected()) {
                    for (const EpubTocEntry &entry : epub.toc()) {
                        if (entry.spine >= 0 && !entry.title.isEmpty() && !map.contains(entry.spine))
                            map.insert(entry.spine, entry.title);
                    }
                }
            }
            it = chapterTitleCache.insert(rec.id, map);
        }
        return it->value(chapter);
    };

    for (const QVariant &v : hits) {
        const QVariantMap hit = v.toMap();
        const int hitBookId = hit.value(QStringLiteral("bookId")).toInt();
        const BookRecord rec = m_library ? m_library->record(hitBookId) : BookRecord{};
        if (!rec.visible())
            continue;
        const int chapter = hit.value(QStringLiteral("chapter")).toInt();
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        QJsonObject item{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("format"), rec.format},
            {QStringLiteral("chapter"), chapter},
            {QStringLiteral("offset"), hit.value(QStringLiteral("offset")).toDouble()},
            {QStringLiteral("snippet"), cleanSnippet(hit.value(QStringLiteral("snippet")).toString())},
        };
        if (isPdf) {
            item.insert(QStringLiteral("page"), chapter + 1);
        } else {
            const QString chTitle = chapterTitleFor(rec, chapter);
            if (!chTitle.isEmpty())
                item.insert(QStringLiteral("chapter_title"), chTitle);
        }
        results.append(item);
    }

    return toolSuccessJson(QJsonObject{
        {QStringLiteral("query"), query},
        {QStringLiteral("count"), int(results.size())},
        {QStringLiteral("results"), results},
    });
}

QJsonObject McpServer::toolSearchPassages(const QJsonObject &args) {
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty())
        return toolError(QStringLiteral("Missing required parameter 'query'."));

    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId))
        return toolError(QStringLiteral("Invalid 'book_id': expected a positive integer."));
    if (hasBookId) {
        if (bookId <= 0 || !m_library || !m_library->record(bookId).visible())
            return toolError(QStringLiteral("Book %1 not found in the library.").arg(bookId));
    }

    int limit = 8;
    if (!readOptionalInt(args, QStringLiteral("limit"), &limit))
        return toolError(QStringLiteral("Invalid 'limit': expected an integer."));
    limit = qBound(1, limit, 25);

    const QString collection = args.value(QStringLiteral("collection")).toString().trimmed();
    int colId = 0;
    if (!collection.isEmpty()) {
        if (!m_library)
            return toolError(QStringLiteral("Library is not available."));
        colId = resolveCollectionId(m_library, collection);
        if (colId < 0) {
            return toolSuccessJson(QJsonObject{
                {QStringLiteral("query"), query},
                {QStringLiteral("mode"), QStringLiteral("lexical")},
                {QStringLiteral("count"), 0},
                {QStringLiteral("passages"), QJsonArray{}},
            });
        }
    }

    QSet<int> allowedBooks;
    if (m_library) {
        if (bookId > 0) {
            if (colId == 0 || m_library->isInCollection(bookId, colId))
                allowedBooks.insert(bookId);
        } else {
            for (const BookRecord &rec : m_library->visibleRecords()) {
                if (colId > 0 && !m_library->isInCollection(rec.id, colId))
                    continue;
                allowedBooks.insert(rec.id);
            }
        }
    }

    if (allowedBooks.isEmpty()) {
        return toolSuccessJson(QJsonObject{
            {QStringLiteral("query"), query},
            {QStringLiteral("mode"), QStringLiteral("lexical")},
            {QStringLiteral("count"), 0},
            {QStringLiteral("passages"), QJsonArray{}},
        });
    }

    struct CitedPassage {
        int bookId = 0;
        int chapter = 0;
        qint64 start = 0;
        qint64 end = 0;
        QString text;
        double score = 0.0;
        QString matchKind;
    };

    QList<CitedPassage> chosen;
    QSet<QPair<int, int>> seenChapters;
    bool usedSemantic = false;
    bool usedLexical = false;

    // 1. Try semantic search via PassageIndex::searchLibrary() when embeddings are available.
    if (m_ai && m_library && m_library->database().isOpen()) {
        PassageIndex passageIndex(m_ai, m_library->database().connectionName());
        if (passageIndex.booksPrepared() > 0) {
            QList<PassageIndex::LibraryHit> semHits;
            bool finished = false;
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            const int askCount = (bookId > 0 || colId > 0) ? qMin(120, limit * 6) : qMin(60, limit * 2);
            passageIndex.searchLibrary(query, askCount, [&](const QList<PassageIndex::LibraryHit> &hits) {
                semHits = hits;
                finished = true;
                loop.quit();
            });
            if (!finished) {
                timeout.start(10000);
                loop.exec();
            }
            for (const PassageIndex::LibraryHit &h : std::as_const(semHits)) {
                if (!allowedBooks.contains(h.bookId))
                    continue;
                const QString text = extractPassageText(h.bookId, h.chapter, h.start, h.end);
                if (text.isEmpty())
                    continue;
                seenChapters.insert({h.bookId, h.chapter});
                chosen.append({h.bookId, h.chapter, h.start, h.end, text, h.score, QStringLiteral("semantic")});
                usedSemantic = true;
                if (chosen.size() >= limit)
                    break;
            }
        }
    }

    // 2. Automatic FTS5 + BM25 lexical fallback (or hybrid completion when semantic hits < limit).
    if (chosen.size() < limit) {
        ensureIndexedForSearch(bookId);
        LibraryIndex *idx = ensureIndex();

        // Gather candidate (bookId, chapter) pairs via FTS5 first.
        QList<QPair<int, int>> candidates;
        QSet<QPair<int, int>> seenCand;
        auto addCandidate = [&](int bId, int ch) {
            if (!allowedBooks.contains(bId))
                return;
            if (seenChapters.contains({bId, ch}) || seenCand.contains({bId, ch}))
                return;
            seenCand.insert({bId, ch});
            candidates.append({bId, ch});
        };

        if (idx && idx->available()) {
            const int ftsLimit = qMin(60, qMax(24, limit * 4));
            const QVariantList direct = idx->search(query, ftsLimit, bookId);
            for (const QVariant &v : direct) {
                const QVariantMap m = v.toMap();
                addCandidate(m.value(QStringLiteral("bookId")).toInt(), m.value(QStringLiteral("chapter")).toInt());
            }
            // If full phrase/AND FTS5 found few candidates (e.g. natural-language question),
            // retry with significant non-stopword terms and individual terms.
            if (candidates.size() < limit) {
                const QStringList terms = significantQueryTerms(query);
                if (terms.size() > 1) {
                    const QVariantList sig = idx->search(terms.join(QLatin1Char(' ')), ftsLimit, bookId);
                    for (const QVariant &v : sig) {
                        const QVariantMap m = v.toMap();
                        addCandidate(m.value(QStringLiteral("bookId")).toInt(),
                                     m.value(QStringLiteral("chapter")).toInt());
                    }
                }
                if (candidates.size() < limit) {
                    for (const QString &term : terms) {
                        const QVariantList perTerm = idx->search(term, qMin(20, ftsLimit), bookId);
                        for (const QVariant &v : perTerm) {
                            const QVariantMap m = v.toMap();
                            addCandidate(m.value(QStringLiteral("bookId")).toInt(),
                                         m.value(QStringLiteral("chapter")).toInt());
                        }
                        if (candidates.size() >= 32)
                            break;
                    }
                }
            }
        }

        // Split candidate chapters into paragraph-aligned passages and rank with PassageIndex::byWords (BM25).
        struct CandidatePassage {
            int bookId = 0;
            PassageIndex::Passage passage;
        };
        QList<CandidatePassage> pool;
        QList<PassageIndex::Passage> bm25Input;

        auto appendBookChapters = [&](const BookRecord &rec, const QList<int> &chaptersToRead) {
            if (!rec.visible() || rec.path.isEmpty() || !QFileInfo::exists(rec.path))
                return;
            if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
                PdfBook pdf(rec.path);
                if (!pdf.open())
                    return;
                const int total = pdf.pageCount();
                if (chaptersToRead.isEmpty()) {
                    const int cap = qMin(total, 80);
                    for (int p = 0; p < cap; ++p) {
                        const QString text = pdf.pageText(p);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(p, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                } else {
                    for (int p : chaptersToRead) {
                        if (p < 0 || p >= total)
                            continue;
                        const QString text = pdf.pageText(p);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(p, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                }
            } else {
                EpubBook epub(rec.path);
                if (!epub.open() || epub.isDrmProtected())
                    return;
                const int total = int(epub.spine().size());
                if (chaptersToRead.isEmpty()) {
                    const int cap = qMin(total, 80);
                    for (int ch = 0; ch < cap; ++ch) {
                        const QString text = epub.chapterText(ch);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(ch, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                } else {
                    for (int ch : chaptersToRead) {
                        if (ch < 0 || ch >= total)
                            continue;
                        const QString text = epub.chapterText(ch);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(ch, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                }
            }
        };

        if (!candidates.isEmpty()) {
            // Group candidate chapters by book so each EPUB/PDF is opened once.
            QHash<int, QList<int>> byBook;
            const int maxCand = qMin(int(candidates.size()), 24);
            for (int i = 0; i < maxCand; ++i)
                byBook[candidates[i].first].append(candidates[i].second);
            for (auto it = byBook.cbegin(); it != byBook.cend(); ++it)
                appendBookChapters(m_library->record(it.key()), it.value());
        } else if (allowedBooks.size() <= 8) {
            // Small scope (single book or small library) without FTS hits: direct BM25 scan.
            for (int allowedId : std::as_const(allowedBooks))
                appendBookChapters(m_library->record(allowedId), {});
        }

        if (!bm25Input.isEmpty()) {
            // Tag each passage's chapter temporarily with its pool index so we can map Hit back to bookId.
            QList<PassageIndex::Passage> tagged = bm25Input;
            for (int i = 0; i < tagged.size(); ++i)
                tagged[i].chapter = i;
            const int needed = limit - int(chosen.size());
            const QList<PassageIndex::Hit> bmHits = PassageIndex::byWords(tagged, query, needed * 3);
            for (const PassageIndex::Hit &h : bmHits) {
                const int poolIdx = h.passage.chapter;
                if (poolIdx < 0 || poolIdx >= pool.size())
                    continue;
                const CandidatePassage &cp = pool[poolIdx];
                if (seenChapters.contains({cp.bookId, cp.passage.chapter}) && allowedBooks.size() > 1)
                    continue;
                seenChapters.insert({cp.bookId, cp.passage.chapter});
                chosen.append({cp.bookId, cp.passage.chapter, cp.passage.start, cp.passage.end, cp.passage.text,
                               h.lexical > 0 ? h.lexical : h.score, QStringLiteral("lexical")});
                usedLexical = true;
                if (chosen.size() >= limit)
                    break;
            }
        }
    }

    QJsonArray passagesJson;
    for (const CitedPassage &p : std::as_const(chosen)) {
        const BookRecord rec = m_library->record(p.bookId);
        if (!rec.visible())
            continue;
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        QJsonObject item{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("format"), rec.format},
            {QStringLiteral("chapter"), p.chapter},
            {QStringLiteral("page"), isPdf ? QJsonValue(p.chapter + 1) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("start"), double(p.start)},
            {QStringLiteral("end"), double(p.end)},
            {QStringLiteral("text"), p.text},
            {QStringLiteral("score"), std::round(p.score * 10000.0) / 10000.0},
            {QStringLiteral("match"), p.matchKind},
        };
        passagesJson.append(item);
    }

    const QString mode = (usedSemantic && usedLexical) ? QStringLiteral("hybrid")
                         : usedSemantic                ? QStringLiteral("semantic")
                                                       : QStringLiteral("lexical");

    return toolSuccessJson(QJsonObject{
        {QStringLiteral("query"), query},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("count"), int(passagesJson.size())},
        {QStringLiteral("passages"), passagesJson},
    });
}

QJsonObject McpServer::buildBookOutlineJson(int bookId, QString *errorOut) const {
    if (!m_library || bookId <= 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
        return {};
    }
    const BookRecord rec = m_library->record(bookId);
    if (!rec.visible()) {
        if (errorOut)
            *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
        return {};
    }
    if (!QFileInfo::exists(rec.path)) {
        if (errorOut)
            *errorOut = QStringLiteral("Book file is missing on disk: %1").arg(rec.path);
        return {};
    }

    QJsonObject out{
        {QStringLiteral("book_id"), rec.id},
        {QStringLiteral("id"), rec.id},
        {QStringLiteral("title"), rec.title},
        {QStringLiteral("author"), rec.author},
        {QStringLiteral("format"), rec.format},
        {QStringLiteral("progress"), rec.progress},
        {QStringLiteral("pages"), rec.pages},
        {QStringLiteral("chapters"), rec.chapters},
        {QStringLiteral("collections"), collectionNamesForBook(m_library, rec.id)},
        {QStringLiteral("filePath"), rec.path},
    };
    if (!rec.editor.isEmpty())
        out.insert(QStringLiteral("editor"), rec.editor);
    if (!rec.publisher.isEmpty())
        out.insert(QStringLiteral("publisher"), rec.publisher);
    if (rec.year > 0)
        out.insert(QStringLiteral("year"), rec.year);
    if (!rec.language.isEmpty())
        out.insert(QStringLiteral("language"), rec.language);
    if (!rec.series.isEmpty())
        out.insert(QStringLiteral("series"), rec.series);
    if (!rec.description.isEmpty())
        out.insert(QStringLiteral("description"), rec.description);

    QJsonArray toc;
    QJsonArray pageMap;

    if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(rec.path);
        if (!pdf.open()) {
            if (errorOut)
                *errorOut = pdf.errorString();
            return {};
        }
        const int pageCount = pdf.pageCount();
        out.insert(QStringLiteral("pages"), pageCount);
        const QList<PdfOutlineEntry> outline = pdf.outline();
        out.insert(QStringLiteral("chapters"), int(outline.size()));
        for (const PdfOutlineEntry &entry : outline) {
            toc.append(QJsonObject{
                {QStringLiteral("title"), entry.title},
                {QStringLiteral("depth"), entry.depth},
                {QStringLiteral("page"), entry.page + 1},
            });
        }
        // Include non-trivial printed page labels (e.g. Roman numerals) if present.
        constexpr int kMaxPageMap = 500;
        bool hasCustomLabels = false;
        for (int p = 0; p < qMin(pageCount, kMaxPageMap); ++p) {
            const QString label = pdf.pageLabel(p).trimmed();
            if (!label.isEmpty() && label != QString::number(p + 1)) {
                hasCustomLabels = true;
                break;
            }
        }
        if (hasCustomLabels) {
            for (int p = 0; p < qMin(pageCount, kMaxPageMap); ++p) {
                const QString label = pdf.pageLabel(p).trimmed();
                if (!label.isEmpty())
                    pageMap.append(QJsonObject{{QStringLiteral("page"), p + 1}, {QStringLiteral("label"), label}});
            }
        }
    } else {
        EpubBook epub(rec.path);
        if (!epub.open()) {
            if (errorOut)
                *errorOut = epub.errorString();
            return {};
        }
        if (epub.isDrmProtected()) {
            if (errorOut)
                *errorOut = QStringLiteral("This book is protected by DRM.");
            return {};
        }
        const int spineCount = int(epub.spine().size());
        out.insert(QStringLiteral("chapters"), spineCount);
        if (!epub.pageList().isEmpty())
            out.insert(QStringLiteral("pages"), int(epub.pageList().size()));

        QHash<int, QString> spineTitle;
        for (const EpubTocEntry &entry : epub.toc()) {
            toc.append(QJsonObject{
                {QStringLiteral("title"), entry.title},
                {QStringLiteral("depth"), entry.depth},
                {QStringLiteral("chapter"), entry.spine},
            });
            if (entry.spine >= 0 && !entry.title.isEmpty() && !spineTitle.contains(entry.spine))
                spineTitle.insert(entry.spine, entry.title);
        }

        QJsonArray spineJson;
        for (int i = 0; i < spineCount; ++i) {
            const EpubSpineItem &item = epub.spine()[i];
            QJsonObject entry{
                {QStringLiteral("chapter"), i},
                {QStringLiteral("linear"), item.linear},
            };
            if (spineTitle.contains(i))
                entry.insert(QStringLiteral("title"), spineTitle.value(i));
            spineJson.append(entry);
        }
        out.insert(QStringLiteral("spine"), spineJson);

        constexpr int kMaxPageMap = 2000;
        const auto &pageList = epub.pageList();
        for (int i = 0; i < qMin(int(pageList.size()), kMaxPageMap); ++i) {
            const EpubPageTarget &pt = pageList[i];
            pageMap.append(QJsonObject{
                {QStringLiteral("label"), pt.label},
                {QStringLiteral("chapter"), pt.spine},
            });
        }
    }

    out.insert(QStringLiteral("toc"), toc);
    if (!pageMap.isEmpty())
        out.insert(QStringLiteral("page_map"), pageMap);
    return out;
}

QJsonObject McpServer::toolGetBookOutline(const QJsonObject &args) {
    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId) || !hasBookId || bookId <= 0)
        return toolError(QStringLiteral("Missing or invalid required parameter 'book_id'."));

    QString err;
    const QJsonObject outline = buildBookOutlineJson(bookId, &err);
    if (!err.isEmpty())
        return toolError(err);
    return toolSuccessJson(outline);
}

QJsonObject McpServer::toolReadPassage(const QJsonObject &args) {
    int bookId = 0;
    bool hasBookId = false;
    if (!readOptionalInt(args, QStringLiteral("book_id"), &bookId, &hasBookId) || !hasBookId || bookId <= 0)
        return toolError(QStringLiteral("Missing or invalid required parameter 'book_id'."));

    if (!m_library)
        return toolError(QStringLiteral("Library is not available."));
    const BookRecord rec = m_library->record(bookId);
    if (!rec.visible())
        return toolError(QStringLiteral("Book %1 not found in the library.").arg(bookId));
    if (!QFileInfo::exists(rec.path))
        return toolError(QStringLiteral("Book file is missing on disk: %1").arg(rec.path));

    int chapterArg = 0;
    bool hasChapter = false;
    if (!readOptionalInt(args, QStringLiteral("chapter"), &chapterArg, &hasChapter))
        return toolError(QStringLiteral("Invalid 'chapter': expected an integer."));

    int pageArg = 0;
    bool hasPage = false;
    if (!readOptionalInt(args, QStringLiteral("page"), &pageArg, &hasPage))
        return toolError(QStringLiteral("Invalid 'page': expected an integer."));

    int pageEndArg = 0;
    bool hasPageEnd = false;
    if (!readOptionalInt(args, QStringLiteral("page_end"), &pageEndArg, &hasPageEnd))
        return toolError(QStringLiteral("Invalid 'page_end': expected an integer."));

    int offsetArg = 0;
    if (!readOptionalInt(args, QStringLiteral("offset"), &offsetArg) || offsetArg < 0)
        return toolError(QStringLiteral("Invalid 'offset': expected a non-negative integer."));

    int maxChars = kDefaultReadMaxChars;
    if (!readOptionalInt(args, QStringLiteral("max_chars"), &maxChars))
        return toolError(QStringLiteral("Invalid 'max_chars': expected an integer."));
    maxChars = qBound(kMinReadMaxChars, maxChars, kHardCapReadMaxChars);

    const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
    QString rawText;
    QString citationLocation;
    QString sectionTitle;
    int resolvedChapter = 0;
    int resolvedPage = 0;
    int resolvedPageEnd = 0;

    if (isPdf) {
        PdfBook pdf(rec.path);
        if (!pdf.open())
            return toolError(pdf.errorString());
        const int totalPages = pdf.pageCount();
        if (totalPages <= 0)
            return toolError(QStringLiteral("This PDF has no pages."));

        int startPage = 1;
        if (hasPage)
            startPage = pageArg;
        else if (hasChapter)
            startPage = chapterArg + 1;

        if (startPage < 1 || startPage > totalPages) {
            return toolError(QStringLiteral("Page %1 is out of range (valid pages: 1–%2).")
                                 .arg(startPage)
                                 .arg(totalPages));
        }

        int endPage = hasPageEnd ? pageEndArg : startPage;
        if (endPage < startPage)
            return toolError(QStringLiteral("'page_end' (%1) must be >= starting page (%2).").arg(endPage).arg(startPage));
        endPage = qMin(endPage, totalPages);
        // Prevent pathological huge page ranges from spinning CPU when max_chars is bounded.
        endPage = qMin(endPage, startPage + 49);

        resolvedChapter = startPage - 1;
        resolvedPage = startPage;
        resolvedPageEnd = endPage;
        sectionTitle = pdfSectionTitle(pdf, startPage - 1);

        QStringList pieces;
        qsizetype accumulated = 0;
        const qsizetype stopCollectingAt = qsizetype(offsetArg) + qsizetype(maxChars) + 256;
        for (int p = startPage; p <= endPage; ++p) {
            const QString pageText = pdf.pageText(p - 1);
            if (endPage > startPage) {
                const QString label = pdf.pageLabel(p - 1).trimmed();
                const QString header = (!label.isEmpty() && label != QString::number(p))
                                           ? QStringLiteral("--- Page %1 (%2) ---\n").arg(p).arg(label)
                                           : QStringLiteral("--- Page %1 ---\n").arg(p);
                pieces.append(header + pageText);
                accumulated += header.size() + pageText.size() + 2;
            } else {
                pieces.append(pageText);
                accumulated += pageText.size();
            }
            if (accumulated >= stopCollectingAt) {
                resolvedPageEnd = p;
                break;
            }
        }
        rawText = pieces.join(QStringLiteral("\n\n"));

        const QString printedLabel = pdf.pageLabel(startPage - 1).trimmed();
        if (resolvedPageEnd > startPage) {
            citationLocation = QStringLiteral("pages %1–%2 of %3").arg(startPage).arg(resolvedPageEnd).arg(totalPages);
        } else if (!printedLabel.isEmpty() && printedLabel != QString::number(startPage)) {
            citationLocation =
                QStringLiteral("page %1 (printed %2) of %3").arg(startPage).arg(printedLabel).arg(totalPages);
        } else {
            citationLocation = QStringLiteral("page %1 of %2").arg(startPage).arg(totalPages);
        }
        if (!sectionTitle.isEmpty())
            citationLocation += QStringLiteral(" · ") + sectionTitle;
    } else {
        EpubBook epub(rec.path);
        if (!epub.open())
            return toolError(epub.errorString());
        if (epub.isDrmProtected())
            return toolError(QStringLiteral("This book is protected by DRM."));
        const int spineCount = int(epub.spine().size());
        if (spineCount <= 0)
            return toolError(QStringLiteral("This EPUB has no readable chapters."));

        int spineIdx = 0;
        if (hasChapter) {
            spineIdx = chapterArg;
        } else if (hasPage) {
            int matchedSpine = -1;
            const QString targetLabel = QString::number(pageArg);
            for (const EpubPageTarget &pt : epub.pageList()) {
                if (pt.label.trimmed() == targetLabel && pt.spine >= 0) {
                    matchedSpine = pt.spine;
                    break;
                }
            }
            if (matchedSpine >= 0) {
                spineIdx = matchedSpine;
                resolvedPage = pageArg;
            } else if (pageArg >= 1 && pageArg <= spineCount) {
                spineIdx = pageArg - 1;
            } else {
                return toolError(QStringLiteral("Page %1 could not be mapped to a chapter in this EPUB (chapters: 0–%2).")
                                     .arg(pageArg)
                                     .arg(spineCount - 1));
            }
        }

        if (spineIdx < 0 || spineIdx >= spineCount) {
            return toolError(QStringLiteral("Chapter %1 is out of range (valid chapters: 0–%2).")
                                 .arg(spineIdx)
                                 .arg(spineCount - 1));
        }

        resolvedChapter = spineIdx;
        sectionTitle = epubChapterTitle(epub, spineIdx);
        rawText = epub.chapterText(spineIdx);

        citationLocation = QStringLiteral("chapter %1/%2").arg(spineIdx).arg(spineCount - 1);
        if (resolvedPage > 0)
            citationLocation += QStringLiteral(" (print p. %1)").arg(resolvedPage);
        if (!sectionTitle.isEmpty())
            citationLocation += QStringLiteral(" · ") + sectionTitle;
    }

    const qsizetype totalChars = rawText.size();
    const qsizetype clampedOffset = std::clamp<qsizetype>(qsizetype(offsetArg), 0, totalChars);
    const QString slice = rawText.mid(clampedOffset, maxChars);
    const bool truncated = (clampedOffset + slice.size()) < totalChars;

    QString authorPart = rec.author.isEmpty() ? QString() : QStringLiteral(" — ") + rec.author;
    const QString citation = QStringLiteral("[Citation: %1%2 | book_id=%3 | %4]")
                                 .arg(rec.title, authorPart)
                                 .arg(rec.id)
                                 .arg(citationLocation);

    QString formatted = citation + QStringLiteral("\n\n") + slice;
    if (truncated) {
        formatted += QStringLiteral("\n\n[Truncated at %1 chars (offset %2 of %3). Pass offset=%4 to continue.]")
                         .arg(slice.size())
                         .arg(clampedOffset)
                         .arg(totalChars)
                         .arg(clampedOffset + slice.size());
    }

    QJsonObject structured{
        {QStringLiteral("book_id"), rec.id},
        {QStringLiteral("title"), rec.title},
        {QStringLiteral("author"), rec.author},
        {QStringLiteral("format"), rec.format},
        {QStringLiteral("chapter"), resolvedChapter},
        {QStringLiteral("chapter_title"), sectionTitle},
        {QStringLiteral("page"), resolvedPage > 0 ? QJsonValue(resolvedPage) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("page_end"), resolvedPageEnd > 0 ? QJsonValue(resolvedPageEnd) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("offset"), double(clampedOffset)},
        {QStringLiteral("total_chars"), double(totalChars)},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("citation"), citation},
        {QStringLiteral("text"), slice},
    };

    return toolSuccess(formatted, structured);
}

QJsonObject McpServer::buildBookMarksJson(int bookId, const QString &query, int limit, QString *errorOut) const {
    if (!m_library) {
        if (errorOut)
            *errorOut = QStringLiteral("Library is not available.");
        return {};
    }

    if (bookId > 0) {
        const BookRecord rec = m_library->record(bookId);
        if (!rec.visible()) {
            if (errorOut)
                *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
            return {};
        }

        Marks marks(m_library);
        marks.setBook(bookId);
        const QStringList tokens = WorksModel::searchable(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;

        QJsonArray arr;
        for (int row = 0; row < marks.count() && arr.size() < limit; ++row) {
            const int id = marks.data(marks.index(row), Marks::IdRole).toInt();
            const QVariantMap m = marks.mark(id);
            const QString quote = m.value(QStringLiteral("text")).toString();
            const QString note = m.value(QStringLiteral("note")).toString();
            const QString label = m.value(QStringLiteral("label")).toString();
            if (!tokens.isEmpty()) {
                const QString hay =
                    WorksModel::searchable(quote + QLatin1Char(' ') + note + QLatin1Char(' ') + label + QLatin1Char(' ') +
                                           rec.title + QLatin1Char(' ') + rec.author);
                bool ok = true;
                for (const QString &tok : tokens) {
                    if (!hay.contains(tok)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    continue;
            }
            const int chapter = m.value(QStringLiteral("chapter")).toInt();
            QJsonObject entry{
                {QStringLiteral("id"), id},
                {QStringLiteral("book_id"), rec.id},
                {QStringLiteral("book_title"), rec.title},
                {QStringLiteral("book_author"), rec.author},
                {QStringLiteral("kind"), m.value(QStringLiteral("kind")).toString()},
                {QStringLiteral("chapter"), chapter},
                {QStringLiteral("page"), isPdf ? QJsonValue(chapter + 1) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("label"), label},
                {QStringLiteral("quote"), quote},
                {QStringLiteral("note"), note},
                {QStringLiteral("color"), m.value(QStringLiteral("color")).toString()},
                {QStringLiteral("start"), double(m.value(QStringLiteral("start")).toLongLong())},
                {QStringLiteral("end"), double(m.value(QStringLiteral("end")).toLongLong())},
                {QStringLiteral("created"), m.value(QStringLiteral("created")).toDateTime().toString(Qt::ISODate)},
            };
            arr.append(entry);
        }

        return QJsonObject{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("book_title"), rec.title},
            {QStringLiteral("count"), int(arr.size())},
            {QStringLiteral("marks"), arr},
        };
    }

    Marks marks(m_library);
    const QVariantList found = marks.searchAll(query, limit);
    QJsonArray arr;
    for (const QVariant &v : found) {
        const QVariantMap m = v.toMap();
        const int bId = m.value(QStringLiteral("bookId")).toInt();
        const BookRecord rec = m_library->record(bId);
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        const int chapter = m.value(QStringLiteral("chapter")).toInt();
        arr.append(QJsonObject{
            {QStringLiteral("id"), m.value(QStringLiteral("id")).toInt()},
            {QStringLiteral("book_id"), bId},
            {QStringLiteral("book_title"), m.value(QStringLiteral("bookTitle")).toString()},
            {QStringLiteral("book_author"), m.value(QStringLiteral("bookAuthor")).toString()},
            {QStringLiteral("kind"), m.value(QStringLiteral("kind")).toString()},
            {QStringLiteral("chapter"), chapter},
            {QStringLiteral("page"), isPdf ? QJsonValue(chapter + 1) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("label"), m.value(QStringLiteral("label")).toString()},
            {QStringLiteral("quote"), m.value(QStringLiteral("text")).toString()},
            {QStringLiteral("note"), m.value(QStringLiteral("note")).toString()},
            {QStringLiteral("color"), m.value(QStringLiteral("color")).toString()},
            {QStringLiteral("start"), double(m.value(QStringLiteral("start")).toLongLong())},
            {QStringLiteral("end"), double(m.value(QStringLiteral("end")).toLongLong())},
            {QStringLiteral("created"), m.value(QStringLiteral("created")).toDateTime().toString(Qt::ISODate)},
        });
    }

    return QJsonObject{
        {QStringLiteral("count"), int(arr.size())},
        {QStringLiteral("marks"), arr},
    };
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
    const QJsonObject payload = buildBookMarksJson(hasBookId ? bookId : 0, query, limit, &err);
    if (!err.isEmpty())
        return toolError(err);
    return toolSuccessJson(payload);
}

int McpServer::runStdio(const QStringList &arguments) {
    Q_UNUSED(arguments);
    installStderrMessageHandler();

    Library library;
    if (!library.open()) {
        std::fprintf(stderr, "[clothbound-mcp ERROR] The library can't be opened: %s\n", qPrintable(library.errorString()));
        std::fflush(stderr);
        return 1;
    }

    // Wire local embedder support only when passage_vectors exist in the library,
    // without touching the desktop keyring (staying strictly local and non-blocking).
    std::unique_ptr<GeminiProvider> gemini;
    std::unique_ptr<LocalModels> localModels;
    std::unique_ptr<AiServices> aiServices;
    if (databaseHasPassageVectors(library.database())) {
        gemini = std::make_unique<GeminiProvider>();
        aiServices = std::make_unique<AiServices>(gemini.get(), nullptr, library.database().connectionName());
        localModels = std::make_unique<LocalModels>();
        aiServices->setLocalModels(localModels.get());
    }

    McpServer server(&library, nullptr, aiServices.get());

    auto writeReply = [](const QJsonObject &reply) {
        if (reply.isEmpty())
            return;
        const QByteArray bytes = QJsonDocument(reply).toJson(QJsonDocument::Compact);
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };

    auto processLine = [&](const QByteArray &raw) {
        const QByteArray trimmed = raw.trimmed();
        if (trimmed.isEmpty())
            return;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
        if (err.error != QJsonParseError::NoError) {
            writeReply(jsonRpcError(QJsonValue::Null, kErrParse,
                                    QStringLiteral("Parse error: %1").arg(err.errorString())));
            return;
        }
        if (!doc.isObject()) {
            writeReply(jsonRpcError(QJsonValue::Null, kErrInvalidRequest,
                                    QStringLiteral("Invalid Request: expected a JSON object.")));
            return;
        }
        writeReply(server.handleRequest(doc.object()));
    };

    QByteArray buffer;
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), stdin) != nullptr) {
        buffer.append(chunk);
        if (!buffer.endsWith('\n'))
            continue;
        processLine(buffer);
        buffer.clear();
    }
    if (!buffer.isEmpty())
        processLine(buffer);

    return 0;
}
