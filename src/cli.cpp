// The command pattern follows Hype's (MIT, David Heinemeier Hansson).
#include "cli.h"

#include "bookinfo.h"
#include "epubbook.h"
#include "pdfbook.h"

#include <QCommandLineParser>
#include <QDir>
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
           "  info <book>                     Metadata and table of contents of an EPUB or PDF\n"
           "  skill [install]                 Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) { return QStringList{"info", "skill", "help"}.contains(word); }

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "info"    ? info(arguments)
           : command == "skill" ? skill(arguments)
                                : help(arguments);
}
