// The command pattern follows Hype's (MIT, David Heinemeier Hansson).
#include "app/cli.h"

#include "ai/aiservices.h"
#include "formats/bookinfo.h"
#include "formats/bookname.h"
#include "formats/epubbook.h"
#include "ai/gemini.h"
#include "ai/keystore.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "ai/localmodels.h"
#include "audio/localspeech.h"
#include "library/marks.h"
#include "formats/pdfbook.h"
#include "audio/speech.h"
#include "audio/voicepacks.h"
#include "online/webimporter.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QSaveFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <memory>

namespace {
McpRunner g_mcpRunner = nullptr;

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
        if (!positional.isEmpty()) {
            QString label = positional;
            if (label.startsWith('<'))
                label.remove(0, 1);
            if (label.endsWith("..."))
                label.chop(3);
            if (label.endsWith('>'))
                label.chop(1);
            parser.addPositionalArgument(label, positional, positional);
        }
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
    Command command("info", "Show what Clothbound knows about a book: its metadata and table of contents.", "<book>");
    command.json();
    command.parser.process(arguments);
    const QString path = command.argument(1);
    if (path.isEmpty())
        return fail("Name an EPUB or PDF book.");
    const QFileInfo file(path);
    if (!file.exists())
        return fail(path + ": no such file");
    if (!BookFiles::isBook(path))
        return fail(path + ": Clothbound reads EPUB and PDF books.");

    // Open and parse the book once for both metadata and table of contents.
    BookInfo book;
    book.path = file.absoluteFilePath();
    book.format = BookFiles::formatOf(path);
    book.size = file.size();
    book.modified = file.lastModified();
    QJsonArray contents;
    if (book.format == "epub") {
        EpubBook epub(path);
        if (!epub.open()) {
            book.problem = epub.errorString();
            book.identity = BookName::identify(path, {}, {}, {}, {}, {});
        } else {
            const EpubMetadata &meta = epub.metadata();
            book.identity = BookName::identify(path, meta.title, meta.authors, meta.editors, meta.publisher, meta.date);
            book.language = meta.language;
            book.description = meta.description;
            book.isbn = meta.isbn;
            book.series = meta.series;
            book.seriesIndex = meta.seriesIndex;
            book.subjects = meta.subjects;
            book.chapters = int(epub.spine().size());
            book.pages = int(epub.pageList().size());
            if (epub.isDrmProtected())
                book.problem = QStringLiteral("This book is protected by DRM.");
            for (const auto &entry : epub.toc())
                contents.append(QJsonObject{{"title", entry.title}, {"depth", entry.depth}, {"chapter", entry.spine}});
        }
    } else if (book.format == "pdf") {
        PdfBook pdf(path);
        if (!pdf.open()) {
            book.problem = pdf.errorString();
            book.identity = BookName::identify(path, {}, {}, {}, {}, {});
        } else {
            book.identity = BookName::identify(path, pdf.title(), pdf.authors(), pdf.editors(), {}, {});
            book.description = pdf.subject();
            book.pages = pdf.pageCount();
            const auto outline = pdf.outline();
            book.chapters = int(outline.size());
            for (const auto &entry : outline)
                contents.append(QJsonObject{{"title", entry.title}, {"depth", entry.depth}, {"page", entry.page + 1}});
        }
    }
    QJsonObject object = identityJson(book);
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

bool isRemoteUrlArg(const QString &arg) {
    const QString trimmed = arg.trimmed();
    return trimmed.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
           trimmed.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) ||
           trimmed.startsWith(QLatin1String("arxiv:"), Qt::CaseInsensitive);
}

int add(const QStringList &arguments) {
    Command command("add",
                    "Add folders, book files, or web/arXiv URLs to the library. Local files are never changed.",
                    "<folder|book|url>...");
    command.json();
    command.parser.process(arguments);
    const QStringList paths = command.parser.positionalArguments().mid(1);
    if (paths.isEmpty())
        return fail("Name a folder of books, a book file, or a URL.");
    Library library;
    if (!openLibrary(library))
        return 1;

    QEventLoop loop;
    bool ok = true;
    QObject::connect(&library, &Library::scanFinished, &loop, &QEventLoop::quit);
    QObject::connect(&library, &Library::problem, [&ok](const QString &message) {
        print(stderr, message);
        ok = false;
    });
    if (!command.asJson())
        QObject::connect(&library, &Library::scanProgressChanged, [&library] {
            fprintf(stderr, "\rReading %d of %d books…", library.scanDone(), library.scanTotal());
        });

    std::unique_ptr<WebImporter> importer;
    QJsonArray importedUrls;
    QList<QUrl> folderUrls;
    int fileAdded = 0, fileUpdated = 0;
    for (const QString &path : paths) {
        if (isRemoteUrlArg(path)) {
            if (!importer)
                importer = std::make_unique<WebImporter>(
                    &library, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
            if (!command.asJson())
                fprintf(stderr, "Importing %s…\n", qPrintable(path));
            const int beforeCount = library.bookCount();
            int bookId = 0;
            QString kind, err;
            if (importer->importUrlSync(path, &bookId, &kind, &err)) {
                if (library.bookCount() > beforeCount)
                    ++fileAdded;
                else
                    ++fileUpdated;
                const BookRecord rec = library.record(bookId);
                importedUrls.append(QJsonObject{{QStringLiteral("id"), bookId},
                                                {QStringLiteral("title"), rec.title},
                                                {QStringLiteral("kind"), kind},
                                                {QStringLiteral("url"), path}});
                if (!command.asJson())
                    print(stdout, QStringLiteral("Saved “%1” (%2)").arg(rec.title, kind));
            } else {
                print(stderr, path + QStringLiteral(": ") +
                                  (err.isEmpty() ? QStringLiteral("Could not import URL.") : err));
                ok = false;
            }
            continue;
        }
        const QFileInfo fi(path);
        if (fi.isDir()) {
            folderUrls.append(QUrl::fromLocalFile(fi.absoluteFilePath()));
            continue;
        }
        const QString canonical = fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
        const int existingId = library.idForPath(canonical);
        const BookRecord before = existingId ? library.record(existingId) : BookRecord();
        if (const int id = library.addBookFile(path)) {
            if (!existingId) {
                ++fileAdded;
            } else {
                const BookRecord after = library.record(id);
                if (before.missing || before.size != after.size || before.mtime != after.mtime)
                    ++fileUpdated;
            }
        } else {
            print(stderr, path + ": " + library.errorString());
            ok = false;
        }
    }

    int scanAdded = 0, scanUpdated = 0;
    if (!folderUrls.isEmpty()) {
        QObject::connect(&library, &Library::scanFinished, [&](int a, int u) {
            scanAdded = a;
            scanUpdated = u;
        });
        // Batch all folders into one addUrls() call so rescan() runs once.
        library.addUrls(folderUrls);
        if (library.scanning()) {
            loop.exec();
            if (!command.asJson())
                fprintf(stderr, "\n");
        }
    }

    const int added = scanAdded + fileAdded;
    const int updated = scanUpdated + fileUpdated;
    if (command.asJson()) {
        QJsonObject out{{"added", added}, {"updated", updated}, {"books", library.bookCount()},
                        {"folders", QJsonArray::fromStringList(library.folders())}};
        if (!importedUrls.isEmpty())
            out.insert(QStringLiteral("imported"), importedUrls);
        print(out);
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

// clothbound notes: what the reader marked in a book, as Markdown to keep
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
    const bool byFile = file.isFile() && BookFiles::isBook(wanted);
    const QString wantedCanonical = byFile ? file.canonicalFilePath() : QString();
    for (const BookRecord &book : library.records()) {
        if (byFile ? (!wantedCanonical.isEmpty() && QFileInfo(book.path).canonicalFilePath() == wantedCanonical)
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
    if (marks.count() == 0) {
        print(stderr, QStringLiteral("Nothing is marked in “%1” yet.").arg(book.title));
        return 0;
    }
    fprintf(stdout, "%s", qPrintable(marks.markdown(book.title, book.author)));
    return 0;
}

// clothbound voices: the built-in voices (made on this computer): which there
// are, installing one, and how fast one speaks here.
int voices(const QStringList &arguments) {
    Command command("voices", "Built-in voices, made on this computer: list, install, remove, bench.");
    command.parser.addPositionalArgument("action", "list (default), install <voice>, remove <voice-or-pack>, or bench <voice>",
                                         "[action]");
    command.parser.addPositionalArgument("voice", "A voice id (such as builtin:kokoro:af_heart) or pack id", "[voice]");
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
        const QVariantMap voice = packs.voice(target);
        if (voice.isEmpty())
            return fail("No built-in voice is called " + target + ". See: clothbound voices list");
        QEventLoop loop;
        int result = 0;
        QString errorText;
        QObject::connect(&packs, &VoicePacks::ready, &loop, [&loop] { loop.quit(); });
        QObject::connect(&packs, &VoicePacks::failed, &loop, [&loop, &result, &errorText](const QString &message) {
            errorText = message;
            result = 1;
            loop.quit();
        });
        int shown = -1;
        if (!command.asJson()) {
            QObject::connect(&packs, &VoicePacks::progressChanged, &loop, [&packs, &shown] {
                const int percent = int(packs.progress() * 100);
                if (!packs.installing().isEmpty() && percent != shown) {
                    shown = percent;
                    fprintf(stderr, "\r%s %3d%%", qPrintable(packs.installing()), percent);
                }
            });
        }
        packs.installVoice(target);
        if (packs.busy()) {
            loop.exec();
            if (!command.asJson())
                fprintf(stderr, "\n");
        } else if (!packs.model(target)) {
            errorText = packs.problem();
            result = 1;
        }
        if (command.asJson()) {
            QJsonObject out{{"voice", target}, {"pack", voice.value("pack").toString()}, {"installed", result == 0}};
            if (!errorText.isEmpty())
                out.insert("error", errorText);
            print(out);
            return result;
        }
        if (result != 0 && !errorText.isEmpty())
            print(stderr, errorText);
        return result;
    }
    if (action == "remove") {
        QString packId = target;
        if (!packs.isInstalled(packId)) {
            const QString mapped = packs.voice(target).value("pack").toString();
            if (!mapped.isEmpty())
                packId = mapped;
        }
        if (!packs.isInstalled(packId))
            return fail(target + " isn't installed.");
        packs.remove(packId);
        if (command.asJson())
            print(QJsonObject{{"target", target}, {"pack", packId}, {"removed", true}});
        return 0;
    }
    if (action == "bench") {
        const auto model = packs.model(target);
        if (!model)
            return fail(target + " isn't installed. Install it with: clothbound voices install " + target);
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
    return fail("Unknown action " + action + ". See: clothbound help voices");
}

// clothbound ai: the reader's Gemini key, costs, and local model servers. The key
// is read from standard input, so it never appears in the shell's history.
int ai(const QStringList &arguments) {
    Command command("ai", "Intelligence for Clothbound: status, key, forget, or local.");
    command.parser.addPositionalArgument("action", "status (default), key (read from standard input), forget, or local (find model servers on this computer)", "[action]");
    command.json();
    command.parser.process(arguments);
    const QString action = command.argument(1).isEmpty() ? QStringLiteral("status") : command.argument(1);

    // Probing local model servers needs neither the library database nor the
    // desktop keyring.
    if (action == "local") {
        const QSettings settings(
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
            QSettings::IniFormat);
        const QString configuredUrl = settings.value(QStringLiteral("local/url")).toString();
        ServerFinder finder;
        QEventLoop loop;
        QObject::connect(&finder, &ServerFinder::finished, &loop, &QEventLoop::quit);
        finder.find(configuredUrl.isEmpty() ? QList<QUrl>() : QList<QUrl>{QUrl(configuredUrl)});
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
            print(QJsonObject{{"servers", found}, {"using", configuredUrl}});
            return 0;
        }
        if (found.isEmpty())
            print(stdout, "No model servers found on this computer (Ollama, llama.cpp, LM Studio, MLX, vLLM).");
        for (const QJsonValue &value : found) {
            const QJsonObject server = value.toObject();
            print(stdout, server["name"].toString() + " at " + server["url"].toString() +
                              (server["url"].toString() == configuredUrl ? " (in use)" : ""));
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

    if (action != "status" && action != "key" && action != "forget")
        return fail("Use clothbound ai status, clothbound ai key, clothbound ai forget, or clothbound ai local.");

    Library library;
    if (!openLibrary(library))
        return 1;
    GeminiProvider gemini;
    SecretKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    services.setLocalModels(&local);

    if (action == "forget") {
        services.forgetKey();
        const bool fromEnv = !qgetenv("GEMINI_API_KEY").trimmed().isEmpty();
        if (command.asJson()) {
            print(QJsonObject{{"removed", true}, {"environmentKeySet", fromEnv}});
            return 0;
        }
        print(stdout, fromEnv ? QStringLiteral("The Gemini key is gone from your keyring (GEMINI_API_KEY is still set in your environment).")
                              : QStringLiteral("The Gemini key is gone from your keyring."));
        return 0;
    }
    if (action == "key") {
        QTextStream in(stdin);
        const QString key = in.readLine().trimmed();
        if (key.isEmpty())
            return fail("Pipe the key in: clothbound ai key < keyfile");
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
        if (command.asJson()) {
            print(QJsonObject{{"ok", ok}, {"message", message}});
            return ok ? 0 : 1;
        }
        print(ok ? stdout : stderr, message);
        return ok ? 0 : 1;
    }

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
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/clothbound and link into ~/.claude/skills",
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
        return fail("Use clothbound skill to print the skill, or clothbound skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/clothbound");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    print(stdout, "Installed " + file.fileName());

    // Clean up our own legacy pre-rebrand skill only if its frontmatter matches ours.
    const QString legacyDir = home.filePath(".agents/skills/dogear");
    QFile legacySkill(legacyDir + "/SKILL.md");
    if (legacySkill.open(QIODevice::ReadOnly)) {
        const QByteArray legacyText = legacySkill.readAll();
        legacySkill.close();
        if (legacyText.contains("name: dogear") && legacyText.contains("Omarchy ebook reader")) {
            QDir(legacyDir).removeRecursively();
            const QString legacyLink = home.filePath(".claude/skills/dogear");
            if (QFileInfo(legacyLink).isSymLink())
                QFile::remove(legacyLink);
        }
    }

    if (!home.exists(".claude"))
        return 0;
    const QString link = home.filePath(".claude/skills/clothbound");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/clothbound", link))
        return fail("Could not link " + link);
    print(stdout, "Linked " + link);
    return 0;
}

void ensureIndexedForCliSearch(const Library &library, const QString &indexPath, int bookId) {
    QHash<int, QPair<qint64, QString>> indexed;
    const QString connName = QStringLiteral("cli-idx-check-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(indexPath);
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

    auto needsIndex = [&indexed](const BookRecord &r) {
        if (!r.visible() || r.path.isEmpty() || !QFileInfo::exists(r.path))
            return false;
        const auto it = indexed.constFind(r.id);
        return it == indexed.cend() || it->first != r.mtime || it->second != r.path;
    };

    if (bookId > 0) {
        const BookRecord rec = library.record(bookId);
        if (needsIndex(rec))
            LibraryIndex::indexBook(indexPath, {rec.id, rec.path, rec.format, rec.mtime});
        return;
    }

    QElapsedTimer budget;
    budget.start();
    int done = 0;
    for (const BookRecord &rec : library.visibleRecords()) {
        if (!needsIndex(rec))
            continue;
        LibraryIndex::indexBook(indexPath, {rec.id, rec.path, rec.format, rec.mtime});
        ++done;
        if (done >= 25 || budget.elapsed() >= 3000)
            break;
    }
}

int search(const QStringList &arguments) {
    Command command("search", "Search across all books (or one book) using the local full-text index.", "<query>");
    command.parser.addOption({"book", "Scope search to a single book id", "id"});
    command.parser.addOption({"limit", "Maximum number of matches (default 15, max 100)", "n", "15"});
    command.json();
    command.parser.process(arguments);

    const QString query = command.parser.positionalArguments().mid(1).join(QLatin1Char(' ')).trimmed();
    if (query.isEmpty())
        return fail("Give a search query: clothbound search <query> [--book <id>] [--limit <n>] [--json]");

    int bookId = 0;
    if (command.parser.isSet("book")) {
        bool ok = false;
        bookId = command.parser.value("book").toInt(&ok);
        if (!ok || bookId <= 0)
            return fail("Invalid --book id: expected a positive integer.");
    }

    bool okLimit = false;
    int limit = command.parser.value("limit").toInt(&okLimit);
    if (!okLimit || limit <= 0)
        return fail("Invalid --limit value: expected a positive integer.");
    limit = qBound(1, limit, 100);

    Library library;
    if (!openLibrary(library))
        return 1;
    if (bookId > 0 && !library.record(bookId).visible())
        return fail(QStringLiteral("Book %1 not found in the library.").arg(bookId));

    const QString indexPath =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/index.sqlite");
    LibraryIndex index(indexPath);
    if (!index.available())
        return fail("Full-text search index (SQLite FTS5) is not available.");
    ensureIndexedForCliSearch(library, indexPath, bookId);

    const QVariantList hits = index.search(query, limit, bookId);
    QJsonArray results;
    for (const QVariant &v : hits) {
        const QVariantMap hit = v.toMap();
        const int hitBookId = hit.value(QStringLiteral("bookId")).toInt();
        const BookRecord rec = library.record(hitBookId);
        if (!rec.visible())
            continue;
        const int chapter = hit.value(QStringLiteral("chapter")).toInt();
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        QString snippet = hit.value(QStringLiteral("snippet")).toString();
        snippet.remove(QChar(0x2039));
        snippet.remove(QChar(0x203A));
        snippet = snippet.simplified();

        QJsonObject item{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("format"), rec.format},
            {QStringLiteral("chapter"), chapter},
            {QStringLiteral("offset"), hit.value(QStringLiteral("offset")).toDouble()},
            {QStringLiteral("snippet"), snippet},
        };
        if (isPdf)
            item.insert(QStringLiteral("page"), chapter + 1);
        results.append(item);
    }

    if (command.asJson()) {
        print(QJsonObject{{QStringLiteral("query"), query},
                          {QStringLiteral("count"), int(results.size())},
                          {QStringLiteral("results"), results}});
        return 0;
    }

    if (results.isEmpty()) {
        print(stderr, QStringLiteral("No matches for “%1”.").arg(query));
        return 0;
    }
    for (const QJsonValue &val : std::as_const(results)) {
        const QJsonObject item = val.toObject();
        const QString where = item.contains(QStringLiteral("page"))
                                  ? QStringLiteral("p. %1").arg(item.value(QStringLiteral("page")).toInt())
                                  : QStringLiteral("chapter %1").arg(item.value(QStringLiteral("chapter")).toInt());
        const QString author = item.value(QStringLiteral("author")).toString();
        print(stdout, QStringLiteral("[#%1] %2%3 (%4)")
                          .arg(item.value(QStringLiteral("book_id")).toInt())
                          .arg(item.value(QStringLiteral("title")).toString(),
                               author.isEmpty() ? QString() : QStringLiteral(" — ") + author, where));
        print(stdout, QStringLiteral("  ") + item.value(QStringLiteral("snippet")).toString());
    }
    return 0;
}

int passage(const QStringList &arguments) {
    Command command("passage", "Read verbatim text from a book chapter or page with a citation header.", "<book_id>");
    command.parser.addOption({"chapter", "0-based chapter index (EPUB spine index or 0-based PDF page)", "n"});
    command.parser.addOption({"page", "1-based page number (PDF page, or EPUB print page / 1-based chapter)", "n"});
    command.parser.addOption({"page-end", "Optional inclusive 1-based end page (PDFs)", "n"});
    command.parser.addOption({"max-chars", "Maximum characters to return (default 8000, max 24000)", "n", "8000"});
    command.json();
    command.parser.process(arguments);

    const QString idArg = command.argument(1);
    bool okId = false;
    const int bookId = idArg.toInt(&okId);
    if (idArg.isEmpty() || !okId || bookId <= 0)
        return fail("Give a numeric book id (see: clothbound list --json).");

    const bool hasChapter = command.parser.isSet("chapter");
    const bool hasPage = command.parser.isSet("page");
    const bool hasPageEnd = command.parser.isSet("page-end");
    if (hasChapter && hasPage)
        return fail("Pass either --chapter or --page, not both.");

    int chapterArg = 0;
    if (hasChapter) {
        bool ok = false;
        chapterArg = command.parser.value("chapter").toInt(&ok);
        if (!ok || chapterArg < 0)
            return fail("Invalid --chapter: expected a non-negative integer.");
    }
    int pageArg = 0;
    if (hasPage) {
        bool ok = false;
        pageArg = command.parser.value("page").toInt(&ok);
        if (!ok || pageArg < 1)
            return fail("Invalid --page: expected a positive 1-based page number.");
    }
    int pageEndArg = 0;
    if (hasPageEnd) {
        bool ok = false;
        pageEndArg = command.parser.value("page-end").toInt(&ok);
        if (!ok || pageEndArg < 1)
            return fail("Invalid --page-end: expected a positive 1-based page number.");
    }

    bool okMax = false;
    int maxChars = command.parser.value("max-chars").toInt(&okMax);
    if (!okMax || maxChars <= 0)
        return fail("Invalid --max-chars: expected a positive integer.");
    maxChars = qBound(100, maxChars, 24000);

    Library library;
    if (!openLibrary(library))
        return 1;
    const BookRecord rec = library.record(bookId);
    if (!rec.visible())
        return fail(QStringLiteral("Book %1 not found in the library.").arg(bookId));
    if (!QFileInfo::exists(rec.path))
        return fail(QStringLiteral("Book file is missing on disk: %1").arg(rec.path));

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
            return fail(pdf.errorString());
        const int totalPages = pdf.pageCount();
        if (totalPages <= 0)
            return fail("This PDF has no pages.");
        const int startPage = hasPage ? pageArg : (hasChapter ? chapterArg + 1 : 1);
        if (startPage < 1 || startPage > totalPages)
            return fail(QStringLiteral("Page %1 is out of range (valid pages: 1–%2).").arg(startPage).arg(totalPages));
        int endPage = hasPageEnd ? pageEndArg : startPage;
        if (endPage < startPage)
            return fail(QStringLiteral("--page-end (%1) must be >= starting page (%2).").arg(endPage).arg(startPage));
        endPage = qMin(endPage, qMin(totalPages, startPage + 49));

        resolvedChapter = startPage - 1;
        resolvedPage = startPage;
        resolvedPageEnd = endPage;
        for (const PdfOutlineEntry &entry : pdf.outline()) {
            if (entry.page <= startPage - 1 && !entry.title.isEmpty())
                sectionTitle = entry.title;
            else if (entry.page > startPage - 1)
                break;
        }
        QStringList pieces;
        qsizetype accumulated = 0;
        for (int p = startPage; p <= endPage; ++p) {
            const QString pageText = pdf.pageText(p - 1);
            if (endPage > startPage) {
                pieces.append(QStringLiteral("--- Page %1 ---\n").arg(p) + pageText);
                accumulated += pageText.size() + 20;
            } else {
                pieces.append(pageText);
                accumulated += pageText.size();
            }
            if (accumulated >= maxChars + 256) {
                resolvedPageEnd = p;
                break;
            }
        }
        rawText = pieces.join(QStringLiteral("\n\n"));
        citationLocation = resolvedPageEnd > startPage
                               ? QStringLiteral("pages %1–%2 of %3").arg(startPage).arg(resolvedPageEnd).arg(totalPages)
                               : QStringLiteral("page %1 of %2").arg(startPage).arg(totalPages);
        if (!sectionTitle.isEmpty())
            citationLocation += QStringLiteral(" · ") + sectionTitle;
    } else {
        EpubBook epub(rec.path);
        if (!epub.open())
            return fail(epub.errorString());
        if (epub.isDrmProtected())
            return fail("This book is protected by DRM.");
        const int spineCount = int(epub.spine().size());
        if (spineCount <= 0)
            return fail("This EPUB has no readable chapters.");

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
                return fail(QStringLiteral("Page %1 could not be mapped in this EPUB (chapters: 0–%2).")
                                .arg(pageArg)
                                .arg(spineCount - 1));
            }
        }
        if (spineIdx < 0 || spineIdx >= spineCount)
            return fail(QStringLiteral("Chapter %1 is out of range (valid chapters: 0–%2).")
                            .arg(spineIdx)
                            .arg(spineCount - 1));
        resolvedChapter = spineIdx;
        for (const EpubTocEntry &entry : epub.toc()) {
            if (entry.spine == spineIdx && !entry.title.isEmpty()) {
                sectionTitle = entry.title;
                break;
            }
            if (entry.spine >= 0 && entry.spine < spineIdx && !entry.title.isEmpty())
                sectionTitle = entry.title;
        }
        rawText = epub.chapterText(spineIdx);
        citationLocation = QStringLiteral("chapter %1/%2").arg(spineIdx).arg(spineCount - 1);
        if (resolvedPage > 0)
            citationLocation += QStringLiteral(" (print p. %1)").arg(resolvedPage);
        if (!sectionTitle.isEmpty())
            citationLocation += QStringLiteral(" · ") + sectionTitle;
    }

    const qsizetype totalChars = rawText.size();
    const QString slice = rawText.left(maxChars);
    const bool truncated = slice.size() < totalChars;
    const QString authorPart = rec.author.isEmpty() ? QString() : QStringLiteral(" — ") + rec.author;
    const QString citation = QStringLiteral("[Citation: %1%2 | book_id=%3 | %4]")
                                 .arg(rec.title, authorPart)
                                 .arg(rec.id)
                                 .arg(citationLocation);

    if (command.asJson()) {
        print(QJsonObject{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("format"), rec.format},
            {QStringLiteral("chapter"), resolvedChapter},
            {QStringLiteral("chapter_title"), sectionTitle},
            {QStringLiteral("page"), resolvedPage > 0 ? QJsonValue(resolvedPage) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("page_end"), resolvedPageEnd > 0 ? QJsonValue(resolvedPageEnd) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("total_chars"), double(totalChars)},
            {QStringLiteral("truncated"), truncated},
            {QStringLiteral("citation"), citation},
            {QStringLiteral("text"), slice},
        });
        return 0;
    }

    print(stdout, citation);
    print(stdout, QString());
    print(stdout, slice);
    if (truncated)
        print(stderr, QStringLiteral("[Truncated at %1 of %2 characters.]").arg(slice.size()).arg(totalChars));
    return 0;
}

int mcp(const QStringList &arguments) {
    Command command("mcp", "Run the local stdio Model Context Protocol (MCP) server for coding and research agents.");
    command.parser.process(arguments);
    if (!g_mcpRunner)
        return fail("MCP server is not available in this build.");
    return g_mcpRunner(arguments);
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (topic == "open") {
        print(stdout, "Usage: clothbound [open] [book...]\n\nOpen the library, or add and open one or more EPUB or PDF books.");
        return 0;
    }
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: clothbound [open] [book...]\n       clothbound <command> [options]\n\n" + cliSummary());
    return 0;
}
}  // namespace

void registerMcpRunner(McpRunner runner) { g_mcpRunner = runner; }

QString cliSummary() {
    return "Reader:\n"
           "  open [book...]                         Open the library, or books where you left off\n\n"
           "Commands that need no display (clothbound help <command> for options):\n"
           "  add <folder|book|url>...               Add folders, books, or web/arXiv URLs to the library\n"
           "  list                                   The books in the library, with reading progress\n"
           "  info <book>                            Metadata and table of contents of an EPUB or PDF\n"
           "  search <query>                         Full-text search across the library (or one book)\n"
           "  passage <book_id>                      Read verbatim text from a chapter or page with citation\n"
           "  notes <book>                           A book's highlights, notes and bookmarks, as Markdown\n"
           "  mcp                                    Local stdio MCP server for coding & research agents\n"
           "  voices [list|install|remove|bench]     Built-in voices, made on this computer, and their speed\n"
           "  ai [status|key|forget|local]           Intelligence: the Gemini key and costs, and models on this computer\n"
           "  skill [install]                        Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"add", "list", "info", "search", "passage", "notes", "mcp", "voices", "ai", "skill", "help"}
        .contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "add"       ? add(arguments)
           : command == "list"    ? list(arguments)
           : command == "info"    ? info(arguments)
           : command == "search"  ? search(arguments)
           : command == "passage" ? passage(arguments)
           : command == "notes"   ? notes(arguments)
           : command == "mcp"     ? mcp(arguments)
           : command == "voices"  ? voices(arguments)
           : command == "ai"      ? ai(arguments)
           : command == "skill"   ? skill(arguments)
                                  : help(arguments);
}
