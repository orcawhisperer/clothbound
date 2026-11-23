// The command pattern follows Hype's (MIT, David Heinemeier Hansson).
#include "cli.h"

#include "aiservices.h"
#include "bookinfo.h"
#include "gemini.h"
#include "keystore.h"
#include "localmodels.h"
#include "epubbook.h"
#include "library.h"
#include "marks.h"
#include "localspeech.h"
#include "speech.h"
#include "voicepacks.h"
#include "pdfbook.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>
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

// dogear notes: what the reader marked in a book, as Markdown to keep
// elsewhere, or as JSON. The book is a file, or a few words of its title.
int notes(const QStringList &arguments) {
    Command command("notes", "Print a book's highlights, notes, and bookmarks as Markdown.", "<book>");
    command.json();
    command.parser.process(arguments);
    const QString wanted = command.argument(1);
    if (wanted.isEmpty())
        return fail("Name a book: its file, or a few words of its title.");
    Library library;
    if (!openLibrary(library))
        return 1;
    QList<BookRecord> found;
    const QFileInfo file(wanted);
    for (const BookRecord &book : library.records()) {
        if (file.exists() ? QFileInfo(book.path).canonicalFilePath() == file.canonicalFilePath()
                          : (book.title + ' ' + book.author).contains(wanted, Qt::CaseInsensitive))
            found.append(book);
    }
    if (found.isEmpty())
        return fail(QStringLiteral("No book in the library matches “%1”.").arg(wanted));
    if (found.size() > 1) {
        QStringList names;
        for (const BookRecord &book : std::as_const(found))
            names.append("  " + book.title + (book.editionLine().isEmpty() ? QString() : " (" + book.editionLine() + ")")
                         + "  " + book.path);
        return fail(QStringLiteral("“%1” matches %2 books; name one by its file:\n").arg(wanted).arg(found.size())
                    + names.join('\n'));
    }
    const BookRecord &book = found.first();
    Marks marks(&library);
    marks.setBook(book.id);
    if (command.asJson()) {
        QJsonArray array;
        for (int row = 0; row < marks.count(); ++row) {
            const QVariantMap mark = marks.mark(marks.data(marks.index(row), Marks::IdRole).toInt());
            QJsonObject object = QJsonObject::fromVariantMap(mark);
            object.insert("created", mark.value("created").toDateTime().toString(Qt::ISODate));
            array.append(object);
        }
        print(QJsonObject{{"book", bookJson(library, book)}, {"marks", array}});
        return 0;
    }
    if (marks.count() == 0)
        return fail(QStringLiteral("Nothing is marked in “%1” yet.").arg(book.title));
    fprintf(stdout, "%s", qPrintable(marks.markdown(book.title, book.author)));
    return 0;
}

// dogear voices: the built-in voices (made on this computer): which there
// are, installing one, and how fast one speaks here.
int voices(const QStringList &arguments) {
    Command command("voices", "Built-in voices, made on this computer: list, install, remove, bench.");
    command.parser.addPositionalArgument("action", "list (default), install <voice>, remove <pack>, or bench <voice>",
                                         "[action]");
    command.parser.addPositionalArgument("voice", "A voice id, such as builtin:kokoro:af_heart", "[voice]");
    command.parser.addOption({"language", "Only voices that speak this language (en, en-GB, de…)", "language"});
    command.json();
    command.parser.process(arguments);
    const QString action = command.argument(1).isEmpty() ? QStringLiteral("list") : command.argument(1);
    const QString target = command.argument(2);
    VoicePacks packs(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/voices"));

    if (action == "list") {
        const QVariantList found = packs.voices(command.parser.value("language"));
        if (command.asJson()) {
            QJsonArray array;
            for (const QVariant &voice : found)
                array.append(QJsonObject::fromVariantMap(voice.toMap()));
            print(array);
            return 0;
        }
        for (const QVariant &value : found) {
            const QVariantMap voice = value.toMap();
            print(stdout, QStringLiteral("%1  %2 (%3, %4%5)%6")
                              .arg(voice.value("id").toString(), voice.value("name").toString(),
                                   voice.value("language").toString(), voice.value("engine").toString(),
                                   voice.value("grade").toString().isEmpty() ? QString() : ", grade " + voice.value("grade").toString(),
                                   voice.value("installed").toBool() ? QStringLiteral("  installed")
                                       : QStringLiteral("  %1 MB to download").arg(voice.value("bytes").toLongLong() / 1000000)));
        }
        return 0;
    }
    if (action == "install") {
        if (packs.voice(target).isEmpty())
            return fail("No built-in voice is called " + target + ". See: dogear voices list");
        QEventLoop loop;
        int result = 0;
        QObject::connect(&packs, &VoicePacks::ready, &loop, [&loop] { loop.quit(); });
        QObject::connect(&packs, &VoicePacks::failed, &loop, [&loop, &result](const QString &message) {
            print(stderr, message);
            result = 1;
            loop.quit();
        });
        int shown = -1;
        QObject::connect(&packs, &VoicePacks::progressChanged, &loop, [&packs, &shown] {
            const int percent = int(packs.progress() * 100);
            if (!packs.installing().isEmpty() && percent != shown) {
                shown = percent;
                fprintf(stderr, "\r%s %3d%%", qPrintable(packs.installing()), percent);
            }
        });
        packs.installVoice(target);
        if (!packs.busy())
            return packs.model(target) ? 0 : fail(packs.problem());
        loop.exec();
        fprintf(stderr, "\n");
        return result;
    }
    if (action == "remove") {
        if (!packs.isInstalled(target))
            return fail(target + " isn't installed.");
        packs.remove(target);
        return 0;
    }
    if (action == "bench") {
        const auto model = packs.model(target);
        if (!model)
            return fail(target + " isn't installed. Install it with: dogear voices install " + target);
        SherpaEngine engine(packs.engineLibrary());
        QElapsedTimer clock;
        clock.start();
        const QString problem = engine.prepare(*model);
        if (!problem.isEmpty())
            return fail(problem);
        const qint64 loading = clock.elapsed();
        const QStringList sentences{
            QStringLiteral("The Time Traveller, for so it will be convenient to speak of him, was expounding a recondite matter to us."),
            QStringLiteral("His grey eyes shone and twinkled, and his usually pale face was flushed and animated."),
            QStringLiteral("The fire burned brightly, and the soft radiance of the incandescent lights caught the bubbles that flashed and passed in our glasses.")};
        std::atomic_bool stop{false};
        engine.speak(sentences.first(), model->speaker, stop);  // warm up
        double seconds = 0;
        clock.restart();
        for (const QString &sentence : sentences)
            seconds += engine.speak(sentence, model->speaker, stop).size() / double(qMax(1, engine.sampleRate()));
        const double took = clock.elapsed() / 1000.0;
        const double speed = took > 0 ? seconds / took : 0;
        if (command.asJson()) {
            print(QJsonObject{{"voice", target}, {"threads", SherpaEngine::threads()}, {"loadSeconds", loading / 1000.0},
                              {"audioSeconds", seconds}, {"seconds", took}, {"speed", speed}});
            return 0;
        }
        print(stdout, QStringLiteral("%1: %2× real time (%3 s of speech in %4 s, %5 threads; loaded in %6 s)")
                          .arg(target).arg(speed, 0, 'f', 2).arg(seconds, 0, 'f', 1).arg(took, 0, 'f', 1)
                          .arg(SherpaEngine::threads()).arg(loading / 1000.0, 0, 'f', 1));
        return 0;
    }
    return fail("Unknown action " + action + ". See: dogear help voices");
}

// dogear ai: the reader's Gemini key and what it has cost. The key is read
// from standard input, so it never appears in the shell's history.
int ai(const QStringList &arguments) {
    Command command("ai", "Gemini for Dogear's listening and companion: status, key, forget.");
    command.parser.addPositionalArgument("action", "status (default), key (read from standard input), forget, or local (find model servers on this computer)", "[action]");
    command.json();
    command.parser.process(arguments);
    const QString action = command.argument(1).isEmpty() ? QStringLiteral("status") : command.argument(1);

    Library library;
    if (!openLibrary(library))
        return 1;
    GeminiProvider gemini;
    SecretKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    services.setLocalModels(&local);

    if (action == "local") {
        ServerFinder finder;
        QEventLoop loop;
        QObject::connect(&finder, &ServerFinder::finished, &loop, &QEventLoop::quit);
        finder.find(services.localUrl().isEmpty() ? QList<QUrl>() : QList<QUrl>{QUrl(services.localUrl())});
        loop.exec();
        QJsonArray found;
        for (const LocalModels::Server &server : finder.servers())
            found.append(QJsonObject{{"kind", LocalModels::kindId(server.kind)},
                                     {"name", LocalModels::kindName(server.kind)},
                                     {"url", server.url.toString()},
                                     {"textModels", QJsonArray::fromStringList(server.textModels)},
                                     {"embeddingModels", QJsonArray::fromStringList(server.embeddingModels)},
                                     {"speechModels", QJsonArray::fromStringList(server.speechModels)}});
        if (command.asJson()) {
            print(QJsonObject{{"servers", found}, {"using", services.localUrl()}});
            return 0;
        }
        if (found.isEmpty())
            print(stdout, "No model servers found on this computer (Ollama, llama.cpp, LM Studio, MLX, vLLM).");
        for (const QJsonValue &value : found) {
            const QJsonObject server = value.toObject();
            print(stdout, server["name"].toString() + " at " + server["url"].toString() +
                              (server["url"].toString() == services.localUrl() ? " (in use)" : ""));
            for (const char *role : {"textModels", "embeddingModels", "speechModels"}) {
                QStringList models;
                for (const QJsonValue &model : server[role].toArray())
                    models.append(model.toString());
                if (!models.isEmpty())
                    print(stdout, QString("  %1: %2").arg(QString(role).remove("Models"), models.join(", ")));
            }
        }
        return 0;
    }

    if (action == "forget") {
        services.forgetKey();
        print(stdout, "The Gemini key is gone from your keyring.");
        return 0;
    }
    if (action == "key") {
        QTextStream in(stdin);
        const QString key = in.readLine().trimmed();
        if (key.isEmpty())
            return fail("Pipe the key in: dogear ai key < keyfile");
        QEventLoop loop;
        bool ok = false;
        QString message;
        QObject::connect(&services, &AiServices::keyChecked, [&](bool success, const QString &text) {
            ok = success;
            message = text;
            loop.quit();
        });
        services.setKey(key);
        loop.exec();
        print(ok ? stdout : stderr, message);
        return ok ? 0 : 1;
    }
    if (action != "status")
        return fail("Use dogear ai status, dogear ai key, or dogear ai forget.");

    if (services.hasKey()) {
        QEventLoop loop;
        QObject::connect(&services, &AiServices::changed, &loop, [&] {
            if (!services.checking())
                loop.quit();
        });
        services.check();
        if (services.checking())
            loop.exec();
    }
    QJsonArray usage;
    for (const QVariant &row : services.usageThisMonth())
        usage.append(QJsonObject::fromVariantMap(row.toMap()));
    const QJsonObject status{{"provider", services.providerName()},
                             {"key", services.hasKey()},
                             {"keyLocation", services.keyLocation()},
                             {"connected", services.connected()},
                             {"status", services.status()},
                             {"textModel", services.textModel()},
                             {"speechModel", services.speechModel()},
                             {"spentThisMonth", services.spentThisMonth()},
                             {"budget", services.budget()},
                             {"usage", usage},
                             {"preset", services.preset()},
                             {"local", services.localUrl()},
                             {"localTextModel", services.localTextModel()},
                             {"localEmbeddingModel", services.localEmbeddingModel()}};
    if (command.asJson()) {
        print(status);
    } else {
        print(stdout, services.providerName() + ": " + services.status());
        if (services.hasKey())
            print(stdout, "Key in " + services.keyLocation() + "; models " + services.textModel() + ", " + services.speechModel());
        print(stdout, QString("This month: about $%1 of a $%2 budget").arg(services.spentThisMonth(), 0, 'f', 2)
                          .arg(services.budget(), 0, 'f', 2));
        if (services.hasLocal())
            print(stdout, QString("On this computer: %1 at %2, with %3").arg(services.localName(), services.localUrl(),
                                                                          services.localTextModel()));
        print(stdout, "Where it runs: " + services.preset() + " (quick work: " + services.whereFor("director") +
                          "; careful work: " + services.whereFor("explain") + ")");
    }
    return services.hasKey() && !services.connected() ? 1 : 0;
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
           "  notes <book>                    A book's highlights, notes and bookmarks, as Markdown\n"
           "  voices [list|install|bench]     Built-in voices, made on this computer, and their speed\n"
           "  ai [status|key|forget|local]    Intelligence: the Gemini key and costs, and models on this computer\n"
           "  skill [install]                 Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"add", "list", "info", "notes", "voices", "ai", "skill", "help"}.contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "add"     ? add(arguments)
           : command == "list"  ? list(arguments)
           : command == "info"  ? info(arguments)
           : command == "notes" ? notes(arguments)
           : command == "voices" ? voices(arguments)
           : command == "ai"    ? ai(arguments)
           : command == "skill" ? skill(arguments)
                                : help(arguments);
}
