// The command pattern follows Hype's (MIT, David Heinemeier Hansson).
#include "cli.h"

#include "bookinfo.h"
#include "epubbook.h"
#include "library.h"
#include "pdfbook.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

namespace {
void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(text)); }
void print(const QJsonValue &value) {
    const QJsonDocument document = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    fprintf(stdout, "%s", document.toJson().constData());
}
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description, const QString &positional = {}) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
        if (!positional.isEmpty())
            parser.addPositionalArgument(positional.mid(1, positional.size() - 2), positional, positional);
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    bool asJson() const { return parser.isSet("json"); }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
};

QJsonObject identityJson(const BookInfo &info) {
    QJsonObject object{{"path", info.path},
                       {"format", info.format},
                       {"title", info.identity.title},
                       {"author", info.identity.author},
                       {"work", info.identity.workKey}};
    auto add = [&object](const char *key, const QString &value) {
        if (!value.isEmpty())
            object.insert(key, value);
    };
    add("editor", info.identity.editor);
    add("publisher", info.identity.publisher);
    if (info.identity.year > 0)
        object.insert("year", info.identity.year);
    add("language", info.language);
    add("isbn", info.isbn);
    add("series", info.series);
    if (!info.subjects.isEmpty())
        object.insert("subjects", QJsonArray::fromStringList(info.subjects));
    add("description", info.description);
    object.insert("chapters", info.chapters);
    if (info.pages > 0)
        object.insert("pages", info.pages);
    object.insert("size", double(info.size));
    add("problem", info.problem);
    return object;
}

int info(const QStringList &arguments) {
    Command command("info", "Show what Dogear knows about a book: its metadata and table of contents.", "<book>");
    command.json();
    command.parser.process(arguments);
    const QString path = command.argument(1);
    if (path.isEmpty())
        return fail("Name an EPUB or PDF book.");
    if (!QFileInfo::exists(path))
        return fail(path + ": no such file");
    if (!BookFiles::isBook(path))
        return fail(path + ": Dogear reads EPUB and PDF books.");

    const BookInfo book = BookFiles::read(path);
    QJsonObject object = identityJson(book);
    QJsonArray contents;
    if (book.format == "epub") {
        EpubBook epub(path);
        if (epub.open())
            for (const auto &entry : epub.toc())
                contents.append(QJsonObject{{"title", entry.title}, {"depth", entry.depth}, {"chapter", entry.spine}});
    } else if (book.format == "pdf") {
        PdfBook pdf(path);
        if (pdf.open())
            for (const auto &entry : pdf.outline())
                contents.append(QJsonObject{{"title", entry.title}, {"depth", entry.depth}, {"page", entry.page + 1}});
    }
    object.insert("contents", contents);

    if (command.asJson()) {
        print(object);
        return book.ok() ? 0 : 1;
    }
    print(stdout, book.identity.title);
    if (!book.identity.author.isEmpty())
        print(stdout, "by " + book.identity.author);
    QStringList edition;
    if (!book.identity.editor.isEmpty())
        edition << "ed. " + book.identity.editor;
    if (!book.identity.publisher.isEmpty())
        edition << book.identity.publisher;
    if (book.identity.year > 0)
        edition << QString::number(book.identity.year);
    if (!edition.isEmpty())
        print(stdout, edition.join(" · "));
    print(stdout, QString("%1, %2 chapters%3").arg(book.format.toUpper()).arg(book.chapters)
                      .arg(book.pages > 0 ? QString(", %1 pages").arg(book.pages) : QString()));
    if (!book.problem.isEmpty())
        print(stderr, book.problem);
    if (!contents.isEmpty()) {
        print(stdout, "");
        for (const auto &value : std::as_const(contents)) {
            const QJsonObject entry = value.toObject();
            print(stdout, QString(entry.value("depth").toInt() * 2, ' ') + entry.value("title").toString());
        }
    }
    return book.ok() ? 0 : 1;
}

bool openLibrary(Library &library) {
    if (library.open())
        return true;
    print(stderr, "The library can't be opened: " + library.errorString());
    return false;
}

QJsonObject bookJson(const Library &library, const BookRecord &book) {
    QJsonObject object{{"id", book.id},         {"title", book.title},   {"author", book.author},
                       {"format", book.format}, {"path", book.path},     {"progress", book.progress},
                       {"finished", book.finishedAt > 0},                {"work", book.workKey}};
    if (!book.editionLine().isEmpty())
        object.insert("edition", book.editionLine());
    if (!book.problem.isEmpty())
        object.insert("problem", book.problem);
    if (const int minutes = library.minutesLeft(book.id); minutes > 0)
        object.insert("minutesLeft", minutes);
    return object;
}

int add(const QStringList &arguments) {
    Command command("add", "Add folders or book files to the library and read them in. Files are never changed.",
                    "<folder-or-book>");
    command.json();
    command.parser.process(arguments);
    const QStringList paths = command.parser.positionalArguments().mid(1);
    if (paths.isEmpty())
        return fail("Name a folder of books, or a book.");
    Library library;
    if (!openLibrary(library))
        return 1;

    QEventLoop loop;
    QObject::connect(&library, &Library::scanFinished, &loop, &QEventLoop::quit);
    QObject::connect(&library, &Library::problem, [](const QString &message) { print(stderr, message); });
    if (!command.asJson())
        QObject::connect(&library, &Library::scanProgressChanged, [&library] {
            fprintf(stderr, "\rReading %d of %d books…", library.scanDone(), library.scanTotal());
        });
    bool scanning = false, ok = true;
    QList<int> files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            library.addFolder(info.absoluteFilePath());
            scanning = true;
        } else if (const int id = library.addBookFile(path)) {
            files.append(id);
        } else {
            print(stderr, path + ": " + library.errorString());
            ok = false;
        }
    }
    int added = 0, updated = 0;
    if (scanning) {
        QObject::connect(&library, &Library::scanFinished, [&](int a, int u) {
            added = a;
            updated = u;
        });
        loop.exec();
        if (!command.asJson())
            fprintf(stderr, "\n");
    }
    added += int(files.size());
    if (command.asJson()) {
        print(QJsonObject{{"added", added}, {"updated", updated}, {"books", library.bookCount()},
                          {"folders", QJsonArray::fromStringList(library.folders())}});
    } else {
        print(stdout, QString("Added %1, updated %2; %3 books in the library.")
                          .arg(added).arg(updated).arg(library.bookCount()));
    }
    return ok ? 0 : 1;
}

int list(const QStringList &arguments) {
    Command command("list", "List the books in the library, by author and title.");
    command.json();
    command.parser.process(arguments);
    Library library;
    if (!openLibrary(library))
        return 1;
    QList<BookRecord> books;
    for (const BookRecord &book : library.records())
        if (book.visible())
            books.append(book);
    std::sort(books.begin(), books.end(), [](const BookRecord &a, const BookRecord &b) {
        const int c = a.sortAuthor.localeAwareCompare(b.sortAuthor);
        return c != 0 ? c < 0 : a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
    });
    if (command.asJson()) {
        QJsonArray array;
        for (const BookRecord &book : std::as_const(books))
            array.append(bookJson(library, book));
        print(array);
        return 0;
    }
    for (const BookRecord &book : std::as_const(books)) {
        QString line = book.title;
        if (!book.author.isEmpty())
            line += " — " + book.author;
        if (!book.editionLine().isEmpty())
            line += " (" + book.editionLine() + ")";
        if (book.finishedAt > 0)
            line += "  ✓";
        else if (book.progress > 0)
            line += QString("  %1%").arg(qRound(book.progress * 100));
        print(stdout, line);
    }
    return 0;
}

// As Hype does: one shared copy, linked into Claude Code, found directly by Codex.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.");
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/dogear and link into ~/.claude/skills",
                                         "[install]");
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return 1;
    const QByteArray text = bundled.readAll();
    if (command.argument(1).isEmpty()) {
        fputs(text.constData(), stdout);
        return 0;
    }
    if (command.argument(1) != "install")
        return fail("Use dogear skill to print the skill, or dogear skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/dogear");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    print(stdout, "Installed " + file.fileName());
    if (!home.exists(".claude"))
        return 0;
    const QString link = home.filePath(".claude/skills/dogear");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/dogear", link))
        return fail("Could not link " + link);
    print(stdout, "Linked " + link);
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: dogear [open] [book]\n       dogear <command> [options]\n\n" + cliSummary());
    return 0;
}
}

QString cliSummary() {
    return "Reader:\n"
           "  open [book]                     Open the library, or a book where you left off\n\n"
           "Commands that need no display (dogear help <command> for options):\n"
           "  add <folder-or-book>...         Add books to the library; files are never changed\n"
           "  list                            The books in the library, with reading progress\n"
           "  info <book>                     Metadata and table of contents of an EPUB or PDF\n"
           "  skill [install]                 Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"add", "list", "info", "skill", "help"}.contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "add"     ? add(arguments)
           : command == "list"  ? list(arguments)
           : command == "info"  ? info(arguments)
           : command == "skill" ? skill(arguments)
                                : help(arguments);
}
