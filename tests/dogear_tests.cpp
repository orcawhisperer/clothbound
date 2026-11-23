#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QProcess>
#include <QPromise>
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
#include "localmodels.h"
#include "companion.h"
#include "passageindex.h"
#include "pdfbook.h"
#include "pdfpages.h"
#include "cast.h"
#include "audiobook.h"
#include <QPdfWriter>
#include <QPainter>
#include "systemcheck.h"
#include "instance.h"
#include "keystore.h"
#include "localspeech.h"
#include "voicepacks.h"
#include "sseparser.h"
#include "bookname.h"
#include "bookscheme.h"
#include "booksearch.h"
#include "library.h"
#include "marks.h"
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

// A keyring that answers when told to, like one starting up or waiting to be
// unlocked.
class SlowKeyStore : public MemoryKeyStore {
public:
    QPromise<QString> answer;
    QFuture<QString> loadLater(const QString &) override {
        answer.start();
        return answer.future();
    }
};

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


    // A small manifest in the shape of src/voices/packs.json, pointing at `base`.
    static QByteArray packsManifest(const QString &base, const QByteArray &engineSha = "00",
                                    const QByteArray &piperSha = "00", qint64 engineSize = 10, qint64 piperSize = 10) {
        const QJsonObject engine{{"version", "1.13.8"},
                                 {"license", "Apache-2.0"},
                                 {"x86_64", QJsonObject{{"url", base + "/engine-x64.tar.bz2"}, {"size", engineSize},
                                                        {"sha256", QString(engineSha)}, {"dir", "engine-x64"}}},
                                 {"aarch64", QJsonObject{{"url", base + "/engine-arm.tar.bz2"}, {"size", engineSize},
                                                         {"sha256", QString(engineSha)}, {"dir", "engine-arm"}}}};
        const QJsonObject kokoro{
            {"id", "kokoro-v1.0"}, {"title", "Kokoro"}, {"license", "Apache-2.0"},
            {"variants", QJsonObject{{"x86_64", QJsonObject{{"url", base + "/k-int8.tar.bz2"}, {"size", 132},
                                                            {"sha256", "11"}, {"dir", "kokoro-int8"}, {"model", "model.int8.onnx"}}},
                                     {"aarch64", QJsonObject{{"url", base + "/k-fp32.tar.bz2"}, {"size", 350},
                                                             {"sha256", "22"}, {"dir", "kokoro-fp32"}, {"model", "model.onnx"}}}}},
            {"languages", QJsonObject{{"en-US", QJsonObject{{"lexicon", "lexicon-us-en.txt"}, {"lang", "en"}}},
                                      {"en-GB", QJsonObject{{"lexicon", "lexicon-gb-en.txt"}, {"lang", "en"}}}}},
            {"voices", QJsonArray{QJsonObject{{"id", "af_heart"}, {"speaker", 3}, {"name", "Heart"}, {"language", "en-US"},
                                              {"gender", "female"}, {"grade", "A"}, {"recommended", true}},
                                  QJsonObject{{"id", "bm_george"}, {"speaker", 26}, {"name", "George"}, {"language", "en-GB"},
                                              {"gender", "male"}, {"grade", "C"}, {"recommended", true}},
                                  QJsonObject{{"id", "bm_lewis"}, {"speaker", 27}, {"name", "Lewis"}, {"language", "en-GB"},
                                              {"gender", "male"}, {"grade", "D+"}, {"recommended", false}}}}};
        const QJsonObject piper{
            {"license", "MIT"},
            {"voices", QJsonArray{QJsonObject{{"id", "de_DE-thorsten-high"}, {"dir", "vits-piper-de_DE-thorsten-high"},
                                              {"model", "de_DE-thorsten-high.onnx"}, {"language", "de-DE"}, {"name", "Thorsten"},
                                              {"quality", "high"}, {"recommended", true}, {"url", base + "/thorsten.tar.bz2"},
                                              {"size", piperSize}, {"sha256", QString(piperSha)}},
                                  QJsonObject{{"id", "de_DE-eva_k-x_low"}, {"dir", "vits-piper-de_DE-eva_k-x_low"},
                                              {"model", "de_DE-eva_k-x_low.onnx"}, {"language", "de-DE"}, {"name", "Eva K"},
                                              {"quality", "x_low"}, {"recommended", false}, {"url", base + "/eva.tar.bz2"},
                                              {"size", 5}, {"sha256", "33"}}}}};
        return QJsonDocument(QJsonObject{{"engine", engine}, {"kokoro", kokoro}, {"piper", piper}}).toJson();
    }

    // A pack's folder, as if it had been downloaded and unpacked.
    static void fakeInstall(const QString &root, const QString &dir) {
        QDir().mkpath(root + "/" + dir);
        QFile marker(root + "/" + dir + "/.dogear-installed");
        QVERIFY(marker.open(QIODevice::WriteOnly));
    }

    // An engine that makes a tone: half a second per sentence at 16 kHz.
    class ToneEngine : public TtsEngine {
    public:
        std::shared_ptr<QStringList> spoken = std::make_shared<QStringList>();
        QString prepare(const VoicePacks::Model &) override { return {}; }
        int sampleRate() const override { return 16000; }
        QVector<float> speak(const QString &text, int, const std::atomic_bool &stop) override {
            spoken->append(text);
            if (text.contains("slowly"))
                QThread::msleep(400);
            if (stop)
                return {};
            QVector<float> samples(8000);
            for (int n = 0; n < samples.size(); ++n)
                samples[n] = 0.5f * std::sin(2 * M_PI * 300 * n / 16000.0);
            return samples;
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
        const QList<SpeechClip> made = Speech::clips(3, blocks);
        // The number and title together, then the paragraph, with the short
        // lines of dialogue after it (so a page of conversation isn't a
        // request a line), then a pause for the scene break.
        QCOMPARE(made[0].text, QString("5. The Golden Age."));
        QCOMPARE(made[0].sentences.size(), 2);
        QCOMPARE(made[0].block, 0);
        QCOMPARE(made[1].text, first + rest + " “Yes,” he said. “No.”");
        QCOMPARE(made[1].block, 2);
        QCOMPARE(made[2].pauseMs, 1300);
        QCOMPARE(made[3].text, QString("Morning came."));
        QCOMPARE(made.size(), 4);
        for (const SpeechClip &clip : made)
            QCOMPARE(clip.chapter, 3);

        // From the start, or from a clip's start: exactly the chapter's own
        // clips, so audio made before is found again.
        QCOMPARE(Speech::clipsFrom(3, blocks, 0).size(), made.size());
        QList<SpeechClip> from = Speech::clipsFrom(3, blocks, made[1].start());
        QCOMPARE(from.size(), 3);
        QCOMPARE(from[0].text, made[1].text);
        // From the middle of a clip: a lead-in from that sentence, then the
        // chapter's own clips again.
        from = Speech::clipsFrom(3, blocks, 15 + first.size() + 5);
        QCOMPARE(from[0].text, rest.trimmed() + " “Yes,” he said. “No.”");
        QCOMPARE(from[0].block, 2);
        QCOMPARE(from[1].pauseMs, 1300);
        QCOMPARE(from[2].text, made[3].text);
        // Past a scene break, the pause isn't read.
        QCOMPARE(Speech::clipsFrom(3, blocks, 240).size(), 1);
    }

    void speechBeginsAClipWhereTheMoodChanges() {
        const QList<SpeechBlock> blocks{block(0, "It was quiet."), block(20, "“Hush,” she said."),
                                        block(40, "They waited."), block(60, "Then the door burst open.")};
        // Short paragraphs share a clip, unless the mood changes between them.
        QCOMPARE(Speech::clips(0, blocks).size(), 1);
        const QList<SpeechClip> made = Speech::clips(0, blocks, {1, 3});
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
        const QList<SpeechClip> made = Speech::clips(0, {block(0, text.trimmed())});
        QVERIFY(made.size() >= 3);
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


    // --- Marks ---------------------------------------------------------

    void marksAreKeptAndUndone() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        QVERIFY(TestBooks::write(dir.filePath("book.epub"), TestBooks::epub2(png(20, 30))));
        const int book = library.addBookFile(dir.filePath("book.epub"));
        Marks marks(&library);
        marks.setBook(book);
        QCOMPARE(marks.count(), 0);

        const int later = marks.addHighlight(1, 50, 60, "a later  passage", "green", "Notes");
        const int earlier = marks.addHighlight(0, 10, 20, "an earlier one", "", "Chapter One");
        const int bookmark = marks.addBookmark(0, 5, "The lamp", "Chapter One");
        QVERIFY(later && earlier && bookmark);
        // In the book's order, whatever order they were made in.
        QCOMPARE(marks.data(marks.index(0), Marks::IdRole).toInt(), bookmark);
        QCOMPARE(marks.data(marks.index(1), Marks::IdRole).toInt(), earlier);
        QCOMPARE(marks.data(marks.index(2), Marks::IdRole).toInt(), later);
        QCOMPARE(marks.mark(earlier).value("color").toString(), QString("yellow"));
        QCOMPARE(marks.mark(later).value("text").toString(), QString("a later passage"));

        QCOMPARE(marks.highlightAt(0, 15), earlier);
        QCOMPARE(marks.highlightAt(0, 25), 0);
        QCOMPARE(marks.bookmarkBetween(0, 0, 100), bookmark);
        QCOMPARE(marks.bookmarkBetween(0, 6, 100), 0);
        QCOMPARE(marks.highlightsIn(0).size(), 1);

        marks.setNote(earlier, "  Remember this.  ");
        QCOMPARE(marks.mark(earlier).value("note").toString(), QString("Remember this."));
        QVERIFY(marks.highlightsIn(0).value(0).toMap().value("noted").toBool());
        marks.setColor(earlier, "pink");

        // Undo takes back each step, newest first; redo puts it back.
        QVERIFY(marks.canUndo());
        marks.undo();
        QCOMPARE(marks.mark(earlier).value("color").toString(), QString("yellow"));
        marks.undo();
        QVERIFY(marks.mark(earlier).value("note").toString().isEmpty());
        marks.redo();
        QCOMPARE(marks.mark(earlier).value("note").toString(), QString("Remember this."));

        // A mark taken away comes back with its note, and its id.
        marks.remove(earlier);
        QCOMPARE(marks.count(), 2);
        marks.undo();
        QCOMPARE(marks.count(), 3);
        QCOMPARE(marks.mark(earlier).value("note").toString(), QString("Remember this."));

        // Kept in the library: a new look at the book finds them.
        Marks again(&library);
        again.setBook(book);
        QCOMPARE(again.count(), 3);
        QCOMPARE(again.mark(earlier).value("note").toString(), QString("Remember this."));
        QVERIFY(!again.canUndo());

        const QString notes = marks.markdown("The Lantern Keeper", "Ada Marsh");
        QVERIFY(notes.startsWith("# The Lantern Keeper\n"));
        QVERIFY(notes.contains("## Chapter One"));
        QVERIFY(notes.contains("> an earlier one\n\nRemember this."));
        QVERIFY(notes.indexOf("## Chapter One") < notes.indexOf("## Notes"));
    }


    // --- Built-in voices ---------------------------------------------------

    void voicePacksOfferTheRightVoices() {
        QTemporaryDir dir;
        VoicePacks arm(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        QVERIFY(arm.supported());
        QVERIFY(!arm.engineInstalled());
        // Kokoro comes in the build that's quick on the processor.
        QCOMPARE(arm.pack("kokoro-v1.0")->dir, QString("kokoro-fp32"));
        VoicePacks intel(dir.filePath("voices"), nullptr, "x86_64", packsManifest("http://x"));
        QCOMPARE(intel.pack("kokoro-v1.0")->dir, QString("kokoro-int8"));
        VoicePacks other(dir.filePath("voices"), nullptr, "riscv64", packsManifest("http://x"));
        QVERIFY(!other.supported());

        // English: the region asked for first, then the recommended.
        QVariantList voices = arm.voices("en-GB");
        QCOMPARE(voices.size(), 3);
        QCOMPARE(voices[0].toMap().value("id").toString(), QString("builtin:kokoro:bm_george"));
        QCOMPARE(voices[2].toMap().value("id").toString(), QString("builtin:kokoro:af_heart"));
        QCOMPARE(arm.voices("fr").size(), 0);
        // German: the recommended Piper voice, with what it takes to get it.
        const QVariantMap offer = arm.offer("de");
        QCOMPARE(offer.value("voice").toMap().value("id").toString(), QString("builtin:piper:de_DE-thorsten-high"));
        QCOMPARE(offer.value("bytes").toLongLong(), 20);  // engine and voice
        QCOMPARE(offer.value("titles").toStringList().size(), 2);
        QVERIFY(!arm.model("builtin:piper:de_DE-thorsten-high"));

        // Once installed, a voice knows its files; English voices share a
        // model by accent.
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        QVERIFY(arm.engineInstalled());
        QVERIFY(arm.engineLibrary().endsWith("engine-arm/lib/libsherpa-onnx-c-api.so"));
        const auto george = arm.model("builtin:kokoro:bm_george");
        QVERIFY(george);
        QCOMPARE(george->speaker, 26);
        QVERIFY(george->model.endsWith("kokoro-fp32/model.onnx"));
        QVERIFY(george->lexicon.endsWith("lexicon-gb-en.txt"));
        QCOMPARE(george->key, arm.model("builtin:kokoro:bm_lewis")->key);
        QVERIFY(george->key != arm.model("builtin:kokoro:af_heart")->key);
        QVERIFY(arm.voice("builtin:kokoro:af_heart").value("installed").toBool());
        QCOMPARE(arm.offer("en").value("bytes").toLongLong(), 0);
    }

    void voicePacksInstallFromTheNetwork() {
        QTemporaryDir dir;
        // Two packs as real archives, served by a fake server.
        auto archive = [&dir](const QString &folder, const QString &file) {
            QDir().mkpath(dir.filePath("src/" + folder + "/lib"));
            write(dir.filePath("src/" + folder + "/lib/" + file), "voice data");
            QProcess tar;
            tar.start("tar", {"-cjf", dir.filePath(folder + ".tar.bz2"), "-C", dir.filePath("src"), folder});
            tar.waitForFinished();
            QFile made(dir.filePath(folder + ".tar.bz2"));
            made.open(QIODevice::ReadOnly);
            return made.readAll();
        };
        const QByteArray engine = archive("engine-arm", "libsherpa-onnx-c-api.so");
        const QByteArray thorsten = archive("vits-piper-de_DE-thorsten-high", "de_DE-thorsten-high.onnx");
        auto sha = [](const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); };

        FakeServer server;
        // The engine arrives whole; the voice resumes from half way.
        const qint64 half = thorsten.size() / 2;
        server.responses = {{200, "application/octet-stream", engine}, {206, "application/octet-stream", thorsten.mid(half)}};
        const QString root = dir.filePath("voices");
        VoicePacks packs(root, nullptr, "aarch64",
                         packsManifest(server.endpoint(), sha(engine), sha(thorsten), engine.size(), thorsten.size()));
        QDir().mkpath(root + "/.downloads");
        write(root + "/.downloads/thorsten.tar.bz2.part", thorsten.left(half));

        QSignalSpy ready(&packs, &VoicePacks::ready);
        packs.installVoice("builtin:piper:de_DE-thorsten-high");
        QVERIFY(packs.busy());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QCOMPARE(ready[0][0].toString(), QString("builtin:piper:de_DE-thorsten-high"));
        QVERIFY(!packs.busy());
        QVERIFY(packs.engineInstalled());
        QVERIFY(QFile::exists(root + "/vits-piper-de_DE-thorsten-high/lib/de_DE-thorsten-high.onnx"));
        QVERIFY(packs.model("builtin:piper:de_DE-thorsten-high"));
        QCOMPARE(server.requests[1].headers.value("range"), QByteArray("bytes=") + QByteArray::number(half) + "-");
        QVERIFY(!QFile::exists(root + "/.downloads/thorsten.tar.bz2"));
        QCOMPARE(packs.installedPacks().size(), 2);

        // Removing takes the files away.
        packs.remove("piper:de_DE-thorsten-high");
        QVERIFY(!packs.model("builtin:piper:de_DE-thorsten-high"));
        QVERIFY(!QFile::exists(root + "/vits-piper-de_DE-thorsten-high"));

        // A download that doesn't match its checksum is refused (one that
        // comes up short is only interrupted, and picks up later).
        server.responses = {{200, "application/octet-stream", QByteArray(thorsten.size(), 'x')}};
        QSignalSpy failed(&packs, &VoicePacks::failed);
        packs.installVoice("builtin:piper:de_DE-thorsten-high");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed[0][0].toString().contains("checksum"));
        QVERIFY(!packs.model("builtin:piper:de_DE-thorsten-high"));
    }

    void resamplingKeepsPitchAndLength() {
        QVector<float> tone(22050);
        for (int n = 0; n < tone.size(); ++n)
            tone[n] = float(std::sin(2 * M_PI * 440 * n / 22050.0));
        const QVector<float> out = Speech::resample(tone, 22050, 24000);
        QVERIFY(std::abs(out.size() - 24000) <= 1);
        int crossings = 0;
        for (int n = 1; n < out.size(); ++n)
            crossings += (out[n - 1] < 0) != (out[n] < 0) ? 1 : 0;
        QVERIFY2(std::abs(crossings - 880) <= 4, qPrintable(QString::number(crossings)));
        QCOMPARE(Speech::resample(tone, 24000, 24000).size(), tone.size());
        const QByteArray pcm = Speech::pcm16({0.0f, 1.0f, -1.0f, 2.0f});
        const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
        QCOMPARE(samples[1], qint16(32767));
        QCOMPARE(samples[2], qint16(-32767));
        QCOMPARE(samples[3], qint16(32767));  // clipped, not wrapped
    }

    void builtinVoicesSpeakASentenceAtATime() {
        QTemporaryDir dir;
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        auto engine = std::make_shared<QStringList>();
        LocalSpeech local(&packs, nullptr, [engine](const QString &) {
            auto tone = std::make_unique<ToneEngine>();
            tone->spoken = engine;
            return tone;
        });
        QVERIFY(local.canSpeak("builtin:kokoro:bm_george"));
        QVERIFY(!local.canSpeak("builtin:piper:de_DE-thorsten-high"));
        QVERIFY(!local.supportsStyle());

        Ai::SpeechRequest request;
        request.voice = "builtin:kokoro:bm_george";
        request.sentences = {"One.", "Two.", "Three."};
        Ai::Reply *reply = local.synthesize(request);
        QSignalSpy audio(reply, &Ai::Reply::audioReceived);
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(!reply->error());
        QCOMPARE(audio.count(), 3);
        QCOMPARE(*engine, QStringList({"One.", "Two.", "Three."}));
        // Each sentence: half a second at 24 kHz, and a beat after it (a
        // longer one at the end); its end is where the next begins.
        const QList<qint64> ends = reply->boundaries();
        QCOMPARE(ends.size(), 3);
        QCOMPARE(ends[0], 12000 + qint64(0.14 * 24000));
        QCOMPARE(ends[2], reply->audio().size() / 2);
        QCOMPARE(reply->sampleRate(), 24000);

        // Text without sentences is split here.
        engine->clear();
        request.sentences.clear();
        request.text = "It rained. Then it stopped.";
        reply = local.synthesize(request);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(engine->size(), 2);

        // Stopping is immediate, and the rest isn't made.
        engine->clear();
        request.sentences = {"Said slowly.", "Never said."};
        reply = local.synthesize(request);
        QTRY_COMPARE(engine->size(), 1);
        reply->abort();
        QVERIFY(reply->isFinished());
        QCOMPARE(reply->error().kind, Ai::Error::Cancelled);
        QTest::qWait(600);
        QCOMPARE(engine->size(), 1);

        // A voice that isn't here says so.
        request.voice = "builtin:piper:de_DE-thorsten-high";
        reply = local.synthesize(request);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->error().kind, Ai::Error::Unsupported);

        // Its pace here is kept.
        local.noteSpeed(packs.model("builtin:kokoro:bm_george")->key, 3.0);
        QCOMPARE(local.speedOf("builtin:kokoro:bm_george"), 3.0);
    }

    // --- Local models ------------------------------------------------------

    // Each server gets the request it understands: Ollama its own API with
    // room for a chapter and no thinking aloud; llama.cpp the OpenAI one with
    // a strict schema; MLX, which has no structured output, the schema in
    // its instructions.
    void localChatBodies() {
        Ai::TextRequest request;
        request.system = "Direct the narration.";
        request.messages = {{true, "Chapter one."}};
        request.schema = QJsonObject{{"type", "object"}};
        request.fast = true;
        const QJsonObject ollama = LocalModels::chatBody(LocalModels::Ollama, request, "qwen3.5:4b");
        QCOMPARE(ollama["model"].toString(), QString("qwen3.5:4b"));
        QCOMPARE(ollama["think"].toBool(true), false);
        QCOMPARE(ollama["format"].toObject()["type"].toString(), QString("object"));
        QCOMPARE(ollama["options"].toObject()["num_ctx"].toInt(), 8192);
        QCOMPARE(ollama["messages"].toArray().first().toObject()["role"].toString(), QString("system"));
        const QJsonObject llama = LocalModels::chatBody(LocalModels::LlamaCpp, request, "m");
        QCOMPARE(llama["response_format"].toObject()["type"].toString(), QString("json_schema"));
        QVERIFY(llama["response_format"].toObject()["json_schema"].toObject()["strict"].toBool());
        QCOMPARE(llama["chat_template_kwargs"].toObject()["enable_thinking"].toBool(true), false);
        const QJsonObject mlx = LocalModels::chatBody(LocalModels::Mlx, request, "m");
        QVERIFY(!mlx.contains("response_format"));
        QVERIFY(mlx["messages"].toArray().first().toObject()["content"].toString().contains("JSON schema"));
    }

    void localAnswersAreCleaned() {
        QCOMPARE(LocalModels::cleanText("<think>hmm, moods</think>\n\nCalm.", false), QString("Calm."));
        QCOMPARE(LocalModels::cleanText("Here:\n```json\n{\"a\": 1}\n```", true), QString("{\"a\": 1}"));
        QCOMPARE(LocalModels::cleanText("<think>still thinking", false), QString());
        QVERIFY(LocalModels::queryText("qwen3-embedding:0.6b", "who is Weena").endsWith("Query: who is Weena"));
        QCOMPARE(LocalModels::queryText("bge-m3", "who"), QString("who"));
    }

    // A streamed answer from an OpenAI-compatible server, and one from
    // Ollama, both arrive as text, and JSON ones as a result.
    void localModelsStream() {
        FakeServer server;
        LocalModels local;
        LocalModels::Server where;
        where.kind = LocalModels::LlamaCpp;
        where.url = QUrl(server.origin());
        local.configure(where, "qwen", "embed");
        server.responses.append({200, "text/event-stream",
                                 "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"mood\\\": \"}}]}\n\n"
                                 "data: {\"choices\":[{\"delta\":{\"content\":\"\\\"calm\\\"}\"}}]}\n\n"
                                 "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":9}}\n\n"
                                 "data: [DONE]\n\n"});
        Ai::TextRequest request;
        request.stream = true;
        request.schema = QJsonObject{{"type", "object"}};
        request.messages = {{true, "?"}};
        Ai::Reply *reply = local.generate(request);
        QSignalSpy finished(reply, &Ai::Reply::finished);
        QVERIFY(finished.wait(3000));
        QVERIFY2(!reply->error(), qPrintable(reply->error().message));
        QCOMPARE(reply->result()["mood"].toString(), QString("calm"));
        QCOMPARE(reply->usage().inputTokens, 120);
        QCOMPARE(server.requests.first().path, QByteArray("/v1/chat/completions"));

        where.kind = LocalModels::Ollama;
        local.configure(where, "qwen3.5:4b", "qwen3-embedding:0.6b");
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"<think>x</think>The \"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"Morlocks.\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"\"},\"done\":true,\"prompt_eval_count\":50,\"eval_count\":4}\n"});
        Ai::TextRequest plain;
        plain.stream = true;
        plain.messages = {{true, "Who?"}};
        reply = local.generate(plain);
        QSignalSpy text(reply, &Ai::Reply::textReceived);
        QSignalSpy done(reply, &Ai::Reply::finished);
        QVERIFY(done.wait(3000));
        QCOMPARE(reply->text(), QString("The Morlocks."));
        QVERIFY(!text.isEmpty());
        for (const auto &delta : text)
            QVERIFY(!delta.first().toString().contains("think"));
        QCOMPARE(reply->usage().outputTokens, 4);
        QCOMPARE(server.requests.last().path, QByteArray("/api/chat"));

        // Embeddings, in the order asked, with the query marked.
        server.responses.append({200, "application/json", "{\"embeddings\":[[0.1,0.2],[0.3,0.4]]}"});
        reply = local.embed({"a", "b"}, true);
        QSignalSpy embedded(reply, &Ai::Reply::finished);
        QVERIFY(embedded.wait(3000));
        QCOMPARE(reply->vectors().size(), 2);
        QCOMPARE(reply->vectors()[1][0], 0.3f);
        QVERIFY(QJsonDocument::fromJson(server.requests.last().body).object()["input"].toArray()[0].toString().contains("Query: a"));
        where.kind = LocalModels::LmStudio;
        local.configure(where, "m", "e");
        server.responses.append({200, "application/json",
                                 "{\"data\":[{\"index\":1,\"embedding\":[2]},{\"index\":0,\"embedding\":[1]}]}"});
        reply = local.embed({"a", "b"}, false);
        QSignalSpy ordered(reply, &Ai::Reply::finished);
        QVERIFY(ordered.wait(3000));
        QCOMPARE(reply->vectors()[0][0], 1.0f);

        // A server that has stopped says so.
        where.url = QUrl("http://127.0.0.1:9");
        local.configure(where, "m");
        reply = local.generate(plain);
        QSignalSpy refused(reply, &Ai::Reply::finished);
        QVERIFY(refused.wait(5000));
        QCOMPARE(reply->error().kind, Ai::Error::Offline);
    }

    // A Studio voice is the local server's, and its audio arrives at the
    // player's rate whatever the server speaks at.
    void studioVoicesSpeak() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);
        QVERIFY(!services.speechFor("studio:af_heart"));
        FakeServer server;
        LocalModels::Server where;
        where.kind = LocalModels::Generic;
        where.url = QUrl(server.origin());
        local.configure(where, {}, {}, "kokoro");
        QVERIFY(services.speechFor("studio:af_heart") == local.speech());
        QCOMPARE(services.providerFor("studio:af_heart")->id(), QString("local"));
        QVERIFY(AiServices::isOnThisComputer("studio:af_heart"));
        QVERIFY(!local.supportsStyle());
        QVERIFY(LocalModels::takesInstructions("Qwen3-TTS-12Hz-1.7B-CustomVoice"));
        // A second of silence at 22.05 kHz.
        QByteArray wav = "RIFF";
        const int samples = 22050;
        auto le32 = [](int v) { return QByteArray::fromRawData(reinterpret_cast<const char *>(&v), 4); };
        auto le16 = [](short v) { return QByteArray::fromRawData(reinterpret_cast<const char *>(&v), 2); };
        wav += le32(36 + samples * 2) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(22050) + le32(44100) +
               le16(2) + le16(16) + "data" + le32(samples * 2) + QByteArray(samples * 2, '\0');
        server.responses.append({200, "audio/wav", wav});
        Ai::SpeechRequest request;
        request.text = "Hello.";
        request.voice = "studio:af_heart";
        Ai::Reply *reply = services.speechFor(request.voice)->synthesize(request);
        QSignalSpy finished(reply, &Ai::Reply::finished);
        QVERIFY(finished.wait(3000));
        QVERIFY2(!reply->error(), qPrintable(reply->error().message));
        QCOMPARE(reply->sampleRate(), 24000);
        QVERIFY(qAbs(reply->audio().size() / 2 - 24000) < 50);
        const QJsonObject body = QJsonDocument::fromJson(server.requests.first().body).object();
        QCOMPARE(server.requests.first().path, QByteArray("/v1/audio/speech"));
        QCOMPARE(body["voice"].toString(), QString("af_heart"));
        QCOMPARE(body["model"].toString(), QString("kokoro"));
    }

    // Servers are told apart by how they answer, not by their port.
    void serversAreFound() {
        FakeServer ollama;
        ollama.routes.insert("/api/tags", {200, "application/json",
                                           "{\"models\":[{\"name\":\"qwen3.5:4b\"},{\"name\":\"qwen3-embedding:0.6b\"}]}"});
        FakeServer llama;
        llama.routes.insert("/props", {200, "application/json", "{\"default_generation_settings\":{},\"model_path\":\"x.gguf\"}"});
        llama.routes.insert("/v1/models", {200, "application/json", "{\"data\":[{\"id\":\"gemma-4-e4b\",\"owned_by\":\"llamacpp\"}]}"});
        FakeServer studio;
        studio.routes.insert("/api/v0/models", {200, "application/json",
                                                "{\"data\":[{\"id\":\"qwen3.5-9b\",\"type\":\"llm\"},{\"id\":\"nomic-embed\",\"type\":\"embeddings\"}]}"});
        ServerFinder finder;
        QSignalSpy finished(&finder, &ServerFinder::finished);
        finder.find({QUrl(ollama.origin()), QUrl(llama.origin()), QUrl(studio.origin() + "/v1")});
        QVERIFY(finished.wait(8000));
        QMap<LocalModels::Kind, LocalModels::Server> found;
        for (const auto &server : finder.servers())
            if (server.url.port() == QUrl(ollama.origin()).port() || server.url.port() == QUrl(llama.origin()).port() ||
                server.url.port() == QUrl(studio.origin()).port())
                found.insert(server.kind, server);
        QCOMPARE(found.size(), 3);
        QCOMPARE(found[LocalModels::Ollama].textModels, QStringList{"qwen3.5:4b"});
        QCOMPARE(found[LocalModels::Ollama].embeddingModels, QStringList{"qwen3-embedding:0.6b"});
        QCOMPARE(found[LocalModels::LlamaCpp].textModels, QStringList{"gemma-4-e4b"});
        QCOMPARE(found[LocalModels::LmStudio].embeddingModels, QStringList{"nomic-embed"});
    }

    // Hybrid sends quick work to this computer and careful work to Gemini;
    // Local never sends anything away; Cloud falls back to local without a key.
    void featuresAreRouted() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);
        const QString before = services.preset();
        QVERIFY(!services.textFor("director"));
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl("http://127.0.0.1:11434");
        local.configure(where, "qwen3.5:4b");
        services.setPreset("cloud");
        QCOMPARE(services.routeFor("explain"), QString("local"));  // no key: local stands in
        gemini.setKey("k");
        QCOMPARE(services.routeFor("explain"), QString("cloud"));
        services.setPreset("hybrid");
        QCOMPARE(services.routeFor("director"), QString("local"));
        QCOMPARE(services.routeFor("ask"), QString("cloud"));
        QVERIFY(services.whereFor("director").contains("Ollama"));
        services.setOverride("ask", "local");
        QCOMPARE(services.routeFor("ask"), QString("local"));
        services.setOverride("ask", "");
        services.setPreset("local");
        QCOMPARE(services.routeFor("ask"), QString("local"));
        local.configure({}, {});
        QCOMPARE(services.routeFor("ask"), QString());  // never away
        QVERIFY(!services.textFor("director"));
        services.setPreset(before);
        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini", QSettings::IniFormat).remove("ai/preset");
    }

    // What a computer has, from its /proc and /sys.
    void computerIsInspected() {
        QTemporaryDir root;
        QDir(root.path()).mkpath("proc");
        QDir(root.path()).mkpath("sys/class/drm/card1/device");
        auto write = [&](const QString &path, const QByteArray &text) {
            QFile file(root.filePath(path));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(text);
        };
        write("proc/meminfo", "MemTotal:       32617612 kB\nMemAvailable:   20000000 kB\n");
        write("proc/cpuinfo", "processor\t: 0\nmodel name\t: AMD Ryzen 7 8840U\nflags\t\t: sse2 avx2 avx_vnni\n\n"
                              "processor\t: 1\nmodel name\t: AMD Ryzen 7 8840U\nflags\t\t: sse2 avx2 avx_vnni\n");
        write("sys/class/drm/card1/device/vendor", "0x1002\n");
        write("sys/class/drm/card1/device/mem_info_vram_total", "536870912\n");
        const SystemCheck::Machine machine = SystemCheck::inspect(root.path());
        QCOMPARE(qRound(machine.memoryGb), 31);
        QCOMPARE(machine.cores, 2);
        QCOMPARE(machine.architecture, QString("x86_64"));
        QCOMPARE(machine.features, (QStringList{"AVX2", "AVX-VNNI"}));
        QCOMPARE(machine.graphics, QString("AMD Radeon"));
        QVERIFY(!machine.discreteGraphics);  // a 780M's carve-out isn't VRAM
        QCOMPARE(SystemCheck::tierFor(machine), QString("Local Standard"));
        QCOMPARE(SystemCheck::suggestedModel(machine), QString("Gemma 4 26B-A4B"));
        QCOMPARE(SystemCheck::billionsIn("qwen3.5:4b"), 4.0);
        QCOMPARE(SystemCheck::billionsIn("Qwen3.5-9B-Instruct-Q4_K_M"), 9.0);
        QCOMPARE(SystemCheck::billionsIn("gemma4:e4b"), 4.0);
        QCOMPARE(SystemCheck::billionsIn("llama"), 0.0);
        SystemCheck::Machine small;
        small.memoryGb = 8;
        QCOMPARE(SystemCheck::tierFor(small), QString("Local voices"));
    }

    // --- The companion -------------------------------------------------------

    // Nothing past the reader's place: the text stops at the offset given.
    void companionReadsOnlySoFar() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        const QList<Companion::Chapter> all = Companion::textUpTo(path, "epub", 1, -1);
        QCOMPARE(all.size(), 2);
        const QList<Companion::Chapter> partway = Companion::textUpTo(path, "epub", 0, 5);
        QCOMPARE(partway.size(), 1);
        QCOMPARE(partway.first().text, all.first().text.left(5));
        // The latest is kept when there's too much.
        const QString joined = Companion::joined({{0, "One", QString(100, 'a')}, {1, "Two", QString(100, 'b')}}, 120);
        QVERIFY(joined.contains("## Two"));
        QVERIFY(joined.endsWith(QString(100, 'b')));
        QVERIFY(!joined.contains(QString(30, 'a')));
    }

    void passagesAreFound() {
        const QString text = "The Eloi were small and gentle.\n\nThey ate fruit.\n\n" + QString(1200, 'x') +
                             "\nThe Morlocks lived underground, in the dark, and came up at night.\n";
        const QList<PassageIndex::Passage> passages = PassageIndex::split(3, text, 200);
        QVERIFY(passages.size() >= 3);
        QCOMPARE(passages.first().chapter, 3);
        QCOMPARE(passages.first().start, 0);
        for (const auto &passage : passages)
            QVERIFY(text.mid(passage.start).startsWith(passage.text.section('\n', 0, 0)));
        const auto hits = PassageIndex::byWords(passages, "Where did the Morlocks live?", 2);
        QVERIFY(!hits.isEmpty());
        QVERIFY(hits.first().passage.text.contains("Morlocks"));
        QCOMPARE(PassageIndex::cosine({1, 0}, {1, 0}), 1.0);
        QCOMPARE(PassageIndex::cosine({1, 0}, {0, 1}), 0.0);
    }

    // Passages embedded once and kept; a question finds the closest by meaning.
    void passagesAreFoundByMeaning() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);
        FakeServer server;
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(server.origin());
        local.configure(where, "qwen3.5:4b", "qwen3-embedding:0.6b");
        services.setPreset("local");
        PassageIndex index(&services, library.database().connectionName());
        const QList<PassageIndex::Passage> passages{{0, 0, 10, "About the Eloi."}, {0, 11, 20, "About the Morlocks."}};
        server.responses.append({200, "application/json", "{\"embeddings\":[[1,0],[0,1]]}"});
        QString problem = "unset";
        index.build(7, passages, nullptr, [&](const QString &error) { problem = error; });
        QTRY_COMPARE(problem, QString());
        QCOMPARE(index.indexed(7), 2);
        // Built again, nothing is asked: it's all kept.
        problem = "unset";
        index.build(7, passages, nullptr, [&](const QString &error) { problem = error; });
        QTRY_COMPARE(problem, QString());
        QCOMPARE(server.requests.size(), 1);
        server.responses.append({200, "application/json", "{\"embeddings\":[[0.1,0.9]]}"});
        QList<PassageIndex::Hit> found;
        bool byMeaning = false;
        index.search(7, passages, "underground people", 1, [&](const QList<PassageIndex::Hit> &hits, bool meaning) {
            found = hits;
            byMeaning = meaning;
        });
        QTRY_COMPARE(found.size(), 1);
        QVERIFY(byMeaning);
        QCOMPARE(found.first().passage.text, QString("About the Morlocks."));
        services.setPreset("cloud");
        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini", QSettings::IniFormat).remove("ai/preset");
    }

    void geminiEmbeddingRequests() {
        const QJsonObject body = GeminiProvider::embedBody({"a", "b"}, true, "gemini-embedding-2");
        const QJsonArray requests = body["requests"].toArray();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(requests[0].toObject()["model"].toString(), QString("models/gemini-embedding-2"));
        QCOMPARE(requests[0].toObject()["taskType"].toString(), QString("RETRIEVAL_QUERY"));
        QCOMPARE(requests[1].toObject()["content"].toObject()["parts"].toArray()[0].toObject()["text"].toString(), QString("b"));
    }

    // --- PDFs ------------------------------------------------------------------

    // A page's paragraphs to read aloud: the page number and running head
    // left out, lines joined, a word broken across lines mended, and every
    // offset still the page's.
    void pdfPagesAreSpoken() {
        const QString page = QString("12\r\nTHE TIME MACHINE\r\nThe Time Traveller was expounding a recon") + QChar(0xFFFE) +
                             QString("\r\ndite matter to us, and the bright fire burned.\r\n"
                                     "His grey eyes shone and twinkled, and his face\r\n"
                                     "was flushed. Short end.\r\n\r\n"
                                     "A second paragraph begins here and goes on a while.\r\n");
        const QJsonArray blocks = PdfBook::speakable(page, "The Time Machine");
        QCOMPARE(blocks.size(), 2);
        const QJsonObject first = blocks[0].toObject();
        const qint64 start = first["start"].toInteger(), end = first["end"].toInteger();
        QVERIFY(page.mid(start).startsWith("The Time Traveller"));
        QCOMPARE(first["text"].toString().size(), end - start);
        QCOMPARE(Speech::speakable(first["text"].toString()).simplified(),
                 QString("The Time Traveller was expounding a recondite matter to us, and the bright fire burned. "
                         "His grey eyes shone and twinkled, and his face was flushed. Short end."));
        QVERIFY(blocks[1].toObject()["text"].toString().startsWith("A second paragraph"));
    }

    // Text, where it is, and what's between two points, on a real PDF page.
    void pdfTextIsPlaced() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.pdf");
        {
            QPdfWriter writer(path);
            writer.setPageSize(QPageSize(QPageSize::A5));
            writer.setResolution(72);
            QPainter painter(&writer);
            painter.setFont(QFont("DejaVu Serif", 14));
            painter.drawText(QPointF(40, 80), "Weena was a little creature.");
            painter.drawText(QPointF(40, 120), "The Morlocks came at night.");
        }
        PdfBook book(path);
        QVERIFY(book.open());
        const QString text = book.pageText(0);
        QVERIFY2(text.contains("Morlocks"), qPrintable(text));
        const qint64 at = text.indexOf("Morlocks");
        const QList<QRectF> rects = book.textRects(0, at, at + 8);
        QVERIFY(!rects.isEmpty());
        QVERIFY(rects.first().y() > 0.15 && rects.first().y() < 0.25);  // the second line, near the top
        QVERIFY(rects.first().x() > 0.05);
        QRectF word;
        for (const QRectF &rect : rects)
            word = word.isNull() ? rect : word.united(rect);
        const PdfBook::Selection chosen = book.select(0, QPointF(word.x() + 0.002, word.center().y()),
                                                      QPointF(word.right() - 0.002, word.center().y()));
        QVERIFY2(chosen.text.contains("Morlocks"), qPrintable(chosen.text));
        QVERIFY(chosen.start >= 0);
        // Cropping finds the text, not the empty page around it.
        const QRectF box = PdfPages::contentBox(book.renderPage(0, QSize(300, 425)));
        QVERIFY(box.x() > 0.05 && box.width() < 0.95 && box.height() < 0.3);
    }

    void pdfPagesTakeTheTheme() {
        QImage page(4, 1, QImage::Format_RGB32);
        page.setPixel(0, 0, qRgb(255, 255, 255));  // paper
        page.setPixel(1, 0, qRgb(0, 0, 0));        // ink
        page.setPixel(2, 0, qRgb(200, 30, 30));    // a red picture
        page.setPixel(3, 0, qRgb(128, 128, 128));
        PdfPages::recolor(page, QColor("#1e1f24"), QColor("#d6d2c9"));
        QCOMPARE(QColor(page.pixel(0, 0)).name(), QString("#1e1f24"));
        QCOMPARE(QColor(page.pixel(1, 0)).name(), QString("#d6d2c9"));
        QVERIFY(QColor(page.pixel(2, 0)).red() > QColor(page.pixel(2, 0)).green() * 2);  // still red
        QImage white(1, 1, QImage::Format_RGB32);
        white.fill(Qt::white);
        PdfPages::recolor(white, Qt::white, Qt::black);
        QCOMPARE(QColor(white.pixel(0, 0)), QColor(Qt::white));
    }

    // Who speaks: paragraphs out of range, the narrator's, and unknown
    // genders are told apart.
    void castLinesAreRead() {
        const QString json = R"({"lines":[{"block":1,"speaker":"Weena","gender":"female"},)"
                             R"({"block":3,"speaker":"narrator","gender":""},)"
                             R"({"block":4,"speaker":"The Time Traveller","gender":"man"},)"
                             R"({"block":9,"speaker":"Filby","gender":"male"}]})";
        const QMap<int, Cast::Line> lines = Cast::parse(json, 5);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[1].speaker, QString("Weena"));
        QCOMPARE(lines[1].gender, QString("female"));
        QCOMPARE(lines[4].gender, QString());
        QVERIFY(Cast::prompt().contains("narrator"));
    }

    // An audiobook's chapters and tags, escaped as ffmpeg reads them.
    void audiobookMetadata() {
        const QString meta = Audiobook::metadata("Love; and Mr Lewisham", "H. G. Wells",
                                                 {{"I. Introduces Mr Lewisham", 0, 61000}, {"A=B #2", 61000, 90500}});
        QVERIFY(meta.startsWith(";FFMETADATA1\n"));
        QVERIFY2(meta.contains("title=Love\\; and Mr Lewisham\n"), qPrintable(meta));
        QVERIFY(meta.contains("artist=H. G. Wells\n"));
        QVERIFY(meta.contains("[CHAPTER]\nTIMEBASE=1/1000\nSTART=61000\nEND=90500\ntitle=A\\=B \\#2\n"));
        QCOMPARE(Audiobook::concatList({"/a/it's.opus"}), QString("ffconcat version 1.0\nfile '/a/it'\\''s.opus'\n"));
    }

    // The window doesn't wait for the keyring: the key arrives when it
    // answers, and one typed in meanwhile outranks it.
    void keyArrivesWhenTheKeyringAnswers() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        SlowKeyStore keys;
        AiServices services(&gemini, nullptr, library.database().connectionName());
        services.lookUpKeyIn(&keys);
        QVERIFY(!services.hasKey());
        QCOMPARE(services.status(), QString("Looking in your keyring…"));
        QSignalSpy changed(&services, &AiServices::changed);
        keys.answer.addResult(QString("from-keyring"));
        keys.answer.finish();
        QVERIFY(changed.wait(2000));
        QVERIFY(services.hasKey());
        QCOMPARE(services.status(), QString("Ready"));

        GeminiProvider other;
        SlowKeyStore late;
        AiServices typed(&other, nullptr, library.database().connectionName());
        typed.lookUpKeyIn(&late);
        typed.forgetKey();
        late.answer.addResult(QString("stale"));
        late.answer.finish();
        QTest::qWait(50);
        QVERIFY(!typed.hasKey());
    }

    void voicesAreSpokenByTheirOwnProvider() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        LocalSpeech local(&packs);
        services.setLocalSpeech(&local);
        // No key: cloud voices can't speak; built-in ones can, once here.
        QVERIFY(!services.canSpeakWith("sulafat"));
        QVERIFY(!services.canSpeakWith("builtin:kokoro:af_heart"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        QVERIFY(services.canSpeakWith("builtin:kokoro:af_heart"));
        QCOMPARE(services.providerFor("builtin:kokoro:af_heart")->id(), QString("builtin"));
        QCOMPARE(services.providerFor("sulafat")->id(), QString("gemini"));
        keys.save("gemini", "k");
        gemini.setKey("k");
        QVERIFY(services.canSpeakWith("sulafat"));
        QVERIFY(services.speechFor("sulafat") != services.speechFor("builtin:kokoro:af_heart"));
    }

    // --- Search --------------------------------------------------------

    void searchIgnoresCaseAccentsAndQuotes() {
        const QString text = QString::fromUtf8("The naïve Café — don’t “Look” Ǖ");
        const QString folded = BookSearch::fold(text);
        QCOMPARE(folded.size(), text.size());
        QCOMPARE(folded, QString("the naive cafe - don't \"look\" u"));
        QCOMPARE(BookSearch::matches(folded, BookSearch::fold("NAIVE"), 10).value(0).first, 4);
        QCOMPARE(BookSearch::matches(folded, BookSearch::fold("don't"), 10).size(), 1);
        // Every occurrence, not overlapping, up to the limit.
        QCOMPARE(BookSearch::matches("aaaa", "aa", 10).size(), 2);
        QCOMPARE(BookSearch::matches("ab ab ab", "ab", 2).size(), 2);
    }

    void searchShowsAHitInContext() {
        const QString text = "It was a dark and stormy night; the rain fell in torrents, except at occasional intervals, "
                             "when it was checked by a violent gust of wind which swept up the streets.";
        const qint64 start = text.indexOf("rain");
        const QVariantMap hit = BookSearch::hit(text, 3, start, start + 4);
        QCOMPARE(hit.value("chapter").toInt(), 3);
        QCOMPARE(hit.value("match").toString(), QString("rain"));
        QVERIFY(hit.value("before").toString().endsWith("the "));
        QVERIFY(hit.value("after").toString().startsWith(" fell"));
        QVERIFY(hit.value("after").toString().endsWith(QChar(0x2026)));
        // Cut at words, not inside them.
        QVERIFY(!hit.value("after").toString().contains("swep…"));
    }

    void searchFindsWordsAcrossTheBook() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int id = library.addBookFile(path);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(id));
        BookSearch search(&reader);

        search.find("STORM");
        QVERIFY(search.searching());
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);
        const QVariantMap storm = search.results().value(0).toMap();
        QCOMPARE(storm.value("chapter").toInt(), 0);
        QCOMPARE(storm.value("match").toString(), QString("storm"));
        QCOMPARE(search.inChapter(0).size(), 1);
        QCOMPARE(search.inChapter(1).size(), 0);

        // "the lamp's" finds "The lamp’s", and the notes chapter is searched too.
        search.find("the lamp's");
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);
        search.find("note");
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);
        QCOMPARE(search.results().value(0).toMap().value("chapter").toInt(), 1);

        // Stepping wraps around.
        search.find("a");
        QCOMPARE(search.count(), 0);  // too short to search
        search.find("in");
        QTRY_VERIFY(!search.searching());
        QVERIFY(search.count() >= 2);
        search.setCurrent(-1);
        QCOMPARE(search.current(), search.count() - 1);
        search.setCurrent(search.count());
        QCOMPARE(search.current(), 0);

        // Another book clears it.
        reader.close();
        QCOMPARE(search.count(), 0);
        QVERIFY(search.query().isEmpty());
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

    // --- One Dogear at a time ----------------------------------------------

    // Opened again, Dogear hands the book to the one running, which takes it
    // (after answering, so the second can leave at once); a second can't
    // take the name.
    void instanceHandsOver() {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("No session bus");
        Instance running;
        QVERIFY(running.claim());
        Instance second;
        QVERIFY(!second.claim());
        QSignalSpy activated(&running, &Instance::activated);
        QVERIFY(Instance::passToRunning(QStringLiteral("/books/The Time Machine.epub")));
        QVERIFY(activated.wait(2000));
        QCOMPARE(activated.first().first().toString(), QStringLiteral("/books/The Time Machine.epub"));
        // Without a window there's nothing to show, and nothing fails.
        running.bringBack();
    }
};

QTEST_MAIN(DogearTests)
#include "dogear_tests.moc"
