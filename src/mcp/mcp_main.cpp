// Lean WebEngine-free stdio MCP server and headless read-only CLI for Clothbound.
// Shares Clothbound's read-only library/index/settings paths via LibraryQueryService
// without linking QtWebEngine, QtQuick, QtQml, or QtMultimedia.

#include "mcp/libraryqueryservice.h"
#include "mcp/mcpserver.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cstdio>

#ifndef CLOTHBOUND_VERSION
#define CLOTHBOUND_VERSION "0.5.0"
#endif

namespace {

void printLine(std::FILE *stream, const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

void printJson(const QJsonObject &obj) {
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

int fail(const QString &message) {
    printLine(stderr, message);
    return 1;
}

void printUsage() {
    std::fputs(
        "Usage: clothbound-mcp [mcp | list | search | notes] [options]\n"
        "\n"
        "WebEngine-free read-only local MCP server and headless CLI for Clothbound.\n"
        "Opens library.sqlite and index.sqlite strictly read-only without mutating files.\n"
        "\n"
        "Commands:\n"
        "  mcp                          Run the local stdio JSON-RPC 2.0 MCP server (default)\n"
        "  list [--json]                List books in the library with reading progress\n"
        "  search <query> [--json]      Full-text FTS5 + lexical search across books\n"
        "  notes [book] [--json]        Print highlights, notes, and bookmarks as Markdown or JSON\n"
        "\n"
        "Options:\n"
        "  -h, --help                   Display this help and exit.\n"
        "  -v, --version                Display version information and exit.\n",
        stdout);
    std::fflush(stdout);
}

bool openReadOnlyService(LibraryQueryService &service) {
    QString err;
    if (service.openReadOnlyLibrary(&err))
        return true;
    printLine(stderr, QStringLiteral("The library can't be opened read-only: %1").arg(err));
    return false;
}

int runList(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("List books in the Clothbound library (read-only)."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("list"), QStringLiteral("List books"), QStringLiteral("list"));
    parser.addOption({QStringLiteral("json"), QStringLiteral("Print the result as JSON")});
    parser.addOption({QStringLiteral("query"), QStringLiteral("Filter matching title, author, series, or subjects"),
                      QStringLiteral("text")});
    parser.addOption({QStringLiteral("format"), QStringLiteral("Filter by format: epub or pdf"), QStringLiteral("fmt")});
    parser.addOption({QStringLiteral("collection"), QStringLiteral("Filter by collection name or id"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("limit"), QStringLiteral("Maximum number of books to return"), QStringLiteral("n")});

    if (!parser.parse(arguments))
        return fail(parser.errorText());
    if (parser.isSet(helpOpt)) {
        std::fputs(qPrintable(parser.helpText()), stdout);
        std::fflush(stdout);
        return 0;
    }

    const QString format = parser.value(QStringLiteral("format")).trimmed().toLower();
    if (!format.isEmpty() && format != QLatin1String("epub") && format != QLatin1String("pdf"))
        return fail(QStringLiteral("Invalid --format: must be 'epub' or 'pdf'."));

    int limit = 10000;
    if (parser.isSet(QStringLiteral("limit"))) {
        bool ok = false;
        limit = parser.value(QStringLiteral("limit")).toInt(&ok);
        if (!ok || limit <= 0)
            return fail(QStringLiteral("Invalid --limit: expected a positive integer."));
    }

    LibraryQueryService service(nullptr, nullptr, nullptr,
                                {LibraryQueryService::defaultIndexFile(), LibraryQueryService::defaultLibraryFile()});
    if (!openReadOnlyService(service))
        return 1;

    const QJsonObject payload = service.listBooks(parser.value(QStringLiteral("query")), format,
                                                  parser.value(QStringLiteral("collection")), limit);
    if (parser.isSet(QStringLiteral("json"))) {
        printJson(payload);
        return 0;
    }

    const QJsonArray books = payload.value(QStringLiteral("books")).toArray();
    for (const QJsonValue &val : books) {
        const QJsonObject book = val.toObject();
        QString line = QStringLiteral("[#%1] %2")
                           .arg(book.value(QStringLiteral("id")).toInt())
                           .arg(book.value(QStringLiteral("title")).toString());
        const QString author = book.value(QStringLiteral("author")).toString();
        if (!author.isEmpty())
            line += QStringLiteral(" — ") + author;
        if (book.value(QStringLiteral("finished")).toBool()) {
            line += QStringLiteral("  ✓");
        } else {
            const double progress = book.value(QStringLiteral("progress")).toDouble();
            if (progress > 0.0)
                line += QStringLiteral("  %1%").arg(qRound(progress * 100.0));
        }
        printLine(stdout, line);
    }
    return 0;
}

int runSearch(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Search across books in the library (read-only FTS5 + lexical)."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("search"), QStringLiteral("Search library"), QStringLiteral("search"));
    parser.addPositionalArgument(QStringLiteral("query"), QStringLiteral("Keywords or phrase to search for"),
                                 QStringLiteral("<query>"));
    parser.addOption({QStringLiteral("book"), QStringLiteral("Scope search to a single book id"), QStringLiteral("id")});
    parser.addOption({QStringLiteral("limit"), QStringLiteral("Maximum number of matches (default 15, max 50)"),
                      QStringLiteral("n"), QStringLiteral("15")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Print the result as JSON")});

    if (!parser.parse(arguments))
        return fail(parser.errorText());
    if (parser.isSet(helpOpt)) {
        std::fputs(qPrintable(parser.helpText()), stdout);
        std::fflush(stdout);
        return 0;
    }

    const QString query = parser.positionalArguments().mid(1).join(QLatin1Char(' ')).trimmed();
    if (query.isEmpty())
        return fail(QStringLiteral("Give a search query: clothbound-mcp search <query> [--book <id>] [--limit <n>] [--json]"));

    int bookId = 0;
    if (parser.isSet(QStringLiteral("book"))) {
        bool ok = false;
        bookId = parser.value(QStringLiteral("book")).toInt(&ok);
        if (!ok || bookId <= 0)
            return fail(QStringLiteral("Invalid --book id: expected a positive integer."));
    }

    bool okLimit = false;
    int limit = parser.value(QStringLiteral("limit")).toInt(&okLimit);
    if (!okLimit || limit <= 0)
        return fail(QStringLiteral("Invalid --limit value: expected a positive integer."));
    limit = qBound(1, limit, 50);

    LibraryQueryService service(nullptr, nullptr, nullptr,
                                {LibraryQueryService::defaultIndexFile(), LibraryQueryService::defaultLibraryFile()});
    if (!openReadOnlyService(service))
        return 1;

    QString err;
    const QJsonObject payload = service.searchLibrary(query, bookId, limit, &err);
    if (!err.isEmpty())
        return fail(err);

    if (parser.isSet(QStringLiteral("json"))) {
        printJson(payload);
        return 0;
    }

    const QJsonArray results = payload.value(QStringLiteral("results")).toArray();
    if (results.isEmpty()) {
        printLine(stderr, QStringLiteral("No matches for “%1”.").arg(query));
        return 0;
    }
    for (const QJsonValue &val : results) {
        const QJsonObject item = val.toObject();
        QString where = item.contains(QStringLiteral("page"))
                            ? QStringLiteral("p. %1").arg(item.value(QStringLiteral("page")).toInt())
                            : QStringLiteral("chapter %1").arg(item.value(QStringLiteral("chapter")).toInt());
        const QString chTitle = item.value(QStringLiteral("chapter_title")).toString();
        if (!chTitle.isEmpty())
            where += QStringLiteral(" · ") + chTitle;
        const QString author = item.value(QStringLiteral("author")).toString();
        printLine(stdout, QStringLiteral("[#%1] %2%3 (%4)")
                              .arg(item.value(QStringLiteral("book_id")).toInt())
                              .arg(item.value(QStringLiteral("title")).toString(),
                                   author.isEmpty() ? QString() : QStringLiteral(" — ") + author, where));
        printLine(stdout, QStringLiteral("  ") + item.value(QStringLiteral("snippet")).toString());
    }
    return 0;
}

int runNotes(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Print highlights, notes, and bookmarks for a book (or the whole library) as Markdown or JSON."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("notes"), QStringLiteral("Export notes"), QStringLiteral("notes"));
    parser.addPositionalArgument(QStringLiteral("book"), QStringLiteral("Optional book id, file path, or title words"),
                                 QStringLiteral("[book]"));
    parser.addOption({QStringLiteral("query"), QStringLiteral("Optional filter matching quote, note, label, or book"),
                      QStringLiteral("text")});
    parser.addOption({QStringLiteral("limit"), QStringLiteral("Maximum number of marks to return (default 200)"),
                      QStringLiteral("n"), QStringLiteral("200")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Print the result as JSON")});

    if (!parser.parse(arguments))
        return fail(parser.errorText());
    if (parser.isSet(helpOpt)) {
        std::fputs(qPrintable(parser.helpText()), stdout);
        std::fflush(stdout);
        return 0;
    }

    bool okLimit = false;
    int limit = parser.value(QStringLiteral("limit")).toInt(&okLimit);
    if (!okLimit || limit <= 0)
        return fail(QStringLiteral("Invalid --limit value: expected a positive integer."));
    limit = qBound(1, limit, 1000);

    LibraryQueryService service(nullptr, nullptr, nullptr,
                                {LibraryQueryService::defaultIndexFile(), LibraryQueryService::defaultLibraryFile()});
    if (!openReadOnlyService(service))
        return 1;

    const QString wanted = parser.positionalArguments().mid(1).join(QLatin1Char(' ')).trimmed();
    int bookId = 0;
    QString bookTitle;
    if (!wanted.isEmpty()) {
        const QList<BookRecord> found = service.matchBooks(wanted);
        if (found.isEmpty())
            return fail(QStringLiteral("No book in the library matches “%1”.").arg(wanted));
        if (found.size() > 1) {
            QStringList names;
            for (const BookRecord &b : found) {
                names.append(QStringLiteral("  [#%1] %2%3  %4")
                                 .arg(b.id)
                                 .arg(b.title,
                                      b.editionLine().isEmpty() ? QString()
                                                                : QStringLiteral(" (") + b.editionLine() + QLatin1Char(')'),
                                      b.path));
            }
            return fail(QStringLiteral("“%1” matches %2 books; pass a book id or file path:\n")
                            .arg(wanted)
                            .arg(found.size()) +
                        names.join(QLatin1Char('\n')));
        }
        bookId = found.first().id;
        bookTitle = found.first().title;
    }

    const QString filterQuery = parser.value(QStringLiteral("query"));
    QString err;
    if (parser.isSet(QStringLiteral("json"))) {
        const QJsonObject payload = service.listMarks(bookId, filterQuery, limit, &err);
        if (!err.isEmpty())
            return fail(err);
        printJson(payload);
        return 0;
    }

    int count = 0;
    const QString md = service.formatMarksMarkdown(bookId, filterQuery, limit, &err, &count);
    if (!err.isEmpty())
        return fail(err);
    if (count == 0) {
        if (bookId > 0)
            printLine(stderr, QStringLiteral("Nothing is marked in “%1” yet.").arg(bookTitle));
        else
            printLine(stderr, QStringLiteral("No highlights, notes, or bookmarks found."));
        return 0;
    }
    const QByteArray utf8 = md.toUtf8();
    std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    McpServer::installStderrMessageHandler();

    QCoreApplication app(argc, argv);
    // Keep the exact application name "clothbound" so QStandardPaths resolves
    // the user's Clothbound library.sqlite, index.sqlite, and settings.ini.
    app.setApplicationName(QStringLiteral("clothbound"));
    app.setOrganizationDomain(QStringLiteral("clothbound.app"));
    app.setApplicationVersion(QStringLiteral(CLOTHBOUND_VERSION));

    const QStringList args = app.arguments();
    if (args.size() == 1)
        return McpServer::runStdio(args);

    const QString &cmd = args.at(1);
    if (cmd == QLatin1String("--version") || cmd == QLatin1String("-v")) {
        std::printf("clothbound-mcp %s\n", CLOTHBOUND_VERSION);
        std::fflush(stdout);
        return 0;
    }
    if (cmd == QLatin1String("--help") || cmd == QLatin1String("-h")) {
        printUsage();
        return 0;
    }
    if (cmd == QLatin1String("help")) {
        const QString topic = args.value(2);
        if (topic == QLatin1String("list"))
            return runList({args.at(0), QStringLiteral("list"), QStringLiteral("--help")});
        if (topic == QLatin1String("search"))
            return runSearch({args.at(0), QStringLiteral("search"), QStringLiteral("--help")});
        if (topic == QLatin1String("notes"))
            return runNotes({args.at(0), QStringLiteral("notes"), QStringLiteral("--help")});
        printUsage();
        return 0;
    }
    if (cmd == QLatin1String("mcp")) {
        if (args.size() == 2)
            return McpServer::runStdio(args);
        if (args.size() == 3 && (args.at(2) == QLatin1String("--help") || args.at(2) == QLatin1String("-h"))) {
            printUsage();
            return 0;
        }
        return fail(QStringLiteral("clothbound-mcp: unexpected argument '%1'. Use --help for usage.").arg(args.at(2)));
    }
    if (cmd == QLatin1String("list"))
        return runList(args);
    if (cmd == QLatin1String("search"))
        return runSearch(args);
    if (cmd == QLatin1String("notes"))
        return runNotes(args);

    return fail(QStringLiteral("clothbound-mcp: unexpected argument '%1'. Use --help for usage.").arg(cmd));
}
