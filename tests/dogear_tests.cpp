#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cmath>
#include <QtTest>

#include "aiservices.h"
#include "audiocache.h"
#include "aiusage.h"
#include "bookinfo.h"
#include "fakeserver.h"
#include "gemini.h"
#include "keystore.h"
#include "sseparser.h"
#include "bookname.h"
#include "bookscheme.h"
#include "library.h"
#include "reader.h"
#include "epubbook.h"
#include "htmltokenizer.h"
#include "pdfbook.h"
#include "speech.h"
#include "speechplayer.h"
#include "testbooks.h"
#include "theme.h"
#include "translator.h"
#include "voicecatalog.h"
#include "ziparchive.h"

class DogearTests : public QObject {
    Q_OBJECT

private:
    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QByteArray png(int width, int height) {
        QImage image(width, height, QImage::Format_RGB32);
        image.fill(QColor("#336699"));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return bytes;
    }

    static QByteArray fixture(const QString &name) {
        QFile file(QFINDTESTDATA("fixtures/gemini/" + name));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("missing fixture %s", qPrintable(name));
        return file.readAll();
    }

    static QByteArray fontPayload() {
        QByteArray font(2000, Qt::Uninitialized);
        for (int i = 0; i < font.size(); ++i)
            font[i] = char(i * 7 % 251);
        return font;
    }

    static QByteArray adobeObfuscate(QByteArray data) {
        const QByteArray key = QByteArray::fromHex("123456789abcdef0123456789abcdef0");
        for (int i = 0; i < 1024; ++i)
            data[i] = char(data[i] ^ key[i % 16]);
        return data;
    }

    static Ai::Voice makeVoice(const QString &id, const QString &language, const QString &persona,
                               const QString &accent = {}, const QString &description = {}) {
        Ai::Voice voice;
        voice.id = id;
        voice.name = id.contains('-') ? QStringLiteral("Storyteller 1") : id;
        voice.language = language;
        voice.persona = persona;
        voice.accent = accent;
        voice.description = description;
        voice.gender = "male";
        voice.pitch = "low";
        return voice;
    }

    struct VoiceRig {
        QTemporaryDir dir;
        Library library{dir.filePath("data"), dir.filePath("cache")};
        BookScheme scheme;
        Reader reader{&library, &scheme};
        FakeServer server;
        GeminiProvider gemini{nullptr, server.endpoint()};
        MemoryKeyStore keys;
        std::unique_ptr<AiServices> services;
        std::unique_ptr<AudioCache> cache;
        std::unique_ptr<VoiceCatalog> catalog;
        VoiceRig() {
            qunsetenv("GEMINI_API_KEY");
            qputenv("DOGEAR_MUTE", "1");
            library.open();
            keys.save("gemini", "test-key");
            services = std::make_unique<AiServices>(&gemini, &keys, library.database().connectionName());
            cache = std::make_unique<AudioCache>(dir.filePath("audio"), library.database().connectionName());
            catalog = std::make_unique<VoiceCatalog>(services.get(), cache.get(), &reader,
                                                     library.database().connectionName(), dir.filePath("voices.json"));
        }
    };

private slots:

    // --- Listening -----------------------------------------------------

    static SpeechBlock block(qint64 start, const QString &text, const QString &kind = "text") {
        SpeechBlock b;
        b.start = start;
        b.end = start + text.size();
        b.text = text;
        b.kind = kind;
        return b;
    }

    void speechFindsSentences() {
        const SpeechBlock b = block(100, "Mr. Hyde met H. G. Wells at noon. “Who are you?” he asked. Nobody knew… "
                                         "and then e.g. tea was served.");
        const QList<SpeechSentence> said = Speech::sentences(b);
        QCOMPARE(said.size(), 3);
        QCOMPARE(said[0].text, QString("Mr. Hyde met H. G. Wells at noon."));
        QCOMPARE(said[1].text, QString("“Who are you?” he asked."));
        QCOMPARE(said[2].text, QString("Nobody knew… and then e.g. tea was served."));
        // Offsets are the reader's, so the highlight lands on the words.
        QCOMPARE(said[0].start, 100);
        QCOMPARE(b.text.mid(said[1].start - 100, said[1].end - said[1].start), said[1].text);
    }

    void speechLeavesOutNoteMarkers() {
        SpeechBlock b = block(0, "He arrived in Paris.12 It rained.");
        b.skips = {{20, 22}};
        const QList<SpeechSentence> said = Speech::sentences(b);
        QCOMPARE(said.size(), 2);
        QCOMPARE(said[0].text, QString("He arrived in Paris."));
        // The marker is inside the highlight, though it isn't said.
        QCOMPARE(said[0].end, 22);
        QCOMPARE(said[1].text, QString("It rained."));
    }

    void speechSaysHeadingsWell() {
        QCOMPARE(Speech::spokenHeading("V"), QString("5."));
        QCOMPARE(Speech::spokenHeading("CHAPTER XII"), QString("Chapter 12."));
        QCOMPARE(Speech::spokenHeading("Part iv: The Return"), QString("Part 4: The Return."));
        QCOMPARE(Speech::spokenHeading("IV. THE GOLDEN AGE"), QString("4. The Golden Age."));
        QCOMPARE(Speech::spokenHeading("XL — Twenty-one"), QString("40 — Twenty-one."));
        // Not numerals: a pronoun, a letter, a word.
        QCOMPARE(Speech::spokenHeading("I AM LEGEND"), QString("I Am Legend."));
        QCOMPARE(Speech::spokenHeading("Appendix C"), QString("Appendix C."));
        QCOMPARE(Speech::spokenHeading("Civil War"), QString("Civil War."));
        QCOMPARE(Speech::spokenHeading("“THE END”"), QString("“The End.”"));
        QCOMPARE(Speech::spokenHeading("Was it?"), QString("Was it?"));
    }

    void speechMakesClips() {
        const QString first = "The Time Traveller was expounding a recondite matter to us.";
        const QString rest = " His grey eyes shone and twinkled. The fire burned brightly. Our chairs embraced us.";
        QList<SpeechBlock> blocks{block(0, "V", "heading"), block(1, "THE GOLDEN AGE", "heading"),
                                  block(15, first + rest), block(200, "“Yes,” he said."),
                                  block(220, "“No.”"), block(230, "* * *", "break"),
                                  block(240, "Morning came.")};
        QList<SpeechClip> made = Speech::clips(3, blocks, 0, true);
        // The number and title together, then one sentence to start at once,
        // then the rest of the paragraph.
        QCOMPARE(made[0].text, QString("5. The Golden Age."));
        QCOMPARE(made[0].sentences.size(), 2);
        QCOMPARE(made[0].block, 0);
        QCOMPARE(made[1].text, first);
        QCOMPARE(made[1].block, 2);
        // Short lines of dialogue join the clip before them, so a page of
        // conversation isn't a request a line.
        QCOMPARE(made[2].text, rest.trimmed() + " “Yes,” he said. “No.”");
        QCOMPARE(made[2].block, 2);
        // A scene break is a pause.
        QCOMPARE(made[3].pauseMs, 1300);
        QCOMPARE(made[4].text, QString("Morning came."));
        QCOMPARE(made.size(), 5);
        for (const SpeechClip &clip : std::as_const(made))
            QCOMPARE(clip.chapter, 3);

        // From the middle of a paragraph: from the sentence there on, and
        // past the headings.
        made = Speech::clips(3, blocks, 15 + first.size() + 5, false);
        QVERIFY(made[0].text.startsWith(rest.trimmed()));
        QCOMPARE(made[0].block, 2);
    }

    void speechBeginsAClipWhereTheMoodChanges() {
        const QList<SpeechBlock> blocks{block(0, "It was quiet."), block(20, "“Hush,” she said."),
                                        block(40, "They waited."), block(60, "Then the door burst open.")};
        // Short paragraphs share a clip, unless the mood changes between them.
        QCOMPARE(Speech::clips(0, blocks, 0, false).size(), 1);
        const QList<SpeechClip> made = Speech::clips(0, blocks, 0, false, 1500, 600, {1, 3});
        QCOMPARE(made.size(), 3);
        QCOMPARE(made[0].block, 0);
        QCOMPARE(made[1].block, 1);
        QCOMPARE(made[1].text, QString("“Hush,” she said. They waited."));
        QCOMPARE(made[2].block, 3);

        const QMap<int, QString> changes{{1, "hushed"}, {3, "urgent"}, {5, ""}};
        QCOMPARE(Speech::moodAt(changes, 0), QString());
        QCOMPARE(Speech::moodAt(changes, 2), QString("hushed"));
        QCOMPARE(Speech::moodAt(changes, 3), QString("urgent"));
        QCOMPARE(Speech::moodAt(changes, 7), QString());
        QCOMPARE(Speech::styleFor("hushed", false), QString("Hushed and low."));
        QCOMPARE(Speech::styleFor("hushed", true), QString("Hushed, almost a whisper."));
        QCOMPARE(Speech::styleFor("", false), QString());
        QCOMPARE(Speech::styleFor("shouty", true), QString());
        for (const QString &mood : Speech::moods())
            QVERIFY(mood == "neutral" || !Speech::styleFor(mood, false).isEmpty());
    }

    void speechSplitsLongParagraphs() {
        QString text;
        for (int i = 0; i < 60; ++i)
            text += QStringLiteral("Sentence number %1 goes on for a little while longer. ").arg(i);
        const QList<SpeechClip> made = Speech::clips(0, {block(0, text.trimmed())}, 0, true, 1500, 600);
        QVERIFY(made.size() > 3);
        QCOMPARE(made[0].sentences.size(), 1);
        QVERIFY(made[1].text.size() <= 400);
        QVERIFY(made[2].text.size() <= 900);
        qsizetype total = 0;
        for (const SpeechClip &clip : made) {
            QVERIFY(clip.text.size() <= 1500);
            total += clip.sentences.size();
        }
        QCOMPARE(total, 60);
    }

    void speechTimelineCoversTheClip() {
        SpeechClip clip;
        clip.sentences = {{0, 10, "Short one."}, {11, 80, "A much longer sentence, with a pause or two; and more after it."}};
        const auto spans = Speech::timeline(clip);
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans[0].first, 0.0);
        QCOMPARE(spans[1].second, 1.0);
        QCOMPARE(spans[0].second, spans[1].first);
        QVERIFY(spans[0].second < 0.3);
    }

    void stretcherKeepsPitchAtSpeed() {
        // Two seconds of a 200 Hz tone, played at 1.5×: two thirds as long,
        // still 200 Hz.
        const int rate = 24000;
        QVector<qint16> tone(2 * rate);
        for (int n = 0; n < tone.size(); ++n)
            tone[n] = qint16(8000 * std::sin(2 * M_PI * 200 * n / rate));
        Stretcher stretcher;
        stretcher.reset();
        QVector<qint16> out;
        while (stretcher.process(tone.constData(), tone.size(), true, 1.5, 4096, out)) {
        }
        const double seconds = double(out.size()) / rate;
        QVERIFY2(std::abs(seconds - 2.0 / 1.5) < 0.1, qPrintable(QString::number(seconds)));
        int crossings = 0;
        for (int n = rate / 10; n + 1 < out.size() - rate / 10; ++n)
            crossings += (out[n] < 0) != (out[n + 1] < 0) ? 1 : 0;
        const double hertz = crossings / 2.0 / (double(out.size() - rate / 5) / rate);
        QVERIFY2(std::abs(hertz - 200) < 6, qPrintable(QString::number(hertz)));
        // The level holds: no dips where frames overlap.
        qint16 peak = 0;
        for (int n = rate / 10; n < out.size() - rate / 10; ++n)
            peak = qMax<qint16>(peak, qAbs(out[n]));
        QVERIFY(peak > 7000 && peak < 9000);
    }

    void stretcherWaitsForAudioStillArriving() {
        QVector<qint16> audio(3000, 100);
        Stretcher stretcher;
        stretcher.reset();
        QVector<qint16> out;
        // Not enough yet for a frame and its search: nothing, but not done.
        QVERIFY(stretcher.process(audio.constData(), 1000, false, 1.25, 4096, out));
        QVERIFY(out.isEmpty());
        QVERIFY(stretcher.process(audio.constData(), audio.size(), false, 1.25, 4096, out));
        QVERIFY(!out.isEmpty());
        // Once complete, it flushes and ends.
        while (stretcher.process(audio.constData(), audio.size(), true, 1.25, 4096, out)) {
        }
        QVERIFY(!stretcher.process(audio.constData(), audio.size(), true, 1.25, 4096, out));
    }

    void audioCacheKeepsClips() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg isn't installed");
        QTemporaryDir dir;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "cachetest");
            db.setDatabaseName(dir.filePath("cache.db"));
            QVERIFY(db.open());
            AudioCache cache(dir.filePath("audio"), "cachetest");
            const QString key = AudioCache::keyFor("gemini", "tts", "sulafat", "", "Hello.");
            QVERIFY(key != AudioCache::keyFor("gemini", "tts", "gacrux", "", "Hello."));
            QVERIFY(key != AudioCache::keyFor("gemini", "tts", "sulafat", "whispered", "Hello."));
            QVERIFY(cache.lookup(key).isEmpty());

            QByteArray pcm(24000 * 2, '\0');
            auto *samples = reinterpret_cast<qint16 *>(pcm.data());
            for (int n = 0; n < 24000; ++n)
                samples[n] = qint16(4000 * std::sin(2 * M_PI * 220 * n / 24000.0));
            QSignalSpy stored(&cache, &AudioCache::stored);
            cache.store(key, pcm, 24000);
            QVERIFY(stored.wait(10000));
            const QString file = cache.lookup(key);
            QVERIFY(file.endsWith(".opus"));
            QVERIFY(QFileInfo(file).size() > 1000);
            QVERIFY(!QFile::exists(file + ".part"));
            QVERIFY(cache.megabytes() > 0);

            // A file gone from the disk is forgotten.
            QFile::remove(file);
            QVERIFY(cache.lookup(key).isEmpty());

            // A director's answer is kept beside the audio, and cleared with it.
            QVERIFY(cache.directions("chapter").isEmpty());
            cache.storeDirections("chapter", "{\"changes\":[]}");
            QCOMPARE(cache.directions("chapter"), QString("{\"changes\":[]}"));
            cache.clear();
            QVERIFY(cache.directions("chapter").isEmpty());
        }
        QSqlDatabase::removeDatabase("cachetest");
    }

    // --- Theme ---------------------------------------------------------

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QVERIFY(!theme.omarchy());
        QVERIFY(!theme.light());
        QCOMPARE(theme.background(), QString("#0e0e10"));
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "mode = \"light\"\naccent = \"#112233\"\n"
                                                 "background = \"#fffcf0\"\nforeground = \"#100f0f\"\n"
                                                 "lighter_background = \"#e6e4d9\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QVERIFY(theme.omarchy());
        QVERIFY(theme.light());
        QCOMPARE(theme.background(), QString("#fffcf0"));
        QCOMPARE(theme.surface(), QString("#e6e4d9"));
        // Keys the theme leaves out come from the fallback.
        QCOMPARE(theme.selection(), Theme::fallbackPalette().value("selection"));
    }

    void themeFollowsChanges() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
        Theme theme(dir.path());
        QSignalSpy changed(&theme, &Theme::changed);
        write(dir.path() + "/theme/colors.toml", "accent = \"#445566\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
        QTRY_COMPARE(theme.accent(), QString("#445566"));
        QVERIFY(changed.count() >= 1);
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    // --- Translator ----------------------------------------------------

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n books": {"one": "%n book", "other": "%n books"}})");
        write(dir.path() + "/ru.json", R"({"%n books": {"one": "%n книга", "few": "%n книги", "many": "%n книг", "other": "%n книги"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 21), QString("%n книга"));
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 22), QString("%n книги"));
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 25), QString("%n книг"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    void shippedCatalogsAreValidJson() {
        const QDir catalogs(":/i18n");
        // Catalogs are compiled into the app, not the tests; check the sources.
        const QDir sources(QFINDTESTDATA("../src/i18n"));
        for (const QString &name : sources.entryList({"*.json"})) {
            QFile file(sources.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError error;
            QJsonDocument::fromJson(file.readAll(), &error);
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + ": " + error.errorString()));
        }
        Q_UNUSED(catalogs);
    }

    // --- ZIP -----------------------------------------------------------

    void zipReadsStoredAndDeflatedEntries() {
        QTemporaryDir dir;
        const QByteArray big(100000, 'w');
        QVERIFY(TestBooks::write(dir.filePath("a.zip"),
                                 TestBooks::zip({{"plain.txt", "stored text", false},
                                                 {"dir/Deflated.TXT", big, true},
                                                 {"ünïcode name.txt", "ok", true}})));
        ZipArchive zip(dir.filePath("a.zip"));
        QVERIFY(zip.open());
        QCOMPARE(zip.names().size(), 3);
        QCOMPARE(zip.read("plain.txt"), QByteArray("stored text"));
        QCOMPARE(zip.read("dir/Deflated.TXT"), big);
        // Books sometimes link with different capitalization.
        QCOMPARE(zip.resolve("DIR/deflated.txt"), QString("dir/Deflated.TXT"));
        QCOMPARE(zip.read("dir/deflated.txt"), big);
        QCOMPARE(zip.read("ünïcode name.txt"), QByteArray("ok"));
        QCOMPARE(zip.size("dir/Deflated.TXT"), qint64(big.size()));
        QString error;
        QVERIFY(zip.read("missing.txt", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void zipReadsZip64() {
        QTemporaryDir dir;
        QVERIFY(TestBooks::write(dir.filePath("b.zip"),
                                 TestBooks::zip({{"one.txt", "first", true}, {"two.txt", "second", false}}, true)));
        ZipArchive zip(dir.filePath("b.zip"));
        QVERIFY2(zip.open(), qPrintable(zip.errorString()));
        QCOMPARE(zip.read("one.txt"), QByteArray("first"));
        QCOMPARE(zip.read("two.txt"), QByteArray("second"));
    }

    void zipRejectsOtherFiles() {
        QTemporaryDir dir;
        write(dir.filePath("c.zip"), "this is not a zip at all");
        ZipArchive zip(dir.filePath("c.zip"));
        QVERIFY(!zip.open());
        QVERIFY(!zip.errorString().isEmpty());
    }

    // --- Markup --------------------------------------------------------

    void tokenizerDecodesEntities() {
        QCOMPARE(HtmlTokenizer::decodeEntities(u"a &amp; b &lt;c&gt; &rsquo;&#8217;&#x2019; &nbsp;"),
                 QString("a & b <c> ’’’  "));
        // Unknown entities and bare ampersands stay as written.
        QCOMPARE(HtmlTokenizer::decodeEntities(u"R&D &bogus; &"), QString("R&D &bogus; &"));
        // Windows-1252 code points mean their curly quotes, as in browsers.
        QCOMPARE(HtmlTokenizer::decodeEntities(u"&#147;hi&#148;"), QString("“hi”"));
    }

    void tokenizerDecodesDeclaredEncodings() {
        const QByteArray latin1 = "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?><p>caf\xe9</p>";
        QVERIFY(HtmlTokenizer::decodeDocument(latin1).contains(QString("café")));
        QVERIFY(HtmlTokenizer::decodeDocument("\xEF\xBB\xBF<p>x</p>\r\n").endsWith("</p>\n"));
    }

    void treeParsesForgivingly() {
        const auto doc = Html::parse(QByteArray("<root><a x='1' Y=\"2\">one<br>two</a></b><dc:title>T &amp; U</dc:title></root>"), true);
        const HtmlNode *a = doc->first("a");
        QVERIFY(a);
        QCOMPARE(a->attribute("x"), QString("1"));
        QCOMPARE(a->attribute("y"), QString("2"));
        QCOMPARE(a->textContent(), QString("onetwo"));
        QCOMPARE(doc->first("title")->textContent(), QString("T & U"));
        // In book XML a <meta> holds its text.
        const auto xml = Html::parse(QByteArray("<metadata><meta property='role'>edt</meta></metadata>"));
        QCOMPARE(xml->first("meta")->textContent(), QString("edt"));
    }

    void textExtractionMatchesTheReader() {
        const QByteArray doc = R"(<html><head><title>Not text</title><style>p{}</style></head>
<body><p>One&nbsp;two</p><script>skip()</script>
<p>Sof&#173;t hy&shy;phens <b>bold</b></p><![CDATA[raw <text>]]></body></html>)";
        QCOMPARE(EpubBook::extractText(doc), QString("One two\nSoft hyphens bold" "raw <text>"));
        // Without a body, everything outside the head counts.
        QCOMPARE(EpubBook::extractText("<html><head><title>x</title></head><p>loose</p></html>"), QString("loose"));
    }

    void linksResolveInsideTheBook() {
        QString fragment;
        QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "../images/b%20c.png", &fragment), QString("OEBPS/images/b c.png"));
        QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "b.xhtml#note%201", &fragment), QString("OEBPS/text/b.xhtml"));
        QCOMPARE(fragment, QString("note 1"));
        QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "#n2", &fragment), QString("OEBPS/text/a.xhtml"));
        QCOMPARE(fragment, QString("n2"));
        QCOMPARE(EpubBook::resolve("content.opf", "./text/c.xhtml"), QString("text/c.xhtml"));
        QVERIFY(EpubBook::resolve("OEBPS/a.xhtml", "https://example.com/x").isEmpty());
        QVERIFY(EpubBook::resolve("OEBPS/a.xhtml", "mailto:someone").isEmpty());
    }

    // --- EPUB ----------------------------------------------------------

    void epub2ReadsPackageAndNcx() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        EpubBook book(path);
        QVERIFY2(book.open(), qPrintable(book.errorString()));
        QCOMPARE(book.version(), QString("2.0"));
        const EpubMetadata &meta = book.metadata();
        QCOMPARE(meta.title, QString("The Lantern Keeper (Harbor Classics)"));
        QCOMPARE(meta.authors, QStringList{"Ada Marsh"});
        QCOMPARE(meta.editors, QStringList{"Tom Reed"});
        QCOMPARE(meta.date, QString("1911-04-02"));
        QCOMPARE(meta.isbn, QString("9780000000002"));
        QCOMPARE(meta.description, QString("A keeper & her light.\n\nSecond paragraph."));
        QCOMPARE(meta.series, QString("Harbor Tales"));
        QCOMPARE(meta.seriesIndex, 2.0);

        QCOMPARE(book.spine().size(), 2);
        QCOMPARE(book.spine()[0].path, QString("OEBPS/text/chapter 1.xhtml"));
        QVERIFY(!book.spine()[1].linear);

        QCOMPARE(book.toc().size(), 3);
        QCOMPARE(book.toc()[1].title, QString("The Storm"));
        QCOMPARE(book.toc()[1].depth, 1);
        QCOMPARE(book.toc()[1].fragment, QString("storm"));
        QCOMPARE(book.toc()[1].spine, 0);
        QCOMPARE(book.toc()[2].spine, 1);

        QCOMPARE(book.pageList().size(), 2);
        QCOMPARE(book.pageList()[1].label, QString("2"));
        QCOMPARE(book.pageList()[1].spine, 1);

        QCOMPARE(book.coverPath(), QString("OEBPS/images/cover.png"));
        QString type;
        QVERIFY(!book.coverImage(&type).isEmpty());
        QCOMPARE(type, QString("image/png"));
        QVERIFY(!book.isDrmProtected());

        QCOMPARE(book.chapterText(0), QString("Chapter One\nThe lamp’s light & the sea wind.\n\nA storm came in."));
    }

    void epub3ReadsNavigationAndRefines() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book3.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub3(adobeObfuscate(fontPayload()))));
        EpubBook book(path);
        QVERIFY2(book.open(), qPrintable(book.errorString()));
        QCOMPARE(book.metadata().title, QString("Salt Roads"));
        QCOMPARE(book.metadata().authors, QStringList{"Wen Lo"});
        QCOMPARE(book.metadata().editors, QStringList{"Ira Bell"});
        QCOMPARE(book.metadata().series, QString("Road Books"));
        QCOMPARE(book.metadata().seriesIndex, 3.0);
        QCOMPARE(book.toc().size(), 3);
        QCOMPARE(book.toc()[1].title, QString("The Salt Flats"));
        QCOMPARE(book.toc()[1].depth, 1);
        QCOMPARE(book.toc()[2].title, QString("Appendices"));
        QVERIFY(book.toc()[2].path.isEmpty());
        QCOMPARE(book.pageList().size(), 1);
        QCOMPARE(book.pageList()[0].label, QString("vii"));
        QCOMPARE(book.coverPath(), QString("OEBPS/cover.jpg"));
        // Font obfuscation is undone, and isn't mistaken for DRM.
        QVERIFY(!book.isDrmProtected());
        QCOMPARE(book.resource("OEBPS/fonts/f.otf"), fontPayload());
    }

    void epubWithEncryptedTextIsDrm() {
        QTemporaryDir dir;
        const QString path = dir.filePath("drm.epub");
        const QByteArray zip = TestBooks::drmEpub();
        QVERIFY(TestBooks::write(path, zip));
        EpubBook book(path);
        QVERIFY(book.open());
        QVERIFY(book.isDrmProtected());
        QCOMPARE(BookFiles::read(path).problem, QString("This book is protected by DRM."));
    }

    void epubWithoutChaptersFails() {
        QTemporaryDir dir;
        const QString path = dir.filePath("empty.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub({{"OEBPS/content.opf", "<package><manifest/><spine/></package>"}})));
        EpubBook book(path);
        QVERIFY(!book.open());
        QVERIFY(!book.errorString().isEmpty());
    }

    void bookInfoReadsEpubMetadataAndCover() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Marsh, Ada - Lantern Keeper, The [ed. Reed] (Harbor, 1911).epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(200, 300))));
        const BookInfo info = BookFiles::read(path, 90);
        QVERIFY(info.ok());
        QCOMPARE(info.format, QString("epub"));
        QCOMPARE(info.identity.title, QString("The Lantern Keeper"));
        QCOMPARE(info.identity.author, QString("Ada Marsh"));
        QCOMPARE(info.identity.editor, QString("Reed"));
        QCOMPARE(info.identity.publisher, QString("Harbor"));
        QCOMPARE(info.identity.year, 1911);
        QCOMPARE(info.chapters, 2);
        QCOMPARE(info.pages, 2);
        QCOMPARE(info.cover.height(), 90);
        QCOMPARE(info.cover.width(), 60);
    }

    // --- PDF -----------------------------------------------------------

    void pdfReadsMetadataAndRenders() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Marsh, Ada - Field Notes (Harbor, 1920).pdf");
        QVERIFY(TestBooks::write(path, TestBooks::pdf("Field Notes", "Ada Marsh / Tom Reed [editor]")));
        PdfBook book(path);
        QVERIFY2(book.open(), qPrintable(book.errorString()));
        QCOMPARE(book.pageCount(), 1);
        QCOMPARE(book.title(), QString("Field Notes"));
        QCOMPARE(book.authors(), QStringList{"Ada Marsh"});
        QCOMPARE(book.editors(), QStringList{"Tom Reed"});
        const QImage cover = book.renderCover(300);
        QCOMPARE(cover.size(), QSize(200, 300));
        // Rendered on white, with the text in black somewhere on it.
        QCOMPARE(QColor(cover.pixel(5, 5)), QColor(Qt::white));

        const BookInfo info = BookFiles::read(path, 150);
        QCOMPARE(info.format, QString("pdf"));
        QCOMPARE(info.pages, 1);
        QCOMPARE(info.identity.year, 1920);
        QCOMPARE(info.cover.height(), 150);
    }

    void unreadableFilesReportWhy() {
        QTemporaryDir dir;
        write(dir.filePath("broken.pdf"), "nope");
        QVERIFY(!BookFiles::read(dir.filePath("broken.pdf")).ok());
        QVERIFY(!BookFiles::read(dir.filePath("missing.epub")).ok());
        QVERIFY(BookFiles::isBook("a/b.EPUB"));
        QVERIFY(!BookFiles::isBook("notes.txt"));
    }


    // --- Library and reader ----------------------------------------------

    void libraryStoresBooksAndReadingState() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Marsh, Ada - Lantern Keeper, The (Harbor, 1911).epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY2(library.open(), qPrintable(library.errorString()));
        const int id = library.addBookFile(path);
        QVERIFY2(id > 0, qPrintable(library.errorString()));
        QCOMPARE(library.addBookFile(path), id);
        const BookRecord book = library.record(id);
        QCOMPARE(book.title, QString("The Lantern Keeper"));
        QVERIFY(book.loose);
        QCOMPARE(library.chapterLengths(id).size(), 2);
        QVERIFY(book.length > 0);
        QVERIFY(QFile::exists(dir.filePath("cache/covers/") + book.cover));

        library.savePosition(id, R"({"chapter":0,"offset":12})", 0.4);
        library.setFinished(id, true);
        Library reopened(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(reopened.open());
        QCOMPARE(reopened.record(id).progress, 0.4);
        QVERIFY(reopened.record(id).finishedAt > 0);
        QCOMPARE(reopened.record(id).position, QString(R"({"chapter":0,"offset":12})"));
    }

    void libraryGroupsEditionsIntoWorks() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        for (const char *name : {"Marsh, Ada - Lantern Keeper, The [ed. Reed] (Harbor, 1911).epub",
                                 "Marsh, Ada - Lantern Keeper, The [ed. Gray] (Penguin, 2005).epub"}) {
            const QString path = dir.filePath(name);
            QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
            QVERIFY(library.addBookFile(path) > 0);
        }
        QTRY_COMPARE(library.works()->count(), 1);
        QCOMPARE(library.works()->get(0).value("editions").toInt(), 2);
        library.works()->setGrouped(false);
        QCOMPARE(library.works()->count(), 2);
        library.works()->setSearchText("penguin");
        QCOMPARE(library.works()->count(), 1);
    }

    void schemeServesWellFormedBooksAsXml() {
        QVERIFY(BookScheme::isWellFormed("<?xml version='1.0'?><html><body><p>x</p></body></html>"));
        QVERIFY(!BookScheme::isWellFormed("<html><body><p>unclosed</body></html>"));
        // Entities the XHTML DTD defines are fine for a browser's XML parser.
        QVERIFY(BookScheme::isWellFormed("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"x\"><html><body>a&nbsp;b</body></html>"));
    }

    void readerMapsPositionsOntoTheBook() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int id = library.addBookFile(path);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY2(reader.openBook(id), qPrintable(reader.errorString()));
        QCOMPARE(reader.chapterCount(), 2);
        const QVariantList spine = reader.manifest().value("spine").toList();
        QCOMPARE(spine[0].toMap().value("href").toString(), QString("OEBPS/text/chapter%201.xhtml"));
        QVERIFY(reader.manifest().value("base").toString().startsWith("dogear://local/book/"));
        QCOMPARE(reader.titleOfChapter(1), QString("Notes"));

        const QList<int> lengths = library.chapterLengths(id);
        const double total = lengths[0] + lengths[1];
        QCOMPARE(reader.fractionAt(1, 0), lengths[0] / total);
        const QVariantMap middle = reader.targetForFraction(lengths[0] / total + 0.001);
        QCOMPARE(middle.value("chapter").toInt(), 1);
        QCOMPARE(reader.targetForPageLabel("2").value("chapter").toInt(), 1);
        QVERIFY(reader.targetForPageLabel("99").value("chapter").isValid());
        QVERIFY(!reader.targetForPageLabel("xx").contains("chapter"));

        reader.relocated({{"chapter", 1}, {"offset", 3}, {"snippet", "note"}, {"page", 0}, {"pages", 1}});
        QCOMPARE(reader.chapter(), 1);
        QCOMPARE(reader.chapterTitle(), QString("Notes"));
        reader.close();
        QCOMPARE(library.record(id).position, QString(R"({"chapter":1,"offset":3,"snippet":"note"})"));
    }


    // --- Intelligence ------------------------------------------------------

    void sseParserHandlesChunkedStreams() {
        const QByteArray stream = fixture("text-stream.sse");
        SseParser whole;
        const QList<SseParser::Event> all = whole.feed(stream) + whole.finish();
        QCOMPARE(all.first().name, QByteArray("interaction.created"));
        QCOMPARE(all.last().data, QByteArray("[DONE]"));
        for (int size : {1, 7, 64}) {
            SseParser pieces;
            QList<SseParser::Event> got;
            for (qsizetype i = 0; i < stream.size(); i += size)
                got += pieces.feed(stream.mid(i, size));
            got += pieces.finish();
            QCOMPARE(got.size(), all.size());
            for (int i = 0; i < all.size(); ++i) {
                QCOMPARE(got[i].name, all[i].name);
                QCOMPARE(got[i].data, all[i].data);
            }
        }
        SseParser crlf;
        const auto events = crlf.feed("event: a\r\ndata: one\r\ndata: two\r\n\r\n: comment\n\n");
        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0].data, QByteArray("one\ntwo"));
    }

    void geminiBuildsTheDocumentedRequests() {
        Ai::SpeechRequest speech;
        speech.text = "Hello.";
        speech.style = "warm";
        speech.voice = "sulafat";
        speech.background = true;
        const QJsonObject body = GeminiProvider::speechBody(speech, "gemini-3.8-flash-tts");
        QCOMPARE(body["model"].toString(), QString("gemini-3.8-flash-tts"));
        QCOMPARE(body["store"].toBool(true), false);
        QCOMPARE(body["stream"].toBool(), true);
        QCOMPARE(body["service_tier"].toString(), QString("flex"));
        QCOMPARE(body["response_format"].toObject()["type"].toString(), QString("audio"));
        const QJsonObject step = body["input"].toArray()[0].toObject();
        QCOMPARE(step["type"].toString(), QString("user_input"));
        const QJsonObject text = step["content"].toArray()[0].toObject();
        QCOMPARE(text["text"].toString(), QString("Hello."));
        QCOMPARE(text["annotations"].toArray()[0].toObject()["type"].toString(), QString("speech_metadata"));
        QCOMPARE(text["annotations"].toArray()[0].toObject()["style"].toString(), QString("warm"));
        QCOMPARE(body["generation_config"].toObject()["speech_config"].toArray()[0].toObject()["voice"].toString(), QString("sulafat"));
        speech.style.clear();
        QVERIFY(!GeminiProvider::speechBody(speech, "m")["input"].toArray()[0].toObject()["content"].toArray()[0].toObject().contains("annotations"));

        Ai::TextRequest ask;
        ask.system = "Be brief.";
        ask.messages = {{true, "Who?"}, {false, "Weena."}, {true, "And?"}};
        ask.schema = QJsonObject{{"type", "object"}};
        const QJsonObject request = GeminiProvider::textBody(ask, "gemini-4-hydrogen");
        QCOMPARE(request["system_instruction"].toString(), QString("Be brief."));
        QCOMPARE(request["input"].toArray().size(), 3);
        QCOMPARE(request["input"].toArray()[1].toObject()["type"].toString(), QString("model_output"));
        QCOMPARE(request["response_format"].toObject()["mime_type"].toString(), QString("application/json"));
        QCOMPARE(request["generation_config"].toObject()["thinking_level"].toString(), QString("low"));

        const QJsonObject design = GeminiProvider::voiceDesignBody({"Narrator", "A calm voice.", "male", "en-GB"}, "gemini-3.8-flash-tts");
        QCOMPARE(design["store"].toBool(), true);
        QCOMPARE(design["voice"].toObject()["type"].toString(), QString("prompted"));
        QCOMPARE(design["voice"].toObject()["prompted"].toObject()["input"].toString(), QString("A calm voice."));
        QCOMPARE(design["voice"].toObject()["language_code"].toString(), QString("en-GB"));
    }

    void geminiReadsRecordedResponses() {
        const Ai::Usage usage = GeminiProvider::parseUsage(QJsonDocument::fromJson(fixture("tts-unary.json")).object()["usage"].toObject());
        QCOMPARE(usage.audioTokens, 90);
        QCOMPARE(usage.inputTokens, 10);
        const QList<Ai::Voice> voices = GeminiProvider::parseVoices(QJsonDocument::fromJson(fixture("voices.json")).object()["voices"].toArray());
        QCOMPARE(voices.size(), 5);
        auto byId = [&voices](const QString &id) { return *std::find_if(voices.cbegin(), voices.cend(), [&](const Ai::Voice &v) { return v.id == id; }); };
        QVERIFY(byId("voice_testnarrator1").designed);
        QVERIFY(byId("voice_testnarrator1").prompt.startsWith("A calm harbor-master"));
        QVERIFY(byId("voice_testnarrator1").expires.isValid());
        QVERIFY(byId("en-gb-storyteller-4").persona.contains("Narrator"));
        QCOMPARE(byId("en-gb-storyteller-4").language, QString("en-GB"));
        QVERIFY(!byId("achernar").designed);

        const QJsonObject unary = QJsonDocument::fromJson(fixture("tts-unary.json")).object();
        const QByteArray wav = QByteArray::fromBase64(unary["steps"].toArray()[0].toObject()["content"].toArray()[0].toObject()["data"].toString().toLatin1());
        int rate = 0;
        QCOMPARE(GeminiProvider::pcmFromWav(wav, &rate).size(), 8);
        QCOMPARE(rate, 24000);
        QCOMPARE(GeminiProvider::pcmFromWav("raw bytes"), QByteArray("raw bytes"));

        QCOMPARE(GeminiProvider::parseError(429, fixture("error-429.json"), 0).kind, Ai::Error::Limited);
        QCOMPARE(GeminiProvider::parseError(400, fixture("error-badkey.json"), 0).kind, Ai::Error::BadKey);
        QCOMPARE(GeminiProvider::parseError(0, {}, QNetworkReply::HostNotFoundError).kind, Ai::Error::Offline);
        QCOMPARE(GeminiProvider::parseError(503, "{}", 0).kind, Ai::Error::Server);
    }

    void geminiStreamsTextAndSpeech() {
        FakeServer server;
        server.responses = {{200, "text/event-stream", fixture("text-stream.sse")},
                            {200, "text/event-stream", fixture("tts-stream.sse")},
                            {200, "application/json", fixture("tts-unary.json")},
                            {429, "application/json", fixture("error-429.json")}};
        GeminiProvider gemini(nullptr, server.endpoint());
        gemini.setKey("test-key");

        Ai::TextRequest request;
        request.messages = {{true, "Count"}};
        request.stream = true;
        Ai::Reply *text = gemini.generate(request);
        QStringList deltas;
        connect(text, &Ai::Reply::textReceived, this, [&deltas](const QString &delta) { deltas.append(delta); });
        QTRY_VERIFY(text->isFinished());
        QVERIFY2(!text->error(), qPrintable(text->error().message));
        QCOMPARE(text->text(), QString("One, two, three"));
        QCOMPARE(deltas.size(), 2);
        QCOMPARE(text->usage().outputTokens, qint64(5));
        QCOMPARE(server.requests[0].path, QByteArray("/v1beta/interactions"));
        QCOMPARE(server.requests[0].headers.value("x-goog-api-key"), QByteArray("test-key"));
        QCOMPARE(server.requests[0].headers.value("api-revision"), QByteArray("2026-05-20"));
        QCOMPARE(QJsonDocument::fromJson(server.requests[0].body).object()["model"].toString(), gemini.textModelName(false));

        Ai::SpeechRequest speech;
        speech.text = "Hi";
        speech.voice = "sulafat";
        Ai::Reply *audio = gemini.synthesize(speech);
        int chunks = 0;
        connect(audio, &Ai::Reply::audioReceived, this, [&chunks] { ++chunks; });
        QTRY_VERIFY(audio->isFinished());
        QVERIFY(!audio->error());
        QCOMPARE(chunks, 3);
        QCOMPARE(audio->audio().size(), 24);
        QCOMPARE(audio->sampleRate(), 24000);
        QCOMPARE(audio->usage().audioTokens, qint64(87));

        speech.stream = false;
        Ai::Reply *unary = gemini.synthesize(speech);
        QTRY_VERIFY(unary->isFinished());
        QCOMPARE(unary->audio().size(), 8);

        Ai::Reply *limited = gemini.synthesize(speech);
        QTRY_VERIFY(limited->isFinished());
        QCOMPARE(limited->error().kind, Ai::Error::Limited);
    }

    void ledgerPricesByDateAndTier() {
        Ai::Usage speech;
        speech.inputTokens = 10;
        speech.outputTokens = 90;
        const QDate today(2026, 11, 23);
        QCOMPARE(AiUsageLedger::cost("gemini-3.8-flash-tts", speech, today, false), (10 * 0.5 + 90 * 9.0) / 1e6);
        QCOMPARE(AiUsageLedger::cost("gemini-3.8-flash-tts", speech, QDate(2027, 1, 2), false), (10 * 1.0 + 90 * 18.0) / 1e6);
        QCOMPARE(AiUsageLedger::cost("gemini-3.8-flash-tts", speech, today, true), (10 * 0.5 + 90 * 9.0) / 2e6);
        Ai::Usage ask;
        ask.inputTokens = 100000;
        ask.cachedTokens = 90000;
        ask.outputTokens = 500;
        ask.thoughtTokens = 500;
        QCOMPARE(AiUsageLedger::cost("gemini-4-hydrogen", ask, today, false), (10000 * 1.5 + 90000 * 0.075 + 1000 * 7.5) / 1e6);
        QCOMPARE(AiUsageLedger::cost("a-local-model", ask, today, false), 0.0);
        QVERIFY(qAbs(AiUsageLedger::listeningCost("gemini-3.8-flash-tts", 60, today, false) - 0.81) < 0.01);
    }

    void servicesKeepOnlyWorkingKeysAndCountCosts() {
        qunsetenv("GEMINI_API_KEY");
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        FakeServer server;
        server.responses = {{400, "application/json", fixture("error-badkey.json")},
                            {200, "application/json", R"({"models":[{"name":"models/gemini-4-hydrogen"},{"name":"models/gemini-3.8-flash-tts"}]})"},
                            {200, "application/json", fixture("tts-unary.json")}};
        GeminiProvider gemini(nullptr, server.endpoint());
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        QVERIFY(!services.hasKey());
        QVERIFY(!services.canSpeak());

        QSignalSpy checked(&services, &AiServices::keyChecked);
        services.setKey("wrong");
        QTRY_COMPARE(checked.count(), 1);
        QCOMPARE(checked[0][0].toBool(), false);
        QVERIFY(keys.load("gemini").isEmpty());
        QVERIFY(!services.hasKey());

        services.setKey("right");
        QTRY_COMPARE(checked.count(), 2);
        QVERIFY(checked[1][0].toBool());
        QCOMPARE(keys.load("gemini"), QString("right"));
        QVERIFY(services.connected());
        QVERIFY(services.canSpeak());

        Ai::SpeechRequest speech;
        speech.text = "Hi";
        speech.voice = "sulafat";
        speech.stream = false;
        Ai::Reply *reply = services.speech()->synthesize(speech);
        services.track(reply);
        QTRY_VERIFY(reply->isFinished());
        QTRY_VERIFY(services.spentThisMonth() > 0);
        QCOMPARE(services.usageThisMonth().value(0).toMap().value("feature").toString(), QString("listen"));

        services.forgetKey();
        QVERIFY(keys.load("gemini").isEmpty());
        QVERIFY(!services.hasKey());
    }

    // --- Voices --------------------------------------------------------

    void voicesSuggestNarratorsForTheBook() {
        VoiceRig rig;
        const QString narrator = "Storyteller & Narrator (Nature Documentary Narrator)";
        Ai::Voice designed = makeVoice("voice_abc", "en-GB", {});
        designed.designed = true;
        designed.name = "Victorian Gentleman";
        designed.prompt = "A measured English gentleman.";
        designed.expires = QDateTime::currentDateTime().addYears(1);
        rig.catalog->setVoices({
            makeVoice("en-us-storyteller-1", "en-US", narrator, "East Coast"),
            makeVoice("en-us-storyteller-2", "en-US", narrator, "Texan"),
            makeVoice("en-us-tutor-1", "en-US", "Educational Tutor", "Midwest"),
            makeVoice("en-gb-storyteller-4", "en-GB", narrator, "Bristol English",
                      "56-year-old Nature Documentary Narrator from Bristol. Speaks with a Bristol English accent. "
                      "Currently in a bookstore. Voice is deep, velvety, and unhurried."),
            makeVoice("en-au-storyteller-1", "en-AU", narrator, "Sydney English"),
            makeVoice("de-de-storyteller-1", "de-DE", narrator, "Berlin German"),
            makeVoice("sulafat", "en-US", "Storyteller & Narrator", {},
                      "Warm, inviting, and comforting voice. Recommended for onboarding screens."),
            makeVoice("puck", "en-US", "Companion & Peer"),
            designed,
        });

        // The book's region first, then one from each other region in turn,
        // then the classic narrators; never a voice that isn't a narrator.
        QVariantList offered = rig.catalog->suggestions("en-GB");
        QStringList ids;
        for (const QVariant &voice : offered)
            ids.append(voice.toMap().value("id").toString());
        QCOMPARE(ids.first(), QString("en-gb-storyteller-4"));
        QVERIFY(ids.indexOf("en-us-storyteller-2") > ids.indexOf("en-au-storyteller-1"));
        QCOMPARE(ids.last(), QString("sulafat"));
        QVERIFY(!ids.contains("en-us-tutor-1") && !ids.contains("puck") && !ids.contains("de-de-storyteller-1"));
        QVERIFY(!ids.contains("voice_abc"));
        // Another language: its narrators, and the classic voices.
        offered = rig.catalog->suggestions("de");
        QCOMPARE(offered.size(), 2);
        QCOMPARE(offered[0].toMap().value("id").toString(), QString("de-de-storyteller-1"));

        // A voice is told apart by its accent; its summary is how it sounds.
        const QVariantMap bristol = rig.catalog->voice("en-gb-storyteller-4");
        QCOMPARE(bristol.value("title").toString(), QString("Bristol English"));
        QCOMPARE(bristol.value("age").toInt(), 56);
        QCOMPARE(bristol.value("summary").toString(), QString("Voice is deep, velvety, and unhurried."));
        QCOMPARE(bristol.value("kind").toString(), QString("library"));
        QCOMPARE(rig.catalog->voice("sulafat").value("summary").toString(), QString("Warm, inviting, and comforting voice."));
        QCOMPARE(rig.catalog->voice("sulafat").value("kind").toString(), QString("classic"));

        // Search finds any word about a voice, narrators first.
        QCOMPARE(rig.catalog->search("bristol", {}).size(), 1);
        QCOMPARE(rig.catalog->search("east coast", {}).value(0).toMap().value("id").toString(), QString("en-us-storyteller-1"));
        QCOMPARE(rig.catalog->search("storyteller", "de").size(), 2);  // German, and Sulafat
        QCOMPARE(rig.catalog->search("measured gentleman", {}).value(0).toMap().value("id").toString(), QString("voice_abc"));

        const QVariantList yours = rig.catalog->designedVoices();
        QCOMPARE(yours.size(), 1);
        QCOMPARE(yours[0].toMap().value("summary").toString(), QString("A measured English gentleman."));
        QCOMPARE(yours[0].toMap().value("kind").toString(), QString("designed"));
    }

    void voicesAreKeptForADay() {
        VoiceRig rig;
        rig.server.responses = {{200, "application/json", fixture("voices.json")}};
        QSignalSpy changed(rig.catalog.get(), &VoiceCatalog::changed);
        rig.catalog->refresh();
        QTRY_VERIFY(!rig.catalog->loading() && rig.catalog->count() > 0);
        const int count = rig.catalog->count();
        // Fresh: no second request.
        rig.catalog->refresh();
        QVERIFY(!rig.catalog->loading());
        QCOMPARE(rig.server.requests.size(), 1);
        QVERIFY(rig.server.requests[0].path.startsWith("/v1beta/voices?"));
        // A new start finds the list kept.
        VoiceCatalog again(rig.services.get(), rig.cache.get(), &rig.reader, rig.library.database().connectionName(),
                           rig.dir.filePath("voices.json"));
        QCOMPARE(again.count(), count);
        QCOMPARE(again.voice("achernar").value("name").toString(), QString("Achernar"));
    }

    void voicesCastADesignedNarrator() {
        VoiceRig rig;
        auto castingReply = [this](const QString &name) {
            QJsonObject reply = QJsonDocument::fromJson(fixture("text-unary.json")).object();
            QJsonArray steps = reply["steps"].toArray();
            QJsonObject output = steps.last().toObject();
            const QJsonObject proposal{{"name", name},
                                       {"description", "A warm, measured English baritone in his fifties."},
                                       {"gender", "male"},
                                       {"language", "en-GB"}};
            output["content"] = QJsonArray{QJsonObject{{"type", "text"},
                                                       {"text", QString::fromUtf8(QJsonDocument(proposal).toJson())}}};
            steps.replace(steps.size() - 1, output);
            reply["steps"] = steps;
            return QJsonDocument(reply).toJson();
        };
        rig.server.responses = {{200, "application/json", castingReply("Victorian Gentleman")},
                                {200, "application/json", fixture("voice-design.json")}};

        rig.catalog->cast("It was a dark and stormy night.");
        QCOMPARE(rig.catalog->casting(), QString("thinking"));
        QTRY_COMPARE(rig.catalog->casting(), QString("proposed"));
        QCOMPARE(rig.catalog->proposal().value("name").toString(), QString("Victorian Gentleman"));
        QCOMPARE(rig.catalog->proposal().value("gender").toString(), QString("male"));
        QCOMPARE(rig.catalog->proposal().value("language").toString(), QString("en-GB"));
        const QJsonObject asked = QJsonDocument::fromJson(rig.server.requests[0].body).object();
        QVERIFY(QJsonDocument(asked).toJson().contains("You cast the narrator"));
        QVERIFY(QJsonDocument(asked).toJson().contains("It was a dark and stormy night."));
        QVERIFY(QJsonDocument(asked).toJson().contains("\"female\""));

        rig.catalog->design(rig.catalog->proposal());
        QCOMPARE(rig.catalog->casting(), QString("designing"));
        QTRY_COMPARE(rig.catalog->casting(), QString("designed"));
        QCOMPARE(rig.catalog->designed().value("id").toString(), QString("voice_jtlkz7mcspcr"));
        const QJsonObject designed = QJsonDocument::fromJson(rig.server.requests[1].body).object();
        QCOMPARE(rig.server.requests[1].path, QByteArray("/v1beta/voices"));
        QVERIFY(designed.value("store").toBool());
        QCOMPARE(designed.value("voice").toObject().value("prompted").toObject().value("input").toString(),
                 QString("A warm, measured English baritone in his fifties."));
        QCOMPARE(rig.catalog->designedVoices().value(0).toMap().value("id").toString(), QString("voice_jtlkz7mcspcr"));
        QSqlQuery remembered(rig.library.database());
        QVERIFY(remembered.exec("SELECT name, description FROM designed_voices"));
        QVERIFY(remembered.next());
        QCOMPARE(remembered.value(0).toString(), QString("Victorian Gentleman"));
        // Its sample plays at once.
        QCOMPARE(rig.catalog->previewing(), QString("voice_jtlkz7mcspcr"));

        // Another idea differs from the ones already offered.
        rig.server.responses = {{200, "application/json", castingReply("Brisk Raconteur")}};
        rig.catalog->cast("It was a dark and stormy night.");
        QTRY_COMPARE(rig.catalog->casting(), QString("proposed"));
        QVERIFY(rig.server.requests[2].body.contains("Earlier proposals"));
        QVERIFY(rig.server.requests[2].body.contains("A warm, measured English baritone"));

        // Deleting is for good, on the provider and here.
        rig.server.responses = {{200, "application/json", "{}"}};
        rig.catalog->forget("voice_jtlkz7mcspcr");
        QTRY_VERIFY(rig.catalog->voice("voice_jtlkz7mcspcr").isEmpty());
        QCOMPARE(rig.server.requests[3].method, QByteArray("DELETE"));
        QCOMPARE(rig.server.requests[3].path, QByteArray("/v1beta/voices/voice_jtlkz7mcspcr"));
        QSqlQuery gone(rig.library.database());
        QVERIFY(gone.exec("SELECT COUNT(*) FROM designed_voices"));
        QVERIFY(gone.next());
        QCOMPARE(gone.value(0).toInt(), 0);
    }

    // --- Names ---------------------------------------------------------

    void fileNamesFollowTheCollectionPattern() {
        FileNameParts parts = BookName::parseFileName("Austen, Jane - Pride and Prejudice [ed. Jones] (Penguin, 2003).epub");
        QVERIFY(parts.matched);
        QCOMPARE(parts.author, QString("Jane Austen"));
        QCOMPARE(parts.title, QString("Pride and Prejudice"));
        QCOMPARE(parts.editor, QString("Jones"));
        QCOMPARE(parts.publisher, QString("Penguin"));
        QCOMPARE(parts.year, 2003);

        parts = BookName::parseFileName("Melville, Herman - Whale Book, The_ A Voyage [illus. Kent] (Dover, 2015).pdf");
        QCOMPARE(parts.title, QString("The Whale Book: A Voyage"));
        QCOMPARE(parts.illustrator, QString("Kent"));

        parts = BookName::parseFileName("Ross, Ann [ed.] The Austen Reader (Harbor, 2001).epub");
        QVERIFY(parts.matched);
        QVERIFY(parts.authorIsEditor);
        QCOMPARE(parts.author, QString("Ann Ross"));
        QCOMPARE(parts.title, QString("The Austen Reader"));

        parts = BookName::parseFileName("Gray, Lee [ed.] - Letters of Two Friends (Harbor, 1959).pdf");
        QVERIFY(parts.authorIsEditor);
        QCOMPARE(parts.author, QString("Lee Gray"));

        QVERIFY(!BookName::parseFileName("just a title.epub").matched);
    }

    void authorsReadTheSameEveryWay() {
        for (const char *spelling : {"Wells, H. G.", "H.G. Wells", "HG Wells", "H. G. WELLS", "H.G.Wells"})
            QCOMPARE(BookName::displayAuthor(spelling), QString("H. G. Wells"));
        QCOMPARE(BookName::displayAuthor("Dickens, Charles, 1812-1870"), QString("Charles Dickens"));
        QCOMPARE(BookName::sortAuthor("H. G. Wells"), QString("Wells, H. G."));
        QCOMPARE(BookName::sortAuthor("Ludwig van Beethoven"), QString("van Beethoven, Ludwig"));
        QCOMPARE(BookName::sortAuthor("Plato"), QString("Plato"));
    }

    void titlesClean() {
        QCOMPARE(BookName::displayTitle("Moon Book, The"), QString("The Moon Book"));
        QCOMPARE(BookName::displayTitle("Coat, The, the Hat and the Glove"), QString("Coat, The, the Hat and the Glove"));
        QCOMPARE(BookName::displayTitle("Letters, Vol. 2_ 1904-1928"), QString("Letters, Vol. 2: 1904-1928"));
        QCOMPARE(BookName::cleanTitle("The New Road (Penguin Classics) -- A_ Writer"), QString("The New Road"));
        QCOMPARE(BookName::cleanTitle("The Isle of Dr. Lark (Barnes &amp; Noble Classics)"), QString("The Isle of Dr. Lark"));
        QCOMPARE(BookName::cleanTitle("Notes (1890)"), QString("Notes (1890)"));
        QCOMPARE(BookName::sortTitle("The Moon Book"), QString("moon book"));
        QVERIFY(BookName::looksLikeGarbageTitle("Microsoft Word - draft.doc"));
        QVERIFY(BookName::looksLikeGarbageTitle("untitled"));
        QVERIFY(!BookName::looksLikeGarbageTitle("Emma"));
    }

    void editionsShareAWorkKey() {
        const QString a = BookName::workKey("H. G. Wells", "The Isle of Doctor Lark");
        QCOMPARE(BookName::workKey("H.G. Wells", "The Isle of Dr. Lark (Barnes & Noble Classics)"), a);
        QCOMPARE(BookName::workKey("Wells, H. G.", "Isle of Dr Lark: A Romance"), a);
        QVERIFY(BookName::workKey("H. G. Wells", "The Sleeper Wakes") != BookName::workKey("H. G. Wells", "The Sleeper Awakes!"));
        // Volumes of one work are different books.
        QCOMPARE(BookName::volumeNumber("The Outline: Volume 2, Rome"), 2);
        QCOMPARE(BookName::volumeNumber("History, Vol. IV"), 4);
        QVERIFY(BookName::workKey("A", "Outline", 1) != BookName::workKey("A", "Outline", 2));
    }

    void identityResolvesSwapsAndMislabels() {
        // A title and author swapped in the metadata.
        BookIdentity id = BookName::identify("/b/Wells, H. G. - Moon Book, The [ed. Ross] (Penguin, 2005).epub",
                                             "H. G. Wells", {"The Moon Book"}, {}, "Penguin Books Ltd", "2005");
        QCOMPARE(id.title, QString("The Moon Book"));
        QCOMPARE(id.author, QString("H. G. Wells"));
        QCOMPARE(id.editor, QString("Ross"));

        // A file named for one book that holds another: the content wins.
        id = BookName::identify("/b/Wells, H. G. - Glass Man [ed. Ross] (Penguin, 2005).epub",
                                "The Isle of Doctor Lark", {"H.G. Wells"}, {}, {}, {});
        QCOMPARE(id.title, QString("The Isle of Doctor Lark"));
        QCOMPARE(id.author, QString("H. G. Wells"));
        QCOMPARE(id.workKey, BookName::workKey("H. G. Wells", "The Isle of Dr. Lark"));

        // No usable metadata: the file name speaks.
        id = BookName::identify("/b/Marsh, Ada - Field Notes, Vol. 2_ Coast (Harbor, 1920).pdf",
                                "Microsoft Word - fn2.doc", {}, {}, {}, {});
        QCOMPARE(id.title, QString("Field Notes, Vol. 2: Coast"));
        QCOMPARE(id.author, QString("Ada Marsh"));
        QCOMPARE(id.year, 1920);
        QVERIFY(id.workKey.endsWith("|v2"));

        // A file name more specific than the metadata title wins.
        id = BookName::identify("/b/Marsh, Ada - Atlas of Tides, Vol. 2_ The South (Harbor, 2004).epub",
                                "The Atlas of Tides", {"Ada Marsh"}, {}, {}, {});
        QCOMPARE(id.title, QString("The Atlas of Tides, Vol. 2: The South"));
        QVERIFY(id.workKey.endsWith("|v2"));

        // Several authors sort under the collection's own.
        id = BookName::identify("/b/Marsh, Ada - Letters [with Tom Reed] (Harbor, 1935).pdf",
                                "Letters", {"Tom Reed & Ada Marsh"}, {}, {}, {});
        QCOMPARE(id.author, QString("Tom Reed & Ada Marsh"));
        QCOMPARE(id.sortAuthor, QString("Marsh, Ada"));

        // Without a file-name pattern, metadata and publisher clean up.
        id = BookName::identify("/b/x.epub", "Emma", {"Austen, Jane"}, {"Ed Itor"}, "Harbor Books Ltd", "1815-12-23");
        QCOMPARE(id.author, QString("Jane Austen"));
        QCOMPARE(id.sortAuthor, QString("Austen, Jane"));
        QCOMPARE(id.publisher, QString("Harbor Books"));
        QCOMPARE(id.editor, QString("Ed Itor"));
        QCOMPARE(id.year, 1815);
    }
};

QTEST_MAIN(DogearTests)
#include "dogear_tests.moc"
