#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QProcess>
#include <QPromise>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <cmath>
#include <limits>
#include <QtTest>

#include "ai/aiservices.h"
#include "formats/coverprovider.h"
#include "mcp/mcpserver.h"
#include "online/epubwriter.h"
#include "online/opdsclient.h"
#include "online/webimporter.h"
#include "audio/audiocache.h"
#include "ai/aiusage.h"
#include "formats/bookinfo.h"
#include "app/cli.h"
#include "app/trayicon.h"
#include "fakeserver.h"
#include "ai/gemini.h"
#include "ai/localmodels.h"
#include "ai/companion.h"
#include "ai/illustrator.h"
#include "ai/passageindex.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "audio/cast.h"
#include "audio/director.h"
#include "audio/download.h"
#include "audio/narration.h"
#include "audio/audiobook.h"
#include <QPdfWriter>
#include <QPainter>
#include "app/systemcheck.h"
#include "ai/localsetup.h"
#include "app/instance.h"
#include "app/migration.h"
#include "ai/keystore.h"
#include "audio/localspeech.h"
#include "app/notifier.h"
#include "app/pointerdriver.h"
#include "app/portalfilepicker.h"
#include "audio/voicepacks.h"
#include "ai/sseparser.h"
#include "app/surfacewatcher.h"
#include "formats/bookname.h"
#include "formats/bookscheme.h"
#include "library/booksearch.h"
#include "library/library.h"
#include "library/libraryfinder.h"
#include "library/libraryindex.h"
#include "library/marks.h"
#include "reader/reader.h"
#include "library/scanner.h"
#include "formats/epubbook.h"
#include "formats/htmltokenizer.h"
#include "formats/pdfbook.h"
#include "audio/speech.h"
#include "audio/speechplayer.h"
#include "testbooks.h"
#include "app/theme.h"
#include "app/translator.h"
#include "audio/voicecatalog.h"
#include "audio/mpris.h"
#include "formats/ziparchive.h"

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

class ClothboundTests : public QObject {
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
            qputenv("CLOTHBOUND_MUTE", "1");
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
        QFile marker(root + "/" + dir + "/.clothbound-installed");
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

    class SilentEngine : public TtsEngine {
    public:
        QString prepare(const VoicePacks::Model &) override { return {}; }
        int sampleRate() const override { return 24000; }
        QVector<float> speak(const QString &, int, const std::atomic_bool &) override { return {}; }
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
        QCOMPARE(theme.background(), QString("#121214"));
        // Without Omarchy the tokens are the design's neutrals.
        QCOMPARE(theme.ground(), QString("#121214"));
        QCOMPARE(theme.sidebar(), QString("#18181b"));
        QCOMPARE(theme.ink1(), QString("#f2f2f3"));
        QCOMPARE(theme.ink3(), QString("#a1a1aa"));
    }

    void themeTokensKeepTextReadableInEveryOmarchyTheme() {
        // Every theme Omarchy ships, plus a deliberately washed-out one: body
        // text must reach WCAG AA on the ground, headings 7:1, hints 3:1, and
        // the accent as ink 4.5:1.
        QList<QHash<QString, QString>> palettes;
        const QDir themes(QStringLiteral("/usr/share/omarchy/themes"));
        for (const QString &name : themes.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const auto palette = Theme::readPalette(themes.filePath(name + "/colors.toml"));
            if (palette.contains("background") && palette.contains("foreground"))
                palettes.append(palette);
        }
        palettes.append({{"mode", "dark"}, {"background", "#303030"}, {"foreground", "#5a5a5a"}, {"accent", "#3a3a6a"}});
        palettes.append({{"mode", "light"}, {"background", "#f0f0f0"}, {"foreground", "#b0b0b0"}, {"accent", "#ffe066"}});
        for (const auto &palette : palettes) {
            const bool light = palette.value("mode") == "light";
            const auto t = Theme::deriveTokens(palette, light, true);
            const QString what = palette.value("background") + " / " + palette.value("foreground");
            QVERIFY2(Theme::contrast(t["ink1"], t["ground"]) >= 7.0 - 1e-6, qPrintable(what));
            QVERIFY2(Theme::contrast(t["ink2"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
            QVERIFY2(Theme::contrast(t["ink3"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
            QVERIFY2(Theme::contrast(t["ink4"], t["ground"]) >= 3.0 - 1e-6, qPrintable(what));
            QVERIFY2(Theme::contrast(t["accentInk"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
            QVERIFY2(Theme::contrast(t["positive"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
            QVERIFY2(Theme::contrast(t["cloud"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
            QCOMPARE(t["ground"], palette.value("background"));
        }
        QVERIFY(palettes.size() >= 2);
    }

    void themeAppearanceCanOverrideTheDesktop() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "mode = \"dark\"\naccent = \"#7aa2f7\"\n"
                                                 "background = \"#1a1b26\"\nforeground = \"#a9b1d6\"\n");
        Theme theme(dir.path());
        QVERIFY(!theme.light());
        QCOMPARE(theme.ground(), QString("#1a1b26"));
        QSignalSpy changed(&theme, &Theme::changed);
        theme.setAppearance("light");
        QVERIFY(changed.count() >= 1);
        QVERIFY(theme.light());
        // Forced light sets the dark theme's palette aside but keeps its accent,
        // darkened where it is used as ink on the light ground.
        QCOMPARE(theme.ground(), QString("#fbfbfc"));
        QCOMPARE(theme.accent(), QString("#7aa2f7"));
        QVERIFY(Theme::contrast(theme.accentInk(), theme.ground()) >= 4.5);
        theme.setAppearance("bogus");
        QCOMPARE(theme.appearance(), QString("match"));
        QCOMPARE(theme.ground(), QString("#1a1b26"));
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "mode = \"light\"\naccent = \"#112233\"\n"
                                                 "background = \"#fffcf0\"\nforeground = \"#100f0f\"\n"
                                                 "lighter_background = \"#e6e4d9\"\n"
                                                 "bad_quotes = \"#ff0000'\n"
                                                 "[terminal]\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QVERIFY(theme.omarchy());
        QVERIFY(theme.light());
        // Keys inside [terminal] must not clobber top-level palette keys.
        QCOMPARE(theme.background(), QString("#fffcf0"));
        QCOMPARE(theme.text(), QString("#100f0f"));
        QCOMPARE(theme.surface(), QString("#e6e4d9"));
        // Partial light themes fall back to the light palette (high contrast on light bg).
        QCOMPARE(theme.selection(), Theme::fallbackLightPalette().value("selection"));
        QCOMPARE(theme.textSecondary(), Theme::fallbackLightPalette().value("light_foreground"));
        QCOMPARE(Theme::readableOn(theme.textSecondary()), QString("white"));
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
        int rounding = 0;
        QLocalServer requestServer, eventServer;
        QVERIFY(requestServer.listen(dir.filePath(".socket.sock")));
        QVERIFY(eventServer.listen(dir.filePath(".socket2.sock")));

        QObject::connect(&requestServer, &QLocalServer::newConnection, &requestServer, [&] {
            while (QLocalSocket *client = requestServer.nextPendingConnection()) {
                QObject::connect(client, &QLocalSocket::readyRead, client, [client, &rounding] {
                    const QByteArray cmd = client->readAll();
                    if (cmd.contains("decoration:rounding"))
                        client->write(QStringLiteral("{\"int\":%1}").arg(rounding).toUtf8());
                    client->disconnectFromServer();
                });
                QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            }
        });

        QPointer<QLocalSocket> eventClient;
        QObject::connect(&eventServer, &QLocalServer::newConnection, &eventServer, [&] {
            eventClient = eventServer.nextPendingConnection();
        });

        Theme theme(dir.path(), nullptr, dir.path());
        QTRY_VERIFY(eventClient != nullptr);
        QCOMPARE(theme.corners(), QString("match"));
        QCOMPARE(theme.radius(), 0);

        rounding = 6;
        eventClient->write("configreloaded>>\n");
        eventClient->flush();
        QTRY_COMPARE(theme.radius(), 6);

        // Explicit rounded/sharp overrides win over Hyprland's rounding, and
        // returning to "match" (or any unknown value) resumes live desktop rounding.
        QSignalSpy cornerSpy(&theme, &Theme::changed);
        theme.setCorners(QStringLiteral("sharp"));
        QCOMPARE(theme.corners(), QString("sharp"));
        QCOMPARE(theme.radius(), 0);
        QCOMPARE(cornerSpy.count(), 1);

        theme.setCorners(QStringLiteral("rounded"));
        QCOMPARE(theme.corners(), QString("rounded"));
        QCOMPARE(theme.radius(), 22);
        QCOMPARE(cornerSpy.count(), 2);

        theme.setCorners(QStringLiteral("bogus"));
        QCOMPARE(theme.corners(), QString("match"));
        QCOMPARE(theme.radius(), 6);
        QCOMPARE(cornerSpy.count(), 3);
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
        write(dir.path() + "/language", "pt_BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());

        // BCP-47 script tags are accepted too.
        write(dir.path() + "/language", "zh-Hans\n");
        QCOMPARE(Translator::readLanguage(dir.path() + "/language"), QString("zh-Hans"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n books": {"one": "%n book", "other": "%n books"}})");
        write(dir.path() + "/ru.json", R"({"%n books": {"one": "%n книга", "few": "%n книги", "many": "%n книг", "other": "%n книг"}, "%n pages": {"one": "%n страница", "many": "%n страниц"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 21), QString("%n книга"));
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 22), QString("%n книги"));
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 25), QString("%n книг"));
        // Missing "other" falls back to "many" rather than blank text.
        QCOMPARE(translator.translate("Main", "%n pages", nullptr, -1), QString("%n страниц"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
        QCOMPARE(Translator::pluralCategory("es", 1000000), QString("many"));
        // Unset OMARCHY_PATH never breaks formatLocale.
        const QByteArray prevOmarchy = qgetenv("OMARCHY_PATH");
        qunsetenv("OMARCHY_PATH");
        QCOMPARE(Translator::formatLocale("fr").language(), QLocale::French);
        if (!prevOmarchy.isEmpty())
            qputenv("OMARCHY_PATH", prevOmarchy);
    }

    void shippedCatalogsAreValidJson() {
        const QDir sources(QFINDTESTDATA("../src/i18n"));
        const QStringList files = sources.entryList({"*.json"});
        QCOMPARE(files.size(), 50);

        // Read English plural catalog first: must include Format.js plurals too.
        QFile enFile(sources.filePath("en.json"));
        QVERIFY(enFile.open(QIODevice::ReadOnly));
        QJsonParseError enError;
        const QJsonObject enObj = QJsonDocument::fromJson(enFile.readAll(), &enError).object();
        QVERIFY2(enError.error == QJsonParseError::NoError, qPrintable(enError.errorString()));
        QVERIFY(enObj.contains("%n min"));
        QVERIFY(enObj.contains("%n h"));
        QVERIFY(enObj.contains("%n books"));

        static const QRegularExpression phRe(QStringLiteral("%L?[0-9n]"));
        static const QRegularExpression tagRe(QStringLiteral("</?([a-zA-Z0-9]+)(?:\\s[^>]*)?>"));
        auto normalizedPh = [](const QString &s) {
            QStringList out;
            auto it = phRe.globalMatch(s);
            while (it.hasNext()) {
                QString p = it.next().captured();
                out.append((p == "%Ln") ? QStringLiteral("%n") : p);
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        auto htmlTags = [](const QString &s) {
            QStringList out;
            auto it = tagRe.globalMatch(s);
            while (it.hasNext())
                out.append(it.next().captured(1));
            std::sort(out.begin(), out.end());
            return out;
        };

        const QHash<QString, QStringList> requiredForms{
            {"ja", {"other"}}, {"ko", {"other"}}, {"zh-CN", {"other"}}, {"zh-TW", {"other"}},
            {"th", {"other"}}, {"vi", {"other"}}, {"id", {"other"}}, {"ms", {"other"}},
            {"fr", {"one", "many", "other"}}, {"pt-BR", {"one", "many", "other"}},
            {"ca", {"one", "many", "other"}}, {"es-MX", {"one", "many", "other"}},
            {"it", {"one", "many", "other"}}, {"pt", {"one", "many", "other"}},
            {"cs", {"one", "few", "other"}}, {"sk", {"one", "few", "other"}},
            {"pl", {"one", "few", "many", "other"}}, {"ru", {"one", "few", "many", "other"}},
            {"uk", {"one", "few", "many", "other"}}, {"hr", {"one", "few", "other"}},
            {"sr", {"one", "few", "other"}}, {"sl", {"one", "two", "few", "other"}},
            {"lt", {"one", "few", "other"}}, {"lv", {"zero", "one", "other"}},
            {"ro", {"one", "few", "other"}}, {"ar", {"zero", "one", "two", "few", "many", "other"}},
            {"he", {"one", "two", "other"}}, {"ga", {"one", "two", "few", "many", "other"}},
        };
        const QHash<QString, QSet<QString>> exactNumberForms{
            {"ar", {"zero", "one", "two"}}, {"he", {"one", "two"}}, {"ro", {"one"}}, {"ga", {"one", "two"}},
        };

        for (const QString &name : files) {
            const QString code = QFileInfo(name).baseName();
            QFile file(sources.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + ": " + error.errorString()));
            QVERIFY2(doc.isObject(), qPrintable(name));
            const QJsonObject obj = doc.object();
            if (code == "en")
                continue;
            const QStringList forms = requiredForms.value(code, {"one", "other"});
            for (auto it = obj.cbegin(); it != obj.cend(); ++it) {
                const QString key = it.key();
                const bool counted = enObj.contains(key) || key.contains("%n") || key.contains("%Ln");
                const QStringList expPh = normalizedPh(key);
                const QStringList expTags = htmlTags(key);
                if (counted) {
                    QVERIFY2(it.value().isObject(), qPrintable(code + ": counted key is not object: " + key));
                    const QJsonObject dict = it.value().toObject();
                    QVERIFY2(dict.contains("other"), qPrintable(code + ": missing 'other' for " + key));
                    for (const QString &form : forms) {
                        const QString val = dict.value(form).toString();
                        QVERIFY2(!val.trimmed().isEmpty(), qPrintable(code + ": empty form " + form + " for " + key));
                        QCOMPARE(htmlTags(val), expTags);
                        const QStringList gotPh = normalizedPh(val);
                        if (gotPh != expPh) {
                            const bool okExact = exactNumberForms.value(code).contains(form);
                            QStringList expNoN = expPh, gotNoN = gotPh;
                            expNoN.removeAll("%n");
                            QVERIFY2(okExact && !gotNoN.contains("%n") && gotNoN == expNoN,
                                     qPrintable(code + " [" + form + "] placeholder mismatch for " + key + ": " + val));
                        }
                    }
                } else {
                    const QString val = it.value().toString();
                    QVERIFY2(!val.trimmed().isEmpty(), qPrintable(code + ": empty value for " + key));
                    QCOMPARE(normalizedPh(val), expPh);
                    QCOMPARE(htmlTags(val), expTags);
                }
            }
        }
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
        QVERIFY(reader.manifest().value("base").toString().startsWith("clothbound://local/book/"));
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
        QCOMPARE(library.record(id).position, QString(R"({"chapter":1,"offset":3,"snippet":"note","title":"Notes"})"));
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
            if (!made.open(QIODevice::ReadOnly))
                return QByteArray();
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
        auto le32 = [](int v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
        auto le16 = [](short v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
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
        QCOMPARE(SystemCheck::suggestedModel(machine), QString("Qwen3.5 9B"));
        QCOMPARE(LocalSetup::modelsFor(31, 0).first, QString("qwen3.5:9b"));
        QCOMPARE(LocalSetup::modelsFor(15.5, 0).first, QString("qwen3.5:4b"));
        QCOMPARE(LocalSetup::modelsFor(8, 0).first, QString("qwen3.5:2b"));
        QVERIFY(LocalSetup::modelsFor(4, 0).first.isEmpty());
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
            const int fontId = QFontDatabase::addApplicationFont(QFINDTESTDATA("../src/fonts/Literata.ttf"));
            const QString family = fontId >= 0 ? QFontDatabase::applicationFontFamilies(fontId).value(0) : QString();
            painter.setFont(family.isEmpty() ? QFont(QStringLiteral("serif"), 14) : QFont(family, 14));
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

        // Fused single-pass compositeAndRecolor handles transparent PDFium paper,
        // semi-transparent premultiplied ink, and chromatic illustrations identically.
        QImage raw(4, 1, QImage::Format_ARGB32_Premultiplied);
        raw.setPixel(0, 0, qRgba(0, 0, 0, 0));           // transparent paper -> bg
        raw.setPixel(1, 0, qRgba(0, 0, 0, 255));         // opaque black ink -> fg
        raw.setPixel(2, 0, qRgba(200, 30, 30, 255));     // chromatic red -> stays red
        raw.setPixel(3, 0, qRgba(0, 0, 0, 128));         // 50% premul black -> ~mid gray
        PdfPages::compositeAndRecolor(raw, QColor("#1e1f24"), QColor("#d6d2c9"));
        QCOMPARE(raw.format(), QImage::Format_RGB32);
        QCOMPARE(QColor(raw.pixel(0, 0)).name(), QString("#1e1f24"));
        QCOMPARE(QColor(raw.pixel(1, 0)).name(), QString("#d6d2c9"));
        QVERIFY(QColor(raw.pixel(2, 0)).red() > QColor(raw.pixel(2, 0)).green() * 2);
        QVERIFY(QColor(raw.pixel(3, 0)).red() > 0x40 && QColor(raw.pixel(3, 0)).red() < 0xB0);

        // White/black identity composites transparent paper onto pure white.
        QImage rawIdentity(2, 1, QImage::Format_ARGB32_Premultiplied);
        rawIdentity.setPixel(0, 0, qRgba(0, 0, 0, 0));
        rawIdentity.setPixel(1, 0, qRgba(0, 0, 0, 255));
        PdfPages::compositeAndRecolor(rawIdentity, Qt::white, Qt::black);
        QCOMPARE(QColor(rawIdentity.pixel(0, 0)), QColor(Qt::white));
        QCOMPARE(QColor(rawIdentity.pixel(1, 0)), QColor(Qt::black));
    }

    void coverProviderBakesCornersAndSpineCrease() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
        QImage src(200, 300, QImage::Format_RGB32);
        src.fill(QColor(210, 180, 120));
        QVERIFY(src.save(coverPath, "PNG"));

        const QImage rounded = CoverProvider::renderCoverImage(coverPath, QSize(256, 256), 5.0, 3.0, 160.0);
        QVERIFY(!rounded.isNull());
        QCOMPARE(rounded.format(), QImage::Format_ARGB32_Premultiplied);
        QVERIFY(rounded.width() <= 256 && rounded.height() <= 256);
        // Outer fore-edge corner is transparent; interior center is opaque cover art.
        QCOMPARE(qAlpha(rounded.pixel(rounded.width() - 1, 0)), 0);
        QCOMPARE(qAlpha(rounded.pixel(rounded.width() / 2, rounded.height() / 2)), 255);
        // Left spine crease darkens x=1 relative to interior center.
        QVERIFY(qRed(rounded.pixel(1, rounded.height() / 2)) < qRed(rounded.pixel(rounded.width() / 2, rounded.height() / 2)));

        // Sharp mode (radius = 0) stays opaque Format_RGB32 with intact corners.
        const QImage sharp = CoverProvider::renderCoverImage(coverPath, QSize(256, 256), 0.0, 0.0, 160.0);
        QVERIFY(!sharp.isNull());
        QCOMPARE(sharp.format(), QImage::Format_RGB32);
        QCOMPARE(qAlpha(sharp.pixel(sharp.width() - 1, 0)), 255);

        // Runtime regression: CoverImage.qml loading a non-2:3 local cover via
        // image://cover must reach loaded=true with ZERO binding-loop warnings.
        const QString widePath = dir.filePath(QStringLiteral("wide_cover.png"));
        QImage wideSrc(240, 260, QImage::Format_RGB32);
        wideSrc.fill(QColor(110, 150, 200));
        QVERIFY(wideSrc.save(widePath, "PNG"));

        QQmlEngine engine;
        engine.addImageProvider(QStringLiteral("cover"), new CoverProvider);
        Theme themeObj;
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &themeObj);
        engine.rootContext()->setContextProperty(QStringLiteral("hasCoverProvider"), true);
        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &errs) { warnings.append(errs); });
        const QString srcRoot = QFINDTESTDATA("../src");
        QQmlComponent comp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CoverImage.qml")));
        QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
        std::unique_ptr<QObject> obj(comp.createWithInitialProperties({
            {QStringLiteral("width"), 160.0},
            {QStringLiteral("height"), 240.0},
            {QStringLiteral("source"), QUrl::fromLocalFile(widePath).toString()},
            {QStringLiteral("title"), QStringLiteral("Wide Cover")},
            {QStringLiteral("author"), QStringLiteral("Author")},
        }));
        QVERIFY2(obj != nullptr, qPrintable(comp.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(obj->property("loaded").toBool(), true, 3000);
        QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
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

    // --- One Clothbound at a time ----------------------------------------------

    // Opened again, Clothbound hands the book to the one running, which takes it
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

    void instanceFocusesHyprlandOverSocketWithoutMutatingEnv() {
        QTemporaryDir dir;
        const QString sockPath = dir.filePath(".socket.sock");
        QLocalServer server;
        QVERIFY(server.listen(sockPath));
        QStringList commands;
        QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
            while (QLocalSocket *client = server.nextPendingConnection()) {
                QObject::connect(client, &QLocalSocket::readyRead, client, [client, &commands] {
                    const QByteArray cmd = client->readAll();
                    commands.append(QString::fromUtf8(cmd));
                    if (cmd == "j/clients") {
                        const QByteArray json = QStringLiteral("[{\"pid\":%1,\"address\":\"0xdeadbeef\"}]")
                                                    .arg(QCoreApplication::applicationPid())
                                                    .toUtf8();
                        client->write(json);
                    } else if (cmd.contains("hl.dsp.focus")) {
                        client->write("unknown request");
                    } else {
                        client->write("ok");
                    }
                    client->disconnectFromServer();
                });
                QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            }
        });

        qunsetenv("XDG_ACTIVATION_TOKEN");
        QWindow win;
        win.show();
        Instance inst;
        inst.setWindow(&win);
        inst.setHyprSocketForTesting(sockPath);
        inst.bringBack(QStringLiteral("test-activation-token"));
        // bringBack() must never mutate environ after threads may be running.
        QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
        QTRY_COMPARE(commands.size(), 3);
        QCOMPARE(commands[0], QString("j/clients"));
        QVERIFY(commands[1].contains("address:0xdeadbeef"));
        QCOMPARE(commands[2], QString("dispatch focuswindow address:0xdeadbeef"));
    }

    void notifierTracksTokensPerIdAndCleansClosed() {
        Notifier notifier;
        QSignalSpy clicked(&notifier, &Notifier::clicked);
        // Simulate clickable IDs via public slots (no live notification daemon required).
        notifier.onActivationToken(10, QStringLiteral("ignored-unknown"));
        notifier.onActionInvoked(10, QStringLiteral("default"));
        QCOMPARE(clicked.count(), 0);
        notifier.onNotificationClosed(10, 2);
        QCOMPARE(notifier.pendingCount(), 0);
    }

    void pointerDriverSendsPublicEvents() {
        QQuickWindow win;
        win.resize(400, 300);
        PointerDriver driver(&win);
        driver.press(10, 10);
        driver.move(20, 20);
        driver.release(20, 20);
        driver.wheel(20, 20, -120, 0);
        driver.key(Qt::Key_Space, QStringLiteral(" "));
        driver.drag(10, 10, 30, 30, 0);  // steps <= 0 clamped safely
        QTest::qWait(80);
        QVERIFY(true);
    }

    void cliCommandsHandleEdgeCases() {
        QVERIFY(isCliCommand("add"));
        QVERIFY(isCliCommand("ai"));
        QVERIFY(!isCliCommand("open"));

        QTemporaryDir dir;
        const QString epubPath = dir.filePath("Marsh, Ada - Lantern Keeper, The (Harbor, 1911).epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

        // Single-pass info succeeds on a valid EPUB.
        QCOMPARE(runCli({"clothbound", "info", "--json", epubPath}), 0);
        QCOMPARE(runCli({"clothbound", "help", "open"}), 0);

        // Add book once, then re-add: idempotent, no crash/hang.
        QCOMPARE(runCli({"clothbound", "add", "--json", epubPath}), 0);
        QCOMPARE(runCli({"clothbound", "add", "--json", epubPath}), 0);

        // Add multiple folders in one call: batches into one scan, never hangs.
        const QString dir1 = dir.filePath("d1"), dir2 = dir.filePath("d2");
        QDir().mkpath(dir1);
        QDir().mkpath(dir2);
        QCOMPARE(runCli({"clothbound", "add", "--json", dir1, dir2}), 0);

        // notes: must not collide with a same-named directory in CWD, and 0 marks exits 0.
        const QString prevCwd = QDir::currentPath();
        QDir().mkpath(dir.filePath("lantern"));
        QVERIFY(QDir::setCurrent(dir.path()));
        const int notesPlain = runCli({"clothbound", "notes", "lantern"});
        const int notesJson = runCli({"clothbound", "notes", "--json", "lantern"});
        QVERIFY(QDir::setCurrent(prevCwd));
        QCOMPARE(notesPlain, 0);
        QCOMPARE(notesJson, 0);

        // ai local runs without opening SQLite or libsecret keyring.
        QCOMPARE(runCli({"clothbound", "ai", "local", "--json"}), 0);
    }

    // --- Post-merge regression tests: Input / PDF -------------------------

    void pdfPagesConcurrentCancelAndRapidBookSwitch() {
        QTemporaryDir dir;
        const QString pathA = dir.filePath("a.pdf");
        const QString pathB = dir.filePath("b.pdf");
        QVERIFY(TestBooks::write(pathA, TestBooks::pdf("Book A", "Author A")));
        QVERIFY(TestBooks::write(pathB, TestBooks::pdf("Book B", "Author B")));
        auto bookA = std::make_shared<PdfBook>(pathA);
        auto bookB = std::make_shared<PdfBook>(pathB);
        QVERIFY(bookA->open());
        QVERIFY(bookB->open());

        const QString tokA = QStringLiteral("reg-tok-a");
        const QString tokB = QStringLiteral("reg-tok-b");
        PdfPages::add(tokA, bookA);
        PdfPages::add(tokB, bookB);

        // Keep responses alive until after PdfPages::~PdfPages() drains its worker pool.
        std::vector<std::unique_ptr<QQuickImageResponse>> responses;
        {
            PdfPages pages;
            int done = 0;
            auto track = [&](QQuickImageResponse *resp) {
                QObject::connect(resp, &QQuickImageResponse::finished, this, [&done] { ++done; });
                responses.emplace_back(resp);
                return resp;
            };

            // Fire a burst for Book A, cancel several mid-flight, then immediately
            // drop Book A and switch to Book B.
            for (int i = 0; i < 6; ++i) {
                auto *r = track(pages.requestImageResponse(
                    QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tokA), QSize(180 + i * 10, 270 + i * 15)));
                if (i % 2 == 1)
                    r->cancel();
            }
            PdfPages::remove(tokA);

            auto *respB = track(pages.requestImageResponse(
                QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tokB), QSize(200, 300)));

            QTRY_COMPARE_WITH_TIMEOUT(done, int(responses.size()), 10000);
            std::unique_ptr<QQuickTextureFactory> factory(respB->textureFactory());
            QVERIFY(factory != nullptr);
            const QImage rendered = factory->image();
            QCOMPARE(rendered.size(), QSize(200, 300));
            QCOMPARE(QColor(rendered.pixel(2, 2)).name(), QString("#1e1f24"));

            // Second request with identical key hits the LRU cache cleanly.
            auto *cachedB = track(pages.requestImageResponse(
                QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tokB), QSize(200, 300)));
            QTRY_COMPARE_WITH_TIMEOUT(done, int(responses.size()), 5000);
            std::unique_ptr<QQuickTextureFactory> cachedFactory(cachedB->textureFactory());
            QVERIFY(cachedFactory != nullptr);
            QCOMPARE(cachedFactory->image().size(), QSize(200, 300));

            PdfPages::remove(tokB);
        }
    }

    void pdfBookOversizedAndExtremeAspectClamped() {
        auto pdfWithBox = [](int w, int h) {
            QByteArray out = "%PDF-1.4\n";
            QList<qsizetype> offsets;
            auto obj = [&](const QByteArray &body) {
                offsets.append(out.size());
                out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
            };
            obj("<< /Type /Catalog /Pages 2 0 R >>");
            obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
            obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + QByteArray::number(w) + " " + QByteArray::number(h) +
                "] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
            const QByteArray stream = "BT /F1 12 Tf 20 20 Td (Clamp test) Tj ET";
            obj("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
            obj("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
            const qsizetype xref = out.size();
            out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
            for (const qsizetype offset : offsets)
                out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
            out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
            out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
            return out;
        };

        QTemporaryDir dir;
        const QString normalPath = dir.filePath("normal.pdf");
        const QString extremePath = dir.filePath("extreme.pdf");
        QVERIFY(TestBooks::write(normalPath, pdfWithBox(200, 300)));
        QVERIFY(TestBooks::write(extremePath, pdfWithBox(40000, 10)));

        PdfBook normal(normalPath);
        QVERIFY(normal.open());
        // Single-dimension clamp (<= 6000 px).
        const QImage wide = normal.renderPage(0, QSize(12000, 100));
        QVERIFY(!wide.isNull());
        QCOMPARE(wide.width(), 6000);
        QCOMPARE(wide.height(), 100);
        // Total pixel budget clamp (<= 24 MP).
        const QImage huge = normal.renderPage(0, QSize(5500, 5500));
        QVERIFY(!huge.isNull());
        QVERIFY(huge.width() <= 6000 && huge.height() <= 6000);
        QVERIFY(qint64(huge.width()) * qint64(huge.height()) <= 24LL * 1000 * 1000);

        // Extreme aspect-ratio PDF (4000:1): cover and crop stay strictly bounded.
        PdfBook extreme(extremePath);
        QVERIFY(extreme.open());
        const QImage cover = extreme.renderCover(5000);
        QVERIFY(!cover.isNull());
        QVERIFY(cover.height() <= 2048);
        QVERIFY(cover.width() <= 2048);
        QVERIFY(double(cover.width()) / double(cover.height()) <= 5.05);
        const QRectF crop = PdfPages::bookCrop(extreme);
        QVERIFY(crop.x() >= 0.0 && crop.y() >= 0.0 && crop.right() <= 1.001 && crop.bottom() <= 1.001);

        // PdfBook::speakable preserves chapter headings and handles mixed \r\n / \n.
        const QString mixed = QStringLiteral("THE LANTERN\r\nCHAPTER ONE\nThe sea was calm.\r\n\nA gull cried.\n");
        const QJsonArray blocks = PdfBook::speakable(mixed, QStringLiteral("The Lantern"));
        QVERIFY(blocks.size() >= 2);
        QVERIFY(blocks[0].toObject().value("text").toString().contains(QStringLiteral("CHAPTER ONE")));
        for (const QJsonValue &v : blocks) {
            const QJsonObject o = v.toObject();
            const qint64 s = o.value("start").toInteger(), e = o.value("end").toInteger();
            QCOMPARE(e - s, o.value("text").toString().size());
        }
    }

    void readerPdfSpeakableIsAsyncAndDropsStaleSession() {
        QTemporaryDir dir;
        const QString pathA = dir.filePath("a.pdf");
        const QString pathB = dir.filePath("b.pdf");
        QVERIFY(TestBooks::write(pathA, TestBooks::pdf("Pdf A", "Author A")));
        QVERIFY(TestBooks::write(pathB, TestBooks::pdf("Pdf B", "Author B")));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int idA = library.addBookFile(pathA);
        const int idB = library.addBookFile(pathB);
        QVERIFY(idA > 0 && idB > 0);

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(idA));
        QVERIFY(reader.hasPageList());
        QCOMPARE(reader.targetForPageLabel("1").value("chapter").toInt(), 0);
        const qint64 sessionA = reader.manifest().value("session").toLongLong();

        QSignalSpy spoke(&reader, &Reader::speakable);
        reader.pdfSpeakable(0);
        // Must be queued asynchronously, never emitted synchronously inside pdfSpeakable().
        QCOMPARE(spoke.count(), 0);
        // Switch books before the queued callback executes: stale session must be dropped.
        QVERIFY(reader.openBook(idB));
        QCoreApplication::processEvents();
        QTest::qWait(30);
        QCOMPARE(spoke.count(), 0);

        // Stale session token from JS surface is also rejected; current session passes.
        const qint64 sessionB = reader.manifest().value("session").toLongLong();
        QVERIFY(sessionB > sessionA);
        reader.speakableReadyFor(sessionA, 0, QStringLiteral("[]"));
        QCOMPARE(spoke.count(), 0);

        reader.pdfSpeakable(0);
        QCOMPARE(spoke.count(), 0);
        QTRY_COMPARE(spoke.count(), 1);
        QCOMPARE(spoke[0][0].toInt(), 0);
        QVERIFY(spoke[0][1].toString().contains(QStringLiteral("Hello reader")));
        reader.close();
    }

    // --- Post-merge regression tests: Parsers / Storage -------------------

    void htmlDeepNestingParsesAndDestroysIteratively() {
        QString deep;
        deep.reserve(1200 * 11 + 20);
        for (int i = 0; i < 1200; ++i)
            deep += QStringLiteral("<div>");
        deep += QStringLiteral("deep leaf");
        for (int i = 0; i < 1200; ++i)
            deep += QStringLiteral("</div>");
        {
            const auto doc = Html::parse(deep, true);
            QVERIFY(doc != nullptr);
            QVERIFY(doc->textContent().contains(QStringLiteral("deep leaf")));
        }

        // Also stress iterative textContent() and ~HtmlNode() on a 2500-deep tree
        // built directly (beyond Html::parse's 512 depth cap).
        auto root = std::make_unique<HtmlNode>();
        root->name = QStringLiteral("div");
        HtmlNode *cur = root.get();
        for (int i = 0; i < 2500; ++i) {
            auto child = std::make_unique<HtmlNode>();
            child->name = QStringLiteral("span");
            child->parent = cur;
            HtmlNode *next = child.get();
            cur->children.push_back(std::move(child));
            cur = next;
        }
        auto leaf = std::make_unique<HtmlNode>();
        leaf->text = QStringLiteral("bottom");
        leaf->parent = cur;
        cur->children.push_back(std::move(leaf));
        QCOMPARE(root->textContent(), QStringLiteral("bottom"));
        root.reset();
    }

    void zipRejectsBombsBadCrcAndZip64Wraparound() {
        QTemporaryDir dir;

        // 1. Lying-header ZIP bomb: claims 100 uncompressed bytes, actually inflates > 128 MiB.
        QByteArray bombDeflated;
        {
            const QByteArray oneMb(1024 * 1024, '\0');
            z_stream zs{};
            QCOMPARE(deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
            QByteArray buf(64 * 1024, Qt::Uninitialized);
            for (int mb = 0; mb < 129; ++mb) {
                zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(oneMb.constData()));
                zs.avail_in = uInt(oneMb.size());
                const int flush = (mb == 128) ? Z_FINISH : Z_NO_FLUSH;
                while (zs.avail_in > 0 || flush == Z_FINISH) {
                    zs.next_out = reinterpret_cast<Bytef *>(buf.data());
                    zs.avail_out = uInt(buf.size());
                    const int rc = deflate(&zs, flush);
                    bombDeflated.append(buf.constData(), qsizetype(buf.size()) - qsizetype(zs.avail_out));
                    if (rc == Z_STREAM_END)
                        break;
                    QVERIFY(rc == Z_OK);
                }
            }
            deflateEnd(&zs);
        }
        auto makeCustomZip = [](const QByteArray &name, const QByteArray &payload, quint16 method, quint32 crc,
                                quint32 declaredUncompressed) {
            QByteArray out, cd;
            TestBooks::put32(out, 0x04034b50);
            TestBooks::put16(out, 20);
            TestBooks::put16(out, 0x0800);
            TestBooks::put16(out, method);
            TestBooks::put32(out, 0);
            TestBooks::put32(out, crc);
            TestBooks::put32(out, quint32(payload.size()));
            TestBooks::put32(out, declaredUncompressed);
            TestBooks::put16(out, quint16(name.size()));
            TestBooks::put16(out, 0);
            out.append(name);
            out.append(payload);

            const quint32 cdOff = quint32(out.size());
            TestBooks::put32(cd, 0x02014b50);
            TestBooks::put16(cd, 20);
            TestBooks::put16(cd, 20);
            TestBooks::put16(cd, 0x0800);
            TestBooks::put16(cd, method);
            TestBooks::put32(cd, 0);
            TestBooks::put32(cd, crc);
            TestBooks::put32(cd, quint32(payload.size()));
            TestBooks::put32(cd, declaredUncompressed);
            TestBooks::put16(cd, quint16(name.size()));
            TestBooks::put16(cd, 0);
            TestBooks::put16(cd, 0);
            TestBooks::put16(cd, 0);
            TestBooks::put16(cd, 0);
            TestBooks::put32(cd, 0);
            TestBooks::put32(cd, 0);
            cd.append(name);
            out.append(cd);

            TestBooks::put32(out, 0x06054b50);
            TestBooks::put16(out, 0);
            TestBooks::put16(out, 0);
            TestBooks::put16(out, 1);
            TestBooks::put16(out, 1);
            TestBooks::put32(out, quint32(cd.size()));
            TestBooks::put32(out, cdOff);
            TestBooks::put16(out, 0);
            return out;
        };

        const QString bombPath = dir.filePath("bomb.zip");
        QVERIFY(TestBooks::write(bombPath, makeCustomZip("bomb.txt", bombDeflated, 8, 0, 100)));
        ZipArchive bombZip(bombPath);
        QVERIFY(bombZip.open());
        QString err;
        QVERIFY(bombZip.read("bomb.txt", &err).isEmpty());
        QVERIFY2(err.contains("too large"), qPrintable(err));

        // 2. Declared > 128 MiB rejected immediately before inflating.
        const QString hugePath = dir.filePath("huge.zip");
        QVERIFY(TestBooks::write(hugePath, makeCustomZip("huge.txt", TestBooks::rawDeflate("hi"), 8, 0, 200u * 1024 * 1024)));
        ZipArchive hugeZip(hugePath);
        QVERIFY(hugeZip.open());
        err.clear();
        QVERIFY(hugeZip.read("huge.txt", &err).isEmpty());
        QVERIFY2(err.contains("too large"), qPrintable(err));

        // 3. Bad CRC-32 rejected on both stored and deflated entries.
        const QString crcPath = dir.filePath("badcrc.zip");
        QVERIFY(TestBooks::write(crcPath, makeCustomZip("corrupt.txt", TestBooks::rawDeflate("hello"), 8, 0xdeadbeef, 5)));
        ZipArchive crcZip(crcPath);
        QVERIFY(crcZip.open());
        err.clear();
        QVERIFY(crcZip.read("corrupt.txt", &err).isEmpty());
        QVERIFY2(err.contains("damaged"), qPrintable(err));

        // 4. ZIP64 directoryOffset + directorySize uint64 wraparound rejected.
        QByteArray z64 = TestBooks::zip({{"one.txt", "first", true}}, true);
        const QByteArray sig("\x50\x4b\x06\x06", 4);
        const qsizetype pos = z64.indexOf(sig);
        QVERIFY(pos > 0 && pos + 56 <= z64.size());
        auto patch64 = [&](qsizetype at, quint64 val) {
            for (int b = 0; b < 8; ++b)
                z64[at + b] = char((val >> (b * 8)) & 0xff);
        };
        patch64(pos + 40, 0x40ULL);                  // directorySize
        patch64(pos + 48, 0xffffffffffffffe0ULL);    // directoryOffset (wraps to 0x20 when added!)
        const QString wrapPath = dir.filePath("wrap64.zip");
        QVERIFY(TestBooks::write(wrapPath, z64));
        ZipArchive wrapZip(wrapPath);
        QVERIFY(!wrapZip.open());
        QVERIFY2(wrapZip.errorString().contains("damaged"), qPrintable(wrapZip.errorString()));
    }

    void bookSchemeEnforcesSecurityHeadersRootRelativeAndXhtmlEntities() {
        // XHTML DTD resolves entities and still validates the ENTIRE document after &nbsp;.
        const QByteArray xhtmlGood =
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>a&nbsp;&mdash;b</p></body></html>";
        const QByteArray xhtmlBrokenAfterEntity =
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>a&nbsp;<b>unclosed</p></body></html>";
        const QByteArray html5WithEntity = "<!DOCTYPE html><html><body><p>a&nbsp;b</p></body></html>";
        QVERIFY(BookScheme::isWellFormed(xhtmlGood));
        QVERIFY(!BookScheme::isWellFormed(xhtmlBrokenAfterEntity));
        QVERIFY(!BookScheme::isWellFormed(html5WithEntity));

        QTemporaryDir dir;
        const QByteArray cover = png(16, 16);
        const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>";
        const QString path = dir.filePath("sec.epub");
        const QByteArray secOpf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\"><metadata/>\n"
            "<manifest>\n"
            "  <item id=\"ok\" href=\"ok.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  <item id=\"bad\" href=\"bad.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  <item id=\"h5\" href=\"h5.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  <item id=\"img\" href=\"images/cover.png\" media-type=\"image/png\"/>\n"
            "  <item id=\"svg\" href=\"images/pic.svg\" media-type=\"image/svg+xml\"/>\n"
            "</manifest>\n"
            "<spine><itemref idref=\"ok\"/><itemref idref=\"bad\"/><itemref idref=\"h5\"/></spine>\n"
            "</package>";
        const QByteArray secBytes = TestBooks::epub({
            {"OEBPS/content.opf", secOpf},
            {"OEBPS/ok.xhtml", xhtmlGood},
            {"OEBPS/bad.xhtml", xhtmlBrokenAfterEntity},
            {"OEBPS/h5.xhtml", html5WithEntity},
            {"OEBPS/images/cover.png", cover},
            {"OEBPS/images/pic.svg", svg},
        });
        QVERIFY(TestBooks::write(path, secBytes));
        auto book = std::make_shared<EpubBook>(path);
        QVERIFY(book->open());

        BookScheme scheme;
        const QString token = scheme.add(book);
        const QByteArray ref = QStringLiteral("clothbound://local/book/%1/OEBPS/ok.xhtml").arg(token).toUtf8();

        auto checkSecurity = [](const BookScheme::ResolvedResource &res) {
            QVERIFY(res.ok);
            QCOMPARE(res.headers.value("X-Content-Type-Options"), QByteArray("nosniff"));
            const QByteArray csp = res.headers.value("Content-Security-Policy");
            QVERIFY(csp.contains("script-src 'none'"));
            QVERIFY(csp.contains("object-src 'none'"));
        };

        const auto okRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/ok.xhtml").arg(token)));
        checkSecurity(okRes);
        QCOMPARE(okRes.mimeType, QByteArray("application/xhtml+xml"));

        const auto badRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/bad.xhtml").arg(token)));
        checkSecurity(badRes);
        QCOMPARE(badRes.mimeType, QByteArray("text/html"));

        const auto h5Res = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/h5.xhtml").arg(token)));
        checkSecurity(h5Res);
        QCOMPARE(h5Res.mimeType, QByteArray("text/html"));

        // SVG/binary resources also carry strict CSP + nosniff.
        const auto svgRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/images/pic.svg").arg(token)));
        checkSecurity(svgRes);
        QCOMPARE(svgRes.mimeType, QByteArray("image/svg+xml"));

        // Root-relative /path resolves safely via valid clothbound://local/book/<token>/ Referer.
        const auto rootRel = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/OEBPS/images/cover.png")), ref);
        checkSecurity(rootRel);
        QCOMPARE(rootRel.data, cover);

        // Missing, external, or traversal root-relative requests are refused.
        QVERIFY(!scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/OEBPS/images/cover.png"))).ok);
        QVERIFY(!scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/OEBPS/images/cover.png")),
                                       "https://evil.example/book/" + token.toUtf8() + "/ok.xhtml")
                     .ok);
        QVERIFY(!scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/../../etc/passwd")), ref).ok);
        const auto wrongHost = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://evil/book/%1/OEBPS/ok.xhtml").arg(token)));
        QVERIFY(!wrongHost.ok && wrongHost.invalidUrl);
    }

    void libraryRelinksMovedFilesByFingerprintAndRollsBackFailedTransactions() {
        QTemporaryDir dir;
        const QString shelf = dir.filePath("shelf");
        QVERIFY(QDir().mkpath(shelf));
        const QString origPath = QDir(shelf).filePath("original.epub");
        QVERIFY(TestBooks::write(origPath, TestBooks::epub2(png(20, 30))));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        library.addFolder(shelf);
        QTRY_VERIFY(!library.scanning());

        const int id = library.idForPath(origPath);
        QVERIFY(id > 0);
        library.savePosition(id, R"({"chapter":0,"offset":42})", 0.65);
        library.setBookSetting(id, QStringLiteral("voice"), QStringLiteral("sulafat"));

        Marks marks(&library);
        marks.setBook(id);
        const int hl = marks.addHighlight(0, 4, 14, QStringLiteral("lamp light"), QStringLiteral("blue"), QStringLiteral("Ch 1"));
        marks.setNote(hl, QStringLiteral("Keep across move"));
        const int bm = marks.addBookmark(0, 2, QStringLiteral("Chapter One"), QStringLiteral("Ch 1"));
        QVERIFY(hl > 0 && bm > 0);

        // Move and rename the book inside the watched folder, then rescan.
        QVERIFY(QDir(shelf).mkpath("nested"));
        const QString movedPath = QDir(shelf).filePath("nested/renamed.epub");
        QVERIFY(QFile::rename(origPath, movedPath));
        library.rescan();
        QTRY_VERIFY(!library.scanning());

        // Same book_id relinked via SHA-256 content fingerprint; reading state and marks intact.
        QCOMPARE(library.idForPath(movedPath), id);
        QCOMPARE(library.idForPath(origPath), 0);
        const BookRecord relinked = library.record(id);
        QVERIFY(!relinked.missing);
        QCOMPARE(relinked.progress, 0.65);
        QCOMPARE(relinked.position, QString(R"({"chapter":0,"offset":42})"));
        QCOMPARE(library.bookSetting(id, QStringLiteral("voice")), QStringLiteral("sulafat"));

        Marks reloaded(&library);
        reloaded.setBook(id);
        QCOMPARE(reloaded.count(), 2);
        QCOMPARE(reloaded.mark(hl).value("note").toString(), QStringLiteral("Keep across move"));

        // Checked SAVEPOINT rollback on multi-statement write failure (addReadingTime).
        QSqlDatabase db = library.database();
        QSqlQuery trigger(db);
        QVERIFY(trigger.exec(
            QStringLiteral("CREATE TEMP TRIGGER fail_reading_days BEFORE INSERT ON reading_days "
                           "BEGIN SELECT RAISE(ABORT, 'injected failure'); END")));
        library.addReadingTime(id, 180, 1200);
        QCOMPARE(library.record(id).secondsRead, qint64(0));
        {
            QSqlQuery check(db);
            QVERIFY(check.exec(QStringLiteral("SELECT seconds_read FROM books WHERE id = %1").arg(id)));
            QVERIFY(check.next());
            QCOMPARE(check.value(0).toLongLong(), qint64(0));
        }
        QVERIFY(trigger.exec(QStringLiteral("DROP TRIGGER fail_reading_days")));
        library.addReadingTime(id, 180, 1200);
        QCOMPARE(library.record(id).secondsRead, qint64(180));

        // Checked SAVEPOINT rollback on setChapterLengths(): earlier DELETE is rolled back.
        const QList<int> beforeLengths = library.chapterLengths(id);
        QVERIFY(!beforeLengths.isEmpty());
        QVERIFY(trigger.exec(
            QStringLiteral("CREATE TEMP TRIGGER fail_chapter_lengths BEFORE INSERT ON chapter_lengths "
                           "BEGIN SELECT RAISE(ABORT, 'injected failure'); END")));
        library.setChapterLengths(id, {999, 888});
        QCOMPARE(library.chapterLengths(id), beforeLengths);
        QVERIFY(trigger.exec(QStringLiteral("DROP TRIGGER fail_chapter_lengths")));
    }

    void marksMarkdownUsesValidUtf8BookmarkIconAndGuardsClosedDb() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        const int id = library.addBookFile(path);
        QVERIFY(id > 0);

        Marks marks(&library);
        // No book selected -> rejected cleanly.
        QCOMPARE(marks.addBookmark(0, 0, QStringLiteral("x"), QStringLiteral("Ch 1")), 0);
        QCOMPARE(marks.addHighlight(0, 0, 5, QStringLiteral("hello"), QStringLiteral("yellow"), QStringLiteral("Ch 1")), 0);

        marks.setBook(id);
        const int bm = marks.addBookmark(0, 1, QStringLiteral("Opening line"), QStringLiteral("Chapter One"));
        const int hl = marks.addHighlight(0, 10, 20, QStringLiteral("Glowing lamp"), QStringLiteral("green"), QStringLiteral("Chapter One"));
        marks.setNote(hl, QStringLiteral("Important motif"));
        Mark multi;
        multi.kind = QStringLiteral("highlight");
        multi.chapter = 0;
        multi.start = 30;
        multi.end = 50;
        multi.text = QStringLiteral("First line\nSecond line");
        multi.color = QStringLiteral("yellow");
        multi.label = QStringLiteral("Chapter One");
        const int mId = marks.insert(multi);
        QVERIFY(bm > 0 && hl > 0 && mId > 0);

        const QString md = marks.markdown(QStringLiteral("The Lantern"), QStringLiteral("Ada Marsh"));
        // Valid 4-byte UTF-8 U+1F516 bookmark emoji (not truncated BMP U+F516).
        QVERIFY2(md.contains(QStringLiteral("- \U0001F516 Opening line")), qPrintable(md));
        QVERIFY(md.toUtf8().contains("\xF0\x9F\x94\x96"));
        QVERIFY(!md.contains(QChar(0xF516)));
        QVERIFY(md.contains(QStringLiteral("> First line\n> Second line")));

        QCOMPARE(marks.filtered(QStringLiteral("bookmarks")).size(), 1);
        QCOMPARE(marks.filtered(QStringLiteral("highlights")).size(), 2);
        QCOMPARE(marks.filtered(QStringLiteral("notes")).size(), 1);
        QCOMPARE(marks.filtered(QStringLiteral("all")).size(), 3);
    }

    void bookSearchNormalizesPdfAndEpubTextAllowsSingleCjkAndCancelsMidIndex() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());

        // 1. EPUB with indented multi-line phrase, soft hyphen, ligature, and CJK.
        const QString epubPath = dir.filePath("norm.epub");
        const QByteArray normOpf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\"><metadata><dc:title>Norm</dc:title></metadata>\n"
            "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
            "<spine><itemref idref=\"c1\"/></spine></package>";
        const QByteArray normBytes = TestBooks::epub({
            {"OEBPS/content.opf", normOpf},
            {"OEBPS/c1.xhtml",
             TestBooks::chapter("C1", "<p>across\n      indented   lines</p>"
                                      "<p>of&#xFB01;ce lan&#xAD;tern</p>"
                                      "<p>\xE6\xA3\xAE\xE3\x81\xAE\xE4\xB8\xAD\xE3\x81\xAE\xE6\x9C\x88</p>")},
        });
        QVERIFY(TestBooks::write(epubPath, normBytes));
        const int epubId = library.addBookFile(epubPath);
        QVERIFY(epubId > 0);

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(epubId));
        BookSearch search(&reader);

        search.find(QStringLiteral("across indented lines"));
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);
        const QVariantList spans = search.inChapter(0);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].toMap().value("chapter").toInt(), 0);

        search.find(QStringLiteral("office lantern"));
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);

        // Single-character CJK searches work; single-character Latin is ignored.
        search.find(QString::fromUtf8("\xE6\x9C\x88"));
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);
        search.find(QStringLiteral("o"));
        QCOMPARE(search.count(), 0);

        // 2. PDF with \r\n line break, PDFium U+FFFE line-break hyphen, and fi ligature.
        const QString pdfPath = dir.filePath("search.pdf");
        {
            QByteArray out = "%PDF-1.4\n";
            QList<qsizetype> offsets;
            auto obj = [&](const QByteArray &body) {
                offsets.append(out.size());
                out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
            };
            obj("<< /Type /Catalog /Pages 2 0 R >>");
            obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
            obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
            const QByteArray stream =
                "BT /F1 12 Tf 20 350 Td (silver) Tj 0 -20 Td (lantern) Tj 0 -20 Td (of\\200ce) Tj 0 -20 Td (recon-) Tj 0 -20 Td (dite) Tj ET";
            obj("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
            obj("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [128 /fi] >> >>");
            const qsizetype xref = out.size();
            out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
            for (const qsizetype offset : offsets)
                out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
            out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
            out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
            QVERIFY(TestBooks::write(pdfPath, out));
        }
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(pdfId > 0);
        QVERIFY(reader.openBook(pdfId));

        search.find(QStringLiteral("silver lantern"));
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);
        const QVariantList pdfSpans = search.inChapter(0);
        QCOMPARE(pdfSpans.size(), 1);
        QCOMPARE(pdfSpans[0].toMap().value("chapter").toInt(), 0);

        search.find(QStringLiteral("office"));
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);

        search.find(QStringLiteral("recondite"));
        QTRY_VERIFY(!search.searching());
        QCOMPARE(search.count(), 1);

        const QVariantMap hyphHit = BookSearch::hit(QStringLiteral("pre recon\uFFFE\r\ndite post"), 2, 4, 16);
        QCOMPARE(hyphHit.value("match").toString(), QStringLiteral("recondite"));
        QCOMPARE(hyphHit.value("chapter").toInt(), 2);

        // 3. Mid-index semantic cancel: switching books while Book A is indexing never leaks results.
        QTcpServer slowServer;
        QVERIFY(slowServer.listen(QHostAddress::LocalHost));
        QList<QPointer<QTcpSocket>> heldSockets;
        int embedRequests = 0;
        QObject::connect(&slowServer, &QTcpServer::newConnection, &slowServer, [&] {
            while (QTcpSocket *sock = slowServer.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &heldSockets, &embedRequests] {
                    *buf += sock->readAll();
                    if (buf->contains("\r\n\r\n")) {
                        ++embedRequests;
                        heldSockets.append(sock);
                    }
                });
            }
        });

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(slowServer.serverPort()));
        local.configure(where, QStringLiteral("qwen"), QStringLiteral("embed"));
        services.setPreset(QStringLiteral("local"));

        search.setMeaning(&services, library.database().connectionName());
        search.setByMeaning(true);
        QVERIFY(reader.openBook(epubId));
        search.find(QStringLiteral("indented lantern"));
        QTRY_VERIFY_WITH_TIMEOUT(embedRequests >= 1, 5000);

        // Switch to the PDF mid-index; Book A's in-flight embed must be cancelled cleanly.
        QVERIFY(reader.openBook(pdfId));
        for (const QPointer<QTcpSocket> &sock : heldSockets) {
            if (sock && sock->state() == QAbstractSocket::ConnectedState) {
                const QByteArray body = "{\"embeddings\":[[1.0,0.0]]}";
                sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                sock->disconnectFromHost();
            }
        }
        QTest::qWait(80);
        QCOMPARE(search.count(), 0);
        QVERIFY(!search.searching());
        QVERIFY(search.preparing().isEmpty());
        QVERIFY(search.query().isEmpty());
        services.setPreset(QStringLiteral("cloud"));
        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini", QSettings::IniFormat)
            .remove("ai/preset");
    }

    // --- Post-merge regression tests: AI / Companion ----------------------

    void companionStripsMarkdownImagesAndHtmlImg() {
        const QString dirty = QStringLiteral(
            "See ![tracker](http://127.0.0.1:9999/leak.png) and ![ref alt][img1] "
            "plus <img src=\"https://evil.test/x.gif\" onerror=\"alert(1)\"> "
            "and <IMG SRC='file:///etc/passwd'/> and [safe](https://example.com) **bold** and stray ![open.");
        const QString cleaned = Companion::cleanMarkdown(dirty);
        QVERIFY2(!cleaned.contains(QStringLiteral("![")), qPrintable(cleaned));
        QVERIFY2(!cleaned.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(cleaned));
        QVERIFY2(!cleaned.contains(QStringLiteral("127.0.0.1")), qPrintable(cleaned));
        QVERIFY2(!cleaned.contains(QStringLiteral("evil.test")), qPrintable(cleaned));
        QVERIFY2(!cleaned.contains(QStringLiteral("/etc/passwd")), qPrintable(cleaned));
        QVERIFY(cleaned.contains(QStringLiteral("tracker")));
        QVERIFY(cleaned.contains(QStringLiteral("ref alt")));
        QVERIFY(cleaned.contains(QStringLiteral("[safe](https://example.com)")));
        QVERIFY(cleaned.contains(QStringLiteral("**bold**")));
        QVERIFY(cleaned.contains(QStringLiteral("!\u200B[open.")));

        // Nested "!![alt](url)" cannot re-form an active "![" after replacement.
        const QString nested = Companion::cleanMarkdown(QStringLiteral("!![alt](http://evil.test/x.png)"));
        QVERIFY(!nested.contains(QStringLiteral("![")));
        QVERIFY(!nested.contains(QStringLiteral("evil.test")));
    }

    void aiServicesLocalOverrideNeverFallsBackToCloud() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        gemini.setKey(QStringLiteral("cloud-key"));
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);

        // Cloud preset with valid Gemini key, no local model configured:
        // explicit "local" override MUST NOT fall back to cloud Gemini.
        services.setPreset(QStringLiteral("cloud"));
        services.setOverride(QStringLiteral("ask"), QStringLiteral("local"));
        QCOMPARE(services.routeFor(QStringLiteral("ask")), QString());
        QVERIFY(services.textFor(QStringLiteral("ask")) == nullptr);
        QVERIFY(services.textProviderFor(QStringLiteral("ask")) == nullptr);

        services.setOverride(QStringLiteral("search"), QStringLiteral("local"));
        QVERIFY(services.embedderProvider() == nullptr);
        QVERIFY(services.embedder() == nullptr);

        // Local preset without overrides also never falls back to cloud embedder.
        services.setOverride(QStringLiteral("ask"), QString());
        services.setOverride(QStringLiteral("search"), QString());
        services.setPreset(QStringLiteral("local"));
        QCOMPARE(services.routeFor(QStringLiteral("explain")), QString());
        QVERIFY(services.embedderProvider() == nullptr);

        // Honest loopback vs remote LAN reporting.
        LocalModels::Server loopback;
        loopback.kind = LocalModels::Ollama;
        loopback.url = QUrl(QStringLiteral("http://127.0.0.1:11434"));
        local.configure(loopback, QStringLiteral("qwen3.5:4b"));
        QVERIFY(services.localIsLoopback());
        QVERIFY(services.whereFor(QStringLiteral("ask")).contains(QStringLiteral("On this computer")));

        LocalModels::Server remote;
        remote.kind = LocalModels::Ollama;
        remote.url = QUrl(QStringLiteral("http://192.168.1.50:11434"));
        local.configure(remote, QStringLiteral("qwen3.5:4b"));
        QVERIFY(!services.localIsLoopback());
        QVERIFY(!local.isLocal());
        QVERIFY(services.whereFor(QStringLiteral("ask")).contains(QStringLiteral("192.168.1.50")));

        services.setPreset(QStringLiteral("cloud"));
        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini", QSettings::IniFormat)
            .remove("ai");
    }

    void aiServicesBudgetZeroStrictVsUnlimitedAndAskRollback() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());

        // $0 strict budget stops all cloud spending unless unlimitedBudget is explicitly enabled.
        services.setUnlimitedBudget(false);
        services.setBudget(0.0);
        QVERIFY(!services.fitsBudget(0.0));
        QVERIFY(!services.fitsBudget(0.001));

        services.setUnlimitedBudget(true);
        QVERIFY(services.fitsBudget(0.0));
        QVERIFY(services.fitsBudget(500.0));

        services.setUnlimitedBudget(false);
        services.setBudget(10.0);
        QVERIFY(services.fitsBudget(10.0));
        QVERIFY(!services.fitsBudget(10.01));

        // Companion::ask() rolls back orphaned user turn when the model fails.
        const QString epubPath = dir.filePath("ask.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});

        FakeServer server;
        server.responses.append({500, "application/json", "{\"error\":\"server exploded\"}"});
        LocalModels local;
        services.setLocalModels(&local);
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(server.origin());
        local.configure(where, QStringLiteral("qwen3.5:4b"));
        services.setPreset(QStringLiteral("local"));

        Companion companion(&services, &reader, library.database().connectionName());
        companion.ask(QStringLiteral("Who keeps the lantern?"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY(!companion.problem().isEmpty());
        QVERIFY(companion.conversation().isEmpty());

        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini", QSettings::IniFormat)
            .remove("ai");
    }

    void geminiPcmFromWavGuards32BitWraparound() {
        auto le32 = [](quint32 v) {
            QByteArray b(4, Qt::Uninitialized);
            b[0] = char(v & 0xff);
            b[1] = char((v >> 8) & 0xff);
            b[2] = char((v >> 16) & 0xff);
            b[3] = char((v >> 24) & 0xff);
            return b;
        };

        // Chunk size 0xFFFFFFF8u: naive 32-bit `at += 8 + size` would wrap back to `at` (infinite loop).
        const QByteArray wrapEven = "RIFF" + le32(32) + "WAVE" + "JUNK" + le32(0xFFFFFFF8u) + QByteArray(16, '\0');
        QVERIFY(GeminiProvider::pcmFromWav(wrapEven).isEmpty());

        // Chunk size 0xFFFFFFFFu: `size + (size & 1)` wraps 32-bit unsigned to 0.
        const QByteArray wrapOdd = "RIFF" + le32(32) + "WAVE" + "JUNK" + le32(0xFFFFFFFFu) + QByteArray(16, '\0');
        QVERIFY(GeminiProvider::pcmFromWav(wrapOdd).isEmpty());

        // Oversized data chunk size safely clamped to available bytes.
        const QByteArray tail("ABCDEFGH");
        const QByteArray clamped = "RIFF" + le32(100) + "WAVE" + "data" + le32(0xFFFFFFFFu) + tail;
        QCOMPARE(GeminiProvider::pcmFromWav(clamped), tail);
    }

    void localModelsStripsChunkSplitThinkTagsAndRejectsNonHttpUrls() {
        QVERIFY(!LocalModels::isHttpUrl(QUrl(QStringLiteral("file:///etc/passwd"))));
        QVERIFY(!LocalModels::isHttpUrl(QUrl(QStringLiteral("ftp://127.0.0.1:11434"))));
        QVERIFY(!LocalModels::isHttpUrl(QUrl(QStringLiteral("http://"))));
        QVERIFY(LocalModels::isHttpUrl(QUrl(QStringLiteral("http://127.0.0.1:11434"))));
        QVERIFY(LocalModels::isHttpUrl(QUrl(QStringLiteral("https://llm.example.com/v1"))));

        LocalModels local;
        LocalModels::Server badServer;
        badServer.kind = LocalModels::Ollama;
        badServer.url = QUrl(QStringLiteral("file:///tmp"));
        local.configure(badServer, QStringLiteral("qwen"));
        QVERIFY(!local.isConfigured());
        QVERIFY(local.text() == nullptr);

        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        services.setLocalModels(&local);
        services.useLocal(QStringLiteral("ollama"), QStringLiteral("file:///etc/passwd"), QStringLiteral("qwen"), {});
        QVERIFY(!services.hasLocal());

        // Chunk-split <think>...</think> across Ollama NDJSON stream lines.
        FakeServer server;
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(server.origin());
        local.configure(where, QStringLiteral("qwen3.5:4b"));
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"<th\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"ink>secret \"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"plan</th\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"ink>\\n\\nFinal \"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"answer.\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"\"},\"done\":true,\"eval_count\":5}\n"});
        Ai::TextRequest req;
        req.stream = true;
        req.messages = {{true, QStringLiteral("Hi")}};
        Ai::Reply *reply = local.generate(req);
        QString streamed;
        connect(reply, &Ai::Reply::textReceived, this, [&](const QString &delta) {
            QVERIFY2(!delta.contains(QStringLiteral("think")), qPrintable(delta));
            QVERIFY2(!delta.contains(QStringLiteral("secret")), qPrintable(delta));
            QVERIFY2(!delta.contains(QStringLiteral("plan")), qPrintable(delta));
            streamed += delta;
        });
        QSignalSpy done(reply, &Ai::Reply::finished);
        QVERIFY(done.wait(3000));
        QVERIFY(!reply->error());
        QCOMPARE(streamed, QStringLiteral("Final answer."));
        QCOMPARE(reply->text(), QStringLiteral("Final answer."));

        // Non-<think> leading "<..." prefix is flushed intact without loss.
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"<em\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"phasis>\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"\"},\"done\":true}\n"});
        Ai::Reply *nonThink = local.generate(req);
        QString nonThinkStreamed;
        connect(nonThink, &Ai::Reply::textReceived, this, [&](const QString &d) { nonThinkStreamed += d; });
        QSignalSpy done2(nonThink, &Ai::Reply::finished);
        QVERIFY(done2.wait(3000));
        QCOMPARE(nonThinkStreamed, QStringLiteral("<emphasis>"));
        QCOMPARE(nonThink->text(), QStringLiteral("<emphasis>"));
    }

    void passageIndexSplitsMinifiedSingleLineEpub() {
        QString minified;
        for (int i = 0; i < 120; ++i)
            minified += QStringLiteral("The lantern shone brightly over the dark harbor #%1. ").arg(i);
        minified = minified.trimmed();
        QVERIFY(!minified.contains(QLatin1Char('\n')));

        const QList<PassageIndex::Passage> passages = PassageIndex::split(2, minified, 400);
        QVERIFY(passages.size() >= 12);
        for (int i = 0; i < passages.size(); ++i) {
            const auto &p = passages[i];
            QCOMPARE(p.chapter, 2);
            QVERIFY2(p.text.size() <= 400, qPrintable(QString::number(p.text.size())));
            QCOMPARE(minified.mid(p.start, p.end - p.start), p.text);
            if (i + 1 < passages.size())
                QVERIFY2(p.text.endsWith(QLatin1Char('.')), qPrintable(p.text));
        }

        // Single-line paragraph without sentence punctuation splits cleanly at word boundaries.
        QString words;
        for (int i = 0; i < 200; ++i)
            words += QStringLiteral("harborlight ");
        words = words.trimmed();
        const QList<PassageIndex::Passage> wordPassages = PassageIndex::split(0, words, 250);
        QVERIFY(wordPassages.size() >= 8);
        for (const auto &p : wordPassages) {
            QVERIFY(p.text.size() <= 250);
            QCOMPARE(words.mid(p.start, p.end - p.start), p.text);
            QVERIFY(p.text.startsWith(QStringLiteral("harborlight")));
            QVERIFY(p.text.endsWith(QStringLiteral("harborlight")));
        }
    }

    // --- Post-merge regression tests: Narration / Audio -------------------

    void narrationNullProviderRecutAndChapterEndSleepCancel() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("two-linear.epub");
        const QByteArray twoOpf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Two Chapters</dc:title><dc:language>en-GB</dc:language></metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
            "</package>";
        const QByteArray twoBytes = TestBooks::epub({
            {"OEBPS/content.opf", twoOpf},
            {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Chapter one sentence.</p>")},
            {"OEBPS/c2.xhtml", TestBooks::chapter("Ch 2", "<p>Chapter two sentence.</p>")},
        });
        QVERIFY(TestBooks::write(epubPath, twoBytes));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        keys.save(QStringLiteral("gemini"), QStringLiteral("test-key"));
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);

        AudioCache cache(dir.filePath("audio"), library.database().connectionName());
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);

        // 1. Null-provider graceful pause (no SIGSEGV when provider disappears mid-session).
        narration.setVoice(QStringLiteral("sulafat"), QStringLiteral("Sulafat"));
        narration.start(0, 0);
        QVERIFY(narration.active() && narration.playing());
        services.forgetKey();
        QVERIFY(services.speechFor(QStringLiteral("sulafat")) == nullptr);
        narration.addBlocks(0, R"([{"start":0,"end":20,"text":"A short first line.","kind":"text"}])");
        QVERIFY(narration.active());
        QVERIFY(!narration.playing());
        QVERIFY(!narration.problem().isEmpty());
        narration.stop();

        // 2. recut() preserves stable track IDs for active/queued clips and assigns monotonic IDs.
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.start(0, 0);
        QJsonArray manyBlocks;
        qint64 off = 0;
        for (int i = 0; i < 10; ++i) {
            const QString para = QStringLiteral("Paragraph %1. ").arg(i) + QString(1100, QLatin1Char('a'));
            manyBlocks.append(QJsonObject{{"start", double(off)}, {"end", double(off + para.size())}, {"text", para}, {"kind", "text"}});
            off += para.size() + 1;
        }
        narration.addBlocks(0, QString::fromUtf8(QJsonDocument(manyBlocks).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() && narration.m_clips[0].queued, 5000);
        // Ensure the tail has Waiting clips so recut() actually replaces them.
        QVERIFY(narration.m_clips.size() >= 10);
        QCOMPARE(narration.m_clips.last().state, Narration::Clip::Waiting);
        const int clip0Id = narration.m_clips[0].id;
        const int queuedId = narration.m_clips[narration.m_queued].id;
        const int maxIdBefore = narration.m_clips.last().id;

        narration.applyMoods(0, {{9, QStringLiteral("hushed")}});
        QCOMPARE(narration.m_clips[narration.m_current].id, clip0Id);
        QCOMPARE(narration.m_clips[narration.m_queued].id, queuedId);
        QVERIFY(narration.m_player->hasTrack(clip0Id));
        QVERIFY(narration.m_clips.last().id > maxIdBefore);
        for (int i = 1; i < narration.m_clips.size(); ++i)
            QVERIFY(narration.m_clips[i].id > narration.m_clips[i - 1].id);
        narration.heard(clip0Id, 100);
        QCOMPARE(narration.m_current, 0);
        narration.stop();

        // 3. sleepAtEndOfChapter() -> sleepIn(0) pumps and queues ready next-chapter clips without stalling.
        narration.start(0, 0);
        narration.sleepAtEndOfChapter();
        QVERIFY(narration.sleepAtChapterEnd());
        narration.addBlocks(0, R"([{"start":0,"end":21,"text":"Chapter one sentence.","kind":"text"}])");
        // Chapter 1 was auto-requested because remaining < 5.
        QVERIFY(narration.m_requested.contains(1));
        narration.addBlocks(1, R"([{"start":0,"end":21,"text":"Chapter two sentence.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(narration.m_clips.size() == 2 &&
                                     narration.m_clips[0].state == Narration::Clip::Complete &&
                                     narration.m_clips[1].state == Narration::Clip::Complete,
                                 5000);
        // Chapter 0 is queued, chapter 1 is held back while chapter-end sleep is armed.
        QVERIFY(narration.m_clips[0].queued && narration.m_player->hasTrack(narration.m_clips[0].id));
        QVERIFY(!narration.m_clips[1].queued && !narration.m_player->hasTrack(narration.m_clips[1].id));

        // Cancelling chapter-end sleep mid-chapter immediately queues chapter 1 (no buffering stall).
        narration.sleepIn(0);
        QVERIFY(!narration.sleepAtChapterEnd());
        QVERIFY(narration.m_clips[1].queued && narration.m_player->hasTrack(narration.m_clips[1].id));

        // SpeechPlayer::drained also pumps if an already-ready clip was not yet queued.
        narration.heard(narration.m_clips[0].id, 0);
        QVERIFY(!narration.buffering());
        narration.m_clips[1].queued = false;
        narration.m_queued = 0;
        narration.m_player->clear();
        QVERIFY(narration.m_player->drainedOut());
        emit narration.m_player->drained();
        QVERIFY(narration.m_clips[1].queued && narration.m_player->hasTrack(narration.m_clips[1].id));
        QVERIFY(!narration.buffering());
        narration.stop();
    }

    void castIsolatesVoiceFamiliesAcrossNarratorSwitches() {
        VoiceRig rig;
        const QString epubPath = rig.dir.filePath("cast.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));
        const int bookId = rig.library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        QVERIFY(rig.reader.openBook(bookId));

        auto classic = [](const QString &id, const QString &gender) {
            Ai::Voice v;
            v.id = id;
            v.name = id;
            v.language = QStringLiteral("en-US");
            v.gender = gender;
            return v;
        };
        rig.catalog->setVoices({classic("sulafat", "female"), classic("gacrux", "female"),
                                classic("achernar", "male"), classic("aoede", "female")});

        VoicePacks packs(rig.dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(rig.dir.filePath("voices"), "engine-arm");
        fakeInstall(rig.dir.filePath("voices"), "kokoro-fp32");

        Cast cast(rig.services.get(), rig.cache.get(), &rig.reader, &rig.library, rig.catalog.get(), &packs);
        cast.setEnabled(true);
        cast.setNarrator(QStringLiteral("sulafat"));
        QVERIFY(cast.available());
        cast.learn(0, {{1, {QStringLiteral("Weena"), QStringLiteral("female")}},
                       {2, {QStringLiteral("Filby"), QStringLiteral("male")}}});
        cast.setVoice(QStringLiteral("Weena"), QStringLiteral("gacrux"));
        QCOMPARE(cast.voiceAt(0, 1), QStringLiteral("gacrux"));
        QVERIFY(!AiServices::isOnThisComputer(cast.voiceAt(0, 2)));

        // Switching narrator to offline Kokoro must NEVER resolve a paid Gemini voice.
        cast.setNarrator(QStringLiteral("builtin:kokoro:af_heart"));
        QVERIFY(cast.available());
        const QString kokoroWeena = cast.voiceAt(0, 1);
        const QString kokoroFilby = cast.voiceAt(0, 2);
        QVERIFY2(kokoroWeena.startsWith(QStringLiteral("builtin:kokoro:")), qPrintable(kokoroWeena));
        QVERIFY2(kokoroFilby.startsWith(QStringLiteral("builtin:kokoro:")), qPrintable(kokoroFilby));
        // Trying to assign a Gemini voice under a Kokoro narrator is rejected.
        cast.setVoice(QStringLiteral("Weena"), QStringLiteral("gacrux"));
        QCOMPARE(cast.voiceAt(0, 1), kokoroWeena);

        // Switching back to Gemini restores the Gemini family map independently.
        cast.setNarrator(QStringLiteral("sulafat"));
        QCOMPARE(cast.voiceAt(0, 1), QStringLiteral("gacrux"));

        // Legacy v1 flat cast JSON with a Gemini voice never leaks when loaded under Kokoro.
        rig.library.setBookSetting(bookId, QStringLiteral("cast"), QStringLiteral("{\"Weena\":\"gacrux\"}"));
        Cast reloaded(rig.services.get(), rig.cache.get(), &rig.reader, &rig.library, rig.catalog.get(), &packs);
        reloaded.setNarrator(QStringLiteral("builtin:kokoro:af_heart"));
        QVERIFY(reloaded.voiceAt(0, 1).startsWith(QStringLiteral("builtin:kokoro:")));
    }

    void audioCacheConcurrentWritesAndCorruptDecodeEviction() {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg isn't installed");
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const QString audioDir = dir.filePath("audio");
        AudioCache cache(audioDir, library.database().connectionName());

        QByteArray pcm(6000 * 2, '\0');
        auto *samples = reinterpret_cast<qint16 *>(pcm.data());
        for (int n = 0; n < 6000; ++n)
            samples[n] = qint16(5000 * std::sin(2 * M_PI * 260 * n / 24000.0));

        // 1. Concurrent stores (including duplicate key) use unique .part.<pid>.<seq> and leave no leftovers.
        QStringList keys;
        for (int i = 0; i < 4; ++i)
            keys.append(AudioCache::keyFor("builtin", "m", "v", "", QStringLiteral("Clip %1.").arg(i)));
        cache.store(keys[0], pcm, 24000);
        cache.store(keys[0], pcm, 24000);
        cache.store(keys[1], pcm, 24000);
        cache.store(keys[2], pcm, 24000);
        cache.store(keys[3], pcm, 24000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.hasPendingStores(), 15000);

        const auto batch = cache.clipInfoBatch(keys);
        QCOMPARE(batch.size(), 4);
        for (const QString &k : keys)
            QVERIFY(QFileInfo(batch.value(k).first).size() > 200);
        QVERIFY(QDir(audioDir).entryList({QStringLiteral("*.part"), QStringLiteral("*.part.*")}, QDir::Files).isEmpty());

        // 2. Corrupt WAV and corrupt Opus decode callbacks are deferred asynchronously with ok == false.
        const QString badWav = dir.filePath("corrupt.wav");
        write(badWav, "RIFFshort");
        bool wavCalled = false, wavOk = true;
        cache.decode(badWav, 24000, this, [&](const QByteArray &, bool ok) {
            wavCalled = true;
            wavOk = ok;
        });
        QVERIFY(!wavCalled);  // strictly deferred via QueuedConnection
        QTRY_VERIFY(wavCalled);
        QVERIFY(!wavOk);

        // 3. Narration::decode() evicts a corrupt cached Opus file and re-synthesizes ONCE without recursion.
        const QString epubPath = dir.filePath("evict.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        GeminiProvider gemini;
        MemoryKeyStore keyStore;
        AiServices services(&gemini, &keyStore, library.database().connectionName());
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        auto spoken = std::make_shared<QStringList>();
        LocalSpeech local(&packs, nullptr, [spoken](const QString &) {
            auto tone = std::make_unique<ToneEngine>();
            tone->spoken = spoken;
            return tone;
        });
        services.setLocalSpeech(&local);

        const QString text = QStringLiteral("Corrupt cache recovery test.");
        const QString corruptKey = AudioCache::keyFor(QStringLiteral("builtin"), local.speechModelName(false),
                                                      QStringLiteral("builtin:kokoro:bm_george"), QString(), text);
        cache.store(corruptKey, pcm, 24000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.hasPendingStores(), 10000);
        const QString cachedFile = cache.lookup(corruptKey);
        QVERIFY(!cachedFile.isEmpty());
        // Corrupt the Opus file on disk so ffmpeg fails to decode it.
        write(cachedFile, "this is not valid opus data");

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.start(0, 0);
        const QString blocksJson =
            QStringLiteral("[{\"start\":0,\"end\":28,\"text\":\"%1\",\"kind\":\"text\"}]").arg(text);
        narration.addBlocks(0, blocksJson);
        QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() && narration.m_clips[0].state == Narration::Clip::Complete,
                                 10000);
        QCOMPARE(spoken->size(), 1);
        QVERIFY(!narration.m_clips[0].pcm.isEmpty());
        narration.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!cache.hasPendingStores(), 10000);
    }

    void stretcherFlushesShortClipsAndTailsAtNonUnitRate() {
        // 1. Short clip (< Stretcher::frame = 1024 samples) at rate != 1.0 must never be dropped.
        QVector<qint16> tiny(300);
        for (int i = 0; i < tiny.size(); ++i)
            tiny[i] = qint16(100 + i * 5);
        for (double rate : {0.75, 1.5}) {
            Stretcher stretcher;
            stretcher.reset();
            QVector<qint16> out;
            while (stretcher.process(tiny.constData(), tiny.size(), true, rate, 4096, out)) {
            }
            QCOMPARE(out, tiny);
            QVERIFY(!stretcher.process(tiny.constData(), tiny.size(), true, rate, 4096, out));
        }

        // 2. Tail flush at rate != 1.0 (even when `room` == Stretcher::hop) keeps tail energy and ends cleanly.
        const int rateHz = 24000;
        QVector<qint16> clip(2400);
        for (int n = 0; n < clip.size(); ++n)
            clip[n] = qint16(9000 * std::sin(2 * M_PI * 300 * n / rateHz));
        for (double speed : {0.8, 1.5}) {
            Stretcher stretcher;
            stretcher.reset();
            QVector<qint16> out;
            while (stretcher.process(clip.constData(), clip.size(), true, speed, Stretcher::hop, out)) {
            }
            const double expected = clip.size() / speed;
            QVERIFY2(std::abs(out.size() - expected) < 700,
                     qPrintable(QStringLiteral("speed=%1 got=%2 expected=%3").arg(speed).arg(out.size()).arg(expected)));
            qint16 tailPeak = 0;
            for (int n = qMax(0, int(out.size()) - 400); n < out.size(); ++n)
                tailPeak = qMax<qint16>(tailPeak, qAbs(out[n]));
            QVERIFY(tailPeak > 1000);
            QVERIFY(!stretcher.process(clip.constData(), clip.size(), true, speed, Stretcher::hop, out));
        }

        // 3. SpeechPlayer track tracking & volume clamping.
        SpeechPlayer player;
        player.setHeadless(true);
        player.setVolume(2.5);
        QCOMPARE(player.volume(), 1.0);
        player.setVolume(-1.0);
        QCOMPARE(player.volume(), 0.0);
        player.addTrack(7);
        QVERIFY(player.hasTrack(7));
        player.appendAudio(7, QByteArray(200, '\0'));
        QCOMPARE(player.trackSamples(7), qint64(100));
        QVERIFY(!player.trackComplete(7));
        player.finishTrack(7);
        QVERIFY(player.trackComplete(7));
        player.clear();
        QVERIFY(!player.hasTrack(7));
    }

    void localReplyRecordsBoundaryBeforeAudioReceived() {
        QTemporaryDir dir;
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");

        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        Ai::SpeechRequest request;
        request.voice = QStringLiteral("builtin:kokoro:bm_george");
        request.sentences = {QStringLiteral("One."), QStringLiteral("Two."), QStringLiteral("Three.")};

        Ai::Reply *reply = local.synthesize(request);
        int emissions = 0;
        QList<int> boundaryCounts;
        QList<bool> matchedEnd;
        connect(reply, &Ai::Reply::audioReceived, this, [&](const QByteArray &) {
            ++emissions;
            const QList<qint64> b = reply->boundaries();
            boundaryCounts.append(int(b.size()));
            matchedEnd.append(!b.isEmpty() && b.last() == reply->audio().size() / 2);
        });
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(!reply->error());
        QCOMPARE(emissions, 3);
        QCOMPARE(boundaryCounts, (QList<int>{1, 2, 3}));
        QCOMPARE(matchedEnd, (QList<bool>{true, true, true}));

        // An engine producing 0 samples for non-empty text reports Ai::Error::Invalid.
        LocalSpeech broken(&packs, nullptr, [](const QString &) { return std::make_unique<SilentEngine>(); });
        Ai::Reply *emptyReply = broken.synthesize(request);
        QTRY_VERIFY(emptyReply->isFinished());
        QCOMPARE(emptyReply->error().kind, Ai::Error::Invalid);
    }

    void audiobookResolvesCoverArtAndSanitizesFilename() {
        // Metadata and concat list strip CR/LF injection cleanly.
        const QString meta = Audiobook::metadata(QStringLiteral("Title\r\n=1"), QStringLiteral("Auth;or"),
                                                 {{QStringLiteral("Ch\r1"), 0, 1000}});
        QVERIFY(!meta.contains(QLatin1Char('\r')));
        QVERIFY(meta.contains(QStringLiteral("title=Title\\\n\\=1\n")));
        QVERIFY(meta.contains(QStringLiteral("artist=Auth\\;or\n")));
        QCOMPARE(Audiobook::concatList({QStringLiteral("/a/b\r\nc'd.opus")}),
                 QStringLiteral("ffconcat version 1.0\nfile '/a/bc'\\''d.opus'\n"));

        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty())
            QSKIP("ffmpeg isn't installed");

        QTemporaryDir dir;
        // Isolate XDG_MUSIC_DIR inside the test XDG config directory so ~/Music is never touched.
        const QString musicDir = dir.filePath("music");
        QVERIFY(QDir().mkpath(musicDir));
        const QString userDirsPath = QString::fromUtf8(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/user-dirs.dirs");
        const bool hadUserDirs = QFile::exists(userDirsPath);
        QByteArray prevUserDirs;
        if (hadUserDirs) {
            QFile f(userDirsPath);
            if (f.open(QIODevice::ReadOnly))
                prevUserDirs = f.readAll();
        }
        auto restoreUserDirs = qScopeGuard([&] {
            if (hadUserDirs)
                write(userDirsPath, prevUserDirs);
            else
                QFile::remove(userDirsPath);
        });
        write(userDirsPath, QStringLiteral("XDG_MUSIC_DIR=\"%1\"\n").arg(musicDir).toUtf8());
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::MusicLocation), musicDir);

        // EPUB with a real PNG cover and a hostile filename title (leading dots + path/shell chars).
        const QString epubPath = dir.filePath("hostile.epub");
        const QByteArray hostileOpf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>...Evil/Title:With*Bad?Chars</dc:title>\n"
            "    <dc:creator>Ada Marsh</dc:creator>\n"
            "    <dc:language>en-GB</dc:language>\n"
            "    <meta name=\"cover\" content=\"cover-image\"/>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"cover-image\" href=\"images/cover.png\" media-type=\"image/png\"/>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/></spine>\n"
            "</package>";
        const QByteArray hostileBytes = TestBooks::epub({
            {"OEBPS/content.opf", hostileOpf},
            {"OEBPS/images/cover.png", png(40, 60)},
            {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Hello audiobook export.</p>")},
        });
        QVERIFY(TestBooks::write(epubPath, hostileBytes));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        const QString coverFile = library.record(bookId).cover;
        QVERIFY(!coverFile.isEmpty());
        QVERIFY(QFileInfo::exists(QDir(library.coverDirectory()).filePath(coverFile)));

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);

        AudioCache cache(dir.filePath("audio"), library.database().connectionName());
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));

        Download download(&services, &cache, &reader, &narration);
        download.start(0, 0);
        download.addBlocks(0, R"([{"start":0,"end":23,"text":"Hello audiobook export.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(download.finished(), 15000);
        QVERIFY(download.problem().isEmpty());

        Audiobook audiobook(&cache, &download, &reader, &library);
        audiobook.save();
        QTRY_VERIFY_WITH_TIMEOUT(!audiobook.busy(), 15000);
        QVERIFY2(audiobook.problem().isEmpty(), qPrintable(audiobook.problem()));
        const QString expected = QDir(musicDir + QStringLiteral("/Clothbound")).filePath(QStringLiteral("Evil Title With Bad Chars.m4b"));
        QCOMPARE(audiobook.saved(), expected);
        QVERIFY(QFileInfo(expected).size() > 1000);

        // Verify ffmpeg embedded the resolved cover art from library.coverDirectory().
        const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
        if (!ffprobe.isEmpty()) {
            QProcess probe;
            probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                                  QStringLiteral("stream=codec_type,codec_name:stream_disposition=attached_pic"),
                                  QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1"), expected});
            QVERIFY(probe.waitForFinished(5000));
            const QByteArray out = probe.readAllStandardOutput();
            QVERIFY2(out.contains("codec_type=video"), out.constData());
            QVERIFY2(out.contains("attached_pic=1"), out.constData());
        }
    }

    // --- Round 4 regression tests: Reader / PDF ----------------------------

    void readerPerBookAppearanceRoundTripAndIsolation() {
        QTemporaryDir dir;
        const QString epubA = dir.filePath("bookA.epub");
        const QString epubB = dir.filePath("bookB.epub");
        QVERIFY(TestBooks::write(epubA, TestBooks::epub2(png(20, 30))));
        QVERIFY(TestBooks::write(epubB, TestBooks::epub3(adobeObfuscate(fontPayload()))));

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreTypography = qScopeGuard([&] {
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("typography"));
        });

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int idA = library.addBookFile(epubA);
        const int idB = library.addBookFile(epubB);
        QVERIFY(idA > 0 && idB > 0 && idA != idB);

        BookScheme scheme;
        Reader reader(&library, &scheme);

        // Global typography controls clamp cleanly.
        reader.setFontWeight(900);
        QCOMPARE(reader.fontWeight(), 700);
        reader.setFontWeight(100);
        QCOMPARE(reader.fontWeight(), 300);
        reader.setFontWeight(450);
        QCOMPARE(reader.fontWeight(), 450);

        reader.setParagraphSpacing(2.5);
        QCOMPARE(reader.paragraphSpacing(), 1.2);
        reader.setParagraphSpacing(-0.5);
        QCOMPARE(reader.paragraphSpacing(), 0.0);
        reader.setParagraphSpacing(0.65);
        QCOMPARE(reader.paragraphSpacing(), 0.65);

        // Book A starts without per-book override.
        QVERIFY(reader.openBook(idA));
        QVERIFY(!reader.bookAppearanceOverride());
        QVERIFY(reader.bookAppearance().isEmpty());

        // Saving per-book appearance clamps typography bounds and persists in book_settings.
        QSignalSpy appearanceSpy(&reader, &Reader::appearanceChanged);
        reader.saveBookAppearance({{QStringLiteral("fontWeight"), 950},
                                   {QStringLiteral("paragraphSpacing"), 3.0},
                                   {QStringLiteral("font"), QStringLiteral("Literata")},
                                   {QStringLiteral("fontScale"), 1.25}});
        QVERIFY(reader.bookAppearanceOverride());
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 700);
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 1.2);
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("font")).toString(), QStringLiteral("Literata"));
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontScale")).toDouble(), 1.25);
        QVERIFY(!library.bookSetting(idA, QStringLiteral("appearance")).isEmpty());
        QVERIFY(appearanceSpy.count() >= 1);

        // Lower-bound clamping on saveBookAppearance.
        reader.saveBookAppearance({{QStringLiteral("fontWeight"), 150},
                                   {QStringLiteral("paragraphSpacing"), -0.4},
                                   {QStringLiteral("font"), QStringLiteral("Literata")}});
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 300);
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 0.0);

        // Switching to Book B isolates per-book appearance completely.
        QVERIFY(reader.openBook(idB));
        QVERIFY(!reader.bookAppearanceOverride());
        QVERIFY(reader.bookAppearance().isEmpty());
        QVERIFY(library.bookSetting(idB, QStringLiteral("appearance")).isEmpty());

        // setBookAppearanceOverride(true) seeds from global defaults for Book B.
        reader.setBookAppearanceOverride(true);
        QVERIFY(reader.bookAppearanceOverride());
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 450);
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 0.65);

        // Cold reload into a fresh Reader restores Book A's saved appearance.
        reader.close();
        Reader reloaded(&library, &scheme);
        QVERIFY(reloaded.openBook(idA));
        QVERIFY(reloaded.bookAppearanceOverride());
        QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 300);
        QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 0.0);
        QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("font")).toString(), QStringLiteral("Literata"));

        // Clearing Book A removes its override without affecting Book B.
        reloaded.clearBookAppearance();
        QVERIFY(!reloaded.bookAppearanceOverride());
        QVERIFY(reloaded.bookAppearance().isEmpty());
        QVERIFY(library.bookSetting(idA, QStringLiteral("appearance")).isEmpty());

        QVERIFY(reloaded.openBook(idB));
        QVERIFY(reloaded.bookAppearanceOverride());
        QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 450);
        reloaded.setBookAppearanceOverride(false);
        QVERIFY(!reloaded.bookAppearanceOverride());
        QVERIFY(library.bookSetting(idB, QStringLiteral("appearance")).isEmpty());
    }

    void pdfBookPageLinksAndSelectWordBounds() {
        // Build a 2-page PDF with an internal GoTo link, an external HTTPS link,
        // an unsafe javascript: link (must be filtered out), and text with a contraction.
        auto linkedPdf = [] {
            QByteArray out = "%PDF-1.4\n";
            QList<qsizetype> offsets;
            auto object = [&](const QByteArray &body) {
                offsets.append(out.size());
                out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
            };
            object("<< /Type /Catalog /Pages 2 0 R >>");                                       // 1
            object("<< /Type /Pages /Kids [3 0 R 9 0 R] /Count 2 >>");                         // 2
            object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 4 0 R "
                   "/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R 7 0 R 8 0 R] >>");   // 3 (page 0)
            const QByteArray s1 = "BT /F1 12 Tf 20 250 Td (Hello reader's world) Tj ET";
            object("<< /Length " + QByteArray::number(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");  // 4
            object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");                               // 5
            object("<< /Type /Annot /Subtype /Link /Rect [20 240 120 265] /A << /S /GoTo /D [9 0 R /XYZ 0 300 1] >> >>");  // 6
            object("<< /Type /Annot /Subtype /Link /Rect [20 200 140 220] "
                   "/A << /S /URI /URI (https://example.org/clothbound) >> >>");                                // 7
            object("<< /Type /Annot /Subtype /Link /Rect [20 160 140 180] "
                   "/A << /S /URI /URI (javascript:alert\\(1\\)) >> >>");                                   // 8
            object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 10 0 R "
                   "/Resources << /Font << /F1 5 0 R >> >> >>");                                            // 9 (page 1)
            const QByteArray s2 = "BT /F1 12 Tf 20 250 Td (Second destination page) Tj ET";
            object("<< /Length " + QByteArray::number(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");  // 10
            const qsizetype xref = out.size();
            out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
            for (const qsizetype off : offsets)
                out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
            out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
            out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
            return out;
        };

        QTemporaryDir dir;
        const QString pdfPath = dir.filePath("linked.pdf");
        QVERIFY(TestBooks::write(pdfPath, linkedPdf()));

        PdfBook pdf(pdfPath);
        QVERIFY2(pdf.open(), qPrintable(pdf.errorString()));
        QCOMPARE(pdf.pageCount(), 2);

        // Out-of-bounds pages return empty link lists and invalid word selections.
        QVERIFY(pdf.pageLinks(-1).isEmpty());
        QVERIFY(pdf.pageLinks(2).isEmpty());
        QVERIFY(pdf.pageLinks(99).isEmpty());
        QCOMPARE(pdf.selectWord(-1, QPointF(0.2, 0.2)).start, qint64(-1));
        QCOMPARE(pdf.selectWord(2, QPointF(0.2, 0.2)).start, qint64(-1));
        QVERIFY(pdf.selectWord(0, QPointF(-0.5, -0.5)).text.isEmpty());
        QVERIFY(pdf.selectWord(0, QPointF(2.0, 2.0)).text.isEmpty());
        QVERIFY(pdf.selectWord(0, QPointF(0.95, 0.95)).text.isEmpty());

        // Page 0 exposes internal + external HTTPS links and strips javascript: links.
        const QList<PdfLinkEntry> links = pdf.pageLinks(0);
        QCOMPARE(links.size(), 2);
        bool sawInternal = false, sawExternal = false;
        for (const PdfLinkEntry &entry : links) {
            QVERIFY(!entry.rects.isEmpty());
            for (const QRectF &r : entry.rects) {
                QVERIFY(r.x() >= 0.0 && r.x() <= 1.0);
                QVERIFY(r.y() >= 0.0 && r.y() <= 1.0);
                QVERIFY(r.width() > 0.0 && r.height() > 0.0);
                QVERIFY(r.right() <= 1.0001 && r.bottom() <= 1.0001);
            }
            if (entry.page == 1 && entry.url.isEmpty())
                sawInternal = true;
            if (entry.page == -1 && entry.url == QUrl(QStringLiteral("https://example.org/clothbound")))
                sawExternal = true;
            QVERIFY(entry.url.scheme() != QLatin1String("javascript"));
        }
        QVERIFY(sawInternal);
        QVERIFY(sawExternal);

        // selectWord() expands across the full word including internal apostrophes (Type1 Helvetica maps ' to U+2019).
        const QString p0Text = pdf.pageText(0);
        QString targetWord = QStringLiteral("reader\u2019s");
        qsizetype wordIdx = p0Text.indexOf(targetWord);
        if (wordIdx < 0) {
            targetWord = QStringLiteral("reader's");
            wordIdx = p0Text.indexOf(targetWord);
        }
        QVERIFY2(wordIdx >= 0, qPrintable(p0Text));
        const QList<QRectF> wordRects = pdf.textRects(0, wordIdx, wordIdx + targetWord.size());
        QVERIFY(!wordRects.isEmpty());
        const QPointF center = wordRects.first().center();
        const PdfBook::Selection sel = pdf.selectWord(0, center);
        QCOMPARE(sel.text, targetWord);
        QVERIFY(sel.start >= 0 && sel.end <= p0Text.size());
        QCOMPARE(p0Text.mid(sel.start, sel.end - sel.start), sel.text);

        // Reader exposes pdfLinks() and pdfSelectWord() cleanly with bounds checks.
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        QCOMPARE(reader.pdfLinks(0).size(), 2);
        QVERIFY(reader.pdfLinks(-1).isEmpty());
        QVERIFY(reader.pdfLinks(5).isEmpty());
        const QVariantMap rSel = reader.pdfSelectWord(0, center.x(), center.y());
        QCOMPARE(rSel.value(QStringLiteral("text")).toString(), targetWord);
        QCOMPARE(rSel.value(QStringLiteral("chapter")).toInt(), 0);
        QVERIFY(reader.pdfSelectWord(5, 0.2, 0.2).isEmpty());
    }

    void readerRtlDirectionJumpBackAndWpmAutomatedExclusion() {
        QTemporaryDir dir;
        // Build an EPUB 3 book declaring page-progression-direction="rtl".
        const QByteArray rtlOpf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:identifier id=\"uid\">urn:uuid:rtl-test-001</dc:identifier>\n"
            "    <dc:title>Vertical Tales</dc:title>\n"
            "    <dc:creator>A. Author</dc:creator>\n"
            "    <dc:language>ja</dc:language>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine page-progression-direction=\"rtl\"><itemref idref=\"c1\"/></spine>\n"
            "</package>";
        const QByteArray rtlNav =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>\n"
            "<nav epub:type=\"toc\"><ol><li><a href=\"c1.xhtml\">Chapter One</a></li></ol></nav>\n"
            "</body></html>";
        const QByteArray longBody = "<p>" + QByteArray(1200, 'a') + "</p>";
        const QString rtlPath = dir.filePath("rtl.epub");
        QVERIFY(TestBooks::write(rtlPath, TestBooks::epub({
                                              {"OEBPS/content.opf", rtlOpf},
                                              {"OEBPS/nav.xhtml", rtlNav},
                                              {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", longBody)},
                                          })));
        const QString ltrPath = dir.filePath("ltr.epub");
        QVERIFY(TestBooks::write(ltrPath, TestBooks::epub2(png(20, 30))));

        EpubBook rtlBook(rtlPath);
        QVERIFY(rtlBook.open());
        QVERIFY(rtlBook.isRightToLeft());

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int rtlId = library.addBookFile(rtlPath);
        const int ltrId = library.addBookFile(ltrPath);
        QVERIFY(rtlId > 0 && ltrId > 0);
        // Pre-seed chapter lengths so globalOffset() is deterministic without waiting on background measurement.
        library.setChapterLengths(rtlId, {4000});
        library.setChapterLengths(ltrId, {4000, 1000});

        BookScheme scheme;
        Reader readerAuto(&library, &scheme);
        QVERIFY(readerAuto.openBook(rtlId));
        QVERIFY(readerAuto.rtl());
        QVERIFY(readerAuto.manifest().value(QStringLiteral("rtl")).toBool());

        // Dynamic CSS writing-mode (vertical-lr -> rtl=false, vertical-rl -> rtl=true) updates Reader::rtl.
        QSignalSpy jumpSpy(&readerAuto, &Reader::jumpRecorded);
        readerAuto.relocated({{"chapter", 0}, {"offset", 0}, {"end", 200}, {"page", 0}, {"pages", 4}, {"rtl", false}});
        QVERIFY(!readerAuto.rtl());
        readerAuto.relocated({{"chapter", 0}, {"offset", 100}, {"end", 300}, {"page", 1}, {"pages", 4},
                              {"rtl", true}, {"canGoBack", true}, {"canGoForward", false},
                              {"backChapter", 0}, {"backPageLabel", "12"}, {"jumped", true}});
        QVERIFY(readerAuto.rtl());
        QVERIFY(readerAuto.canGoBack());
        QVERIFY(!readerAuto.canGoForward());
        QCOMPARE(readerAuto.backLabel(), QStringLiteral("p. 12"));
        QCOMPARE(jumpSpy.count(), 1);

        // Start manual reader in parallel so one 2.1s wait verifies BOTH automated exclusion and manual inclusion.
        Reader readerManual(&library, &scheme);
        QVERIFY(readerManual.openBook(ltrId));
        QVERIFY(!readerManual.rtl());
        readerManual.relocated({{"chapter", 0}, {"offset", 0}, {"end", 200}, {"page", 0}, {"pages", 4}});

        QTest::qWait(2100);

        // Automated narration page turn must NOT accumulate visual reading time or WPM chars.
        readerAuto.relocated({{"chapter", 0}, {"offset", 900}, {"end", 1100}, {"page", 2}, {"pages", 4},
                              {"automated", true}});
        // atEnd reports 0 minutes remaining in chapter and book.
        readerAuto.relocated({{"chapter", 0}, {"offset", 3900}, {"end", 4000}, {"page", 3}, {"pages", 4},
                              {"atEnd", true}, {"automated", true}});
        QCOMPARE(readerAuto.minutesLeftInChapter(), 0);
        QCOMPARE(readerAuto.minutesLeftInBook(), 0);
        readerAuto.close();

        // Non-automated manual page turn DOES accumulate reading time.
        readerManual.relocated({{"chapter", 0}, {"offset", 900}, {"end", 1100}, {"page", 1}, {"pages", 4},
                                {"automated", false}});
        readerManual.close();

        QCOMPARE(library.record(rtlId).secondsRead, qint64(0));
        QVERIFY(library.record(ltrId).secondsRead >= 2);
    }

    // --- Round 4 regression tests: Companion / Search / LocalModels --------

    void companionPersistsThreadsWhosWhoCascadesAndGuardsSpoilers() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("story.epub");
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>Harbor Mystery</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en</dc:language>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
            "</package>";
        QVERIFY(TestBooks::write(
            epubPath,
            TestBooks::epub({
                {"OEBPS/content.opf", opf},
                {"OEBPS/c1.xhtml",
                 TestBooks::chapter("Ch 1", "<p>Ada Marsh lit the brass lantern at dusk along the pier.</p>")},
                {"OEBPS/c2.xhtml",
                 TestBooks::chapter("Ch 2", "<p>Captain Reed arrived aboard the silver cutter at dawn.</p>")},
            })));

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restorePreset = qScopeGuard([&] {
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai/preset"));
        });

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 55}, {"page", 0}, {"pages", 1}});

        FakeServer server;
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(server.origin());
        local.configure(where, QStringLiteral("qwen3.5:4b"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        Companion companion(&services, &reader, library.database().connectionName());

        // 1. Ask persists conversation turns + passage citations to SQLite `companion_threads`.
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"Ada Marsh keeps the brass lantern.\"},\"done\":false}\n"
                                 "{\"message\":{\"content\":\"\"},\"done\":true}\n"});
        companion.ask(QStringLiteral("Who lit the brass lantern?"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QCOMPARE(companion.conversation().size(), 2);
        QCOMPARE(companion.conversation()[0].toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Who lit the brass lantern?"));
        QCOMPARE(companion.conversation()[1].toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Ada Marsh keeps the brass lantern."));
        QVERIFY(!companion.conversation()[1].toMap().value(QStringLiteral("sources")).toList().isEmpty());

        // A fresh Companion instance restores the persisted Ask thread from SQLite.
        {
            Companion restored(&services, &reader, library.database().connectionName());
            QCOMPARE(restored.conversation().size(), 2);
            QCOMPARE(restored.conversation()[1].toMap().value(QStringLiteral("text")).toString(),
                     QStringLiteral("Ada Marsh keeps the brass lantern."));
            QVERIFY(!restored.conversation()[1].toMap().value(QStringLiteral("sources")).toList().isEmpty());
        }

        // 2. Who's Who persists checkpoints to `companion_whoswho` with first-seen jump metadata.
        const QByteArray ch0Who =
            "{\"message\":{\"content\":\"{\\\"people\\\":[{\\\"name\\\":\\\"Ada Marsh\\\",\\\"also\\\":\\\"\\\","
            "\\\"who\\\":\\\"Lantern keeper.\\\",\\\"chapter\\\":\\\"Chapter 1\\\"}]}\"},\"done\":true}\n";
        server.responses.append({200, "application/x-ndjson", ch0Who});
        companion.whosWho();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY(companion.peopleLoaded());
        QCOMPARE(companion.peopleChapter(), 0);
        QCOMPARE(companion.people().size(), 1);
        QCOMPARE(companion.people().first().toMap().value(QStringLiteral("chapterIndex")).toInt(), 0);
        QCOMPARE(companion.people().first().toMap().value(QStringLiteral("offset")).toLongLong(), qint64(0));

        // Advance to chapter 1 and save a second Who's Who checkpoint with Captain Reed.
        reader.relocated({{"chapter", 1}, {"offset", 0}, {"end", 55}, {"page", 0}, {"pages", 1}});
        const QByteArray ch1Who =
            "{\"message\":{\"content\":\"{\\\"people\\\":[{\\\"name\\\":\\\"Ada Marsh\\\",\\\"also\\\":\\\"\\\","
            "\\\"who\\\":\\\"Lantern keeper.\\\",\\\"chapter\\\":\\\"Chapter 1\\\"},"
            "{\\\"name\\\":\\\"Captain Reed\\\",\\\"also\\\":\\\"\\\","
            "\\\"who\\\":\\\"Cutter captain.\\\",\\\"chapter\\\":\\\"Chapter 2\\\"}]}\"},\"done\":true}\n";
        server.responses.append({200, "application/x-ndjson", ch1Who});
        companion.whosWho();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QCOMPARE(companion.peopleChapter(), 1);
        QCOMPARE(companion.people().size(), 2);

        // Cold reload at chapter 1 hits SQLite checkpoint with zero network calls.
        const int reqCountBefore = int(server.requests.size());
        {
            Companion reloaded(&services, &reader, library.database().connectionName());
            QVERIFY(reloaded.peopleLoaded());
            QCOMPARE(reloaded.peopleChapter(), 1);
            QCOMPARE(reloaded.people().size(), 2);
            reloaded.whosWho();
            QVERIFY(!reloaded.busy());
            QCOMPARE(int(server.requests.size()), reqCountBefore);
        }

        // 3. Backward-nav spoiler guard: jumping back to chapter 0 hides chapter 1's Captain Reed.
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 55}, {"page", 0}, {"pages", 1}});
        QVERIFY(companion.peopleLoaded());
        QCOMPARE(companion.peopleChapter(), 0);
        QCOMPARE(companion.people().size(), 1);
        QCOMPARE(companion.people().first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Ada Marsh"));

        // Navigating before the earliest saved offset (end=10 < saved offset 55) clears Who's Who completely.
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 10}, {"page", 0}, {"pages", 1}});
        QVERIFY(!companion.peopleLoaded());
        QVERIFY(companion.people().isEmpty());

        // 4. AFTER DELETE ON books trigger cascades cleanup to companion_threads and companion_whoswho.
        auto countRows = [&](const QString &table) {
            QSqlQuery q(library.database());
            q.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE book_id = ?").arg(table));
            q.addBindValue(bookId);
            return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
        };
        QCOMPARE(countRows(QStringLiteral("companion_threads")), 2);
        QCOMPARE(countRows(QStringLiteral("companion_whoswho")), 2);

        QSqlQuery delBook(library.database());
        delBook.prepare(QStringLiteral("DELETE FROM books WHERE id = ?"));
        delBook.addBindValue(bookId);
        QVERIFY(delBook.exec());

        QCOMPARE(countRows(QStringLiteral("companion_threads")), 0);
        QCOMPARE(countRows(QStringLiteral("companion_whoswho")), 0);
    }

    void hybridSearchRrfSentenceSpanAndSpoilerFree() {
        // 1. PassageIndex::fuseHybrid() RRF blending + matchKind classification + edge cases.
        const QList<PassageIndex::Passage> passages{
            {0, 0, 50, QStringLiteral("She calibrated the brass astrolabe for nocturnal navigation.")},
            {0, 60, 120, QStringLiteral("They sailed under bright constellations across calm water.")},
            {0, 130, 180, QStringLiteral("An antique astrolabe sat dusty on the library shelf.")},
            {0, 190, 240, QStringLiteral("Morning bread baked slowly inside the quiet village oven.")},
        };
        const QList<double> cosines{0.85, 0.90, 0.10, 0.05};
        const QList<PassageIndex::Hit> fused =
            PassageIndex::fuseHybrid(passages, cosines, QStringLiteral("astrolabe navigation"), 4, 0.25);
        QCOMPARE(fused.size(), 3);  // passage 3 excluded (low cosine + zero BM25)
        // Passage 0 has both strong semantic (#2) and #1 lexical BM25 -> wins RRF over semantic-only passage 1!
        QCOMPARE(fused[0].passage.start, qint64(0));
        QCOMPARE(fused[0].matchKind, QStringLiteral("hybrid"));
        QVERIFY(fused[0].semantic > 0.8 && fused[0].lexical > 0.0);
        // Semantic-only passage 1 & lexical-only passage 2 both surface with proper matchKind.
        QCOMPARE(fused[1].passage.start, qint64(60));
        QCOMPARE(fused[1].matchKind, QStringLiteral("semantic"));
        QCOMPARE(fused[1].lexical, 0.0);
        QCOMPARE(fused[2].passage.start, qint64(130));
        QCOMPARE(fused[2].matchKind, QStringLiteral("lexical"));
        QVERIFY(fused[2].lexical > 0.0);
        // Edge cases.
        QVERIFY(PassageIndex::fuseHybrid(passages, {0.5}, QStringLiteral("astrolabe"), 4).isEmpty());
        QVERIFY(PassageIndex::fuseHybrid({}, {}, QStringLiteral("astrolabe"), 4).isEmpty());
        QVERIFY(PassageIndex::fuseHybrid(passages, cosines, QStringLiteral("astrolabe"), 0).isEmpty());

        // 2. BookSearch::sentenceSpan() keeps abbreviations intact, picks best sentence, and caps long spans.
        const QString chText = QStringLiteral(
            "Dr. Wells examined the brass astrolabe at noon. Meanwhile Mr. Hyde waited quietly outside the gate. "
            "The dense harbor fog thickened rapidly before midnight.");
        const auto s1 = BookSearch::sentenceSpan(chText, 0, chText.size(), QStringLiteral("astrolabe"));
        QCOMPARE(chText.mid(s1.first, s1.second - s1.first),
                 QStringLiteral("Dr. Wells examined the brass astrolabe at noon."));
        const auto s3 = BookSearch::sentenceSpan(chText, 0, chText.size(), QStringLiteral("harbor fog"));
        QCOMPARE(chText.mid(s3.first, s3.second - s3.first),
                 QStringLiteral("The dense harbor fog thickened rapidly before midnight."));
        // Short sentence (<45 chars) coalesces with following sentence when total <= 240.
        const QString shortPair = QStringLiteral("The key vanished. Nobody saw who unlocked the iron chest.");
        const auto sp = BookSearch::sentenceSpan(shortPair, 0, shortPair.size(), QStringLiteral("key"));
        QCOMPARE(shortPair.mid(sp.first, sp.second - sp.first), shortPair);
        // Overlong punctuationless text caps cleanly <= 240 chars on a word boundary.
        QString longRun;
        for (int i = 0; i < 45; ++i)
            longRun += QStringLiteral("harborlight ");
        longRun = longRun.trimmed();
        const auto capped = BookSearch::sentenceSpan(longRun, 0, longRun.size(), QStringLiteral("harborlight"));
        QVERIFY(capped.second - capped.first <= 240);
        QVERIFY(longRun.mid(capped.first, capped.second - capped.first).endsWith(QStringLiteral("harborlight")));

        // 3. BookSearch::spoilerFree restricts semantic search strictly up to the reader's current place.
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("spoiler.epub");
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Vault</dc:title><dc:language>en</dc:language></metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
            "</package>";
        QVERIFY(TestBooks::write(
            epubPath,
            TestBooks::epub({
                {"OEBPS/content.opf", opf},
                {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>A quiet morning over the misty harbor pier.</p>")},
                {"OEBPS/c2.xhtml",
                 TestBooks::chapter("Ch 2", "<p>The secret sapphire treasure lay hidden beneath the clocktower.</p>")},
            })));

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreSettings = qScopeGuard([&] {
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai/preset"));
            s.remove(QStringLiteral("search/spoilerFree"));
        });

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 46}, {"page", 0}, {"pages", 1}});

        FakeServer server;
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(server.origin());
        local.configure(where, QStringLiteral("qwen3.5:4b"), QStringLiteral("qwen3-embedding:0.6b"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        BookSearch search(&reader);
        search.setMeaning(&services, library.database().connectionName());
        QVERIFY(search.canSearchByMeaning());
        search.setByMeaning(true);
        search.setSpoilerFree(true);
        QVERIFY(search.spoilerFree());

        // Build chapter 0 (1 passage) + query embedding; chapter 1 must NOT appear.
        server.responses.append({200, "application/json", "{\"embeddings\":[[1,0]]}"});
        server.responses.append({200, "application/json", "{\"embeddings\":[[0,1]]}"});
        search.find(QStringLiteral("sapphire treasure"));
        QTRY_VERIFY_WITH_TIMEOUT(!search.searching(), 5000);
        QCOMPARE(search.count(), 0);

        // Disabling spoilerFree re-runs the search across the full book and finds chapter 1!
        server.responses.append({200, "application/json", "{\"embeddings\":[[0,1]]}"});  // embeds chapter 1 passage
        server.responses.append({200, "application/json", "{\"embeddings\":[[0,1]]}"});  // query embedding
        search.setSpoilerFree(false);
        QTRY_VERIFY_WITH_TIMEOUT(!search.searching() && search.count() > 0, 5000);
        QCOMPARE(search.results().first().toMap().value(QStringLiteral("chapter")).toInt(), 1);
        QVERIFY(search.results().first().toMap().value(QStringLiteral("match")).toString().contains(
            QStringLiteral("sapphire")));
        QCOMPARE(search.results().first().toMap().value(QStringLiteral("matchKind")).toString(),
                 QStringLiteral("hybrid"));
    }

    void localModelsNormalizesProxyUrlsAndParsesContextWindow() {
        // 1. normalizeServerUrl() preserves reverse-proxy prefixes while stripping API suffixes/query/fragment.
        QCOMPARE(LocalModels::normalizeServerUrl(QUrl(QStringLiteral("http://127.0.0.1:11434/api/tags"))).toString(),
                 QStringLiteral("http://127.0.0.1:11434"));
        QCOMPARE(LocalModels::normalizeServerUrl(
                     QUrl(QStringLiteral("https://proxy.example.com/ollama/v1/chat/completions?foo=1#bar")))
                     .toString(),
                 QStringLiteral("https://proxy.example.com/ollama"));
        QCOMPARE(LocalModels::normalizeServerUrl(QUrl(QStringLiteral("https://proxy.example.com/llm/v1/"))).toString(),
                 QStringLiteral("https://proxy.example.com/llm"));
        QCOMPARE(LocalModels::normalizeServerUrl(QUrl(QStringLiteral("https://proxy.example.com/llama/props"))).toString(),
                 QStringLiteral("https://proxy.example.com/llama"));
        QCOMPARE(LocalModels::normalizeServerUrl(QUrl(QStringLiteral("http://localhost:8080/nested/sub/v1/models")))
                     .toString(),
                 QStringLiteral("http://localhost:8080/nested/sub"));

        // 2. parseContextWindow() parses Ollama /api/show and llama.cpp /props payloads accurately.
        const QJsonObject ollamaModelInfo{
            {"model_info", QJsonObject{{"qwen3.context_length", 131072}, {"general.architecture", "qwen3"}}},
            {"parameters", "num_ctx 4096\ntemperature 0.7"},
        };
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::Ollama, ollamaModelInfo), 131072);

        const QJsonObject ollamaParamsOnly{{"parameters", "stop <|end|>\nnum_ctx 16384\n"}};
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::Ollama, ollamaParamsOnly), 16384);

        const QJsonObject ollamaTopFallback{{"context_length", 8192}};
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::Ollama, ollamaTopFallback), 8192);

        const QJsonObject llamaProps{{"default_generation_settings", QJsonObject{{"n_ctx", 65536}}}};
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::LlamaCpp, llamaProps), 65536);

        const QJsonObject llamaTop{{"n_ctx", 4096}};
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::LlamaCpp, llamaTop), 4096);
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::Generic, llamaProps), 0);
        QCOMPARE(LocalModels::parseContextWindow(LocalModels::Ollama, QJsonObject{}), 0);

        // 3. Live reverse-proxy path preservation + queryContextWindow() against FakeServer.
        FakeServer server;
        LocalModels local;
        LocalModels::Server proxyOllama;
        proxyOllama.kind = LocalModels::Ollama;
        proxyOllama.url = QUrl(server.origin() + QStringLiteral("/proxy/ollama/v1/chat/completions"));
        local.configure(proxyOllama, QStringLiteral("qwen3.5:4b"));
        QCOMPARE(local.server().url.path(), QStringLiteral("/proxy/ollama"));

        server.responses.append({200, "application/json", "{\"models\":[{\"name\":\"qwen3.5:4b\"}]}"});
        Ai::Reply *checkReply = local.check();
        QTRY_VERIFY(checkReply->isFinished());
        QVERIFY(!checkReply->error());
        QCOMPARE(server.requests.last().path, QStringLiteral("/proxy/ollama/api/tags"));

        server.responses.append({200, "application/json", "{\"model_info\":{\"qwen3.context_length\":16384}}"});
        QSignalSpy ctxSpy(&local, &LocalModels::contextWindowChanged);
        local.queryContextWindow();
        QTRY_COMPARE(ctxSpy.count(), 1);
        QCOMPARE(server.requests.last().path, QStringLiteral("/proxy/ollama/api/show"));
        QCOMPARE(local.reportedContextWindow(), 16384);
        QCOMPARE(local.contextWindow(false), 16384);
        QCOMPARE(local.contextWindow(true), 8192);  // fast tier caps at min(8192, reported)

        LocalModels::Server llamaServer;
        llamaServer.kind = LocalModels::LlamaCpp;
        llamaServer.url = QUrl(server.origin() + QStringLiteral("/edge/llama/props"));
        local.configure(llamaServer, QStringLiteral("gemma-3"));
        server.responses.append({200, "application/json", "{\"default_generation_settings\":{\"n_ctx\":12288}}"});
        local.queryContextWindow();
        QTRY_COMPARE(local.reportedContextWindow(), 12288);
        QCOMPARE(server.requests.last().path, QStringLiteral("/edge/llama/props"));
        QCOMPARE(local.contextWindow(false), 12288);
    }

    // --- Round 4 regression tests: Audio / Narration / Cast / Audiobook ----

    void speechHeadingWhitelistAndDialogueSpanSplitting() {
        // 1. spokenHeading() structural-word whitelist leaves names/pronouns untouched.
        QCOMPARE(Speech::spokenHeading(QStringLiteral("When I")), QStringLiteral("When I."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Henry V")), QStringLiteral("Henry V."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Louis XIV")), QStringLiteral("Louis XIV."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Appendix C")), QStringLiteral("Appendix C."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Chapter IV")), QStringLiteral("Chapter 4."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Part II — The Crossing")),
                 QStringLiteral("Part 2 — The Crossing."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Chapitre IX")), QStringLiteral("Chapitre 9."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Kapitel III")), QStringLiteral("Kapitel 3."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Глава XII")), QStringLiteral("Глава 12."));
        QCOMPARE(Speech::spokenHeading(QStringLiteral("Chapter CCX")), QStringLiteral("Chapter CCX."));

        // 2. splitDialogueSpans() with curly quotes + narration attribution + exact offsets.
        const SpeechBlock quoted = block(50, QStringLiteral("“Watch out!” Harry shouted. “It’s falling!”"));
        const QList<DialogueSpan> qSpans = Speech::splitDialogueSpans(quoted);
        QCOMPARE(qSpans.size(), 3);
        QVERIFY(qSpans[0].dialogue);
        QCOMPARE(qSpans[0].sentences.first().text, QStringLiteral("“Watch out!”"));
        QVERIFY(!qSpans[1].dialogue);
        QCOMPARE(qSpans[1].sentences.first().text, QStringLiteral("Harry shouted."));
        QVERIFY(qSpans[2].dialogue);
        QCOMPARE(qSpans[2].sentences.first().text, QStringLiteral("“It’s falling!”"));
        for (const DialogueSpan &span : qSpans) {
            for (const SpeechSentence &s : span.sentences)
                QCOMPARE(quoted.text.mid(s.start - quoted.start, s.end - s.start), s.text);
        }

        // British single quotes do not break inside English contractions like "We’re".
        const SpeechBlock british = block(0, QStringLiteral("‘We’re leaving now,’ she whispered."));
        const QList<DialogueSpan> bSpans = Speech::splitDialogueSpans(british);
        QCOMPARE(bSpans.size(), 2);
        QVERIFY(bSpans[0].dialogue);
        QCOMPARE(bSpans[0].sentences.first().text, QStringLiteral("‘We’re leaving now,’"));
        QVERIFY(!bSpans[1].dialogue);
        QCOMPARE(bSpans[1].sentences.first().text, QStringLiteral("she whispered."));

        // Continental em-dash dialogue splits cleanly into alternating dialogue & narration spans.
        const SpeechBlock emDash = block(10, QStringLiteral("— Watch out! — Harry shouted. — Run!"));
        const QList<DialogueSpan> eSpans = Speech::splitDialogueSpans(emDash);
        QCOMPARE(eSpans.size(), 3);
        QVERIFY(eSpans[0].dialogue);
        QVERIFY(!eSpans[1].dialogue);
        QVERIFY(eSpans[2].dialogue);
        QCOMPARE(eSpans[1].sentences.first().text, QStringLiteral("Harry shouted."));
        for (const DialogueSpan &span : eSpans) {
            for (const SpeechSentence &s : span.sentences)
                QCOMPARE(emDash.text.mid(s.start - emDash.start, s.end - s.start), s.text);
        }

        // Speech::clips() splits inline dialogue spans into separate character and narrator clips.
        const QList<SpeechClip> castClips =
            Speech::clips(0, {quoted}, {}, 1500, 600, {{0, QStringLiteral("char_harry")}}, QStringLiteral("narrator"));
        QCOMPARE(castClips.size(), 3);
        QCOMPARE(castClips[0].voice, QStringLiteral("char_harry"));
        QCOMPARE(castClips[1].voice, QStringLiteral("narrator"));
        QCOMPARE(castClips[2].voice, QStringLiteral("char_harry"));
    }

    void speechPlayerOffThreadWsolaAndMidPlaybackRateRewind() {
        constexpr int sr = 24000;
        constexpr int totalSamples = sr * 2;  // 2 seconds of 220 Hz sine wave
        QByteArray pcm(totalSamples * 2, '\0');
        auto *samples = reinterpret_cast<qint16 *>(pcm.data());
        for (int n = 0; n < totalSamples; ++n)
            samples[n] = qint16(8000 * std::sin(2.0 * M_PI * 220.0 * n / sr));

        SpeechPlayer player(sr);
        player.setMuted(true);
        player.addTrack(42);
        player.appendAudio(42, pcm);
        player.finishTrack(42);

        // Drive the real off-thread WSOLA std::thread worker without opening a hardware QAudioSink.
        {
            QMutexLocker lock(&player.m_mutex);
            player.m_headless = false;
            player.m_playing = true;
            player.startWorker();
            player.m_cond.wakeOne();
        }

        // Wait for the off-thread worker to populate at least 2 ready chunks at 1.0x.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QMutexLocker lock(&player.m_mutex);
            return player.m_ready.size() >= 2;
        }()), 5000);

        // Simulate sink consuming chunk 0; record exact sourceStart of the first unplayed chunk.
        qint64 expectedRewind = -1;
        {
            QMutexLocker lock(&player.m_mutex);
            const SpeechPlayer::ReadyChunk played = player.m_ready.takeFirst();
            player.m_readySamples = qMax<qint64>(0, player.m_readySamples - played.bytes.size() / 2);
            QVERIFY(!player.m_ready.isEmpty());
            expectedRewind = player.m_ready.first().sourceStart;
        }
        QVERIFY(expectedRewind > 0 && expectedRewind < totalSamples);

        // Mid-playback setRate(1.5) must discard stale ready chunks and rewind worker to expectedRewind.
        player.setRate(1.5);
        {
            QMutexLocker lock(&player.m_mutex);
            QCOMPARE(player.m_rate, 1.5);
            QVERIFY(player.m_ready.isEmpty() || player.m_ready.first().sourceStart == expectedRewind);
            if (player.m_ready.isEmpty()) {
                QCOMPARE(player.m_workerIndex, 0);
                QCOMPARE(player.m_workerRead, expectedRewind);
            }
        }

        // Drain all 1.5x off-thread WSOLA chunks until track completion.
        QVector<qint16> stretched;
        bool sawFirstRateChunk = false;
        bool allRateOk = true;
        qint64 firstRateSourceStart = -1;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QMutexLocker lock(&player.m_mutex);
            while (!player.m_ready.isEmpty()) {
                const SpeechPlayer::ReadyChunk chunk = player.m_ready.takeFirst();
                player.m_readySamples = qMax<qint64>(0, player.m_readySamples - chunk.bytes.size() / 2);
                if (!chunk.bytes.isEmpty()) {
                    if (!sawFirstRateChunk) {
                        sawFirstRateChunk = true;
                        firstRateSourceStart = chunk.sourceStart;
                    }
                    allRateOk = allRateOk && qFuzzyCompare(chunk.rate, 1.5);
                    const qsizetype n = chunk.bytes.size() / 2;
                    const qsizetype prev = stretched.size();
                    stretched.resize(prev + n);
                    std::memcpy(stretched.data() + prev, chunk.bytes.constData(), size_t(n) * sizeof(qint16));
                }
            }
            player.m_cond.wakeOne();
            return player.m_workerFinishedAll && player.m_ready.isEmpty();
        }()), 5000);

        player.stopWorker();

        QVERIFY(sawFirstRateChunk);
        QVERIFY(allRateOk);
        QCOMPARE(firstRateSourceStart, expectedRewind);
        const double expectedOutSamples = double(totalSamples - expectedRewind) / 1.5;
        QVERIFY2(std::abs(double(stretched.size()) - expectedOutSamples) < 2400.0,
                 qPrintable(QStringLiteral("got=%1 expected=%2").arg(stretched.size()).arg(expectedOutSamples)));

        // Verify off-thread WSOLA kept the 220 Hz pitch intact.
        int crossings = 0;
        const int margin = sr / 10;
        QVERIFY(stretched.size() > 3 * margin);
        for (int n = margin; n + 1 < stretched.size() - margin; ++n)
            crossings += (stretched[n] < 0) != (stretched[n + 1] < 0) ? 1 : 0;
        const double hertz = crossings / 2.0 / (double(stretched.size() - 2 * margin) / sr);
        QVERIFY2(std::abs(hertz - 220.0) < 8.0, qPrintable(QString::number(hertz)));
    }

    void narrationSkipSecondsSleepFadeRestoreAndPendingStart() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("listen.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);
        AudioCache cache(dir.filePath("audio"), library.database().connectionName());

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);

        // 1. hasPendingStart + startPending() when no key and no local voice pack installed yet.
        QSignalSpy missingSpy(&narration, &Narration::voiceMissing);
        narration.start(0, 42);
        QVERIFY(!narration.active());
        QVERIFY(narration.hasPendingStart());
        QCOMPARE(missingSpy.count(), 1);

        // Install offline voice pack, pick it, and invoke startPending().
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.startPending();
        QVERIFY(!narration.hasPendingStart());
        QVERIFY(narration.active());
        QCOMPARE(narration.m_startChapter, 0);
        QCOMPARE(narration.m_startOffset, qint64(42));

        // 2. skipSeconds(+15/-15) across clips & pause preservation.
        // Feed 3 long blocks so each forms its own clip (~10s each via manual PCM sizing).
        narration.stop();
        narration.start(0, 0);
        QJsonArray arr;
        qint64 off = 0;
        for (int i = 0; i < 3; ++i) {
            const QString para = QStringLiteral("Clip %1 first sentence. Clip %1 second sentence. ").arg(i) +
                                 QString(700, QLatin1Char('a')) + QStringLiteral(".");
            arr.append(QJsonObject{{"start", double(off)}, {"end", double(off + para.size())},
                                   {"text", para}, {"kind", "text"}});
            off += para.size() + 1;
        }
        narration.addBlocks(0, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY_WITH_TIMEOUT(narration.m_clips.size() == 3 &&
                                     narration.m_clips[0].state == Narration::Clip::Complete &&
                                     narration.m_clips[1].state == Narration::Clip::Complete &&
                                     narration.m_clips[2].state == Narration::Clip::Complete,
                                 5000);
        // Set exact 10-second (240,000-sample) PCM lengths on all 3 clips for deterministic ±15s checks.
        for (int i = 0; i < 3; ++i) {
            narration.m_clips[i].pcm = QByteArray(240000 * 2, '\0');
            narration.m_clips[i].samples = 240000;
        }
        narration.heard(narration.m_clips[0].id, 0);
        QCOMPARE(narration.m_current, 0);
        QCOMPARE(narration.m_currentSample, qint64(0));

        QSignalSpy seekSpy(&narration, &Narration::seeked);
        narration.skipSeconds(15);  // +360,000 samples -> clip 1 @ 120,000 samples (5s into clip 1)
        QCOMPARE(narration.m_current, 1);
        QCOMPARE(narration.m_currentSample, qint64(120000));
        QCOMPARE(seekSpy.count(), 1);

        // Skipping while paused preserves paused state.
        narration.pause();
        QVERIFY(!narration.playing());
        narration.skipSeconds(-15);  // back to clip 0 @ 0 samples
        QCOMPARE(narration.m_current, 0);
        QCOMPARE(narration.m_currentSample, qint64(0));
        QVERIFY(!narration.playing());
        narration.resume();

        // 3. Sleep timer 10s quadratic fade-out + volume restore on resume() and cancel.
        narration.setVolume(0.8);
        QCOMPARE(narration.volume(), 0.8);
        narration.sleepIn(1);
        narration.m_sleepAt = QDateTime::currentDateTime().addMSecs(5000);  // halfway into 10s fade
        narration.onSleepTick();
        QVERIFY(narration.sleepFading());
        QVERIFY2(narration.volume() > 0.12 && narration.volume() < 0.30,
                 qPrintable(QString::number(narration.volume())));

        narration.finishSleep();
        QVERIFY(!narration.playing());
        QVERIFY(!narration.sleepFading());
        QCOMPARE(narration.volume(), 0.0);

        // Resuming after sleep-pause restores the original pre-fade volume (0.8).
        narration.resume();
        QVERIFY(narration.playing());
        QCOMPARE(narration.volume(), 0.8);

        // Cancelling mid-fade via sleepIn(0) also restores the pre-fade volume immediately.
        narration.sleepIn(1);
        narration.m_sleepAt = QDateTime::currentDateTime().addMSecs(4000);
        narration.onSleepTick();
        QVERIFY(narration.sleepFading());
        QVERIFY(narration.volume() < 0.5);
        narration.sleepIn(0);
        QVERIFY(!narration.sleepFading());
        QCOMPARE(narration.volume(), 0.8);
        narration.stop();
    }

    void directorPdfBatchRangeAndSplitMoodsByPage() {
        // Build a 20-page PDF with outline sections starting at page 0 and page 10.
        auto outlinedPdf = [] {
            QByteArray out = "%PDF-1.4\n";
            QList<qsizetype> offsets;
            auto object = [&](const QByteArray &body) {
                offsets.append(out.size());
                out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
            };
            constexpr int pages = 20;
            // Obj 1: Catalog, Obj 2: Pages, Obj 3: Font, Obj 4: shared Content stream
            // Obj 5..24: 20 Page objects (page p -> obj 5 + p)
            // Obj 25: Outlines root, Obj 26: Outline "Part One" (page 0 -> obj 5), Obj 27: Outline "Part Two" (page 10 -> obj 15)
            object("<< /Type /Catalog /Pages 2 0 R /Outlines 25 0 R >>");  // 1
            QByteArray kids;
            for (int p = 0; p < pages; ++p)
                kids += QByteArray::number(5 + p) + " 0 R ";
            object("<< /Type /Pages /Kids [ " + kids + "] /Count 20 >>");  // 2
            object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");  // 3
            const QByteArray stream = "BT /F1 12 Tf 20 250 Td (Batch page text.) Tj ET";
            object("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream");  // 4
            for (int p = 0; p < pages; ++p)
                object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 4 0 R "
                       "/Resources << /Font << /F1 3 0 R >> >> >>");  // 5..24
            object("<< /Type /Outlines /First 26 0 R /Last 27 0 R /Count 2 >>");                      // 25
            object("<< /Title (Part One) /Parent 25 0 R /Next 27 0 R /Dest [5 0 R /XYZ 0 300 1] >>");        // 26 (page 0)
            object("<< /Title (Part Two) /Parent 25 0 R /Prev 26 0 R /Dest [15 0 R /XYZ 0 300 1] >>");       // 27 (page 10)
            const qsizetype xref = out.size();
            out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
            for (const qsizetype off : offsets)
                out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
            out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
            out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
            return out;
        };

        QTemporaryDir dir;
        const QString pdfPath = dir.filePath("outlined.pdf");
        QVERIFY(TestBooks::write(pdfPath, outlinedPdf()));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        QCOMPARE(reader.chapterCount(), 20);

        // Null reader returns single-page fallback.
        QCOMPARE(Director::pdfBatchRange(nullptr, 5), (QPair<int, int>{5, 5}));
        // Section 1 (pages 0..9): capped at 8 pages (0..7), then remainder bounded by outline at page 10 (8..9).
        QCOMPARE(Director::pdfBatchRange(&reader, 0), (QPair<int, int>{0, 7}));
        QCOMPARE(Director::pdfBatchRange(&reader, 7), (QPair<int, int>{0, 7}));
        QCOMPARE(Director::pdfBatchRange(&reader, 8), (QPair<int, int>{8, 9}));
        QCOMPARE(Director::pdfBatchRange(&reader, 9), (QPair<int, int>{8, 9}));
        // Section 2 (pages 10..19): 10..17, then 18..19.
        QCOMPARE(Director::pdfBatchRange(&reader, 10), (QPair<int, int>{10, 17}));
        QCOMPARE(Director::pdfBatchRange(&reader, 17), (QPair<int, int>{10, 17}));
        QCOMPARE(Director::pdfBatchRange(&reader, 18), (QPair<int, int>{18, 19}));
        QCOMPARE(Director::pdfBatchRange(&reader, 19), (QPair<int, int>{18, 19}));

        // splitMoodsByPage() maps global block indices to local per-page indices and carries active mood across pages.
        const QMap<int, QString> combined{
            {1, QStringLiteral("tense")},
            {3, QString()},
            {4, QStringLiteral("hushed")},
        };
        const QHash<int, QPair<int, int>> ranges{
            {0, {0, 2}},  // blocks 0, 1
            {1, {2, 3}},  // blocks 2, 3, 4
            {2, {5, 2}},  // blocks 5, 6
        };
        const auto byPage = Director::splitMoodsByPage(combined, ranges, 0, 2);
        QCOMPARE(byPage.value(0), (QMap<int, QString>{{1, QStringLiteral("tense")}}));
        QCOMPARE(byPage.value(1), (QMap<int, QString>{{0, QStringLiteral("tense")},
                                                      {1, QString()},
                                                      {2, QStringLiteral("hushed")}}));
        QCOMPARE(byPage.value(2), (QMap<int, QString>{{0, QStringLiteral("hushed")}}));
    }

    void audiobookSynthesizesPauseSilenceAndPartialChapterSuffix() {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg isn't installed");

        QTemporaryDir dir;
        const QString musicDir = dir.filePath("music");
        QVERIFY(QDir().mkpath(musicDir));
        const QString userDirsPath = QString::fromUtf8(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/user-dirs.dirs");
        const bool hadUserDirs = QFile::exists(userDirsPath);
        QByteArray prevUserDirs;
        if (hadUserDirs) {
            QFile f(userDirsPath);
            if (f.open(QIODevice::ReadOnly))
                prevUserDirs = f.readAll();
        }
        auto restoreUserDirs = qScopeGuard([&] {
            if (hadUserDirs)
                write(userDirsPath, prevUserDirs);
            else
                QFile::remove(userDirsPath);
        });
        write(userDirsPath, QStringLiteral("XDG_MUSIC_DIR=\"%1\"\n").arg(musicDir).toUtf8());

        // 4 linear chapters so partial exports trigger "(Ch X–Y)" and "(Ch X)" suffixes.
        const QString epubPath = dir.filePath("four.epub");
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>Sea Voyages</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en-GB</dc:language>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c0\" href=\"c0.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c3\" href=\"c3.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c0\"/><itemref idref=\"c1\"/><itemref idref=\"c2\"/><itemref idref=\"c3\"/></spine>\n"
            "</package>";
        QVERIFY(TestBooks::write(
            epubPath,
            TestBooks::epub({
                {"OEBPS/content.opf", opf},
                {"OEBPS/c0.xhtml", TestBooks::chapter("Ch 1", "<p>Opening harbor scene.</p>")},
                {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 2", "<p>Before break.</p><p>* * *</p><p>After break.</p>")},
                {"OEBPS/c2.xhtml", TestBooks::chapter("Ch 3", "<p>Third chapter line.</p>")},
                {"OEBPS/c3.xhtml", TestBooks::chapter("Ch 4", "<p>Final epilogue line.</p>")},
            })));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        library.setChapterLengths(bookId, {30, 40, 30, 30});

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath("voices"), nullptr, "aarch64", packsManifest("http://x"));
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);

        AudioCache cache(dir.filePath("audio"), library.database().connectionName());
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));

        // Export partial chapters 1..2 (1-based Ch 2–3) with a 1300 ms scene-break pause.
        Download download(&services, &cache, &reader, &narration);
        download.start(1, 2);
        download.addBlocks(1, R"([
            {"start":0,"end":13,"text":"Before break.","kind":"text"},
            {"start":14,"end":19,"text":"* * *","kind":"break"},
            {"start":20,"end":32,"text":"After break.","kind":"text"}
        ])");
        download.addBlocks(2, R"([{"start":0,"end":19,"text":"Third chapter line.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(download.finished(), 15000);
        QVERIFY2(download.problem().isEmpty(), qPrintable(download.problem()));
        QVERIFY(download.plannedKeys().value(1).contains(QStringLiteral("pause:1300")));

        Audiobook audiobook(&cache, &download, &reader, &library);
        audiobook.save();
        QTRY_VERIFY_WITH_TIMEOUT(!audiobook.busy(), 15000);
        QVERIFY2(audiobook.problem().isEmpty(), qPrintable(audiobook.problem()));
        const QString expectedRange =
            QDir(musicDir + QStringLiteral("/Clothbound")).filePath(QStringLiteral("Sea Voyages (Ch 2\u20133).m4b"));
        QCOMPARE(audiobook.saved(), expectedRange);
        QVERIFY(QFileInfo(expectedRange).size() > 1000);

        // Verify total duration includes the 3 half-second spoken clips (~1.5s) + 1.3s pause (>= 2.5s).
        const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
        if (!ffprobe.isEmpty()) {
            QProcess probe;
            probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                                  QStringLiteral("format=duration"), QStringLiteral("-of"),
                                  QStringLiteral("default=noprint_wrappers=1:nokey=1"), expectedRange});
            QVERIFY(probe.waitForFinished(5000));
            const double durationSec = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
            QVERIFY2(durationSec >= 2.5, qPrintable(QString::number(durationSec)));
        }

        // Export a single chapter (chapter 0 -> "(Ch 1).m4b").
        Download single(&services, &cache, &reader, &narration);
        single.start(0, 0);
        single.addBlocks(0, R"([{"start":0,"end":21,"text":"Opening harbor scene.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(single.finished(), 15000);
        Audiobook singleBook(&cache, &single, &reader, &library);
        singleBook.save();
        QTRY_VERIFY_WITH_TIMEOUT(!singleBook.busy(), 15000);
        QVERIFY2(singleBook.problem().isEmpty(), qPrintable(singleBook.problem()));
        const QString expectedSingle =
            QDir(musicDir + QStringLiteral("/Clothbound")).filePath(QStringLiteral("Sea Voyages (Ch 1).m4b"));
        QCOMPARE(singleBook.saved(), expectedSingle);
        QVERIFY(QFileInfo(expectedSingle).size() > 1000);
    }

    // --- Round 4 regression tests: Library / Marks / CLI -------------------

    void meaningSearchSpansTheLibrary() {
        QTemporaryDir dir;
        const QString path = dir.filePath("library.sqlite");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("meaning-seed"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
                                          "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))")));
            auto add = [&](int book, const QString &model, const QString &key, QList<float> v) {
                q.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, ?, ?, ?)"));
                q.addBindValue(book);
                q.addBindValue(model);
                q.addBindValue(key);
                q.addBindValue(QByteArray(reinterpret_cast<const char *>(v.constData()), v.size() * qsizetype(sizeof(float))));
                QVERIFY(q.exec());
            };
            add(1, "m", "2:100:400:aa", {1, 0, 0});       // exactly what's asked
            add(1, "m", "2:400:700:bb", {0.9f, 0.1f, 0}); // the same chapter again
            add(2, "m", "5:0:300:cc", {0.7f, 0.7f, 0});   // another book, close
            add(3, "m", "1:0:300:dd", {0, 0, 1});         // unrelated
            add(4, "other", "1:0:300:ee", {1, 0, 0});     // another embedder's vectors
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("meaning-seed"));

        const auto hits = PassageIndex::rankLibrary(path, "m", {1, 0, 0}, 5);
        QCOMPARE(hits.size(), 2);
        QCOMPARE(hits[0].bookId, 1);
        QCOMPARE(hits[0].chapter, 2);
        QCOMPARE(hits[0].start, qint64(100));
        QCOMPARE(hits[0].end, qint64(400));
        QCOMPARE(hits[1].bookId, 2);
        QVERIFY(hits[0].score > hits[1].score);
        QVERIFY(PassageIndex::rankLibrary(path, "missing", {1, 0, 0}, 5).isEmpty());
        QCOMPARE(PassageIndex::rankLibrary(path, "m", {1, 0, 0}, 1).size(), 1);
    }

    void libraryIndexFindsWordsAcrossBooks() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));
        QCOMPARE(LibraryIndex::matchExpression(QStringLiteral("time  trav")), QStringLiteral("\"time\" \"trav\"*"));
        QCOMPARE(LibraryIndex::matchExpression(QStringLiteral("\" OR NEAR(")), QStringLiteral("\"OR\" \"NEAR\"*"));
        QCOMPARE(LibraryIndex::matchExpression(QStringLiteral("—")), QString());

        LibraryIndex index(dir.filePath("index.sqlite"));
        if (!index.available())
            QSKIP("SQLite without FTS5");
        QVERIFY(LibraryIndex::indexBook(dir.filePath("index.sqlite"), {7, epubPath, "epub", 100}));
        EpubBook epub(epubPath);
        QVERIFY(epub.open());
        // A word from the book's text is found, with where it is.
        QString word;
        for (int c = 0; c < int(epub.spine().size()) && word.isEmpty(); ++c) {
            const auto m = QRegularExpression(QStringLiteral("\\b\\p{L}{6,}\\b")).match(epub.chapterText(c));
            if (m.hasMatch())
                word = m.captured(0);
        }
        QVERIFY(!word.isEmpty());
        const QVariantList hits = index.search(word.left(4));
        QVERIFY(!hits.isEmpty());
        const QVariantMap hit = hits.first().toMap();
        QCOMPARE(hit.value("bookId").toInt(), 7);
        const QString chapterText = epub.chapterText(hit.value("chapter").toInt());
        QVERIFY(chapterText.mid(qint64(hit.value("offset").toDouble()), 4).compare(word.left(4), Qt::CaseInsensitive) == 0);
        QVERIFY(hit.value("snippet").toString().contains(QStringLiteral("‹")));
        QCOMPARE(index.bookCount(), 1);
        QVERIFY(index.search(QStringLiteral("zzqxv")).isEmpty());

        // A book no longer in the library leaves the index.
        index.update({});
        QCOMPARE(index.bookCount(), 0);
        QVERIFY(index.search(word.left(4)).isEmpty());

        // Accents don't matter: "cafe" finds "café".
        QVERIFY(LibraryIndex::indexBook(dir.filePath("index.sqlite"), {8, epubPath, "epub", 100}));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("idx-accents"));
            db.setDatabaseName(dir.filePath("index.sqlite"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("INSERT INTO chapter_text (body, book, chapter) VALUES ('Un café noir, s''il vous plaît.', 8, 99)")));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("idx-accents"));
        const QVariantList accents = index.search(QStringLiteral("cafe noir"));
        QVERIFY(!accents.isEmpty());
        QCOMPARE(accents.first().toMap().value("chapter").toInt(), 99);
        QCOMPARE(qint64(accents.first().toMap().value("offset").toDouble()), qint64(3));
    }

    void libraryFinderOffersFoldersWithBooks() {
        QTemporaryDir dir;
        const QString home = dir.path();
        // ~/Books holds three books (one too deep to count), Documents one,
        // Downloads none, and ~/Library is already watched.
        for (const char *rawPath : {"Books/a.epub", "Books/Wells/b.EPUB", "Books/c.pdf", "Books/1/2/3/4/5/deep.epub",
                                    "Books/notes.txt", "Documents/d.epub", "Downloads/e.zip", "Library/f.epub"}) {
            const QString path = QString::fromLatin1(rawPath);
            QVERIFY(QDir().mkpath(QFileInfo(home + "/" + path).absolutePath()));
            write(home + "/" + path, "x");
        }
        const QStringList places = LibraryFinder::candidates(home, home + "/Documents", home + "/Downloads");
        QVERIFY(places.contains(home + "/Books"));
        QVERIFY(!places.contains(home + "/books") || QFileInfo(home + "/books").exists());
        QCOMPARE(LibraryFinder::countBooks(home + "/Books"), 3);
        QCOMPARE(LibraryFinder::countBooks(home + "/Books", 4, 2), 2);

        const QVariantList found = LibraryFinder::search(places + QStringList{home + "/Library"},
                                                         {home + "/Library"}, home);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].toMap().value("label").toString(), QStringLiteral("~/Books"));
        QCOMPARE(found[0].toMap().value("count").toInt(), 3);
        QCOMPARE(found[1].toMap().value("label").toString(), QStringLiteral("~/Documents"));
        // A folder already watched, or one inside it, is never offered again.
        QCOMPARE(LibraryFinder::search(places, {home + "/Books"}, home).size(), 1);
        QCOMPARE(LibraryFinder::label("/srv/books", home), QStringLiteral("/srv/books"));
    }

    void libraryShelvesWantToReadListeningAndChapter() {
        QTemporaryDir dir;
        const QString dataDir = dir.filePath("data");
        QVERIFY(QDir().mkpath(dataDir));
        // Four books written straight into a fresh library, then read back.
        {
            Library seed(dataDir, dir.filePath("cache"));
            QVERIFY2(seed.open(), qPrintable(seed.errorString()));
            QSqlQuery insert(seed.database());
            for (int id = 1; id <= 4; ++id)
                QVERIFY(insert.exec(QStringLiteral("INSERT INTO books (id, path, format, title, sort_title, work_key, added_at) "
                                                   "VALUES (%1, '/b/%1.epub', 'epub', 'Book %1', 'book %1', 'w%1', 100)")
                                        .arg(id)));
            // Book 2 is under way in Chapter III; book 3 is finished.
            QVERIFY(insert.exec(QStringLiteral("UPDATE books SET opened_at = 500, progress = 0.2, position = "
                                               "'{\"chapter\":3,\"offset\":10,\"title\":\"Chapter III\"}' WHERE id = 2")));
            QVERIFY(insert.exec(QStringLiteral("UPDATE books SET opened_at = 400, finished_at = 600, "
                                               "fingerprint = 'fp3' WHERE id = 3")));
            // Back to a version 3 library, as an existing install has it.
            QVERIFY(insert.exec(QStringLiteral("ALTER TABLE books DROP COLUMN want_at")));
            QVERIFY(insert.exec(QStringLiteral("ALTER TABLE books DROP COLUMN listened_at")));
            QVERIFY(insert.exec(QStringLiteral("PRAGMA user_version = 3")));
        }
        Library library(dataDir, dir.filePath("cache"));
        QVERIFY2(library.open(), qPrintable(library.errorString()));
        QCOMPARE(library.schemaVersion(), 4);
        QCOMPARE(library.bookCount(), 4);
        QCOMPARE(library.record(3).title, QStringLiteral("Book 3"));

        library.setWantToRead(1, true);
        library.setWantToRead(3, true);  // finished books never sit on Want to read
        library.markListened(2);
        library.markListened(2);
        const QVariantMap counts = library.shelfCounts();
        QCOMPARE(counts.value("all").toInt(), 4);
        QCOMPARE(counts.value("reading").toInt(), 1);
        QCOMPARE(counts.value("finished").toInt(), 1);
        QCOMPARE(counts.value("want").toInt(), 1);
        QCOMPARE(counts.value("listening").toInt(), 1);
        QCOMPARE(library.book(1).value("wantToRead").toBool(), true);
        QCOMPARE(library.book(3).value("wantToRead").toBool(), false);
        QCOMPARE(library.book(2).value("chapterTitle").toString(), QStringLiteral("Chapter III"));
        QCOMPARE(library.book(1).value("chapterTitle").toString(), QString());

        library.works()->setStatusFilter(QStringLiteral("want"));
        QTRY_COMPARE(library.works()->rowCount(), 1);
        QCOMPARE(library.works()->get(0).value("bookId").toInt(), 1);
        library.works()->setStatusFilter(QStringLiteral("listening"));
        QTRY_COMPARE(library.works()->rowCount(), 1);
        QCOMPARE(library.works()->get(0).value("bookId").toInt(), 2);

        // Kept across a restart.
        Library reopened(dataDir, dir.filePath("cache"));
        QVERIFY(reopened.open());
        QVERIFY(reopened.record(1).wantAt > 0);
        QVERIFY(reopened.record(2).listenedAt > 0);
        reopened.setWantToRead(1, false);
        QCOMPARE(reopened.shelfCounts().value("want").toInt(), 0);
    }

    void librarySchemaMigrationsCollectionsStatsAndSeriesSort() {
        QTemporaryDir dir;
        const QString dataDir = dir.filePath("data");
        const QString cacheDir = dir.filePath("cache");
        QVERIFY(QDir().mkpath(dataDir));
        const QString dbPath = dataDir + QStringLiteral("/library.sqlite");

        // 1. Simulate a pre-v1 (user_version = 0) legacy database without fingerprint/collections/prefix/suffix.
        {
            QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-v0"));
            legacy.setDatabaseName(dbPath);
            QVERIFY(legacy.open());
            QSqlQuery q(legacy);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE folders (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, added_at INTEGER NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE books ("
                " id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, format TEXT NOT NULL,"
                " size INTEGER NOT NULL DEFAULT 0, mtime INTEGER NOT NULL DEFAULT 0,"
                " title TEXT NOT NULL DEFAULT '', sort_title TEXT NOT NULL DEFAULT '',"
                " author TEXT NOT NULL DEFAULT '', sort_author TEXT NOT NULL DEFAULT '',"
                " editor TEXT NOT NULL DEFAULT '', publisher TEXT NOT NULL DEFAULT '', year INTEGER NOT NULL DEFAULT 0,"
                " language TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '', subjects TEXT NOT NULL DEFAULT '',"
                " isbn TEXT NOT NULL DEFAULT '', series TEXT NOT NULL DEFAULT '', series_index REAL NOT NULL DEFAULT 0,"
                " work_key TEXT NOT NULL DEFAULT '', chapters INTEGER NOT NULL DEFAULT 0, pages INTEGER NOT NULL DEFAULT 0,"
                " length INTEGER NOT NULL DEFAULT 0, cover TEXT NOT NULL DEFAULT '', problem TEXT NOT NULL DEFAULT '',"
                " added_at INTEGER NOT NULL DEFAULT 0, opened_at INTEGER NOT NULL DEFAULT 0,"
                " finished_at INTEGER NOT NULL DEFAULT 0, progress REAL NOT NULL DEFAULT 0, position TEXT NOT NULL DEFAULT '',"
                " seconds_read INTEGER NOT NULL DEFAULT 0, missing INTEGER NOT NULL DEFAULT 0,"
                " hidden INTEGER NOT NULL DEFAULT 0, loose INTEGER NOT NULL DEFAULT 0)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE annotations ("
                " id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, kind TEXT NOT NULL,"
                " chapter INTEGER NOT NULL DEFAULT 0, start INTEGER NOT NULL DEFAULT 0, finish INTEGER NOT NULL DEFAULT 0,"
                " text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '',"
                " label TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO books (id, path, format, title, sort_title, author, sort_author, work_key, added_at) "
                "VALUES (1, '/legacy/book.epub', 'epub', 'Legacy Book', 'legacy book', 'Old Author', 'author, old', 'wk1', 100)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color, label, created_at, updated_at) "
                "VALUES (1, 1, 'highlight', 0, 5, 15, 'kept quote', 'kept note', 'yellow', 'Ch 1', 100, 100)")));
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 0")));
            legacy.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy-v0"));

        // Opening migrates v0 -> v1 -> v2 -> v3 -> v4 cleanly while preserving legacy rows.
        {
            Library migrated(dataDir, cacheDir);
            QVERIFY2(migrated.open(), qPrintable(migrated.errorString()));
            QCOMPARE(migrated.schemaVersion(), 4);
            QCOMPARE(migrated.record(1).title, QStringLiteral("Legacy Book"));
            QCOMPARE(migrated.record(1).wantAt, qint64(0));
            QCOMPARE(migrated.record(1).listenedAt, qint64(0));

            auto hasCol = [&](const QString &table, const QString &col) {
                QSqlQuery info(migrated.database());
                if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
                    return false;
                while (info.next())
                    if (info.value(1).toString() == col)
                        return true;
                return false;
            };
            QVERIFY(hasCol(QStringLiteral("books"), QStringLiteral("fingerprint")));
            QVERIFY(hasCol(QStringLiteral("annotations"), QStringLiteral("prefix")));
            QVERIFY(hasCol(QStringLiteral("annotations"), QStringLiteral("suffix")));

            QSqlQuery ann(migrated.database());
            QVERIFY(ann.exec(QStringLiteral("SELECT text, note, prefix, suffix FROM annotations WHERE id = 1")));
            QVERIFY(ann.next());
            QCOMPARE(ann.value(0).toString(), QStringLiteral("kept quote"));
            QCOMPARE(ann.value(1).toString(), QStringLiteral("kept note"));
            QCOMPARE(ann.value(2).toString(), QString());
            QCOMPARE(ann.value(3).toString(), QString());
        }

        // 2. Collections CRUD, readingStats(), and WorksModel series sort on a fresh Library.
        QTemporaryDir freshDir;
        Library library(freshDir.filePath("data"), freshDir.filePath("cache"));
        QVERIFY(library.open());
        QCOMPARE(library.schemaVersion(), 4);

        auto makeSeriesEpub = [&](const QString &fileName, const QString &title, const QString &author,
                                  const QString &series, double index) {
            const QString path = freshDir.filePath(fileName);
            const QString seriesMeta =
                series.isEmpty()
                    ? QString()
                    : QStringLiteral("<meta name=\"calibre:series\" content=\"%1\"/>"
                                     "<meta name=\"calibre:series_index\" content=\"%2\"/>")
                          .arg(series)
                          .arg(index);
            const QByteArray opf =
                QStringLiteral(
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                    "    <dc:title>%1</dc:title><dc:creator>%2</dc:creator><dc:language>en</dc:language>\n"
                    "    %3\n"
                    "  </metadata>\n"
                    "  <manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
                    "  <spine><itemref idref=\"c1\"/></spine>\n"
                    "</package>")
                    .arg(title, author, seriesMeta)
                    .toUtf8();
            const bool ok = TestBooks::write(path, TestBooks::epub({
                                                       {"OEBPS/content.opf", opf},
                                                       {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Text.</p>")},
                                                   }));
            return ok ? library.addBookFile(path) : 0;
        };

        const int bDune2 = makeSeriesEpub("d2.epub", "Dune Messiah", "Frank Herbert", "Dune", 2.0);
        const int bDune1 = makeSeriesEpub("d1.epub", "Dune", "Frank Herbert", "Dune", 1.0);
        const int bAlpha1 = makeSeriesEpub("a1.epub", "Alpha Rising", "Cara Lin", "Alpha", 1.0);
        const int bStandalone = makeSeriesEpub("z1.epub", "Zeta Standalone", "Zed Author", "", 0.0);
        QVERIFY(bDune2 > 0 && bDune1 > 0 && bAlpha1 > 0 && bStandalone > 0);

        // Series sort places series books alphabetically by series then ascending by seriesIndex, standalones last.
        library.works()->setSortOrder(QStringLiteral("series"));
        QTRY_COMPARE(library.works()->count(), 4);
        QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), bAlpha1);
        QCOMPARE(library.works()->get(1).value(QStringLiteral("bookId")).toInt(), bDune1);
        QCOMPARE(library.works()->get(2).value(QStringLiteral("bookId")).toInt(), bDune2);
        QCOMPARE(library.works()->get(3).value(QStringLiteral("bookId")).toInt(), bStandalone);
        QCOMPARE(library.works()->seriesList(), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Dune")}));

        library.works()->setSeriesFilter(QStringLiteral("Dune"));
        QCOMPARE(library.works()->count(), 2);
        QVERIFY(library.works()->hasActiveFilters());
        library.works()->clearFilters();
        QCOMPARE(library.works()->count(), 4);

        // Collections CRUD + case-insensitive dedup + WorksModel filter + delete reset.
        QCOMPARE(library.createCollection(QStringLiteral("   ")), 0);
        const int colSciFi = library.createCollection(QStringLiteral("Sci-Fi"));
        QVERIFY(colSciFi > 0);
        QCOMPARE(library.createCollection(QStringLiteral("sci-fi")), colSciFi);
        const int colFav = library.createCollection(QStringLiteral("Favorites"));
        QVERIFY(colFav > 0 && colFav != colSciFi);

        QVERIFY(library.addToCollection(bDune1, colSciFi));
        QVERIFY(library.addToCollection(bDune2, colSciFi));
        QVERIFY(library.isInCollection(bDune1, colSciFi));
        QCOMPARE(library.collectionsForBook(bDune1).size(), 1);

        library.works()->setCollectionId(colSciFi);
        QCOMPARE(library.works()->count(), 2);

        QVERIFY(!library.renameCollection(colSciFi, QStringLiteral("favorites")));  // duplicate conflict rejected
        QVERIFY(library.renameCollection(colSciFi, QStringLiteral("Speculative")));
        QCOMPARE(library.collectionsForBook(bDune1).first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Speculative"));

        QVERIFY(library.removeFromCollection(bDune2, colSciFi));
        QVERIFY(!library.isInCollection(bDune2, colSciFi));
        QCOMPARE(library.works()->count(), 1);

        QVERIFY(library.deleteCollection(colSciFi));
        QCOMPARE(library.works()->collectionId(), 0);
        QCOMPARE(library.works()->count(), 4);
        QVERIFY(library.collectionsForBook(bDune1).isEmpty());

        // readingStats() aggregates book statuses, streaks, 14-day history, and bookReadingStats().
        library.setFinished(bDune1, true);
        library.markOpened(bDune2);
        library.addReadingTime(bDune1, 600, 12000);  // today: 600s
        const QDate today = QDate::currentDate();
        auto addDay = [&](int daysAgo, int sec) {
            QSqlQuery ins(library.database());
            ins.prepare(QStringLiteral("INSERT OR REPLACE INTO reading_days (book_id, day, seconds) VALUES (?, ?, ?)"));
            ins.addBindValue(bDune2);
            ins.addBindValue(today.addDays(-daysAgo).toString(Qt::ISODate));
            ins.addBindValue(sec);
            QVERIFY(ins.exec());
        };
        // Current 3-day streak (today, -1, -2) and an older 4-day longest streak (-10..-7).
        addDay(1, 300);
        addDay(2, 200);
        addDay(7, 100);
        addDay(8, 100);
        addDay(9, 100);
        addDay(10, 100);

        const QVariantMap stats = library.readingStats();
        QCOMPARE(stats.value(QStringLiteral("totalBooks")).toInt(), 4);
        QCOMPARE(stats.value(QStringLiteral("finishedBooks")).toInt(), 1);
        QCOMPARE(stats.value(QStringLiteral("readingBooks")).toInt(), 1);
        QCOMPARE(stats.value(QStringLiteral("unreadBooks")).toInt(), 2);
        QCOMPARE(stats.value(QStringLiteral("todaySeconds")).toLongLong(), qint64(600));
        QCOMPARE(stats.value(QStringLiteral("weekSeconds")).toLongLong(), qint64(1100));
        QCOMPARE(stats.value(QStringLiteral("streakDays")).toInt(), 3);
        QCOMPARE(stats.value(QStringLiteral("longestStreak")).toInt(), 4);
        QCOMPARE(stats.value(QStringLiteral("activeDays")).toInt(), 7);
        const QVariantList recent = stats.value(QStringLiteral("recentDays")).toList();
        QCOMPARE(recent.size(), 14);
        QCOMPARE(recent.last().toMap().value(QStringLiteral("seconds")).toLongLong(), qint64(600));

        const QVariantMap bookStats = library.bookReadingStats(bDune1);
        QCOMPARE(bookStats.value(QStringLiteral("secondsRead")).toLongLong(), qint64(600));
        QVERIFY(bookStats.value(QStringLiteral("daysRead")).toInt() >= 1);
    }

    void marksW3cQuoteHealingCrossBookSearchAndObsidianExport() {
        // 1. Marks::locateQuote() disambiguates repeated phrases by W3C prefix/suffix and handles whitespace drift.
        const QString chapter = QStringLiteral("At dawn the harbor slept quietly. By noon the harbor roared with wind.");
        const qint64 secondHarbor = chapter.indexOf(QStringLiteral("the harbor"), 15);
        QVERIFY(secondHarbor > 15);
        const auto disamb = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                               QStringLiteral("By noon "), QStringLiteral(" roared"), 0);
        QCOMPARE(disamb.first, secondHarbor);
        QCOMPARE(disamb.second, secondHarbor + 10);

        const QString wrappedChapter = QStringLiteral("Intro text.\nThe\n  harbor   roared\tloudly.");
        const auto wsHit = Marks::locateQuote(wrappedChapter, QStringLiteral("The harbor roared"),
                                              QStringLiteral("Intro text."), QStringLiteral(" loudly"), -1);
        QVERIFY(wsHit.first >= 0);
        QCOMPARE(wrappedChapter.mid(wsHit.first, wsHit.second - wsHit.first),
                 QStringLiteral("The\n  harbor   roared"));
        QCOMPARE(Marks::locateQuote(chapter, QStringLiteral("nonexistent phrase"), {}, {}, 0),
                 (QPair<qint64, qint64>{-1, -1}));

        // 2. healChapter(), searchAll(), and Obsidian/Readwise exportMarkdownToFile().
        QTemporaryDir dir;
        const QString epub1 = dir.filePath("b1.epub");
        const QString epub2 = dir.filePath("b2.epub");
        QVERIFY(TestBooks::write(epub1, TestBooks::epub2(png(20, 30))));
        QVERIFY(TestBooks::write(epub2, TestBooks::epub3(adobeObfuscate(fontPayload()))));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int id1 = library.addBookFile(epub1);
        const int id2 = library.addBookFile(epub2);
        QVERIFY(id1 > 0 && id2 > 0);

        Marks marks(&library);
        marks.setBook(id1);
        const int h1 = marks.addHighlight(0, 0, 10, QStringLiteral("the harbor"), QStringLiteral("blue"),
                                          QStringLiteral("Ch. 1"), QStringLiteral("By noon "),
                                          QStringLiteral(" roared"));
        const int b1 = marks.addBookmark(0, 0, QStringLiteral("the harbor"), QStringLiteral("p. 1"),
                                         QStringLiteral("At dawn "), QStringLiteral(" slept"));
        marks.setNote(h1, QStringLiteral("Great café atmosphere"));

        // resolveOffset() detects drift before healing.
        const QVariantMap resolved = marks.resolveOffset(h1, chapter);
        QVERIFY(resolved.value(QStringLiteral("found")).toBool());
        QVERIFY(resolved.value(QStringLiteral("drifted")).toBool());
        QCOMPARE(resolved.value(QStringLiteral("start")).toLongLong(), secondHarbor);
        QCOMPARE(resolved.value(QStringLiteral("end")).toLongLong(), secondHarbor + 10);

        // healChapter() fixes both highlight and bookmark, persists to SQLite, and is idempotent.
        QCOMPARE(marks.healChapter(0, chapter), 2);
        QCOMPARE(marks.healChapter(0, chapter), 0);
        QCOMPARE(marks.mark(h1).value(QStringLiteral("start")).toLongLong(), secondHarbor);
        QCOMPARE(marks.mark(h1).value(QStringLiteral("end")).toLongLong(), secondHarbor + 10);
        const qint64 firstHarbor = chapter.indexOf(QStringLiteral("the harbor"));
        QCOMPARE(marks.mark(b1).value(QStringLiteral("start")).toLongLong(), firstHarbor);
        QCOMPARE(marks.mark(b1).value(QStringLiteral("end")).toLongLong(), firstHarbor);

        // Reloading Marks verifies healed offsets persisted in SQLite.
        {
            Marks reloaded(&library);
            reloaded.setBook(id1);
            QCOMPARE(reloaded.mark(h1).value(QStringLiteral("start")).toLongLong(), secondHarbor);
            QCOMPARE(reloaded.mark(b1).value(QStringLiteral("start")).toLongLong(), firstHarbor);
        }

        // Add a mark in Book 2 for cross-book searchAll().
        marks.setBook(id2);
        const int h2 = marks.addHighlight(0, 0, 12, QStringLiteral("Café on salt"), QStringLiteral("green"),
                                          QStringLiteral("p. 7"));
        QVERIFY(h2 > 0);
        marks.setBook(id1);

        // Accent-folded searchAll("cafe") returns hits from both books and prioritizes the active book + note match.
        const QVariantList foundBoth = marks.searchAll(QStringLiteral("cafe"));
        QCOMPARE(foundBoth.size(), 2);
        QCOMPARE(foundBoth.first().toMap().value(QStringLiteral("bookId")).toInt(), id1);
        QCOMPARE(foundBoth.last().toMap().value(QStringLiteral("bookId")).toInt(), id2);

        // Hidden books are excluded from searchAll().
        library.setHidden(id2, true);
        const QVariantList foundVisible = marks.searchAll(QStringLiteral("cafe"));
        QCOMPARE(foundVisible.size(), 1);
        QCOMPARE(foundVisible.first().toMap().value(QStringLiteral("bookId")).toInt(), id1);

        // Obsidian & Readwise exportMarkdownToFile() writes YAML frontmatter and metadata atomically.
        const QString obsidianPath = dir.filePath("exports/notes-obsidian.md");
        const QString writtenObs = marks.exportMarkdownToFile(
            QUrl::fromLocalFile(obsidianPath).toString(), QStringLiteral("The \"Lantern\""),
            QStringLiteral("Ada Marsh"), QStringLiteral("obsidian"));
        QCOMPARE(writtenObs, obsidianPath);
        QFile obsFile(obsidianPath);
        QVERIFY(obsFile.open(QIODevice::ReadOnly));
        const QString obsContent = QString::fromUtf8(obsFile.readAll());
        QVERIFY(obsContent.startsWith(QStringLiteral("---\n")));
        QVERIFY(obsContent.contains(QStringLiteral("title: \"The \\\"Lantern\\\"\"\n")));
        QVERIFY(obsContent.contains(QStringLiteral("author: \"Ada Marsh\"\n")));
        QVERIFY(obsContent.contains(QStringLiteral("tags:\n  - clothbound\n  - reading-notes\n")));
        QVERIFY(obsContent.contains(QStringLiteral("> the harbor\n")));
        QVERIFY(obsContent.contains(QStringLiteral("Great café atmosphere\n")));
        QVERIFY(obsContent.contains(QStringLiteral("— *Blue · Ch. 1 · ")));

        const QString readwisePath = dir.filePath("exports/notes-readwise.md");
        QCOMPARE(marks.exportMarkdownToFile(readwisePath, QStringLiteral("Lantern"), QStringLiteral("Ada Marsh"),
                                            QStringLiteral("readwise")),
                 readwisePath);
        QFile rwFile(readwisePath);
        QVERIFY(rwFile.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(rwFile.readAll()).startsWith(QStringLiteral("---\n")));

        const QString stdPath = dir.filePath("exports/notes-standard.md");
        QCOMPARE(marks.exportMarkdownToFile(stdPath, QStringLiteral("Lantern"), QStringLiteral("Ada Marsh"),
                                            QStringLiteral("standard")),
                 stdPath);
        QFile stdFile(stdPath);
        QVERIFY(stdFile.open(QIODevice::ReadOnly));
        const QString stdContent = QString::fromUtf8(stdFile.readAll());
        QVERIFY(!stdContent.startsWith(QStringLiteral("---\n")));
        QVERIFY(stdContent.startsWith(QStringLiteral("# Lantern\n")));
    }

    void cliVersionFastPathExitsCleanlyWithoutDisplay() {
        const QString clothboundBin = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../clothbound"));
        if (!QFileInfo::exists(clothboundBin))
            QSKIP("build/clothbound binary not built yet");

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("DISPLAY"));
        env.remove(QStringLiteral("WAYLAND_DISPLAY"));
        env.remove(QStringLiteral("QT_QPA_PLATFORM"));
        proc.setProcessEnvironment(env);

        QElapsedTimer timer;
        timer.start();
        proc.start(clothboundBin, {QStringLiteral("--version")});
        QVERIFY(proc.waitForFinished(5000));
        const qint64 elapsedMs = timer.elapsed();

        QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
        QCOMPARE(proc.exitCode(), 0);
        const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        QVERIFY2(out.startsWith(QStringLiteral("clothbound ")), qPrintable(out));
        QVERIFY2(QRegularExpression(QStringLiteral("^clothbound \\d+\\.\\d+\\.\\d+")).match(out).hasMatch(), qPrintable(out));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QVERIFY2(elapsedMs < 2000, qPrintable(QStringLiteral("slow --version: %1 ms").arg(elapsedMs)));

        // --help also completes via the lightweight QCoreApplication fast path without display.
        QProcess helpProc;
        helpProc.setProcessEnvironment(env);
        helpProc.start(clothboundBin, {QStringLiteral("--help")});
        QVERIFY(helpProc.waitForFinished(5000));
        QCOMPARE(helpProc.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helpProc.exitCode(), 0);
        const QString helpOut = QString::fromUtf8(helpProc.readAllStandardOutput());
        QVERIFY(helpOut.contains(QStringLiteral("Usage:")));
        QVERIFY(QString::fromUtf8(helpProc.readAllStandardError()).trimmed().isEmpty());
    }

    // --- Workspace-return repaint / black-surface regression tests ----------

    void surfaceWatcherMergesChromiumFlagsCleanly() {
        QCOMPARE(SurfaceWatcher::mergeChromiumFlags({}),
                 QByteArray("--disable-backgrounding-occluded-windows"));
        QCOMPARE(SurfaceWatcher::mergeChromiumFlags("  --enable-features=Foo\t--ozone-platform=wayland  "),
                 QByteArray("--enable-features=Foo --ozone-platform=wayland --disable-backgrounding-occluded-windows"));
        // Idempotent when already present.
        const QByteArray once = SurfaceWatcher::mergeChromiumFlags("--foo --disable-backgrounding-occluded-windows --bar");
        QCOMPARE(once, QByteArray("--foo --disable-backgrounding-occluded-windows --bar"));
        QCOMPARE(SurfaceWatcher::mergeChromiumFlags(once), once);
        // Token-accurate: a flag merely sharing the prefix must not suppress adding the real flag.
        QCOMPARE(SurfaceWatcher::mergeChromiumFlags("--disable-backgrounding-occluded-windows-custom"),
                 QByteArray("--disable-backgrounding-occluded-windows-custom --disable-backgrounding-occluded-windows"));

        // applyChromiumFlags() updates QTWEBENGINE_CHROMIUM_FLAGS cleanly.
        const bool hadOrig = qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS");
        const QByteArray orig = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--use-gl=egl");
        SurfaceWatcher::applyChromiumFlags();
        QCOMPARE(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"),
                 QByteArray("--use-gl=egl --disable-backgrounding-occluded-windows"));
        if (hadOrig)
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", orig);
        else
            qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    }

    void surfaceWatcherHandlesExposeHyprlandAndSceneGraph() {
        // Hyprland .socket2.sock workspace event filter.
        QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("workspace>>2"));
        QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("workspacev2>>2,reading\n"));
        QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("focusedmon>>DP-1,2"));
        QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("focusedmonv2>>DP-1,2"));
        QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("activespecial>>scratch,DP-1"));
        QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("activespecialv2>>-98,special:scratch,DP-1"));
        QVERIFY(!SurfaceWatcher::isHyprWorkspaceEvent("activewindow>>kitty,zsh"));
        QVERIFY(!SurfaceWatcher::isHyprWorkspaceEvent("openwindow>>0x1,1,clothbound,Clothbound"));
        QVERIFY(!SurfaceWatcher::isHyprWorkspaceEvent(""));

        QTemporaryDir dir;
        const QString sockPath = dir.filePath(".socket2.sock");
        QLocalServer server;
        QVERIFY(server.listen(sockPath));
        QPointer<QLocalSocket> serverPeer;
        QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
            serverPeer = server.nextPendingConnection();
        });

        QQuickWindow win;
        win.resize(640, 480);
        win.show();
        QVERIFY(QTest::qWaitFor([&] { return win.isExposed(); }, 2000));

        SurfaceWatcher watcher;
        watcher.setHyprEventSocketForTesting(sockPath);
        watcher.attach(&win);
        QVERIFY(watcher.isExposed());
        QTRY_VERIFY(serverPeer != nullptr);

        QSignalSpy repaintSpy(&watcher, &SurfaceWatcher::repaintRequested);
        QSignalSpy exposedSpy(&watcher, &SurfaceWatcher::exposedChanged);

        // Unrelated Hyprland events must not trigger unnecessary repaints.
        serverPeer->write("activewindow>>kitty,term\n");
        serverPeer->flush();
        QTest::qWait(40);
        QCOMPARE(repaintSpy.count(), 0);

        // Returning to a workspace while tiled and unfocused must still request a repaint.
        serverPeer->write("workspacev2>>1,1\n");
        serverPeer->flush();
        QTRY_COMPARE(repaintSpy.count(), 1);
        QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("hyprWorkspace"));

        // Fragmented socket reads are buffered until newline.
        serverPeer->write("workspacev2>>");
        serverPeer->flush();
        QTest::qWait(30);
        QCOMPARE(repaintSpy.count(), 1);
        serverPeer->write("2,2\n");
        serverPeer->flush();
        QTRY_COMPARE(repaintSpy.count(), 2);
        QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("hyprWorkspace"));

        // Hide -> Show (unexposed -> exposed) emits exposedChanged and requests repaint on return.
        const int beforeHide = repaintSpy.count();
        win.hide();
        QTRY_VERIFY(!win.isExposed());
        QVERIFY(!watcher.isExposed());
        QVERIFY(exposedSpy.count() >= 1);

        // Workspace events while unexposed must not fire prematurely while hidden.
        serverPeer->write("workspacev2>>3,3\n");
        serverPeer->flush();
        QTest::qWait(40);
        QCOMPARE(repaintSpy.count(), beforeHide);

        win.show();
        QTRY_VERIFY(win.isExposed());
        QVERIFY(watcher.isExposed());
        QTRY_VERIFY(repaintSpy.count() > beforeHide);

        // Queued sceneGraphInitialized triggers a sceneGraph repaint when exposed.
        const int beforeSg = repaintSpy.count();
        emit win.sceneGraphInitialized();
        QTRY_VERIFY(repaintSpy.count() > beforeSg);
        QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("sceneGraph"));

        // forceItemUpdate safely marks parent, nested child items (like WebEngine's delegate), and window.
        QQuickItem parentItem(win.contentItem());
        parentItem.setSize(QSizeF(400, 300));
        QQuickItem childItem(&parentItem);
        childItem.setSize(QSizeF(400, 300));
        watcher.forceItemUpdate(&parentItem);
        watcher.forceItemUpdate(nullptr);

        // Detaching cleanly clears state without crashing on subsequent window events.
        watcher.attach(nullptr);
        QVERIFY(!watcher.isExposed());
    }

    void surfaceWatcherCoalescesReturnNudgesAndHyprBursts() {
        QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("expose")));
        QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("sceneGraph")));
        QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("visibility")));
        QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("hyprWorkspace")));
        QVERIFY(!SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("active")));
        QVERIFY(!SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("appActive")));
        QVERIFY(!SurfaceWatcher::isSurfaceNudgeReason(QString()));

        QTemporaryDir dir;
        const QString sockPath = dir.filePath(".socket2.sock");
        QLocalServer server;
        QVERIFY(server.listen(sockPath));
        QPointer<QLocalSocket> serverPeer;
        QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
            serverPeer = server.nextPendingConnection();
        });

        QQuickWindow win;
        win.resize(640, 480);
        win.show();
        QVERIFY(QTest::qWaitFor([&] { return win.isExposed(); }, 2000));

        SurfaceWatcher watcher;
        watcher.setHyprEventSocketForTesting(sockPath);
        watcher.attach(&win);
        QTRY_VERIFY(serverPeer != nullptr);

        QSignalSpy repaintSpy(&watcher, &SurfaceWatcher::repaintRequested);

        // Rapid multi-line Hyprland burst (workspace>> + workspacev2>> + focusedmon>>)
        // across separate flushes must coalesce into a single hyprWorkspace signal.
        serverPeer->write("workspace>>1\n");
        serverPeer->flush();
        QTest::qWait(5);
        serverPeer->write("workspacev2>>1,1\n");
        serverPeer->flush();
        QTest::qWait(5);
        serverPeer->write("focusedmon>>DP-1,1\n");
        serverPeer->flush();
        QTRY_COMPARE(repaintSpy.count(), 1);
        QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("hyprWorkspace"));
        QTest::qWait(50);
        QCOMPARE(repaintSpy.count(), 1);

        // Rapid back-to-back workspace return triggers must coalesce into ONE
        // surface nudge and never re-nudge mid-debounce (no strongTrigger bypass),
        // and ordinary focus switches ("active" / "appActive") never nudge.
        QVERIFY(watcher.takeSurfaceNudge(QStringLiteral("hyprWorkspace"), 400));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("visibility"), 400));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("sceneGraph"), 400));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("active"), 0));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("appActive"), 0));

        // Leaving a workspace (workspace event immediately followed by Expose(false))
        // must cancel the pending hyprWorkspace timer and reset cooldown for return.
        const int beforeLeave = repaintSpy.count();
        serverPeer->write("workspacev2>>2,2\n");
        serverPeer->flush();
        QTest::qWait(5);
        win.hide();
        QTRY_VERIFY(!win.isExposed());
        QTest::qWait(50);
        QCOMPARE(repaintSpy.count(), beforeLeave);
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));

        // Returning from another workspace (Expose false->true) immediately permits
        // a single fresh nudge even within the old cooldown window.
        win.show();
        QTRY_VERIFY(win.isExposed());
        QVERIFY(watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("hyprWorkspace"), 400));

        // Scene-graph invalidation resets the cooldown so rebuild gets a fresh nudge.
        emit win.sceneGraphInvalidated();
        QCoreApplication::processEvents();
        QVERIFY(watcher.takeSurfaceNudge(QStringLiteral("sceneGraph"), 400));
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));

        // Detached watcher never grants nudges.
        watcher.attach(nullptr);
        QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 0));
    }

    void readerSurfaceRefreshAvoidsElidedOutlineAndNoOpRelayout() {
        QFile jsFile(QFINDTESTDATA("../src/reader/reader.js"));
        QVERIFY(jsFile.open(QIODevice::ReadOnly));
        const QString js = QString::fromUtf8(jsFile.readAll());
        QVERIFY(!js.contains(QStringLiteral("1px solid transparent")));
        QVERIFY(!js.contains(QStringLiteral("root.style.opacity = '0.999'")));
        QVERIFY(!js.contains(QStringLiteral("root.style.transform = 'translateZ(0)'")));
        QVERIFY(js.contains(QStringLiteral("cb-paint-probe")));
        QVERIFY(js.contains(QStringLiteral("beginSurfaceNudge")));
        QVERIFY(js.contains(QStringLiteral("endSurfaceNudge")));
        QVERIFY(js.contains(QStringLiteral("isNudgeGuarded")));
        QVERIFY(js.contains(QStringLiteral("isNudgeDelta")));

        QFile qmlFile(QFINDTESTDATA("../src/ui/reader/EpubReader.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("backgroundColor: view.look.bg")));
        QVERIFY(qml.contains(QStringLiteral("target: surfaceWatcher")));
        QVERIFY(qml.contains(QStringLiteral("surfaceWatcher.forceItemUpdate")));
        QVERIFY(qml.contains(QStringLiteral("surfaceWatcher.takeSurfaceNudge")));
        QVERIFY(qml.contains(QStringLiteral("clothbound.beginSurfaceNudge")));
        QVERIFY(qml.contains(QStringLiteral("clothbound.endSurfaceNudge")));
        QVERIFY(qml.contains(QStringLiteral("anchors.bottomMargin: view.surfaceNudge")));
        QVERIFY(qml.contains(QStringLiteral("visible: win.visible && win.visibility !== Window.Minimized")));
        QVERIFY(qml.contains(QStringLiteral("WebEngineView.LifecycleState.Frozen")));
        QVERIFY(!qml.contains(QStringLiteral("strongTrigger")));
    }

    void dropCapOpensTheFirstRealParagraph() {
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (node.isEmpty())
            QSKIP("node executable not available");
        const QString script = QStringLiteral(R"JS(
const assert = require('assert');
const DropCap = require(process.argv[1]);
const long = 'The Time Traveller (for so it will be convenient to speak of him) was expounding a recondite matter to us. His grey eyes shone.';
// The first long paragraph of running text, past an epigraph and a short line.
assert.strictEqual(DropCap.choose([
    { text: 'To my friends.', excluded: false },
    { text: long, excluded: true },
    { text: 'Short.', excluded: false },
    { text: long, excluded: false },
]), 3);
// Opening quotes are skipped over; a digit or a CJK character means none.
assert.strictEqual(DropCap.choose([{ text: '“' + long, excluded: false }]), 0);
assert.strictEqual(DropCap.choose([{ text: '1854. ' + long, excluded: false }]), -1);
assert.strictEqual(DropCap.choose([{ text: '吾輩は猫である。'.repeat(20), excluded: false }]), -1);
// Cyrillic and Greek have capitals.
assert.strictEqual(DropCap.choose([{ text: 'Все счастливые семьи похожи друг на друга. '.repeat(4), excluded: false }]), 0);
// Nothing long enough near the top: no drop cap.
assert.strictEqual(DropCap.choose(Array(10).fill({ text: 'A line of verse.', excluded: false }).concat([{ text: long }])), -1);
assert.strictEqual(DropCap.choose([]), -1);
console.log('ok');
)JS");
        QProcess process;
        process.start(node, {"-e", script, QFINDTESTDATA("../src/reader/dropcap.js")});
        QVERIFY(process.waitForFinished(15000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
        QCOMPARE(process.readAllStandardOutput().trimmed(), QByteArray("ok"));
    }

    void readerJsSuppressesNudgeRelayoutAndAllowsRealResize() {
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (node.isEmpty())
            QSKIP("node executable not available");

        const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
        QVERIFY(QFileInfo::exists(readerJsPath));

        const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let resizeCallback = null;
let domReady = null;
const rafQueue = [];

function makeStyle() {
    return {
        cssText: '', opacity: '', transform: '', backgroundColor: '',
        left: '', top: '', width: '', height: '',
        setProperty(k, v) { this[k] = v; },
    };
}

const stageEl = {
    clientWidth: 1200,
    clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: this.clientWidth, height: this.clientHeight }; },
};

const styleNodes = new Map();
const textNode = { nodeType: 3, data: 'Hello world from Clothbound chapter text.', parentElement: null };
const bodyEl = {
    childNodes: [textNode],
    lastElementChild: {
        getClientRects() { return [{ left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }]; },
        getBoundingClientRect() { return { left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }; },
    },
};
textNode.parentElement = bodyEl;

const chapterDocEl = {
    style: makeStyle(),
    scrollWidth: 1200,
    scrollHeight: 692,
    scrollLeft: 0,
    scrollTop: 0,
    scrollTo(opts) {
        if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
        if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
    },
    appendChild() {},
};

const chapterDoc = {
    URL: 'clothbound://book/1/ch1.xhtml',
    documentElement: chapterDocEl,
    head: { appendChild(el) { styleNodes.set(el.id, el); } },
    body: bodyEl,
    scrollingElement: chapterDocEl,
    fonts: { ready: Promise.resolve() },
    images: [],
    defaultView: {
        getComputedStyle() { return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start' }; },
        CSS: { highlights: new Map() },
        Highlight: class {},
    },
    getElementById(id) { return styleNodes.get(id) || null; },
    createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    addEventListener() {},
    removeEventListener() {},
    createTreeWalker() {
        let done = false;
        return { nextNode() { if (done) return null; done = true; return textNode; } };
    },
    createRange() {
        return {
            selectNodeContents() {},
            setStart() {},
            setEnd() {},
            getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
            getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
        };
    },
    getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
};

const classes = new Set();
const frameEl = {
    style: makeStyle(),
    classList: {
        add(c) { classes.add(c); },
        remove(c) { classes.delete(c); },
        contains(c) { return classes.has(c); },
    },
    contentDocument: chapterDoc,
    contentWindow: chapterDoc.defaultView,
    onload: null,
    _src: '',
    set src(v) {
        this._src = v;
        if (typeof this.onload === 'function')
            queueMicrotask(() => this.onload());
    },
    get src() { return this._src; },
    getBoundingClientRect() { return { left: 0, top: 54, width: 800, height: 692 }; },
};

const bodyChildren = [];
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: {
        children: bodyChildren,
        appendChild(el) { bodyChildren.push(el); return el; },
    },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'book') return frameEl;
        if (id === 'overlay') return overlayEl;
        if (id === 'cb-paint-probe') return bodyChildren.find(c => c.id === 'cb-paint-probe') || null;
        return null;
    },
    createElement(tag) {
        return {
            tagName: tag.toUpperCase(),
            id: '',
            attrs: {},
            style: makeStyle(),
            setAttribute(k, v) { this.attrs[k] = v; },
        };
    },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

let relocatedCount = 0;
const bridge = {
    surfaceLoaded() {},
    relocated() { ++relocatedCount; },
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console,
    setTimeout,
    clearTimeout,
    performance,
    Promise,
    URL,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1200, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class {
        constructor(cb) { resizeCallback = cb; }
        observe() {}
    },
    matchMedia: () => ({ matches: false }),
    requestAnimationFrame: fn => { rafQueue.push(fn); return rafQueue.length; },
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

const wait = ms => new Promise(r => setTimeout(r, ms));
const flushRaf = () => {
    while (rafQueue.length) {
        const batch = rafQueue.splice(0, rafQueue.length);
        for (const fn of batch) fn();
    }
};

(async () => {
    const book = { session: 1, base: 'clothbound://book/1/', language: 'en', rtl: false,
                   spine: [{ href: 'ch1.xhtml', linear: true }], pageTargets: [] };
    const style = {
        theme: { bg: '#fbfaf6', fg: '#23221f', link: '#8a6a2a', muted: '#6f6b63', popover: '#fff',
                 border: 'rgba(0,0,0,0.12)', selection: '#cde', search: '#ff0', found: '#fa0',
                 markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f', markPink: '#f0f',
                 speaking: '#eef', dark: false },
        font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.4,
        margin: 0.06, measure: 34, justify: false, hyphenate: false, layout: 'single',
        animate: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
        listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
    };

    await clothbound.open(book, { chapter: 0, offset: 0 }, style);
    assert.strictEqual(relocatedCount, 1, 'initial open relocates once');
    assert.strictEqual(frameEl.style.height, '692px');

    // 1. Transient 1 px surface nudge (beginSurfaceNudge -> shrink -> restore -> endSurfaceNudge):
    // must NOT trigger relayout even if held longer than the 120 ms resize debounce.
    clothbound.beginSurfaceNudge(600);
    stageEl.clientHeight = 799;
    resizeCallback();
    await wait(180);
    assert.strictEqual(relocatedCount, 1, 'guarded 1px nudge must not trigger relayout');
    assert.strictEqual(frameEl.style.height, '692px', '#book height must remain locked during nudge');
    stageEl.clientHeight = 800;
    clothbound.endSurfaceNudge(80);
    resizeCallback();
    await wait(160);
    assert.strictEqual(relocatedCount, 1, 'restoring 1px nudge must not trigger relayout');

    // 2. IPC race: ResizeObserver fires BEFORE beginSurfaceNudge arrives.
    stageEl.clientHeight = 799;
    resizeCallback();
    await wait(10);
    clothbound.beginSurfaceNudge(600);
    await wait(170);
    assert.strictEqual(relocatedCount, 1, 'late beginSurfaceNudge must still suppress relayout');
    stageEl.clientHeight = 800;
    clothbound.endSurfaceNudge(60);
    resizeCallback();
    await wait(120);
    assert.strictEqual(relocatedCount, 1, 'restoring after late guard must not trigger relayout');

    // 3. refresh() must NOT toggle opacity/transform on document or chapter iframe,
    // and must use the 1x1 corner paint probe and clean it up cleanly.
    clothbound.refresh();
    assert.strictEqual(docEl.style.opacity, '', 'outer html opacity untouched');
    assert.strictEqual(docEl.style.transform, '', 'outer html transform untouched');
    assert.strictEqual(chapterDocEl.style.opacity, '', 'chapter html opacity untouched');
    assert.strictEqual(chapterDocEl.style.transform, '', 'chapter html transform untouched');
    const probe = documentMock.getElementById('cb-paint-probe');
    assert.ok(probe, 'paint probe created');
    assert.ok(probe.style.backgroundColor.startsWith('rgba('), 'paint probe painted with non-zero alpha');
    flushRaf();
    assert.strictEqual(probe.style.backgroundColor, '', 'paint probe cleaned up after double-rAF');

    // 4. Genuine user window resize STILL relayouts cleanly (even during a nudge guard window).
    clothbound.beginSurfaceNudge(600);
    stageEl.clientHeight = 650;
    resizeCallback();
    await wait(180);
    assert.strictEqual(relocatedCount, 2, 'genuine resize (>2px) must still relayout');
    assert.strictEqual(frameEl.style.height, '542px', '#book height updated for real resize');

    // 5. After guard expires, even a genuine 1 px user resize relayouts cleanly.
    clothbound.endSurfaceNudge(0);
    stageEl.clientHeight = 649;
    resizeCallback();
    await wait(180);
    assert.strictEqual(relocatedCount, 3, 'unguarded 1px user resize must still relayout');
    assert.strictEqual(frameEl.style.height, '541px');

    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");

        QProcess proc;
        proc.start(node, {QStringLiteral("-e"), harness, readerJsPath});
        QVERIFY2(proc.waitForFinished(10000), "node reader.js harness timed out");
        const QByteArray stderrOut = proc.readAllStandardError();
        QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                 stderrOut.isEmpty() ? "node reader.js harness failed" : stderrOut.constData());
    }

    // --- Wave 2 regression tests ------------------------------------------

    void wave2LibraryCancelScanAudioShelvesFinderAndIndexScoping() {
        QTemporaryDir dir;
        const QString dataDir = dir.filePath("data");
        const QString cacheDir = dir.filePath("cache");
        const QString booksDir = dir.filePath("books");
        QVERIFY(QDir().mkpath(booksDir));

        const QString epubA = booksDir + QStringLiteral("/alpha.epub");
        const QString epubB = booksDir + QStringLiteral("/beta.epub");
        const QString pdfPath = booksDir + QStringLiteral("/manual.pdf");
        QVERIFY(TestBooks::write(epubA, TestBooks::epub2(png(20, 30))));
        QVERIFY(TestBooks::write(epubB, TestBooks::epub3(adobeObfuscate(fontPayload()))));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Grid Systems", "Josef")));

        Library library(dataDir, cacheDir);
        QVERIFY2(library.open(), qPrintable(library.errorString()));

        // 1. cancelScan(): safe no-op while idle, stops an active scan cleanly.
        QVERIFY(!library.scanning());
        library.cancelScan();
        QVERIFY(!library.scanning());

        QSignalSpy scanningSpy(&library, &Library::scanningChanged);
        library.addFolder(booksDir);
        QVERIFY(library.scanning());
        library.cancelScan();
        QVERIFY(!library.scanning());
        QVERIFY(scanningSpy.count() >= 2);
        library.rescan();
        QVERIFY(library.scanning());
        library.cancelScan();
        QVERIFY(!library.scanning());

        // 2. Audio shelves ("audio_downloaded", "audiobook_saved"), shelfCounts, and book enrichment.
        const int idA = library.addBookFile(epubA);
        const int idB = library.addBookFile(epubB);
        const int idPdf = library.addBookFile(pdfPath);
        QVERIFY(idA > 0 && idB > 0 && idPdf > 0);

        QVariantMap counts = library.shelfCounts();
        QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 3);
        QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("downloaded")).toInt(), 0);
        QCOMPARE(counts.value(QStringLiteral("audiobooks")).toInt(), 0);

        library.markOpened(idA);
        library.savePosition(idA, QStringLiteral(R"({"chapter":0,"offset":12,"title":"Chapter One","snippet":"A lantern   in the fog."})"), 0.25);
        Marks marks(&library);
        marks.setBook(idA);
        QVERIFY(marks.addHighlight(0, 0, 10, QStringLiteral("The lamp"), QStringLiteral("yellow"), QStringLiteral("Ch 1")) > 0);
        QVERIFY(marks.addHighlight(0, 12, 20, QStringLiteral("sea wind"), QStringLiteral("blue"), QStringLiteral("Ch 1")) > 0);
        library.setBookSetting(idA, QStringLiteral("voiceName"), QStringLiteral("Emma"));
        library.setBookSetting(idA, QStringLiteral("audio_downloaded"), QStringLiteral("1"));
        library.setBookSetting(idB, QStringLiteral("audiobook_saved"), QStringLiteral("1"));

        const QVariantMap mapA = library.book(idA);
        QCOMPARE(mapA.value(QStringLiteral("downloaded")).toBool(), true);
        QCOMPARE(mapA.value(QStringLiteral("hasAudiobook")).toBool(), false);
        QCOMPARE(mapA.value(QStringLiteral("highlightCount")).toInt(), 2);
        QCOMPARE(mapA.value(QStringLiteral("voiceName")).toString(), QStringLiteral("Emma"));
        QCOMPARE(mapA.value(QStringLiteral("snippet")).toString(), QStringLiteral("A lantern in the fog."));

        const QVariantList cont = library.continueReading();
        QVERIFY(!cont.isEmpty());
        QCOMPARE(cont.first().toMap().value(QStringLiteral("id")).toInt(), idA);
        QCOMPARE(cont.first().toMap().value(QStringLiteral("highlightCount")).toInt(), 2);
        QCOMPARE(cont.first().toMap().value(QStringLiteral("voiceName")).toString(), QStringLiteral("Emma"));
        QCOMPARE(cont.first().toMap().value(QStringLiteral("snippet")).toString(), QStringLiteral("A lantern in the fog."));

        counts = library.shelfCounts();
        QCOMPARE(counts.value(QStringLiteral("downloaded")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("audiobooks")).toInt(), 1);

        library.works()->setStatusFilter(QStringLiteral("downloaded"));
        QTRY_COMPARE(library.works()->count(), 1);
        QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), idA);
        QCOMPARE(library.works()->data(library.works()->index(0), WorksModel::DownloadedRole).toBool(), true);

        library.works()->setStatusFilter(QStringLiteral("audiobooks"));
        QTRY_COMPARE(library.works()->count(), 1);
        QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), idB);
        QCOMPARE(library.works()->data(library.works()->index(0), WorksModel::AudiobookRole).toBool(), true);

        // Clearing audio_downloaded removes the book from the Downloaded shelf live.
        library.works()->setStatusFilter(QStringLiteral("downloaded"));
        QTRY_COMPARE(library.works()->count(), 1);
        library.setBookSetting(idA, QStringLiteral("audio_downloaded"), QStringLiteral("0"));
        QTRY_COMPARE(library.works()->count(), 0);
        QCOMPARE(library.shelfCounts().value(QStringLiteral("downloaded")).toInt(), 0);

        // Audio badges persist across restart via loadAudioBadges().
        {
            Library reopened(dataDir, cacheDir);
            QVERIFY(reopened.open());
            QCOMPARE(reopened.shelfCounts().value(QStringLiteral("audiobooks")).toInt(), 1);
            QCOMPARE(reopened.book(idB).value(QStringLiteral("hasAudiobook")).toBool(), true);
            reopened.works()->setStatusFilter(QStringLiteral("audiobooks"));
            QCOMPARE(reopened.works()->count(), 1);
            QCOMPARE(reopened.works()->get(0).value(QStringLiteral("bookId")).toInt(), idB);
        }

        // 3. LibraryFinder::countByFormat + countBooks compat + search() format breakdown.
        const QString home = dir.filePath("finder-home");
        for (const QString &rel : {QStringLiteral("Books/a.epub"), QStringLiteral("Books/sub/b.EPUB"),
                                   QStringLiteral("Books/c.PDF"), QStringLiteral("Books/notes.txt"),
                                   QStringLiteral("Books/1/2/3/4/5/deep.epub")}) {
            QVERIFY(QDir().mkpath(QFileInfo(home + QLatin1Char('/') + rel).absolutePath()));
            write(home + QLatin1Char('/') + rel, "x");
        }
        const LibraryFinder::FormatCounts fmt = LibraryFinder::countByFormat(home + QStringLiteral("/Books"));
        QCOMPARE(fmt.epub, 2);
        QCOMPARE(fmt.pdf, 1);
        QCOMPARE(fmt.total(), 3);
        QCOMPARE(LibraryFinder::countBooks(home + QStringLiteral("/Books")), 3);
        QCOMPARE(LibraryFinder::countBooks(home + QStringLiteral("/Books"), 4, 2), 2);
        const QVariantList found = LibraryFinder::search({home + QStringLiteral("/Books")}, {}, home);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().toMap().value(QStringLiteral("count")).toInt(), 3);
        QCOMPARE(found.first().toMap().value(QStringLiteral("epubCount")).toInt(), 2);
        QCOMPARE(found.first().toMap().value(QStringLiteral("pdfCount")).toInt(), 1);

        // 4. LibraryIndex::search(query, limit, bookId) scoping.
        const QString idxPath = dir.filePath("scoped-index.sqlite");
        LibraryIndex index(idxPath);
        if (index.available()) {
            const QString epub1 = dir.filePath("s1.epub");
            const QString epub2 = dir.filePath("s2.epub");
            auto simpleEpub = [](const QByteArray &title, const QByteArray &body) {
                return TestBooks::epub({
                    {"OEBPS/content.opf",
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                     "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">"
                     "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title + "</dc:title></metadata>"
                     "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                     "<spine><itemref idref=\"c\"/></spine></package>"},
                    {"OEBPS/c.xhtml", TestBooks::chapter(title, body)},
                });
            };
            QVERIFY(TestBooks::write(epub1, simpleEpub("One", "<p>The brass compass pointed toward the lighthouse.</p>")));
            QVERIFY(TestBooks::write(epub2, simpleEpub("Two", "<p>Another brass compass lay upon the wooden chart.</p>")));
            QVERIFY(LibraryIndex::indexBook(idxPath, {11, epub1, QStringLiteral("epub"), 100}));
            QVERIFY(LibraryIndex::indexBook(idxPath, {22, epub2, QStringLiteral("epub"), 100}));

            const QVariantList allHits = index.search(QStringLiteral("compass"), 8);
            QCOMPARE(allHits.size(), 2);
            const QVariantList only11 = index.search(QStringLiteral("compass"), 8, 11);
            QCOMPARE(only11.size(), 1);
            QCOMPARE(only11.first().toMap().value(QStringLiteral("bookId")).toInt(), 11);
            const QVariantList only22 = index.search(QStringLiteral("compass"), 8, 22);
            QCOMPARE(only22.size(), 1);
            QCOMPARE(only22.first().toMap().value(QStringLiteral("bookId")).toInt(), 22);
            QVERIFY(index.search(QStringLiteral("compass"), 8, 999).isEmpty());
        }
    }

    void wave2ReaderBookPagesPreviousOpenedAtAndSurfaceProtections() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("pages.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

        // Build a 4-page PDF for PDF bookPageStart/bookPageEnd/previousOpenedAt tests.
        const QString pdfPath = dir.filePath("four.pdf");
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QPageSize::A5));
            writer.setResolution(72);
            QPainter painter(&writer);
            for (int p = 0; p < 4; ++p) {
                if (p > 0)
                    writer.newPage();
                painter.drawText(QPointF(30, 60), QStringLiteral("PDF page %1 content.").arg(p + 1));
            }
        }

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int epubId = library.addBookFile(epubPath);
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(epubId > 0 && pdfId > 0);
        // 6600 total chars / 1650 chars-per-page = 4 estimated book pages.
        library.setChapterLengths(epubId, {3300, 3300});

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QCOMPARE(reader.paragraphSpacing(), 0.0);

        // Fresh EPUB open: previousOpenedAt == 0, then markOpened sets openedAt > 0.
        QVERIFY(reader.openBook(epubId));
        QCOMPARE(reader.previousOpenedAt(), qint64(0));
        QVERIFY(library.record(epubId).openedAt > 0);

        reader.relocated({{"chapter", 0}, {"offset", 0}, {"page", 0}, {"pages", 2}, {"columns", 1}});
        QCOMPARE(reader.visibleColumns(), 1);
        QCOMPARE(reader.bookPages(), 4);
        QCOMPARE(reader.bookPageStart(), 1);
        QCOMPARE(reader.bookPageEnd(), 1);

        reader.relocated({{"chapter", 0}, {"offset", 0}, {"page", 0}, {"pages", 1}, {"columns", 2}});
        QCOMPARE(reader.visibleColumns(), 2);
        QCOMPARE(reader.bookPageStart(), 1);
        QCOMPARE(reader.bookPageEnd(), 2);

        reader.relocated({{"chapter", 1}, {"offset", 1650}, {"page", 0}, {"pages", 1}, {"columns", 2}, {"atEnd", true}});
        QCOMPARE(reader.bookPageStart(), 3);
        QCOMPARE(reader.bookPageEnd(), 4);

        reader.close();
        QCOMPARE(reader.previousOpenedAt(), qint64(0));
        QCOMPARE(reader.bookPages(), 1);
        QCOMPARE(reader.visibleColumns(), 1);

        // Seed past opened_at timestamps and verify previousOpenedAt captures them BEFORE markOpened().
        {
            QSqlQuery q(library.database());
            QVERIFY(q.exec(QStringLiteral("UPDATE books SET opened_at = 1700000123 WHERE id = %1").arg(epubId)));
            QVERIFY(q.exec(QStringLiteral("UPDATE books SET opened_at = 1700000456 WHERE id = %1").arg(pdfId)));
        }
        Library reopened(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(reopened.open());
        Reader reader2(&reopened, &scheme);

        QVERIFY(reader2.openBook(epubId));
        QCOMPARE(reader2.previousOpenedAt(), qint64(1700000123));
        QVERIFY(reopened.record(epubId).openedAt > 1700000123);
        reader2.close();

        QVERIFY(reader2.openBook(pdfId));
        QCOMPARE(reader2.previousOpenedAt(), qint64(1700000456));
        QVERIFY(reopened.record(pdfId).openedAt > 1700000456);
        reader2.relocated({{"chapter", 1}, {"offset", 0}, {"page", 1}, {"pages", 4}, {"columns", 1}});
        QCOMPARE(reader2.bookPages(), 4);
        QCOMPARE(reader2.bookPageStart(), 2);
        QCOMPARE(reader2.bookPageEnd(), 2);
        reader2.relocated({{"chapter", 1}, {"offset", 0}, {"page", 1}, {"pages", 4}, {"columns", 2}, {"visiblePageEnd", 2}});
        QCOMPARE(reader2.visibleColumns(), 2);
        QCOMPARE(reader2.bookPageStart(), 2);
        QCOMPARE(reader2.bookPageEnd(), 3);
        reader2.close();

        // reader.js Node VM test: singleColumn docking, cb-spoken dimming/clearing, and 2-column bleed containment.
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (node.isEmpty())
            QSKIP("node executable not available");
        const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
        const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}

const stageEl = {
    clientWidth: 1400, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: this.clientWidth, height: this.clientHeight }; },
};

const styleNodes = new Map();
const textNode = { nodeType: 3, data: 'Filmer worked in Shoreditch and Dymchurch on his machine.', parentElement: null };
const nowrapP = { localName: 'p', _ws: 'nowrap', style: makeStyle() };
const preDiv = { localName: 'div', _ws: 'pre', style: makeStyle() };
const tableCell = { localName: 'td', _ws: 'nowrap', style: makeStyle() };
const preEl = { localName: 'pre', _ws: 'pre', style: makeStyle() };
const bodyEl = {
    childNodes: [textNode],
    lastElementChild: {
        getClientRects() { return [{ left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }]; },
        getBoundingClientRect() { return { left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }; },
    },
};
textNode.parentElement = bodyEl;

const chapterDocEl = {
    style: makeStyle(), scrollWidth: 1400, scrollHeight: 692, scrollLeft: 0, scrollTop: 0,
    scrollTo(opts) {
        if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
        if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
    },
    appendChild() {},
};

const highlights = new Map();
const chapterDoc = {
    URL: 'clothbound://book/1/ch1.xhtml',
    documentElement: chapterDocEl,
    head: { appendChild(el) { styleNodes.set(el.id, el); } },
    body: bodyEl,
    scrollingElement: chapterDocEl,
    fonts: { ready: Promise.resolve() },
    images: [],
    defaultView: {
        getComputedStyle(el) {
            return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start',
                     whiteSpace: (el && el._ws) || 'normal' };
        },
        CSS: { highlights },
        Highlight: class { constructor(...ranges) { this.ranges = ranges; this.priority = 0; } },
    },
    getElementById(id) { return styleNodes.get(id) || null; },
    createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
    querySelector() { return null; },
    querySelectorAll(sel) {
        if (sel === 'body *') return [nowrapP, preDiv, tableCell, preEl];
        return [];
    },
    addEventListener() {},
    removeEventListener() {},
    createTreeWalker() {
        let done = false;
        return { nextNode() { if (done) return null; done = true; return textNode; } };
    },
    createRange() {
        return {
            start: 0, end: 0,
            selectNodeContents() {},
            setStart(_n, o) { this.start = o; },
            setEnd(_n, o) { this.end = o; },
            getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
            getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
        };
    },
    getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
};

const classes = new Set();
const frameEl = {
    style: makeStyle(),
    classList: { add(c) { classes.add(c); }, remove(c) { classes.delete(c); }, contains(c) { return classes.has(c); } },
    contentDocument: chapterDoc,
    contentWindow: chapterDoc.defaultView,
    onload: null,
    _src: '',
    set src(v) { this._src = v; if (typeof this.onload === 'function') queueMicrotask(() => this.onload()); },
    get src() { return this._src; },
    getBoundingClientRect() { return { left: 0, top: 54, width: 1200, height: 692 }; },
};

const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'book') return frameEl;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement(tag) { return { tagName: tag.toUpperCase(), id: '', style: makeStyle(), setAttribute() {} }; },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

let lastLoc = null;
const bridge = {
    surfaceLoaded() {},
    relocated(loc) { lastLoc = loc; },
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console, setTimeout, clearTimeout, performance, Promise, URL,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1400, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class { observe() {} },
    matchMedia: () => ({ matches: false }),
    requestAnimationFrame: fn => { fn(performance.now()); return 1; },
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

(async () => {
    const book = { session: 1, base: 'clothbound://book/1/', language: 'en', rtl: false,
                   spine: [{ href: 'ch1.xhtml', linear: true }], pageTargets: [] };
    const baseStyle = {
        theme: { bg: '#fbfaf6', fg: '#23221f', link: '#8a6a2a', accentInk: '#7a5a1a', muted: '#6f6b63',
                 popover: '#fff', border: 'rgba(0,0,0,0.12)', selection: '#cde', search: '#ff0', found: '#fa0',
                 markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f', markPink: '#f0f',
                 speaking: '#eef', dark: false },
        font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.0,
        margin: 0.06, measure: 34, justify: true, hyphenate: false, layout: 'double', singleColumn: false,
        animate: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
        listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
    };

    await clothbound.open(book, { chapter: 0, offset: 0 }, baseStyle);
    assert.strictEqual(lastLoc.columns, 2, 'double layout on 1400px uses 2 columns');

    // Verify 2-column horizontal bleed containment CSS & fitBlocks nowrap/pre neutralization.
    const typoCss = styleNodes.get('cb-typography').textContent;
    const layCss = styleNodes.get('cb-layout').textContent;
    assert.ok(typoCss.includes('overflow-wrap: break-word'), 'typographyCss enables break-word');
    assert.ok(typoCss.includes('text-wrap-mode: wrap !important'), 'typographyCss forces wrap mode');
    assert.ok(typoCss.includes('nobr { white-space: normal !important; }'), 'typographyCss neutralizes nobr');
    assert.ok(layCss.includes('column-span: none !important'), 'layoutCss blocks column-span');
    assert.ok(layCss.includes('max-width: 100% !important'), 'layoutCss caps child max-width');
    assert.ok(layCss.includes('overflow-x: clip'), 'layoutCss clips column horizontal overflow');
    assert.deepStrictEqual(nowrapP.style.props['white-space'], { value: 'normal', prio: 'important' });
    assert.deepStrictEqual(preDiv.style.props['white-space'], { value: 'pre-wrap', prio: 'important' });
    assert.strictEqual(tableCell.style.props['white-space'], undefined, 'table cells untouched');
    assert.strictEqual(preEl.style.props['white-space'], undefined, 'pre blocks handled by CSS');

    // Companion docked seam: singleColumn forces 1 column even in layout=double, then restores 2.
    await clothbound.setStyle({ ...baseStyle, singleColumn: true });
    assert.strictEqual(lastLoc.columns, 1, 'singleColumn forces 1 column');
    await clothbound.setStyle({ ...baseStyle, singleColumn: false });
    assert.strictEqual(lastLoc.columns, 2, 'clearing singleColumn restores 2 columns');

    // cb-spoken dims prior text [0, start) at priority 0 < marks (1) < cb-speaking (2) < cb-search (3) < cb-found (4).
    clothbound.highlight('cb-mark-yellow', [{ start: 0, end: 6 }]);
    clothbound.found([{ start: 7, end: 13 }], { start: 7, end: 13 });
    clothbound.speak(15, 28, false);
    assert.ok(highlights.has('cb-spoken'), 'cb-spoken added for start > 0');
    assert.ok(highlights.has('cb-speaking'), 'cb-speaking added');
    assert.strictEqual(highlights.get('cb-spoken').priority, 0);
    assert.strictEqual(highlights.get('cb-mark-yellow').priority, 1);
    assert.strictEqual(highlights.get('cb-speaking').priority, 2);
    assert.strictEqual(highlights.get('cb-search').priority, 3);
    assert.strictEqual(highlights.get('cb-found').priority, 4);
    assert.strictEqual(highlights.get('cb-spoken').ranges[0].start, 0);
    assert.strictEqual(highlights.get('cb-spoken').ranges[0].end, 15);

    // Clearing cb-speaking automatically clears cb-spoken as well.
    clothbound.highlight('cb-speaking', []);
    assert.ok(!highlights.has('cb-speaking'));
    assert.ok(!highlights.has('cb-spoken'), 'cb-spoken cleared when cb-speaking clears');

    // Speaking at offset 0 creates no cb-spoken range.
    clothbound.speak(0, 6, false);
    assert.ok(highlights.has('cb-speaking'));
    assert.ok(!highlights.has('cb-spoken'));
    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");
        QProcess proc;
        proc.start(node, {QStringLiteral("-e"), harness, readerJsPath});
        QVERIFY2(proc.waitForFinished(10000), "node wave2 reader.js harness timed out");
        const QByteArray stderrOut = proc.readAllStandardError();
        QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                 stderrOut.isEmpty() ? "node wave2 reader.js harness failed" : stderrOut.constData());
    }

    void wave2NarrationScrubberVoiceAccentAndAudioBadgesPersistence() {
        // 1. VoicePacks::accentFor + curated note/suggested/accent search.
        QCOMPARE(VoicePacks::accentFor(QStringLiteral("en-GB")), QStringLiteral("British"));
        QCOMPARE(VoicePacks::accentFor(QStringLiteral("en-US")), QStringLiteral("American"));
        QCOMPARE(VoicePacks::accentFor(QStringLiteral("en-AU")), QStringLiteral("Australian"));
        QCOMPARE(VoicePacks::accentFor(QStringLiteral("pt-BR")), QStringLiteral("Brazilian"));
        QCOMPARE(VoicePacks::accentFor(QStringLiteral("fr-CA")), QStringLiteral("Canadian French"));
        QCOMPARE(VoicePacks::accentFor(QStringLiteral("en-GB"), QStringLiteral("builtin:piper:en_GB-alba-medium")),
                 QStringLiteral("Scottish"));

        QTemporaryDir dir;
        VoicePacks packs(dir.filePath("voices"), nullptr, QStringLiteral("aarch64"), packsManifest("http://x"));
        const QVariantMap heart = packs.voice(QStringLiteral("builtin:kokoro:af_heart"));
        QCOMPARE(heart.value(QStringLiteral("accent")).toString(), QStringLiteral("American"));
        QCOMPARE(heart.value(QStringLiteral("note")).toString(), QStringLiteral("clear, friendly"));
        QCOMPARE(heart.value(QStringLiteral("suggested")).toBool(), true);
        QVERIFY(!packs.searchVoices(QStringLiteral("British")).isEmpty());
        QVERIFY(!packs.searchVoices(QStringLiteral("friendly")).isEmpty());

        // 2. Narration: positionUs, durationUs, timeChanged, seekToUs, voiceAccent.
        const QString epubPath = dir.filePath("audio.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        fakeInstall(dir.filePath("voices"), "engine-arm");
        fakeInstall(dir.filePath("voices"), "kokoro-fp32");
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);
        AudioCache cache(dir.filePath("audio"), library.database().connectionName());

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);

        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        QCOMPARE(narration.voiceAccent(), QStringLiteral("British"));
        narration.setVoice(QStringLiteral("builtin:kokoro:af_heart"), QStringLiteral("Heart"));
        QCOMPARE(narration.voiceAccent(), QStringLiteral("American"));
        narration.setVoice(QStringLiteral("sulafat"), QStringLiteral("Sulafat"));
        QCOMPARE(narration.voiceAccent(), QStringLiteral("Warm"));
        narration.setVoice(QStringLiteral("studio:v1"), QStringLiteral("Studio 1"));
        QCOMPARE(narration.voiceAccent(), QStringLiteral("Studio"));

        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        QSignalSpy timeSpy(&narration, &Narration::timeChanged);
        narration.start(0, 0);
        QJsonArray arr;
        qint64 off = 0;
        for (int i = 0; i < 2; ++i) {
            const QString para = QStringLiteral("Paragraph %1 sentence one. ").arg(i) + QString(700, QLatin1Char('b')) + QLatin1Char('.');
            arr.append(QJsonObject{{"start", double(off)}, {"end", double(off + para.size())}, {"text", para}, {"kind", "text"}});
            off += para.size() + 1;
        }
        narration.addBlocks(0, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY_WITH_TIMEOUT(narration.m_clips.size() == 2 &&
                                     narration.m_clips[0].state == Narration::Clip::Complete &&
                                     narration.m_clips[1].state == Narration::Clip::Complete,
                                 5000);
        QVERIFY(timeSpy.count() >= 1);

        // Pin both clips to exact 10s (240,000 samples @ 24kHz) -> 20,000,000 us total duration.
        for (int i = 0; i < 2; ++i) {
            narration.m_clips[i].pcm = QByteArray(240000 * 2, '\0');
            narration.m_clips[i].samples = 240000;
        }
        narration.heard(narration.m_clips[0].id, 0);
        QCOMPARE(narration.durationUs(), qint64(20000000));
        QCOMPARE(narration.positionUs(), qint64(0));

        const int beforeSeek = timeSpy.count();
        narration.seekToUs(12500000);  // 12.5s -> clip 1 @ 2.5s (60,000 samples)
        QCOMPARE(narration.m_current, 1);
        QCOMPARE(narration.m_currentSample, qint64(60000));
        QCOMPARE(narration.positionUs(), qint64(12500000));
        QVERIFY(timeSpy.count() > beforeSeek);

        // heard() across a whole-second boundary emits timeChanged.
        const int beforeHeard = timeSpy.count();
        narration.heard(narration.m_clips[1].id, 96000);  // 14.0s
        QCOMPARE(narration.positionUs(), qint64(14000000));
        QVERIFY(timeSpy.count() > beforeHeard);

        // setRate emits timeChanged while preserving 1.0x microsecond semantics.
        const int beforeRate = timeSpy.count();
        narration.setRate(1.5);
        QVERIFY(timeSpy.count() > beforeRate);
        QCOMPARE(narration.durationUs(), qint64(20000000));
        narration.setRate(1.0);

        const int beforeStop = timeSpy.count();
        narration.stop();
        QVERIFY(timeSpy.count() > beforeStop);
        QCOMPARE(narration.positionUs(), qint64(0));

        // 3. Download completion persists "audio_downloaded"="1" and Audiobook save persists "audiobook_saved"="1".
        if (!QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            const QString musicDir = dir.filePath("music");
            QVERIFY(QDir().mkpath(musicDir));
            const QString userDirsPath = QString::fromUtf8(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/user-dirs.dirs");
            const bool hadUserDirs = QFile::exists(userDirsPath);
            QByteArray prevUserDirs;
            if (hadUserDirs) {
                QFile f(userDirsPath);
                if (f.open(QIODevice::ReadOnly))
                    prevUserDirs = f.readAll();
            }
            auto restoreUserDirs = qScopeGuard([&] {
                if (hadUserDirs)
                    write(userDirsPath, prevUserDirs);
                else
                    QFile::remove(userDirsPath);
            });
            write(userDirsPath, QStringLiteral("XDG_MUSIC_DIR=\"%1\"\n").arg(musicDir).toUtf8());

            Download dl(&services, &cache, &reader, &narration);
            dl.start(0, 0);
            dl.addBlocks(0, R"([{"start":0,"end":18,"text":"Opening test line.","kind":"text"}])");
            QTRY_VERIFY_WITH_TIMEOUT(dl.finished(), 15000);
            QCOMPARE(library.bookSetting(bookId, QStringLiteral("audio_downloaded")), QStringLiteral("1"));
            QCOMPARE(library.shelfCounts().value(QStringLiteral("downloaded")).toInt(), 1);

            Audiobook ab(&cache, &dl, &reader, &library);
            ab.save();
            QTRY_VERIFY_WITH_TIMEOUT(!ab.busy(), 15000);
            QVERIFY2(ab.problem().isEmpty(), qPrintable(ab.problem()));
            QCOMPARE(library.bookSetting(bookId, QStringLiteral("audiobook_saved")), QStringLiteral("1"));
            QCOMPARE(library.shelfCounts().value(QStringLiteral("audiobooks")).toInt(), 1);
        }
    }

    void wave2CompanionMigrationWhereLocalSuggestionsAiServicesAndSystemCheck() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath("comp.epub");
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreAiSettings = qScopeGuard([&] {
            QSettings(settingsPath, QSettings::IniFormat).remove(QStringLiteral("ai"));
        });

        // 1. Seed a legacy companion_threads table WITHOUT provenance / is_local columns to verify additive migration.
        {
            QSqlQuery q(library.database());
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE companion_threads ("
                " id INTEGER PRIMARY KEY,"
                " book_id INTEGER NOT NULL,"
                " seq INTEGER NOT NULL,"
                " from_user INTEGER NOT NULL,"
                " text TEXT NOT NULL,"
                " sources TEXT NOT NULL DEFAULT '[]',"
                " created_at INTEGER NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO companion_threads (book_id, seq, from_user, text, sources, created_at) "
                "VALUES (%1, 0, 1, 'Earlier question?', '[]', 100)").arg(bookId)));
        }

        FakeServer cloudServer;
        FakeServer localServer;
        GeminiProvider gemini(nullptr, cloudServer.endpoint());
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);

        // 2. AiServices: routeFor("illustrate"), whereFor("illustrate"), friendlyModelTitle, localModelsSummary.
        QCOMPARE(AiServices::friendlyModelTitle(QStringLiteral("qwen3.5:4b")), QStringLiteral("Qwen3.5 4B"));
        QCOMPARE(AiServices::friendlyModelTitle(QStringLiteral("embeddinggemma:latest")), QStringLiteral("EmbeddingGemma"));
        QCOMPARE(AiServices::friendlyModelTitle(QStringLiteral("custom:latest")), QStringLiteral("custom"));
        QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QString());

        gemini.setKey(QStringLiteral("cloud-key"));
        services.setPreset(QStringLiteral("cloud"));
        QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QStringLiteral("cloud"));
        QVERIFY(services.whereFor(QStringLiteral("illustrate")).contains(QStringLiteral("Gemini")));
        services.setOverride(QStringLiteral("illustrate"), QStringLiteral("local"));
        QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QString());
        services.clearOverrides();
        services.setPreset(QStringLiteral("local"));
        QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QString());
        QVERIFY(services.whereFor(QStringLiteral("illustrate")).contains(QStringLiteral("Unavailable")));

        LocalModels::Server where;
        where.kind = LocalModels::Ollama;
        where.url = QUrl(localServer.origin());
        local.configure(where, QStringLiteral("qwen3.5:4b"), QStringLiteral("qwen3-embedding:0.6b"));
        QCOMPARE(services.localModelsSummary(), QStringLiteral("Qwen3.5 4B · Qwen3-Embedding 0.6B"));

        // 3. Companion additive migration check + local ask provenance + persistence + reload.
        Companion companion(&services, &reader, library.database().connectionName());
        {
            QSqlQuery info(library.database());
            QVERIFY(info.exec(QStringLiteral("PRAGMA table_info(companion_threads)")));
            QSet<QString> cols;
            while (info.next())
                cols.insert(info.value(1).toString());
            QVERIFY(cols.contains(QStringLiteral("provenance")));
            QVERIFY(cols.contains(QStringLiteral("is_local")));
        }
        QCOMPARE(companion.conversation().size(), 1);
        QCOMPARE(companion.conversation().first().toMap().value(QStringLiteral("provenance")).toString(), QString());
        QCOMPARE(companion.conversation().first().toMap().value(QStringLiteral("local")).toBool(), false);

        // Configure text-only local model for Ask so Companion uses word search (deterministic 1-request FakeServer).
        local.configure(where, QStringLiteral("qwen3.5:4b"));
        localServer.responses.append({200, "application/x-ndjson",
                                      "{\"message\":{\"content\":\"The lamp shines by the sea.\"},\"done\":false}\n"
                                      "{\"message\":{\"content\":\"\"},\"done\":true}\n"});
        companion.ask(QStringLiteral("What shines by the sea?"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QCOMPARE(companion.whereLocal(), true);
        QCOMPARE(companion.conversation().size(), 3);
        const QVariantMap localTurn = companion.conversation().last().toMap();
        QVERIFY(localTurn.value(QStringLiteral("provenance")).toString().contains(QStringLiteral("On this computer")));
        QCOMPARE(localTurn.value(QStringLiteral("local")).toBool(), true);
        {
            Companion reloaded(&services, &reader, library.database().connectionName());
            QCOMPARE(reloaded.conversation().size(), 3);
            QCOMPARE(reloaded.conversation().last().toMap().value(QStringLiteral("local")).toBool(), true);
            QVERIFY(reloaded.conversation().last().toMap().value(QStringLiteral("provenance")).toString()
                        .contains(QStringLiteral("On this computer")));
        }

        // 4. whereLocal == false on cloud Gemini reply, and whereLocal == true on offline sdcv Define even in Cloud mode.
        services.setPreset(QStringLiteral("cloud"));
        cloudServer.responses.append({200, "text/event-stream", fixture("text-stream.sse")});
        companion.ask(QStringLiteral("Tell me more in cloud mode."));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QCOMPARE(companion.whereLocal(), false);
        QVERIFY(companion.conversation().last().toMap().value(QStringLiteral("provenance")).toString()
                    .contains(QStringLiteral("Gemini")));
        QCOMPARE(companion.conversation().last().toMap().value(QStringLiteral("local")).toBool(), false);

        // Hermetic fake sdcv executable on PATH verifies offline dictionary sets whereLocal=true even in Cloud preset.
        const QString fakeBin = dir.filePath("fake-bin");
        QVERIFY(QDir().mkpath(fakeBin));
        const QString sdcvPath = fakeBin + QStringLiteral("/sdcv");
        write(sdcvPath, "#!/bin/sh\nprintf '[{\"word\":\"lamp\",\"dict\":\"LocalDict\",\"definition\":\"A vessel of light.\"}]'\n");
        QVERIFY(QFile::setPermissions(sdcvPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray origPath = qgetenv("PATH");
        auto restorePath = qScopeGuard([&] { qputenv("PATH", origPath); });
        qputenv("PATH", fakeBin.toUtf8() + ":" + origPath);

        companion.define(QStringLiteral("lamp"), 0, 4);
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QCOMPARE(companion.whereLocal(), true);
        QCOMPARE(companion.where(), QStringLiteral("Dictionary on this computer"));
        QVERIFY(companion.answer().contains(QStringLiteral("A vessel of light.")));

        // 5. Spoiler-safe suggestions() uses loaded people + chapter title and skips already-asked prompts.
        services.setPreset(QStringLiteral("local"));
        companion.newConversation();
        localServer.responses.append({200, "application/x-ndjson",
                                      "{\"message\":{\"content\":\"{\\\"people\\\":[{\\\"name\\\":\\\"Ada Marsh\\\","
                                      "\\\"also\\\":\\\"\\\",\\\"who\\\":\\\"Keeper.\\\",\\\"chapter\\\":\\\"Chapter One\\\"}]}\"},\"done\":true}\n"});
        companion.whosWho();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        const QStringList firstSuggestions = companion.suggestions();
        QCOMPARE(firstSuggestions.size(), 2);
        QCOMPARE(firstSuggestions[0], QStringLiteral("Who is Ada Marsh?"));
        QVERIFY(firstSuggestions[1].contains(QStringLiteral("Chapter One")));

        // After asking about Ada Marsh, suggestions() skips that character and rotates cleanly.
        localServer.responses.append({200, "application/x-ndjson",
                                      "{\"message\":{\"content\":\"She tends the lantern.\"},\"done\":true}\n"});
        companion.ask(QStringLiteral("Who is Ada Marsh?"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        const QStringList nextSuggestions = companion.suggestions();
        QCOMPARE(nextSuggestions.size(), 2);
        QVERIFY(!nextSuggestions.contains(QStringLiteral("Who is Ada Marsh?")));

        // 6. SystemCheck::summaryVerdicts populates 3 rows on refresh() and live-updates on routing changes.
        LocalModels freshLocal;
        AiServices checkAi(&gemini, &keys, library.database().connectionName());
        checkAi.setLocalModels(&freshLocal);
        SystemCheck check(&checkAi, nullptr);
        check.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(check.summaryVerdicts().size(), 3, 5000);
        QCOMPARE(check.summaryVerdicts()[0].toMap().value(QStringLiteral("feature")).toString(), QStringLiteral("Narration"));
        QCOMPARE(check.summaryVerdicts()[1].toMap().value(QStringLiteral("feature")).toString(), QStringLiteral("Define, translate"));
        QCOMPARE(check.summaryVerdicts()[2].toMap().value(QStringLiteral("feature")).toString(), QStringLiteral("Ask the whole book"));

        QSignalSpy checkSpy(&check, &SystemCheck::changed);
        freshLocal.configure(where, QStringLiteral("qwen3.5:4b"));
        emit checkAi.routingChanged();
        QTRY_VERIFY_WITH_TIMEOUT(checkSpy.count() >= 1, 2000);
        QCOMPARE(check.summaryVerdicts()[1].toMap().value(QStringLiteral("detail")).toString(), QStringLiteral("a few seconds"));
    }

    void wave2QmlComponentsCompileCleanlyAndListColumnsAligned() {
        QQmlEngine engine;
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));

        QStringList qmlFiles;
        QDirIterator it(srcRoot + QStringLiteral("/ui"), {QStringLiteral("*.qml")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            qmlFiles.append(it.next());
        qmlFiles.sort();

        QVERIFY2(qmlFiles.size() >= 65, qPrintable(QString::number(qmlFiles.size())));
        QStringList failures;
        for (const QString &file : qmlFiles) {
            QQmlComponent component(&engine, QUrl::fromLocalFile(file));
            if (component.isError() || component.status() != QQmlComponent::Ready)
                failures.append(QFileInfo(file).fileName() + QStringLiteral(": ") + component.errorString().trimmed());
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));

        // Regression check for All Books list mode column alignment in LibraryView.qml.
        QFile listFile(srcRoot + QStringLiteral("/ui/library/LibraryView.qml"));
        QVERIFY(listFile.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(listFile.readAll());
        QVERIFY(qml.contains(QStringLiteral("Layout.preferredWidth: 48")));
        QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 48")));
        QVERIFY(qml.contains(QStringLiteral("Layout.maximumWidth: 48")));
        QVERIFY(qml.contains(QStringLiteral("Layout.preferredWidth: 112")));
        QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 112")));
        QVERIFY(qml.contains(QStringLiteral("Layout.maximumWidth: 112")));
        QVERIFY(qml.contains(QStringLiteral("Layout.preferredWidth: 0")));
        QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 0")));
    }

    void wave2TransitionsDualFramePdfPrefetchAndSfx() {
        // 1. Dual-frame reader.js Node VM test:
        //    - zero .shown during src navigation
        //    - #book-back idle standby pre-warming & 0-load chapter swap reuse
        //    - backward chapter turn pre-scrolls to last page BEFORE .shown reveal
        //    - rapid in-chapter turn retargeting mid-flight without snapping/queuing
        //    - close() resets both frames and clears #viewport.turning
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (!node.isEmpty()) {
            const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
            const QString dualHarness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        willChange: '', colorScheme: '', left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}

function makeClassList(onAdd) {
    const set = new Set();
    return {
        add(c) { set.add(c); if (onAdd) onAdd(c); },
        remove(c) { set.delete(c); },
        contains(c) { return set.has(c); },
    };
}

const chapterSpecs = {
    'clothbound://book/1/ch0.xhtml': { pages: 1, text: 'Chapter zero opening words.' },
    'clothbound://book/1/ch1.xhtml': { pages: 3, text: 'Chapter one has three full pages of prose.' },
    'clothbound://book/1/ch2.xhtml': { pages: 2, text: 'Chapter two follows with two pages.' },
};

function makeChapterDoc(ownerFrame, url) {
    const spec = chapterSpecs[url] || { pages: 1, text: '' };
    const styleNodes = new Map();
    const textNode = { nodeType: 3, data: spec.text, parentElement: null };
    const bodyEl = {
        childNodes: [textNode],
        lastElementChild: {
            getClientRects() { return [{ left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }]; },
            getBoundingClientRect() { return { left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }; },
        },
    };
    textNode.parentElement = bodyEl;
    const docEl = {
        style: makeStyle(),
        get scrollWidth() { return spec.pages * (parseFloat(ownerFrame.style.width) || 680); },
        scrollHeight: 692,
        scrollLeft: 0,
        scrollTop: 0,
        scrollTo(opts) {
            if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
            if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
        },
        appendChild() {},
    };
    return {
        URL: url,
        documentElement: docEl,
        head: { appendChild(el) { styleNodes.set(el.id, el); } },
        body: bodyEl,
        scrollingElement: docEl,
        fonts: { ready: Promise.resolve() },
        images: [],
        defaultView: {
            getComputedStyle() {
                return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start', whiteSpace: 'normal' };
            },
            CSS: { highlights: new Map() },
            Highlight: class { constructor(...ranges) { this.ranges = ranges; this.priority = 0; } },
        },
        getElementById(id) { return styleNodes.get(id) || null; },
        createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
        querySelector() { return null; },
        querySelectorAll() { return []; },
        addEventListener() {},
        removeEventListener() {},
        createTreeWalker() {
            let done = false;
            return { nextNode() { if (done) return null; done = true; return textNode; } };
        },
        createRange() {
            return {
                selectNodeContents() {},
                setStart() {},
                setEnd() {},
                getClientRects() { return [{ left: 10, right: 120, top: 10, bottom: 28, width: 110, height: 18 }]; },
                getBoundingClientRect() { return { left: 10, right: 120, top: 10, bottom: 28, width: 110, height: 18 }; },
            };
        },
        getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
    };
}

const srcLoads = [];
const shownEvents = [];

function makeFrame(id) {
    const f = {
        id,
        style: makeStyle(),
        contentDocument: null,
        contentWindow: null,
        onload: null,
        _src: 'about:blank',
        _attrs: {},
        setAttribute(k, v) { this._attrs[k] = v; },
        removeAttribute(k) { delete this._attrs[k]; },
        getBoundingClientRect() { return { left: 160, top: 54, width: parseFloat(this.style.width) || 680, height: 692 }; },
    };
    f.classList = makeClassList(cls => {
        if (cls === 'shown') {
            shownEvents.push({
                id: f.id,
                url: f._src,
                scrollLeft: f.contentDocument ? f.contentDocument.documentElement.scrollLeft : -1,
            });
        }
    });
    Object.defineProperty(f, 'src', {
        get() { return this._src; },
        set(v) {
            assert.strictEqual(this.classList.contains('shown'), false,
                `frame #${this.id} must NEVER carry .shown during src assignment (${v})`);
            this._src = v;
            if (v && v !== 'about:blank') {
                srcLoads.push({ id: this.id, url: v });
                const doc = makeChapterDoc(this, v);
                this.contentDocument = doc;
                this.contentWindow = doc.defaultView;
            } else {
                this.contentDocument = null;
                this.contentWindow = null;
            }
            const cb = this.onload;
            if (typeof cb === 'function') {
                queueMicrotask(() => {
                    assert.strictEqual(this.classList.contains('shown'), false,
                        `frame #${this.id} must still hide .shown when onload fires (${v})`);
                    cb();
                });
            }
        },
    });
    return f;
}

const stageEl = {
    clientWidth: 1000, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: this.clientWidth, height: this.clientHeight }; },
};
const viewportEl = { style: makeStyle(), classList: makeClassList() };
const bookFrame = makeFrame('book');
const backFrame = makeFrame('book-back');
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };

const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'viewport') return viewportEl;
        if (id === 'book') return bookFrame;
        if (id === 'book-back') return backFrame;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement(tag) { return { tagName: tag.toUpperCase(), id: '', style: makeStyle(), setAttribute() {} }; },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

let fakeNow = 1000;
let autoRaf = true;
let nextRafId = 1;
const rafQueue = new Map();

function requestAnimationFrame(cb) {
    const id = nextRafId++;
    rafQueue.set(id, cb);
    if (autoRaf) {
        queueMicrotask(() => {
            const fn = rafQueue.get(id);
            if (!fn) return;
            rafQueue.delete(id);
            fakeNow += 120;
            fn(fakeNow);
        });
    }
    return id;
}

function cancelAnimationFrame(id) { rafQueue.delete(id); }

function stepRaf(deltaMs) {
    fakeNow += deltaMs;
    const batch = [...rafQueue.entries()];
    rafQueue.clear();
    for (const [, fn] of batch) fn(fakeNow);
}

let lastLoc = null;
const turnDirs = [];
const bridge = {
    surfaceLoaded() {},
    relocated(loc) { lastLoc = loc; },
    notifyPageTurn(dir) { turnDirs.push(dir); },
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console, setTimeout, clearTimeout, Promise, URL,
    performance: { now: () => fakeNow },
    requestAnimationFrame, cancelAnimationFrame,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1000, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class { observe() {} },
    matchMedia: () => ({ matches: false }),
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

(async () => {
    const book = {
        session: 1, base: 'clothbound://book/1/', language: 'en', rtl: false,
        spine: [
            { href: 'ch0.xhtml', linear: true },
            { href: 'ch1.xhtml', linear: true },
            { href: 'ch2.xhtml', linear: true },
        ],
        pageTargets: [],
    };
    const style = {
        theme: { bg: '#1e1d1b', fg: '#dcd5c8', link: '#9db8f2', accentInk: '#9db8f2', muted: '#958d80',
                 popover: '#2a2926', border: 'rgba(255,255,255,0.12)', selection: '#345', search: '#ff0', found: '#fa0',
                 markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f', markPink: '#f0f',
                 speaking: '#345', dark: true },
        font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.0,
        margin: 0.1, measure: 30, justify: true, hyphenate: false, layout: 'single', singleColumn: false,
        animate: true, reducedMotion: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
        listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
    };

    await clothbound.open(book, { chapter: 0, offset: 0 }, style);
    assert.strictEqual(lastLoc.chapter, 0);
    assert.strictEqual(bookFrame.classList.contains('shown'), true);
    assert.strictEqual(backFrame.classList.contains('shown'), false);
    assert.strictEqual(srcLoads.length, 1);

    // Wait for 45ms scheduleWarm timer to pre-render ch1 into hidden #book-back.
    await new Promise(r => setTimeout(r, 90));
    assert.strictEqual(srcLoads.length, 2, 'standby #book-back should pre-warm ch1');
    assert.strictEqual(srcLoads[1].id, 'book-back');
    assert.strictEqual(srcLoads[1].url, 'clothbound://book/1/ch1.xhtml');
    assert.strictEqual(backFrame.classList.contains('shown'), false, 'pre-warmed standby stays hidden');

    // Cross chapter 0 -> chapter 1: reuses warm #book-back with ZERO extra src load.
    const loadsBeforeSwap = srcLoads.length;
    await clothbound.next();
    assert.strictEqual(srcLoads.length, loadsBeforeSwap, 'warm chapter swap must not re-navigate src');
    assert.strictEqual(lastLoc.chapter, 1);
    assert.strictEqual(lastLoc.page, 0);
    assert.strictEqual(lastLoc.pages, 3);
    assert.strictEqual(backFrame.classList.contains('shown'), true, 'swapped #book-back is now active');
    assert.strictEqual(bookFrame.classList.contains('shown'), false, 'old #book is hidden after swap');
    assert.strictEqual(viewportEl.classList.contains('turning'), false);
    assert.deepStrictEqual(turnDirs, [1]);

    // Rapid in-chapter turn retargeting mid-flight (ch1: page 0 -> 1 -> 2).
    const activeDocEl = backFrame.contentDocument.documentElement;
    const step = parseFloat(backFrame.style.width);
    assert.ok(step > 200);
    autoRaf = false;
    const p1 = clothbound.next();
    await new Promise(r => setTimeout(r, 10));
    assert.strictEqual(viewportEl.classList.contains('turning'), true, 'feather mask enabled while turning');
    stepRaf(65);
    const midScroll = activeDocEl.scrollLeft;
    assert.ok(midScroll > 0 && midScroll < step, `expected mid-flight scroll, got ${midScroll}/${step}`);

    // Fire second turn before first finishes: keeps live scroll and retargets cleanly to page 2.
    const p2 = clothbound.next();
    await new Promise(r => setTimeout(r, 10));
    assert.strictEqual(activeDocEl.scrollLeft, midScroll, 'retargeting must not snap scroll back before gliding');
    stepRaf(20);
    assert.ok(activeDocEl.scrollLeft > midScroll && activeDocEl.scrollLeft < 2 * step);
    stepRaf(160);
    await Promise.all([p1, p2]);
    assert.strictEqual(activeDocEl.scrollLeft, 2 * step);
    assert.strictEqual(lastLoc.chapter, 1);
    assert.strictEqual(lastLoc.page, 2);
    assert.strictEqual(viewportEl.classList.contains('turning'), false, 'feather mask cleared at rest');
    assert.deepStrictEqual(turnDirs, [1, 1, 1]);

    // Turn forward into ch2 (after standby warms), then backward into 3-page ch1 (end: true).
    autoRaf = true;
    await new Promise(r => setTimeout(r, 90));
    const loadsBeforeCh2 = srcLoads.length;
    await clothbound.next();
    assert.strictEqual(srcLoads.length, loadsBeforeCh2, 'ch2 should also reuse warm standby');
    assert.strictEqual(lastLoc.chapter, 2);
    assert.strictEqual(lastLoc.page, 0);

    // Backward chapter turn: ch1 must be pre-scrolled to its LAST page (2 * step) BEFORE .shown is added.
    const shownCountBeforePrev = shownEvents.length;
    await clothbound.prev();
    assert.strictEqual(lastLoc.chapter, 1);
    assert.strictEqual(lastLoc.page, 2);
    const ch1Reveal = shownEvents.slice(shownCountBeforePrev).find(e => e.url === 'clothbound://book/1/ch1.xhtml');
    assert.ok(ch1Reveal, 'ch1 reveal should be recorded');
    assert.strictEqual(ch1Reveal.scrollLeft, 2 * step,
        `backward turn must pre-scroll to last page (${2 * step}) before adding .shown (saw ${ch1Reveal.scrollLeft})`);

    // close() hides both frames and resets them to about:blank.
    await clothbound.close();
    assert.strictEqual(bookFrame.classList.contains('shown'), false);
    assert.strictEqual(backFrame.classList.contains('shown'), false);
    assert.strictEqual(bookFrame.src, 'about:blank');
    assert.strictEqual(backFrame.src, 'about:blank');
    assert.strictEqual(viewportEl.classList.contains('turning'), false);
    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");
            QProcess proc;
            proc.start(node, {QStringLiteral("-e"), dualHarness, readerJsPath});
            QVERIFY2(proc.waitForFinished(10000), "node dual-frame reader.js harness timed out");
            const QByteArray stderrOut = proc.readAllStandardError();
            QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                     stderrOut.isEmpty() ? "node dual-frame reader.js harness failed" : stderrOut.constData());
        }

        // 2. C++ PdfPages::prefetch() cache hit + cancelPrefetch + Reader::playPageTurn(+1/-1) headless & throttle.
        QTemporaryDir dir;
        const QString pdfPath = dir.filePath(QStringLiteral("multi.pdf"));
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QSizeF(200, 300), QPageSize::Point));
            QPainter painter(&writer);
            for (int p = 0; p < 3; ++p) {
                if (p > 0)
                    writer.newPage();
                painter.fillRect(0, 0, writer.width(), writer.height(), Qt::white);
                painter.setPen(Qt::black);
                painter.drawText(80, 200, QStringLiteral("PDF Page %1").arg(p + 1));
            }
            painter.end();
        }

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(pdfId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(pdfId));
        const QString token = reader.pdfSource().mid(QStringLiteral("image://pdf/").size());
        QVERIFY(!token.isEmpty());

        const QString query = QStringLiteral("?bg=%231e1f24&fg=%23d6d2c9");
        const QString id1 = token + QStringLiteral("/1") + query;
        const QString id2 = token + QStringLiteral("/2") + query;
        const QString key1 = PdfPages::makeCacheKey(id1, QSize(220, 0));
        const QString key2 = PdfPages::makeCacheKey(id2, QSize(220, 0));

        std::vector<std::unique_ptr<QQuickImageResponse>> responses;
        {
            PdfPages pages;
            QVERIFY(PdfPages::cached(key1).isNull());
            QVERIFY(PdfPages::cached(key2).isNull());

            reader.pdfPrefetch({1, 2}, 220, query);
            QTRY_VERIFY_WITH_TIMEOUT(!PdfPages::cached(key1).isNull() && !PdfPages::cached(key2).isNull(), 8000);

            int done = 0;
            auto *resp = pages.requestImageResponse(id1, QSize(220, 0));
            connect(resp, &QQuickImageResponse::finished, this, [&done] { ++done; });
            responses.emplace_back(resp);
            QTRY_COMPARE_WITH_TIMEOUT(done, 1, 2000);
            std::unique_ptr<QQuickTextureFactory> factory(resp->textureFactory());
            QVERIFY(factory != nullptr);
            const QImage img = factory->image();
            QCOMPARE(img.width(), 220);
            QCOMPARE(QColor(img.pixel(2, 2)).name(), QStringLiteral("#1e1f24"));

            PdfPages::cancelPrefetch();
            reader.close();
            QVERIFY(PdfPages::cached(key1).isNull());
            QVERIFY(PdfPages::cached(key2).isNull());
        }

        // Reader::notifyPageTurn direction normalization + playPageTurn headless no-op + PageTurnSfx throttle.
        QSignalSpy turnSpy(&reader, &Reader::pageTurnRequested);
        reader.notifyPageTurn(3);
        reader.notifyPageTurn(0);
        reader.notifyPageTurn(-2);
        QCOMPARE(turnSpy.count(), 3);
        QCOMPARE(turnSpy[0][0].toInt(), 1);
        QCOMPARE(turnSpy[1][0].toInt(), 1);
        QCOMPARE(turnSpy[2][0].toInt(), -1);

        // Under offscreen test platform, playPageTurn() is a silent no-op (never opens an audio sink).
        reader.playPageTurn(1);
        reader.playPageTurn(-1);
        QVERIFY(reader.m_sfx == nullptr);

        // Dry-run synthesis + 85 ms rapid-repeat throttle verification (no audio hardware touched).
        QByteArray fwdPcm, throttledPcm, bwdPcm;
        QVERIFY(reader.playPageTurnDryRun(1, &fwdPcm));
        QVERIFY(fwdPcm.size() >= 9000);
        // Immediate back-to-back call is throttled (< 85ms).
        QVERIFY(!reader.playPageTurnDryRun(-1, &throttledPcm));
        QVERIFY(throttledPcm.isEmpty());
        // After the 85ms throttle window, backward flip plays a distinct synthesized clip.
        QTest::qWait(95);
        QVERIFY(reader.playPageTurnDryRun(-1, &bwdPcm));
        QVERIFY(bwdPcm.size() >= 9000);
        QVERIFY(bwdPcm != fwdPcm);
        const auto *samples = reinterpret_cast<const qint16 *>(fwdPcm.constData());
        const qsizetype count = fwdPcm.size() / 2;
        int peak = 0;
        for (qsizetype i = 0; i < count; ++i)
            peak = qMax(peak, std::abs(int(samples[i])));
        QVERIFY2(peak > 1000 && peak <= 7000, qPrintable(QString::number(peak)));
    }

    void wave2VoiceNarrationSleepFadeAndEmptyLeadGuard() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath(QStringLiteral("sleep.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.setVolume(1.0);
        narration.start(0, 0);
        narration.addBlocks(0, R"([{"start":0,"end":44,"text":"First sentence here. Second sentence follows.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() && narration.m_clips[0].state == Narration::Clip::Complete, 5000);

        // 1. pause() clears active sleep fade, restores pre-fade volume, and emits sleepChanged().
        narration.sleepIn(1);
        narration.m_sleepAt = QDateTime::currentDateTime().addMSecs(5000);
        narration.onSleepTick();
        QCOMPARE(narration.sleepFading(), true);
        QVERIFY(narration.volume() < 0.9);
        {
            QSignalSpy sleepSpy(&narration, &Narration::sleepChanged);
            narration.pause();
            QCOMPARE(narration.sleepFading(), false);
            QVERIFY(qFuzzyCompare(narration.volume(), 1.0));
            QVERIFY(sleepSpy.count() >= 1);
        }

        // 2. setVolume() clears active sleep fade and emits sleepChanged().
        narration.resume();
        narration.m_sleepAt = QDateTime::currentDateTime().addMSecs(4000);
        narration.onSleepTick();
        QCOMPARE(narration.sleepFading(), true);
        {
            QSignalSpy sleepSpy(&narration, &Narration::sleepChanged);
            narration.setVolume(0.75);
            QCOMPARE(narration.sleepFading(), false);
            QVERIFY(qFuzzyCompare(narration.volume(), 0.75));
            QVERIFY(sleepSpy.count() >= 1);
        }

        // 3. resume() clears lingering sleep fade, restores pre-fade volume, and emits sleepChanged().
        narration.m_sleepFading = true;
        narration.m_preFadeVolume = 0.85;
        narration.m_player->setVolume(0.15);
        narration.m_wanted = false;
        {
            QSignalSpy sleepSpy(&narration, &Narration::sleepChanged);
            narration.resume();
            QCOMPARE(narration.sleepFading(), false);
            QVERIFY(qFuzzyCompare(narration.volume(), 0.85));
            QVERIFY(sleepSpy.count() >= 1);
        }

        // 4. onSleepTick() clearing active fade when !m_wanted also emits sleepChanged().
        narration.m_sleepAt = QDateTime::currentDateTime().addMSecs(4000);
        narration.onSleepTick();
        QCOMPARE(narration.sleepFading(), true);
        narration.m_wanted = false;
        {
            QSignalSpy sleepSpy(&narration, &Narration::sleepChanged);
            narration.onSleepTick();
            QCOMPARE(narration.sleepFading(), false);
            QVERIFY(qFuzzyCompare(narration.volume(), 0.85));
            QVERIFY(sleepSpy.count() >= 1);
        }
        narration.stop();

        // 5. Speech::clipsFrom() mid-clip trimming and empty/boundary/degenerate lead.sentences safety.
        const QList<SpeechBlock> merged = {
            block(0, QStringLiteral("Alpha first sentence.")),
            block(25, QStringLiteral("Beta second sentence.")),
        };
        const QList<SpeechClip> trimmed = Speech::clipsFrom(0, merged, 25);
        QCOMPARE(trimmed.size(), 1);
        QCOMPARE(trimmed.first().block, 1);
        QCOMPARE(trimmed.first().text, QStringLiteral("Beta second sentence."));

        // Past the final sentence returns empty cleanly.
        QVERIFY(Speech::clipsFrom(0, merged, 999).isEmpty());
        // Empty and whitespace-only blocks return empty cleanly.
        QVERIFY(Speech::clipsFrom(0, {}, 0).isEmpty());
        QVERIFY(Speech::clipsFrom(0, {block(0, QStringLiteral("   "))}, 1).isEmpty());
        // Non-monotonic / overlapping block offsets where lead.sentences trimming is exercised never crash.
        SpeechBlock overlapA = block(0, QStringLiteral("First line."));
        SpeechBlock overlapB;
        overlapB.start = 4;
        overlapB.end = 22;
        overlapB.text = QStringLiteral("Second line.");
        overlapB.kind = QStringLiteral("text");
        const QList<SpeechClip> overlapOut = Speech::clipsFrom(0, {overlapA, overlapB}, 11);
        QVERIFY(!overlapOut.isEmpty());
        QVERIFY(!overlapOut.first().sentences.isEmpty());
    }

    void wave2SettingsAiLocalSetupAudioCacheVoicePacksAndFolders() {
        QTemporaryDir dir;
        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
        auto restoreSettings = qScopeGuard([&] {
            if (origEnvKey.isEmpty())
                qunsetenv("GEMINI_API_KEY");
            else
                qputenv("GEMINI_API_KEY", origEnvKey);
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai"));
            s.remove(QStringLiteral("local"));
        });
        qunsetenv("GEMINI_API_KEY");

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());

        // 1. AiServices::maskedKey() + resetUsageThisMonth().
        FakeServer cloudSrv;
        GeminiProvider gemini(nullptr, cloudSrv.endpoint());
        MemoryKeyStore keys;
        {
            AiServices noKey(&gemini, &keys, library.database().connectionName());
            QCOMPARE(noKey.maskedKey(), QString());
        }
        keys.save(QStringLiteral("gemini"), QStringLiteral("shortkey"));
        {
            AiServices shortKey(&gemini, &keys, library.database().connectionName());
            QCOMPARE(shortKey.maskedKey(), QStringLiteral("••••••••"));
        }
        keys.save(QStringLiteral("gemini"), QStringLiteral("AIzaSyDemo1234567890ABCD"));
        AiServices services(&gemini, &keys, library.database().connectionName());
        QCOMPARE(services.maskedKey(), QStringLiteral("AIza••••ABCD"));

        Ai::Usage usage;
        usage.inputTokens = 200000;
        usage.outputTokens = 50000;
        services.ledger().record(QStringLiteral("gemini"), QStringLiteral("gemini-3.5-flash-lite"),
                                 QStringLiteral("ask"), usage, false);
        QVERIFY(services.spentThisMonth() > 0.0);
        QVERIFY(!services.usageThisMonth().isEmpty());
        {
            QSignalSpy usageSpy(&services, &AiServices::usageChanged);
            services.resetUsageThisMonth();
            QCOMPARE(usageSpy.count(), 1);
            QCOMPARE(services.spentThisMonth(), 0.0);
            QVERIFY(services.usageThisMonth().isEmpty());
        }

        // 2. AiServices::testLocal() + ServerFinder loopback alias dedup (localhost == 127.0.0.1).
        FakeServer ollamaSrv;
        ollamaSrv.routes.insert("/api/tags", {200, "application/json",
                                              R"({"models":[{"name":"qwen3.5:4b"},{"name":"qwen3-embedding:0.6b"}]})"});
        ollamaSrv.routes.insert("/api/show", {200, "application/json",
                                              R"({"model_info":{"qwen3.context_length":32768}})"});
        LocalModels local;
        services.setLocalModels(&local);
        services.useLocal(QStringLiteral("ollama"), ollamaSrv.origin(), QStringLiteral("qwen3.5:4b"), QString());

        services.testLocal();
        QVERIFY(services.testingLocal());
        QTRY_VERIFY_WITH_TIMEOUT(!services.testingLocal(), 5000);
        QVERIFY2(services.localError().isEmpty(), qPrintable(services.localError()));
        QVERIFY2(services.localTestResult().startsWith(QStringLiteral("Connected")), qPrintable(services.localTestResult()));

        // Invalid custom URL surfaces immediate validation error without probing.
        services.findLocal(QStringLiteral("ftp://example.com"));
        QVERIFY(!services.localError().isEmpty());

        // Loopback alias dedup: passing localhost:<port>/v1 and 127.0.0.1:<port>/api/tags finds it ONCE.
        const int port = QUrl(ollamaSrv.origin()).port();
        ServerFinder finder;
        QSignalSpy finderDone(&finder, &ServerFinder::finished);
        finder.find({QUrl(QStringLiteral("http://localhost:%1/v1").arg(port)),
                     QUrl(QStringLiteral("http://127.0.0.1:%1/api/tags").arg(port))});
        QTRY_COMPARE_WITH_TIMEOUT(finderDone.count(), 1, 5000);
        int matchedOnPort = 0;
        QUrl matchedUrl;
        for (const LocalModels::Server &srv : finder.servers()) {
            if (srv.url.port() == port) {
                ++matchedOnPort;
                matchedUrl = srv.url;
            }
        }
        QCOMPARE(matchedOnPort, 1);
        QCOMPARE(matchedUrl.host(), QStringLiteral("localhost"));

        // 3. LocalSetup::pullModel() progress & slot assignment + cancel() state reset.
        ollamaSrv.routes.insert("/api/pull",
                                {200, "application/x-ndjson",
                                 "{\"status\":\"downloading\",\"digest\":\"sha256:aaa\",\"completed\":50,\"total\":100}\n"
                                 "{\"status\":\"downloading\",\"digest\":\"sha256:aaa\",\"completed\":100,\"total\":100}\n"
                                 "{\"status\":\"success\"}\n"});
        LocalSetup setup(&services);
        setup.pullModel(QStringLiteral("qwen3-embedding:0.6b"));
        QVERIFY(setup.pulling());
        QTRY_VERIFY_WITH_TIMEOUT(!setup.pulling(), 5000);
        QVERIFY2(setup.problem().isEmpty(), qPrintable(setup.problem()));
        QCOMPARE(setup.progress(), 1.0);
        QCOMPARE(services.localEmbeddingModel(), QStringLiteral("qwen3-embedding:0.6b"));
        QCOMPARE(services.localTextModel(), QStringLiteral("qwen3.5:4b"));

        // Hanging server tests LocalSetup::cancel() mid-pull resets pulling/progress/status cleanly.
        QTcpServer hangServer;
        QVERIFY(hangServer.listen(QHostAddress::LocalHost));
        QList<QPointer<QTcpSocket>> heldSockets;
        connect(&hangServer, &QTcpServer::newConnection, &hangServer, [&hangServer, &heldSockets] {
            while (QTcpSocket *s = hangServer.nextPendingConnection())
                heldSockets.append(s);
        });
        services.useLocal(QStringLiteral("ollama"),
                          QStringLiteral("http://127.0.0.1:%1").arg(hangServer.serverPort()),
                          QStringLiteral("qwen3.5:4b"), QString());
        QSignalSpy setupChanged(&setup, &LocalSetup::changed);
        setup.pullModel(QStringLiteral("qwen3.5:2b"));
        QVERIFY(setup.pulling());
        setup.cancel();
        QVERIFY(!setup.pulling());
        QVERIFY(!setup.waiting());
        QCOMPARE(setup.progress(), 0.0);
        QVERIFY(setup.status().isEmpty());
        QVERIFY(setupChanged.count() >= 2);
        for (const QPointer<QTcpSocket> &s : std::as_const(heldSockets))
            if (s)
                s->close();
        hangServer.close();
        services.forgetLocal();

        // 4. AudioCache async clear() + O(1) clipCount.
        {
            AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
            QCOMPARE(cache.clipCount(), 0);
            QCOMPARE(cache.megabytes(), 0.0);
            QVERIFY(!cache.clearing());

            const QByteArray pcm(24000 * 2 / 5, '\x10');  // 0.2s mono 16-bit PCM
            QSignalSpy idleSpy(&cache, &AudioCache::storesIdle);
            cache.store(QStringLiteral("clip-1"), pcm, 24000);
            cache.store(QStringLiteral("clip-2"), pcm, 24000);
            QTRY_VERIFY_WITH_TIMEOUT(cache.clipCount() == 2 && !cache.hasPendingStores(), 10000);
            const QString file1 = cache.lookupPath(QStringLiteral("clip-1"));
            const QString file2 = cache.lookupPath(QStringLiteral("clip-2"));
            QVERIFY(QFileInfo::exists(file1));
            QVERIFY(QFileInfo::exists(file2));
            QVERIFY(cache.megabytes() > 0.0);

            QSignalSpy cacheChanged(&cache, &AudioCache::changed);
            cache.clear();
            QCOMPARE(cache.clipCount(), 0);
            QCOMPARE(cache.megabytes(), 0.0);
            QVERIFY(cache.clearing());
            QTRY_VERIFY_WITH_TIMEOUT(!cache.clearing(), 5000);
            QVERIFY(!QFileInfo::exists(file1));
            QVERIFY(!QFileInfo::exists(file2));
            QVERIFY(cacheChanged.count() >= 2);
        }

        // 5. VoicePacks::catalogPacks() + installPack().
        {
            auto makeArchive = [&dir](const QString &folder, const QString &file) {
                QDir().mkpath(dir.filePath(QStringLiteral("vp-src/") + folder + QStringLiteral("/lib")));
                write(dir.filePath(QStringLiteral("vp-src/") + folder + QStringLiteral("/lib/") + file), "pack bytes");
                QProcess tar;
                tar.start(QStringLiteral("tar"),
                          {QStringLiteral("-cjf"), dir.filePath(folder + QStringLiteral(".tar.bz2")),
                           QStringLiteral("-C"), dir.filePath(QStringLiteral("vp-src")), folder});
                tar.waitForFinished();
                QFile f(dir.filePath(folder + QStringLiteral(".tar.bz2")));
                return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            };
            const QByteArray engArc = makeArchive(QStringLiteral("engine-arm"), QStringLiteral("libsherpa-onnx-c-api.so"));
            const QByteArray kokArc = makeArchive(QStringLiteral("kokoro-fp32"), QStringLiteral("model.onnx"));
            auto sha = [](const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); };

            FakeServer vpSrv;
            vpSrv.routes.insert("/v1beta/engine-arm.tar.bz2", {200, "application/octet-stream", engArc});
            vpSrv.routes.insert("/v1beta/k-fp32.tar.bz2", {200, "application/octet-stream", kokArc});

            QJsonObject manifest = QJsonDocument::fromJson(
                packsManifest(vpSrv.endpoint(), sha(engArc).toLatin1(), "00", engArc.size(), 10)).object();
            QJsonObject kokoro = manifest.value(QStringLiteral("kokoro")).toObject();
            QJsonObject variants = kokoro.value(QStringLiteral("variants")).toObject();
            QJsonObject armVar = variants.value(QStringLiteral("aarch64")).toObject();
            armVar[QStringLiteral("sha256")] = sha(kokArc);
            armVar[QStringLiteral("size")] = kokArc.size();
            variants[QStringLiteral("aarch64")] = armVar;
            kokoro[QStringLiteral("variants")] = variants;
            manifest[QStringLiteral("kokoro")] = kokoro;
            manifest[QStringLiteral("pocket")] = QJsonObject{
                {QStringLiteral("id"), QStringLiteral("pocket-v1")},
                {QStringLiteral("title"), QStringLiteral("Pocket TTS")},
                {QStringLiteral("license"), QStringLiteral("MIT")},
                {QStringLiteral("url"), vpSrv.endpoint() + QStringLiteral("/pocket.tar.bz2")},
                {QStringLiteral("size"), 50},
                {QStringLiteral("sha256"), QStringLiteral("aa")},
                {QStringLiteral("dir"), QStringLiteral("pocket-v1")},
                {QStringLiteral("voices"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("bria")},
                                                                  {QStringLiteral("name"), QStringLiteral("Bria")},
                                                                  {QStringLiteral("language"), QStringLiteral("en-US")},
                                                                  {QStringLiteral("reference"), QStringLiteral("bria.wav")}}}},
            };

            VoicePacks packs(dir.filePath(QStringLiteral("vp-root")), nullptr, QStringLiteral("aarch64"),
                             QJsonDocument(manifest).toJson());
            QVERIFY(!packs.engineVersion().isEmpty());
            const QVariantList beforeCatalog = packs.catalogPacks();
            QCOMPARE(beforeCatalog.size(), 2);
            QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("kokoro-v1.0"));
            QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("installed")).toBool(), false);
            QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("bytes")).toLongLong(), qint64(engArc.size() + kokArc.size()));
            QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("voiceCount")).toInt(), 3);
            QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("languageCount")).toInt(), 1);
            QCOMPARE(beforeCatalog[1].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("pocket-v1"));

            packs.installPack(QStringLiteral("kokoro-v1.0"));
            QVERIFY(packs.busy());
            QTRY_VERIFY_WITH_TIMEOUT(!packs.busy(), 10000);
            QVERIFY2(packs.problem().isEmpty(), qPrintable(packs.problem()));
            QVERIFY(packs.engineInstalled());
            QVERIFY(packs.isInstalled(QStringLiteral("kokoro-v1.0")));
            QCOMPARE(packs.installedVoiceCount(), 3);

            const QVariantList afterCatalog = packs.catalogPacks();
            QCOMPARE(afterCatalog[0].toMap().value(QStringLiteral("installed")).toBool(), true);
            QCOMPARE(afterCatalog[0].toMap().value(QStringLiteral("bytes")).toLongLong(), qint64(0));
            // Installing an already-installed pack is a clean no-op.
            packs.installPack(QStringLiteral("kokoro-v1.0"));
            QVERIFY(!packs.busy());
        }

        // 6. Library::folderStats() + empty-path guard + Scanner::cancel() sets m_running=false.
        QSignalSpy libProblem(&library, &Library::problem);
        library.addFolder(QString());
        library.addFolder(QStringLiteral("   "));
        library.removeFolder(QString());
        library.removeFolder(QStringLiteral("   "));
        QCOMPARE(libProblem.count(), 0);
        QVERIFY(library.folders().isEmpty());

        const QString booksDir = dir.filePath(QStringLiteral("shelf-a"));
        const QString goneDir = dir.filePath(QStringLiteral("shelf-gone"));
        QVERIFY(QDir().mkpath(booksDir));
        QVERIFY(QDir().mkpath(goneDir));
        QVERIFY(TestBooks::write(booksDir + QStringLiteral("/one.epub"), TestBooks::epub2(png(16, 20))));
        QVERIFY(TestBooks::write(booksDir + QStringLiteral("/two.pdf"), TestBooks::pdf("Two", "Auth")));

        QSignalSpy scanDone(&library, &Library::scanFinished);
        library.addFolder(booksDir);
        QTRY_COMPARE_WITH_TIMEOUT(scanDone.count(), 1, 10000);
        library.addFolder(goneDir);
        QTRY_COMPARE_WITH_TIMEOUT(scanDone.count(), 2, 10000);
        QVERIFY(QDir(goneDir).removeRecursively());

        const QVariantList stats = library.folderStats();
        QCOMPARE(stats.size(), 2);
        QCOMPARE(stats[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("shelf-a"));
        QCOMPARE(stats[0].toMap().value(QStringLiteral("exists")).toBool(), true);
        QCOMPARE(stats[0].toMap().value(QStringLiteral("books")).toInt(), 2);
        QCOMPARE(stats[1].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("shelf-gone"));
        QCOMPARE(stats[1].toMap().value(QStringLiteral("exists")).toBool(), false);
        QCOMPARE(stats[1].toMap().value(QStringLiteral("books")).toInt(), 0);

        Scanner scanner(dir.filePath(QStringLiteral("scan-covers")));
        scanner.scan({booksDir}, {}, {});
        QVERIFY(scanner.running());
        scanner.cancel();
        QVERIFY(!scanner.running());
    }

    void wave2CompanionRecapsFirstSeenResumeSdcvSseGeminiIllustratorAndTranslator() {
        QTemporaryDir dir;
        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
        const QByteArray origPath = qgetenv("PATH");
        const QLocale origLocale;
        const Qt::LayoutDirection origDir = QGuiApplication::layoutDirection();
        auto cleanup = qScopeGuard([&] {
            if (origEnvKey.isEmpty())
                qunsetenv("GEMINI_API_KEY");
            else
                qputenv("GEMINI_API_KEY", origEnvKey);
            qputenv("PATH", origPath);
            QLocale::setDefault(origLocale);
            QGuiApplication::setLayoutDirection(origDir);
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai"));
            s.remove(QStringLiteral("local"));
            s.remove(QStringLiteral("appearance/language"));
        });
        qunsetenv("GEMINI_API_KEY");

        // Build a 3-chapter linear EPUB so resolveFirstSeen() and recap spoiler-guard span real chapters.
        const QByteArray epubBytes = TestBooks::epub({
            {"OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<package version=\"2.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
             "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
             "    <dc:title>Fog Over Baker Street</dc:title>\n"
             "    <dc:creator>Arthur Doyle</dc:creator>\n"
             "    <dc:language>en</dc:language>\n"
             "  </metadata>\n"
             "  <manifest>\n"
             "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
             "    <item id=\"ch0\" href=\"ch0.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
             "    <item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
             "    <item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
             "  </manifest>\n"
             "  <spine toc=\"ncx\">\n"
             "    <itemref idref=\"ch0\"/>\n"
             "    <itemref idref=\"ch1\"/>\n"
             "    <itemref idref=\"ch2\"/>\n"
             "  </spine>\n"
             "</package>"},
            {"OEBPS/toc.ncx",
             "<?xml version=\"1.0\"?>\n"
             "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>\n"
             "  <navPoint id=\"n0\" playOrder=\"1\"><navLabel><text>Prologue</text></navLabel><content src=\"ch0.xhtml\"/></navPoint>\n"
             "  <navPoint id=\"n1\" playOrder=\"2\"><navLabel><text>Interlude</text></navLabel><content src=\"ch1.xhtml\"/></navPoint>\n"
             "  <navPoint id=\"n2\" playOrder=\"3\"><navLabel><text>Revelation</text></navLabel><content src=\"ch2.xhtml\"/></navPoint>\n"
             "</navMap></ncx>"},
            {"OEBPS/ch0.xhtml", TestBooks::chapter("Prologue",
                "<p>A quiet figure called The Detective watched the brass lantern while Lestrade took notes.</p>")},
            {"OEBPS/ch1.xhtml", TestBooks::chapter("Interlude",
                "<p>Rain drummed softly across the empty cobblestone square until dawn.</p>")},
            {"OEBPS/ch2.xhtml", TestBooks::chapter("Revelation",
                "<p>At last Sherlock Holmes greeted Inspector Lestrade beneath the archway.</p>")},
        });
        const QString epubPath = dir.filePath(QStringLiteral("fog.epub"));
        QVERIFY(TestBooks::write(epubPath, epubBytes));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        reader.relocated({{"chapter", 2}, {"offset", 0}, {"end", 120}, {"page", 0}, {"pages", 1}});

        FakeServer cloudSrv;
        FakeServer localSrv;
        GeminiProvider gemini(nullptr, cloudSrv.endpoint());
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        services.setLocalModels(&local);

        // 1. Companion::resumePendingSetup() handoff when invoked unconfigured.
        Companion companion(&services, &reader, library.database().connectionName());
        companion.ask(QStringLiteral("Who watched the lantern?"));
        QVERIFY(!companion.busy());
        QVERIFY(companion.problemIsConfig());
        QVERIFY(companion.hasPendingSetup());
        QVERIFY(companion.pendingSetupLabel().contains(QStringLiteral("Who watched the lantern?")));

        LocalModels::Server srv;
        srv.kind = LocalModels::Ollama;
        srv.url = QUrl(localSrv.origin());
        local.configure(srv, QStringLiteral("qwen3.5:4b"));
        services.setPreset(QStringLiteral("local"));
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"The Detective watched the lantern.\"},\"done\":true}\n"});
        companion.resumePendingSetup();
        QVERIFY(!companion.hasPendingSetup());
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QCOMPARE(companion.conversation().size(), 2);
        QVERIFY(companion.conversation().last().toMap().value(QStringLiteral("text")).toString()
                    .contains(QStringLiteral("The Detective")));

        // 2. Companion::resolveFirstSeen() earliest-mention ordering across chapters (alias & surname in ch0 beat full name in ch2).
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"{\\\"people\\\":["
                                   "{\\\"name\\\":\\\"Sherlock Holmes\\\",\\\"also\\\":\\\"The Detective\\\","
                                   "\\\"who\\\":\\\"Consulting detective.\\\",\\\"chapter\\\":\\\"Revelation\\\"},"
                                   "{\\\"name\\\":\\\"Inspector Lestrade\\\",\\\"also\\\":\\\"\\\","
                                   "\\\"who\\\":\\\"Scotland Yard inspector.\\\",\\\"chapter\\\":\\\"Revelation\\\"}"
                                   "]}\"},\"done\":true}\n"});
        companion.whosWho();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QCOMPARE(companion.people().size(), 2);
        const QVariantMap holmes = companion.people()[0].toMap();
        const QVariantMap lestrade = companion.people()[1].toMap();
        QCOMPARE(holmes.value(QStringLiteral("chapterIndex")).toInt(), 0);
        QCOMPARE(holmes.value(QStringLiteral("chapter")).toString(), QStringLiteral("Prologue"));
        QVERIFY(holmes.value(QStringLiteral("offset")).toLongLong() > 0);
        QCOMPARE(lestrade.value(QStringLiteral("chapterIndex")).toInt(), 0);
        QCOMPARE(lestrade.value(QStringLiteral("chapter")).toString(), QStringLiteral("Prologue"));
        QVERIFY(lestrade.value(QStringLiteral("offset")).toLongLong() > holmes.value(QStringLiteral("offset")).toLongLong());

        // 3. companion_recaps persistence, memory cache, reload, backward spoiler guard, forward auto-restore & AFTER DELETE trigger.
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"Recap up to Revelation.\"},\"done\":true}\n"});
        const qsizetype reqBeforeRecap = localSrv.requests.size();
        companion.recap();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QCOMPARE(localSrv.requests.size(), reqBeforeRecap + 1);
        QCOMPARE(companion.recapLoaded(), true);
        QCOMPARE(companion.recapChapter(), 2);
        QCOMPARE(companion.recapText(), QStringLiteral("Recap up to Revelation."));
        QCOMPARE(companion.recapWhereLocal(), true);

        // Second call at same chapter hits memory cache (0 new requests).
        companion.recap();
        QVERIFY(!companion.busy());
        QCOMPARE(localSrv.requests.size(), reqBeforeRecap + 1);

        // Fresh Companion restores persisted recap from SQLite with 0 network requests.
        {
            Companion fresh(&services, &reader, library.database().connectionName());
            QCOMPARE(fresh.recapLoaded(), true);
            QCOMPARE(fresh.recapChapter(), 2);
            QCOMPARE(fresh.recapText(), QStringLiteral("Recap up to Revelation."));
        }

        // Backward navigation to chapter 0 hides chapter 2 recap (strict spoiler guard).
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});
        QCOMPARE(companion.recapLoaded(), false);
        QVERIFY(companion.recapText().isEmpty());

        // Returning forward to chapter 2 automatically restores the cached chapter 2 recap.
        reader.relocated({{"chapter", 2}, {"offset", 0}, {"end", 120}, {"page", 0}, {"pages", 1}});
        QCOMPARE(companion.recapLoaded(), true);
        QCOMPARE(companion.recapText(), QStringLiteral("Recap up to Revelation."));

        // 4. sdcv punctuation trim & single fallback guarantee.
        const QString fakeBin = dir.filePath(QStringLiteral("sdcv-bin"));
        QVERIFY(QDir().mkpath(fakeBin));
        const QString sdcvLog = dir.filePath(QStringLiteral("sdcv.log"));
        const QString sdcvScript = fakeBin + QStringLiteral("/sdcv");
        write(sdcvScript,
              QStringLiteral("#!/bin/sh\n"
                             "last=\"\"\nfor a in \"$@\"; do last=\"$a\"; done\n"
                             "printf '%s\\n' \"$last\" >> \"%1\"\n"
                             "if [ \"$last\" = \"lantern\" ]; then\n"
                             "  printf '[{\"word\":\"lantern\",\"dict\":\"WordNet\",\"definition\":\"A portable lamp.\"}]'\n"
                             "  exit 0\n"
                             "fi\n"
                             "exit 1\n")
                  .arg(sdcvLog)
                  .toUtf8());
        QVERIFY(QFile::setPermissions(sdcvScript, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("PATH", fakeBin.toUtf8() + ":" + origPath);

        companion.define(QStringLiteral("“lantern,”"), 0, 10);
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QCOMPARE(companion.where(), QStringLiteral("Dictionary on this computer"));
        QVERIFY(companion.answer().contains(QStringLiteral("A portable lamp.")));
        {
            QFile logFile(sdcvLog);
            QVERIFY(logFile.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromUtf8(logFile.readAll()).trimmed(), QStringLiteral("lantern"));
        }

        // Missing dictionary word falls back to the AI model exactly ONCE.
        const qsizetype reqBeforeMiss = localSrv.requests.size();
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"*noun* — A rare fogbound word.\"},\"done\":true}\n"});
        companion.define(QStringLiteral("unlistedword"), 0, 10);
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QTest::qWait(50);
        QCOMPARE(localSrv.requests.size(), reqBeforeMiss + 1);
        QVERIFY(companion.answer().contains(QStringLiteral("rare fogbound word")));

        // Broken non-executable sdcv (FailedToStart -> errorOccurred) also triggers a single fallback.
        QVERIFY(QFile::setPermissions(sdcvScript, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const qsizetype reqBeforeBroken = localSrv.requests.size();
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"*noun* — Fallback after FailedToStart.\"},\"done\":true}\n"});
        companion.define(QStringLiteral("anotherword"), 0, 10);
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QTest::qWait(50);
        QCOMPARE(localSrv.requests.size(), reqBeforeBroken + 1);
        QVERIFY(companion.answer().contains(QStringLiteral("Fallback after FailedToStart")));
        qputenv("PATH", origPath);

        // Verify AFTER DELETE ON books trigger cascades companion_recaps, companion_threads, companion_whoswho.
        {
            QSqlQuery countBefore(library.database());
            QVERIFY(countBefore.exec(QStringLiteral("SELECT COUNT(*) FROM companion_recaps WHERE book_id = %1").arg(bookId)));
            QVERIFY(countBefore.next());
            QCOMPARE(countBefore.value(0).toInt(), 1);
            QSqlQuery delBook(library.database());
            QVERIFY(delBook.exec(QStringLiteral("DELETE FROM books WHERE id = %1").arg(bookId)));
            for (const char *tbl : {"companion_recaps", "companion_threads", "companion_whoswho"}) {
                QSqlQuery check(library.database());
                QVERIFY(check.exec(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE book_id = %2")
                                       .arg(QString::fromLatin1(tbl))
                                       .arg(bookId)));
                QVERIFY(check.next());
                QCOMPARE(check.value(0).toInt(), 0);
            }
        }

        // 5. SseParser: UTF-8 BOM + chunk-split \r\n + bare \r.
        {
            SseParser sse;
            const auto bomEvents = sse.feed("\xEF\xBB\xBF" "event: step.delta\r\ndata: hello\r\n\r\n");
            QCOMPARE(bomEvents.size(), 1);
            QCOMPARE(bomEvents[0].name, QByteArray("step.delta"));
            QCOMPARE(bomEvents[0].data, QByteArray("hello"));

            QVERIFY(sse.feed("event: step.delta\r").isEmpty());
            QVERIFY(sse.feed("\ndata: line1\r").isEmpty());
            const auto splitEvents = sse.feed("\ndata: line2\r\n\r\n");
            QCOMPARE(splitEvents.size(), 1);
            QCOMPARE(splitEvents[0].name, QByteArray("step.delta"));
            QCOMPARE(splitEvents[0].data, QByteArray("line1\nline2"));

            // Bare CR preserves a trailing '\r' across chunk boundaries so a chunk-split CRLF
            // isn't prematurely split, and flushes cleanly once followed by another line or finish().
            QVERIFY(sse.feed("data: bare1\rdata: bare2\r\r").isEmpty());
            const auto bareCr = sse.finish();
            QCOMPARE(bareCr.size(), 1);
            QCOMPARE(bareCr[0].data, QByteArray("bare1\nbare2"));
        }

        // 6. GeminiReply: mid-stream SSE error mapping & single-emit abort().
        {
            gemini.setKey(QStringLiteral("test-gemini-key"));
            struct SseCase {
                QByteArray body;
                Ai::Error::Kind expected;
            };
            const QList<SseCase> cases = {
                {"event: error\ndata: {\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"message\":\"quota\"}}\n\n",
                 Ai::Error::Limited},
                {"event: interaction.failed\ndata: {\"interaction\":{\"error\":{\"status\":\"UNAUTHENTICATED\",\"message\":\"bad key\"}}}\n\n",
                 Ai::Error::BadKey},
                {"event: error\ndata: {\"error\":{\"status\":\"PERMISSION_DENIED\",\"message\":\"denied\"}}\n\n",
                 Ai::Error::Refused},
                {"event: error\ndata: {\"error\":{\"code\":404,\"message\":\"missing\"}}\n\n",
                 Ai::Error::NotFound},
                {"event: error\ndata: {\"error\":{\"status\":\"INVALID_ARGUMENT\",\"message\":\"bad arg\"}}\n\n",
                 Ai::Error::Invalid},
            };
            for (const SseCase &tc : cases) {
                cloudSrv.responses.append({200, "text/event-stream", tc.body});
                Ai::TextRequest req;
                req.feature = QStringLiteral("ask");
                req.stream = true;
                req.messages = {{true, QStringLiteral("hi")}};
                std::unique_ptr<Ai::Reply> reply(gemini.generate(req));
                QSignalSpy fin(reply.get(), &Ai::Reply::finished);
                QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 5000);
                QCOMPARE(reply->error().kind, tc.expected);
            }

            // Single-emit abort() on an active hanging GeminiReply.
            QTcpServer hangSrv;
            QVERIFY(hangSrv.listen(QHostAddress::LocalHost));
            QList<QPointer<QTcpSocket>> socks;
            connect(&hangSrv, &QTcpServer::newConnection, &hangSrv, [&hangSrv, &socks] {
                while (QTcpSocket *s = hangSrv.nextPendingConnection())
                    socks.append(s);
            });
            GeminiProvider hangGemini(nullptr, QStringLiteral("http://127.0.0.1:%1/v1beta").arg(hangSrv.serverPort()));
            hangGemini.setKey(QStringLiteral("test-gemini-key"));
            Ai::TextRequest req;
            req.feature = QStringLiteral("ask");
            req.stream = true;
            req.messages = {{true, QStringLiteral("abort me")}};
            std::unique_ptr<Ai::Reply> abortReply(hangGemini.generate(req));
            QSignalSpy abortSpy(abortReply.get(), &Ai::Reply::finished);
            abortReply->abort();
            abortReply->abort();
            QTest::qWait(50);
            QCOMPARE(abortSpy.count(), 1);
            QCOMPARE(abortReply->error().kind, Ai::Error::Cancelled);
            for (const QPointer<QTcpSocket> &s : std::as_const(socks))
                if (s)
                    s->close();
            hangSrv.close();
        }

        // 7. Illustrator::budgetOk / unavailableReason / saveCopyTo().
        {
            const int illBookId = library.addBookFile(epubPath);
            QVERIFY(illBookId > 0);
            QVERIFY(reader.openBook(illBookId));
            reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 90}, {"page", 0}, {"pages", 1}});

            Illustrator illustrator(&services, &reader, library.database().connectionName(),
                                    dir.filePath(QStringLiteral("illustrations")));
            // No key
            gemini.setKey({});
            services.setPreset(QStringLiteral("cloud"));
            QVERIFY(!illustrator.available());
            QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("add your key")));

            // Key present, strict local mode
            gemini.setKey(QStringLiteral("test-gemini-key"));
            services.setPreset(QStringLiteral("local"));
            QVERIFY(!illustrator.available());
            QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("Hybrid or Cloud")));

            // Cloud mode with $0 monthly cap
            services.setPreset(QStringLiteral("cloud"));
            services.setUnlimitedBudget(false);
            services.setBudget(0.0);
            QVERIFY(illustrator.available());
            QVERIFY(!illustrator.budgetOk());
            QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("$0")));

            // Cloud mode with non-zero cap below picture cost
            services.setBudget(0.001);
            QVERIFY(!illustrator.budgetOk());
            QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("monthly budget")));

            // Cloud mode with healthy budget -> generate and saveCopyTo()
            services.setBudget(10.0);
            QVERIFY(illustrator.budgetOk());
            QVERIFY(illustrator.unavailableReason().isEmpty());

            const QByteArray imgPng = png(48, 36);
            const QByteArray promptReply = QJsonDocument(QJsonObject{
                {QStringLiteral("steps"), QJsonArray{QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("model_output")},
                    {QStringLiteral("content"), QJsonArray{QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("{\"prompt\":\"Victorian fog over Baker Street lantern\",\"caption\":\"The brass lantern\"}")},
                    }}},
                }}},
            }).toJson(QJsonDocument::Compact);
            const QByteArray imageReply = QJsonDocument(QJsonObject{
                {QStringLiteral("candidates"), QJsonArray{QJsonObject{
                    {QStringLiteral("content"), QJsonObject{
                        {QStringLiteral("parts"), QJsonArray{QJsonObject{
                            {QStringLiteral("inlineData"), QJsonObject{
                                {QStringLiteral("mimeType"), QStringLiteral("image/png")},
                                {QStringLiteral("data"), QString::fromLatin1(imgPng.toBase64())},
                            }},
                        }}},
                    }},
                }}},
            }).toJson(QJsonDocument::Compact);
            cloudSrv.responses.append({200, "application/json", promptReply});
            cloudSrv.responses.append({200, "application/json", imageReply});
            QSignalSpy madeSpy(&illustrator, &Illustrator::made);
            illustrator.illustrate(QStringLiteral("watched the brass lantern"), 0, 60);
            QTRY_COMPARE_WITH_TIMEOUT(madeSpy.count(), 1, 5000);
            const int picId = madeSpy[0][0].toInt();
            QVERIFY(picId > 0);

            const QString explicitDest = dir.filePath(QStringLiteral("exports/lantern-copy.png"));
            QCOMPARE(illustrator.saveCopyTo(picId, explicitDest), explicitDest);
            QFile copied(explicitDest);
            QVERIFY(copied.open(QIODevice::ReadOnly));
            QCOMPARE(copied.readAll(), imgPng);

            const QString exportDir = dir.filePath(QStringLiteral("exports-dir"));
            QVERIFY(QDir().mkpath(exportDir));
            const QString intoDir = illustrator.saveCopyTo(picId, exportDir);
            QVERIFY(intoDir.startsWith(exportDir + QLatin1Char('/')));
            QVERIFY(QFileInfo::exists(intoDir));
            QVERIFY(illustrator.saveCopyTo(999999, explicitDest).isEmpty());
        }

        // 8. Translator::setLanguage() / languageOverride isolation & live reload.
        {
            const QString langFile = dir.filePath(QStringLiteral("omarchy-lang"));
            const QString catDir = dir.filePath(QStringLiteral("i18n"));
            QVERIFY(QDir().mkpath(catDir));
            write(langFile, "de\n");
            write(catDir + QStringLiteral("/en.json"), R"({"Hello":"Hello"})");
            write(catDir + QStringLiteral("/de.json"), R"({"Hello":"Hallo"})");
            write(catDir + QStringLiteral("/fr.json"), R"({"Hello":"Bonjour"})");
            write(catDir + QStringLiteral("/ar.json"), R"({"Hello":"مرحبا"})");

            Translator isolated(langFile, catDir);
            QCOMPARE(isolated.language(), QStringLiteral("de"));
            QCOMPARE(isolated.languageOverride(), QString());
            QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("Hallo"));

            QSignalSpy changedSpy(&isolated, &Translator::changed);
            isolated.setLanguage(QStringLiteral("fr"));
            QCOMPARE(isolated.languageOverride(), QStringLiteral("fr"));
            QCOMPARE(isolated.language(), QStringLiteral("fr"));
            QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("Bonjour"));
            QCOMPARE(changedSpy.count(), 1);
            // Explicit languageFile stays isolated from settings.ini.
            QVERIFY(QSettings(settingsPath, QSettings::IniFormat).value(QStringLiteral("appearance/language")).toString().isEmpty());

            // Invalid language codes are ignored safely.
            isolated.setLanguage(QStringLiteral("../evil"));
            QCOMPARE(isolated.languageOverride(), QStringLiteral("fr"));
            QCOMPARE(changedSpy.count(), 1);

            // Live RTL reload on Arabic, then reset to desktop ("") restores German + LTR.
            isolated.setLanguage(QStringLiteral("ar"));
            QCOMPARE(isolated.language(), QStringLiteral("ar"));
            QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("مرحبا"));
            QCOMPARE(QGuiApplication::layoutDirection(), Qt::RightToLeft);

            isolated.setLanguage(QString());
            QCOMPARE(isolated.languageOverride(), QString());
            QCOMPARE(isolated.language(), QStringLiteral("de"));
            QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("Hallo"));
            QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);

            // Default constructor (empty languageFile) persists override under appearance/language.
            {
                Translator persisted(QString(), catDir);
                persisted.setLanguage(QStringLiteral("fr"));
                QCOMPARE(QSettings(settingsPath, QSettings::IniFormat).value(QStringLiteral("appearance/language")).toString(),
                         QStringLiteral("fr"));
            }
            {
                Translator reloaded(QString(), catDir);
                QCOMPARE(reloaded.languageOverride(), QStringLiteral("fr"));
                QCOMPARE(reloaded.language(), QStringLiteral("fr"));
                reloaded.setLanguage(QString());
                QVERIFY(QSettings(settingsPath, QSettings::IniFormat).value(QStringLiteral("appearance/language")).toString().isEmpty());
            }
        }
    }

    // --- Legacy Dogear -> Clothbound XDG migration & guard tests -----------

    void migrationMovesGenuineLegacyDataAndRenamesVoiceSentinels() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
        const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
        const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
        const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
        const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
        const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

        // 1. Seed a genuine legacy config, library DB, voice pack sentinel, illustration, and cache.
        QDir().mkpath(legacyCfg);
        QDir().mkpath(legacyData + QStringLiteral("/voices/kokoro-v1.0"));
        QDir().mkpath(legacyData + QStringLiteral("/illustrations"));
        QDir().mkpath(legacyCache + QStringLiteral("/covers"));
        QDir().mkpath(legacyCache + QStringLiteral("/audio"));

        write(legacyCfg + QStringLiteral("/settings.ini"),
              "[reading]\ntheme=sepia\n[onboarding]\nwelcomed=true\n");

        QDir().mkpath(dir.filePath(QStringLiteral("books")));
        const QString bookPath = dir.filePath(QStringLiteral("books/legacy.epub"));
        QVERIFY(TestBooks::write(bookPath, TestBooks::epub2({})));
        {
            Library legacyLib(legacyData, legacyCache);
            QVERIFY(legacyLib.open());
            const int id = legacyLib.addBookFile(bookPath);
            QVERIFY(id > 0);
        }
        write(legacyData + QStringLiteral("/voices/kokoro-v1.0/.dogear-installed"), "kokoro-v1.0 deadbeef\n");
        write(legacyData + QStringLiteral("/illustrations/scene.png"), "PNGDATA");
        write(legacyCache + QStringLiteral("/audio/clip.opus"), "OPUSDATA");
        write(legacyCache + QStringLiteral("/voices.json"), "{\"voices\":[]}");

        QVERIFY(Migration::isLegacyConfigDir(legacyCfg));
        QVERIFY(Migration::isLegacyDataDir(legacyData));

        const Migration::Paths paths{legacyCfg, newCfg, legacyData, newData, legacyCache, newCache};
        const Migration::Result res = Migration::run(paths);
        QVERIFY(res.configMigrated);
        QVERIFY(res.dataMigrated);
        QVERIFY(res.cacheMigrated);
        QCOMPARE(res.voiceSentinelsRenamed, 1);

        // Verify everything landed cleanly in Clothbound paths and sentinels were renamed.
        QVERIFY(!QFileInfo::exists(legacyCfg));
        QVERIFY(!QFileInfo::exists(legacyData));
        QVERIFY(!QFileInfo::exists(legacyCache));
        QCOMPARE(QSettings(newCfg + QStringLiteral("/settings.ini"), QSettings::IniFormat)
                     .value(QStringLiteral("reading/theme")).toString(),
                 QStringLiteral("sepia"));
        QVERIFY(QFileInfo::exists(newData + QStringLiteral("/library.sqlite")));
        QVERIFY(QFileInfo::exists(newData + QStringLiteral("/voices/kokoro-v1.0/.clothbound-installed")));
        QVERIFY(!QFileInfo::exists(newData + QStringLiteral("/voices/kokoro-v1.0/.dogear-installed")));
        QVERIFY(QFileInfo::exists(newData + QStringLiteral("/illustrations/scene.png")));
        QVERIFY(QFileInfo::exists(newCache + QStringLiteral("/audio/clip.opus")));
        QVERIFY(QFileInfo::exists(newCache + QStringLiteral("/voices.json")));

        // Migrated library opens cleanly with the book preserved.
        {
            Library migratedLib(newData, newCache);
            QVERIFY(migratedLib.open());
            QCOMPARE(migratedLib.bookCount(), 1);
            QCOMPARE(migratedLib.records().first().title, QStringLiteral("The Lantern Keeper"));
        }

        // Second run is a clean idempotent no-op.
        const Migration::Result second = Migration::run(paths);
        QVERIFY(!second.configMigrated);
        QVERIFY(!second.dataMigrated);
        QVERIFY(!second.cacheMigrated);
        QCOMPARE(second.voiceSentinelsRenamed, 0);

        // VoicePacks::isInstalled() also honors legacy .dogear-installed as a read fallback.
        const QString fallbackVoicesDir = dir.filePath(QStringLiteral("fallback-voices"));
        QDir().mkpath(fallbackVoicesDir + QStringLiteral("/engine"));
        write(fallbackVoicesDir + QStringLiteral("/engine/.dogear-installed"), "engine legacy\n");
        const QByteArray manifest = R"({"engine":{"x86_64":{"url":"http://127.0.0.1/e.tar.bz2","size":10,"sha256":"00","dir":"engine"}}})";
        VoicePacks packs(fallbackVoicesDir, nullptr, QStringLiteral("x86_64"), manifest);
        QVERIFY(packs.isInstalled(QStringLiteral("engine")));
    }

    void migrationRejectsForeignDogearDirectories() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
        const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
        const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
        const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
        const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
        const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

        // Foreign config (e.g. ninepointlabs/dogear) with unrecognized groups.
        QDir().mkpath(legacyCfg);
        write(legacyCfg + QStringLiteral("/settings.ini"), "[sync]\nendpoint=https://example.invalid\n");

        // Foreign SQLite DB with unrelated schema.
        QDir().mkpath(legacyData);
        const QString foreignDbPath = legacyData + QStringLiteral("/library.sqlite");
        {
            const QString conn = QStringLiteral("foreign-dogear-test");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(foreignDbPath);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("CREATE TABLE bookmarks (id INTEGER PRIMARY KEY, url TEXT)")));
                QVERIFY(q.exec(QStringLiteral("INSERT INTO bookmarks (url) VALUES ('https://example.invalid')")));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }

        // Foreign cache directory.
        QDir().mkpath(legacyCache + QStringLiteral("/covers"));
        write(legacyCache + QStringLiteral("/covers/foreign.bin"), "FOREIGN");

        QVERIFY(!Migration::isLegacyConfigDir(legacyCfg));
        QVERIFY(!Migration::isLegacyDataDir(legacyData));
        QVERIFY(!Migration::isLegacyCacheDir(legacyCache));

        const Migration::Result res = Migration::run({legacyCfg, newCfg, legacyData, newData, legacyCache, newCache});
        QVERIFY(!res.configMigrated);
        QVERIFY(!res.dataMigrated);
        QVERIFY(!res.cacheMigrated);
        QCOMPARE(res.voiceSentinelsRenamed, 0);

        // Foreign files must remain completely untouched and nothing created in Clothbound paths.
        QVERIFY(QFileInfo::exists(legacyCfg + QStringLiteral("/settings.ini")));
        QVERIFY(QFileInfo::exists(foreignDbPath));
        QVERIFY(QFileInfo::exists(legacyCache + QStringLiteral("/covers/foreign.bin")));
        QVERIFY(!QFileInfo::exists(newCfg));
        QVERIFY(!QFileInfo::exists(newData));
        QVERIFY(!QFileInfo::exists(newCache));

        // Corrupt / non-SQLite file named library.sqlite is also rejected safely.
        const QString corruptData = dir.filePath(QStringLiteral("corrupt/dogear"));
        QDir().mkpath(corruptData);
        write(corruptData + QStringLiteral("/library.sqlite"), "not a sqlite database");
        QVERIFY(!Migration::isLegacyDataDir(corruptData));
    }

    void migrationNeverClobbersExistingClothboundData() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
        const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
        const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
        const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
        const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
        const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

        // Seed genuine legacy Dogear data...
        QDir().mkpath(legacyCfg);
        QDir().mkpath(legacyData);
        QDir().mkpath(legacyCache + QStringLiteral("/audio"));
        write(legacyCfg + QStringLiteral("/settings.ini"), "[reading]\ntheme=paper\n");
        {
            Library legacyLib(legacyData, legacyCache);
            QVERIFY(legacyLib.open());
        }
        write(legacyCache + QStringLiteral("/voices.json"), "{\"source\":\"legacy\"}");

        // ...AND already-populated Clothbound buckets.
        QDir().mkpath(newCfg);
        QDir().mkpath(newData);
        QDir().mkpath(newCache);
        write(newCfg + QStringLiteral("/settings.ini"), "[reading]\ntheme=night\n");
        QDir().mkpath(dir.filePath(QStringLiteral("books")));
        const QString newBook = dir.filePath(QStringLiteral("books/clothbound.epub"));
        QVERIFY(TestBooks::write(newBook, TestBooks::epub2({})));
        {
            Library existingLib(newData, newCache);
            QVERIFY(existingLib.open());
            QVERIFY(existingLib.addBookFile(newBook) > 0);
        }
        write(newCache + QStringLiteral("/voices.json"), "{\"source\":\"clothbound\"}");

        const Migration::Result res = Migration::run({legacyCfg, newCfg, legacyData, newData, legacyCache, newCache});
        QVERIFY(!res.configMigrated);
        QVERIFY(!res.dataMigrated);
        QVERIFY(!res.cacheMigrated);

        // Existing Clothbound config, library, and cache are completely untouched.
        QCOMPARE(QSettings(newCfg + QStringLiteral("/settings.ini"), QSettings::IniFormat)
                     .value(QStringLiteral("reading/theme")).toString(),
                 QStringLiteral("night"));
        {
            Library checkLib(newData, newCache);
            QVERIFY(checkLib.open());
            QCOMPARE(checkLib.bookCount(), 1);
            QCOMPARE(checkLib.records().first().title, QStringLiteral("The Lantern Keeper"));
        }
        QFile cacheFile(newCache + QStringLiteral("/voices.json"));
        QVERIFY(cacheFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(cacheFile.readAll()), QStringLiteral("{\"source\":\"clothbound\"}"));

        // Legacy data also remains intact (never deleted when skipped).
        QVERIFY(QFileInfo::exists(legacyCfg + QStringLiteral("/settings.ini")));
        QVERIFY(QFileInfo::exists(legacyData + QStringLiteral("/library.sqlite")));
        QVERIFY(QFileInfo::exists(legacyCache + QStringLiteral("/voices.json")));
    }

    // --- Wave 4 Regression Tests (Track A: Study Companion, Track B: PDF Research Craft, Track C: Local MCP & CLI) ---

    void wave4MultimodalPayloadsAndVisionErrors() {
        const QByteArray samplePng = png(48, 32);
        const QString expectedBase64 = QString::fromLatin1(samplePng.toBase64());

        // 1. GeminiProvider::textBody() attaches inline base64 image part to the LAST user turn.
        {
            Ai::TextRequest req;
            req.feature = QStringLiteral("ask");
            req.system = QStringLiteral("Be concise.");
            req.stream = true;
            req.imagePng = samplePng;
            req.messages = {
                {true, QStringLiteral("Earlier question")},
                {false, QStringLiteral("Earlier answer")},
                {true, QStringLiteral("Explain this equation")},
            };
            const QJsonObject body = GeminiProvider::textBody(req, QStringLiteral("gemini-2.5-flash"));
            QVERIFY(body.value(QStringLiteral("stream")).toBool());
            const QJsonArray input = body.value(QStringLiteral("input")).toArray();
            QCOMPARE(input.size(), 3);
            QCOMPARE(input[0].toObject().value(QStringLiteral("content")).toArray().size(), 1);
            const QJsonArray lastContent = input[2].toObject().value(QStringLiteral("content")).toArray();
            QCOMPARE(lastContent.size(), 2);
            QCOMPARE(lastContent[0].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("text"));
            QCOMPARE(lastContent[0].toObject().value(QStringLiteral("text")).toString(),
                     QStringLiteral("Explain this equation"));
            const QJsonObject imgPart = lastContent[1].toObject();
            QCOMPARE(imgPart.value(QStringLiteral("type")).toString(), QStringLiteral("image"));
            QCOMPARE(imgPart.value(QStringLiteral("mime_type")).toString(), QStringLiteral("image/png"));
            QCOMPARE(imgPart.value(QStringLiteral("data")).toString(), expectedBase64);

            // Image-only request with no user messages still emits a valid user_input turn.
            Ai::TextRequest imageOnly;
            imageOnly.imagePng = samplePng;
            const QJsonObject onlyBody = GeminiProvider::textBody(imageOnly, QStringLiteral("gemini-2.5-flash"));
            const QJsonArray onlyInput = onlyBody.value(QStringLiteral("input")).toArray();
            QCOMPARE(onlyInput.size(), 1);
            QCOMPARE(onlyInput[0].toObject().value(QStringLiteral("content")).toArray()[0].toObject().value(QStringLiteral("type")).toString(),
                     QStringLiteral("image"));
        }

        // 2. GeminiProvider::generate() streams SSE multimodal response cleanly.
        {
            FakeServer server;
            const QByteArray sse =
                "event: step.delta\n"
                "data: {\"delta\":{\"type\":\"text\",\"text\":\"Euler's identity \"}}\n\n"
                "event: step.delta\n"
                "data: {\"delta\":{\"type\":\"text\",\"text\":\"holds.\"}}\n\n"
                "event: interaction.completed\n"
                "data: {\"interaction\":{\"usage\":{\"total_input_tokens\":42,\"total_output_tokens\":9}}}\n\n";
            server.responses.append({200, "text/event-stream", sse});

            GeminiProvider gemini(nullptr, server.endpoint());
            gemini.setKey(QStringLiteral("test-key"));
            Ai::TextRequest req;
            req.feature = QStringLiteral("ask");
            req.stream = true;
            req.imagePng = samplePng;
            req.messages = {{true, QStringLiteral("What is shown?")}};
            Ai::Reply *reply = gemini.generate(req);
            QString streamed;
            QObject::connect(reply, &Ai::Reply::textReceived, [&](const QString &d) { streamed += d; });
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
            QVERIFY(!reply->error());
            QCOMPARE(streamed, QStringLiteral("Euler's identity holds."));
            QCOMPARE(reply->usage().inputTokens, qint64(42));
            QCOMPARE(server.requests.size(), 1);
            QVERIFY(server.requests.first().body.contains(samplePng.toBase64()));
            delete reply;
        }

        // 3. LocalModels::chatBody() uses OpenAI content array (text + image_url data URI) across backends.
        {
            Ai::TextRequest req;
            req.feature = QStringLiteral("ask");
            req.stream = true;
            req.imagePng = samplePng;
            req.messages = {{true, QStringLiteral("Convert to LaTeX")}};
            const QString expectedUri = QStringLiteral("data:image/png;base64,") + expectedBase64;

            for (LocalModels::Kind kind : {LocalModels::Ollama, LocalModels::LmStudio, LocalModels::LlamaCpp}) {
                const QJsonObject body = LocalModels::chatBody(kind, req, QStringLiteral("qwen2.5vl:7b"));
                // Ollama multimodal switches to OpenAI format (no native `options` block).
                QVERIFY(!body.contains(QStringLiteral("options")));
                const QJsonArray msgs = body.value(QStringLiteral("messages")).toArray();
                QCOMPARE(msgs.size(), 1);
                const QJsonArray content = msgs[0].toObject().value(QStringLiteral("content")).toArray();
                QCOMPARE(content.size(), 2);
                QCOMPARE(content[0].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("text"));
                QCOMPARE(content[1].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("image_url"));
                QCOMPARE(content[1].toObject().value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString(),
                         expectedUri);
            }
        }

        // 4. Ollama multimodal routes to /v1/chat/completions (while text-only uses /api/chat),
        //    and HTTP 400 / 422 / unsupported-image errors map to LocalModels::visionUnsupportedMessage().
        {
            FakeServer server;
            LocalModels local;
            LocalModels::Server cfg;
            cfg.kind = LocalModels::Ollama;
            cfg.url = QUrl(server.origin());
            local.configure(cfg, QStringLiteral("gemma3:12b"));

            // Multimodal -> /v1/chat/completions
            const QByteArray openAiSse =
                "data: {\"choices\":[{\"delta\":{\"content\":\"x^2 + y^2\"}}]}\n\n"
                "data: [DONE]\n\n";
            server.responses.append({200, "text/event-stream", openAiSse});
            Ai::TextRequest mmReq;
            mmReq.feature = QStringLiteral("ask");
            mmReq.stream = true;
            mmReq.imagePng = samplePng;
            mmReq.messages = {{true, QStringLiteral("Read formula")}};
            Ai::Reply *mmReply = local.generate(mmReq);
            QTRY_VERIFY_WITH_TIMEOUT(mmReply->isFinished(), 5000);
            QVERIFY2(!mmReply->error(), qPrintable(mmReply->error().message));
            QCOMPARE(mmReply->text(), QStringLiteral("x^2 + y^2"));
            QCOMPARE(server.requests.last().path, QByteArray("/v1/chat/completions"));
            delete mmReply;

            // Text-only -> /api/chat
            server.responses.append({200, "application/x-ndjson",
                                     "{\"message\":{\"content\":\"Plain text reply\"},\"done\":true}\n"});
            Ai::TextRequest txtReq;
            txtReq.feature = QStringLiteral("ask");
            txtReq.stream = true;
            txtReq.messages = {{true, QStringLiteral("Hello")}};
            Ai::Reply *txtReply = local.generate(txtReq);
            QTRY_VERIFY_WITH_TIMEOUT(txtReply->isFinished(), 5000);
            QVERIFY(!txtReply->error());
            QCOMPARE(server.requests.last().path, QByteArray("/api/chat"));
            delete txtReply;

            // HTTP 400, 422, and 500 vision-unsupported responses map cleanly to visionUnsupportedMessage().
            const QList<QPair<int, QByteArray>> errorCases{
                {400, "{\"error\":{\"message\":\"invalid input\"}}"},
                {422, "{\"error\":\"unprocessable multimodal content\"}"},
                {500, "{\"error\":\"model does not support vision or image inputs\"}"},
            };
            for (const auto &[status, body] : errorCases) {
                server.responses.append({status, "application/json", body});
                Ai::Reply *errReply = local.generate(mmReq);
                QTRY_VERIFY_WITH_TIMEOUT(errReply->isFinished(), 5000);
                QCOMPARE(errReply->error().kind, Ai::Error::Invalid);
                QCOMPARE(errReply->error().message, LocalModels::visionUnsupportedMessage());
                QVERIFY(errReply->error().message.contains(QStringLiteral("Gemma 3")));
                QVERIFY(errReply->error().message.contains(QStringLiteral("Qwen2.5-VL")));
                delete errReply;
            }
        }
    }

    void wave4AdaptiveReadingModePersistenceAndGlossary() {
        // 1. Companion::detectReadingMode() classification heuristics.
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("pdf"), QStringLiteral("A Novel"), {}, QStringLiteral("Once upon a time.")),
                 QStringLiteral("study"));
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Preprint"), {},
                                              QStringLiteral("See arXiv:2401.12345 for details.")),
                 QStringLiteral("study"));
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Article"), {},
                                              QStringLiteral("doi:10.1145/1234567.1234568")),
                 QStringLiteral("study"));
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Notes"), {},
                                              QStringLiteral("We prove Theorem 3.2 using induction.")),
                 QStringLiteral("study"));
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Survey"),
                                              {QStringLiteral("Abstract"), QStringLiteral("1. Intro"), QStringLiteral("References")},
                                              QStringLiteral("Plain overview.")),
                 QStringLiteral("study"));
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("The Lantern Keeper"),
                                              {QStringLiteral("Chapter One"), QStringLiteral("The Storm")},
                                              QStringLiteral("The lamp's light shone across the harbor.")),
                 QStringLiteral("fiction"));
        // A single weak signal alone (only "Abstract" without references/formal markers) stays fiction.
        QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Abstract Art"),
                                              {QStringLiteral("Abstract")},
                                              QStringLiteral("Gallery paintings from the summer.")),
                 QStringLiteral("fiction"));

        // 2. Database schema stays at version 4; companion_mode persists in book_settings;
        //    companion_recaps & companion_whoswho stay isolated by mode + migrate legacy tables + cascade on book delete.
        QTemporaryDir dir;
        const QString fictionPath = dir.filePath(QStringLiteral("story.epub"));
        const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>Harbor Tale</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en</dc:language>\n"
            "  </metadata>\n"
            "  <manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
            "  <spine><itemref idref=\"c1\"/></spine>\n"
            "</package>";
        QVERIFY(TestBooks::write(
            fictionPath,
            TestBooks::epub({
                {"OEBPS/content.opf", opf},
                {"OEBPS/c1.xhtml",
                 TestBooks::chapter("Ch 1",
                                    "<p>Ada Marsh studies SGD optimization where θ minimizes the loss ℒ with gradient ∇ℒ(θ).</p>")},
            })));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Optimization Paper", "Jane Doe")));

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreSettings = qScopeGuard([&] {
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai/preset"));
        });

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        QCOMPARE(library.schemaVersion(), 4);

        const int fictionId = library.addBookFile(fictionPath);
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(fictionId > 0);
        QVERIFY(pdfId > 0);

        // Seed pre-Wave-3 legacy tables (without `mode` column) BEFORE instantiating Companion to test zero-loss migration.
        {
            QSqlQuery q(library.database());
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE companion_whoswho ("
                " book_id INTEGER NOT NULL, chapter INTEGER NOT NULL, offset INTEGER NOT NULL,"
                " page_start INTEGER NOT NULL DEFAULT 0, where_label TEXT NOT NULL DEFAULT '',"
                " is_local INTEGER NOT NULL DEFAULT -1, people TEXT NOT NULL, updated_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, chapter, offset))")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO companion_whoswho (book_id, chapter, offset, page_start, where_label, is_local, people, updated_at) "
                "VALUES (%1, 0, 120, 0, 'Ollama', 1, "
                "'[{\"name\":\"Ada Marsh\",\"also\":\"\",\"who\":\"Legacy keeper\",\"chapter\":\"Ch 1\",\"chapterIndex\":0,\"offset\":0}]', 100)")
                               .arg(fictionId)));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE companion_recaps ("
                " book_id INTEGER NOT NULL, chapter INTEGER NOT NULL, offset INTEGER NOT NULL,"
                " page_start INTEGER NOT NULL DEFAULT 0, where_label TEXT NOT NULL DEFAULT '',"
                " is_local INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL, updated_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, chapter, offset))")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO companion_recaps (book_id, chapter, offset, page_start, where_label, is_local, text, updated_at) "
                "VALUES (%1, 0, 120, 0, 'Ollama', 1, 'Legacy fiction recap.', 100)")
                               .arg(fictionId)));
        }

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(fictionId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 120}, {"page", 0}, {"pages", 1}});

        FakeServer server;
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        LocalModels::Server cfg;
        cfg.kind = LocalModels::Ollama;
        cfg.url = QUrl(server.origin());
        local.configure(cfg, QStringLiteral("qwen3.5:4b"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        Companion companion(&services, &reader, library.database().connectionName());
        // Fiction book auto-detects "fiction" + default scope "upto" + restores migrated legacy rows!
        QCOMPARE(companion.readingMode(), QStringLiteral("fiction"));
        QVERIFY(!companion.studyMode());
        QCOMPARE(companion.scope(), QStringLiteral("upto"));
        QVERIFY(companion.recapLoaded());
        QCOMPARE(companion.recapText(), QStringLiteral("Legacy fiction recap."));
        QVERIFY(companion.peopleLoaded());
        QCOMPARE(companion.people().first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Ada Marsh"));

        // Switch manually to Study mode: persisted in book_settings, scope flips to "book", caches stay isolated.
        companion.setReadingMode(QStringLiteral("study"));
        QCOMPARE(companion.readingMode(), QStringLiteral("study"));
        QVERIFY(companion.studyMode());
        QCOMPARE(companion.scope(), QStringLiteral("book"));
        QCOMPARE(library.bookSetting(fictionId, QStringLiteral("companion_mode")), QStringLiteral("study"));
        QCOMPARE(library.schemaVersion(), 4);
        QVERIFY(!companion.recapLoaded());
        QVERIFY(!companion.peopleLoaded());

        // Generate Study Brief & Study Glossary (with symbols θ, ℒ, ∇, SGD and First-defined offsets).
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"**Core Claims**\\nMinimizes loss $\\\\mathcal{L}(\\\\theta)$.\"},\"done\":true}\n"});
        companion.recap();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QVERIFY(companion.recapLoaded());
        QVERIFY(companion.recapText().contains(QStringLiteral("Core Claims")));

        const QByteArray glossaryReply =
            "{\"message\":{\"content\":\"{\\\"people\\\":["
            "{\\\"name\\\":\\\"θ\\\",\\\"also\\\":\\\"parameters\\\",\\\"who\\\":\\\"Model weights\\\",\\\"chapter\\\":\\\"Ch 1\\\"},"
            "{\\\"name\\\":\\\"ℒ\\\",\\\"also\\\":\\\"loss\\\",\\\"who\\\":\\\"Objective function\\\",\\\"chapter\\\":\\\"Ch 1\\\"},"
            "{\\\"name\\\":\\\"∇\\\",\\\"also\\\":\\\"gradient\\\",\\\"who\\\":\\\"Gradient operator\\\",\\\"chapter\\\":\\\"Ch 1\\\"},"
            "{\\\"name\\\":\\\"SGD\\\",\\\"also\\\":\\\"\\\",\\\"who\\\":\\\"Stochastic Gradient Descent\\\",\\\"chapter\\\":\\\"Ch 1\\\"}"
            "]}\"},\"done\":true}\n";
        server.responses.append({200, "application/x-ndjson", glossaryReply});
        companion.whosWho();
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QVERIFY(companion.peopleLoaded());
        QCOMPARE(companion.people().size(), 4);
        for (const QVariant &v : companion.people()) {
            const QVariantMap entry = v.toMap();
            QCOMPARE(entry.value(QStringLiteral("chapterIndex")).toInt(), 0);
            QVERIFY2(entry.value(QStringLiteral("offset")).toLongLong() > 0,
                     qPrintable(entry.value(QStringLiteral("name")).toString()));
        }

        // Fresh Companion restores persisted "study" override + Study caches; switching back to "fiction" restores legacy fiction caches.
        {
            Companion restored(&services, &reader, library.database().connectionName());
            QCOMPARE(restored.readingMode(), QStringLiteral("study"));
            QVERIFY(restored.recapLoaded());
            QVERIFY(restored.recapText().contains(QStringLiteral("Core Claims")));
            QCOMPARE(restored.people().size(), 4);

            restored.setReadingMode(QStringLiteral("fiction"));
            QVERIFY(restored.recapLoaded());
            QCOMPARE(restored.recapText(), QStringLiteral("Legacy fiction recap."));
            QCOMPARE(restored.people().size(), 1);
            QCOMPARE(restored.people().first().toMap().value(QStringLiteral("name")).toString(),
                     QStringLiteral("Ada Marsh"));
        }

        // PDF book auto-detects "study" mode with default scope "book".
        QVERIFY(reader.openBook(pdfId));
        QCOMPARE(companion.readingMode(), QStringLiteral("study"));
        QCOMPARE(companion.scope(), QStringLiteral("book"));

        // AFTER DELETE ON books trigger cascades both fiction & study rows in companion_recaps / companion_whoswho.
        {
            QSqlQuery del(library.database());
            QVERIFY(del.exec(QStringLiteral("DELETE FROM books WHERE id = %1").arg(fictionId)));
            QSqlQuery check(library.database());
            QVERIFY(check.exec(QStringLiteral("SELECT COUNT(*) FROM companion_recaps WHERE book_id = %1").arg(fictionId)));
            QVERIFY(check.next());
            QCOMPARE(check.value(0).toInt(), 0);
            QVERIFY(check.exec(QStringLiteral("SELECT COUNT(*) FROM companion_whoswho WHERE book_id = %1").arg(fictionId)));
            QVERIFY(check.next());
            QCOMPARE(check.value(0).toInt(), 0);
        }
    }

    void wave4SmartScopeSpoilerGuardAndShelfCitations() {
        QTemporaryDir dir;
        auto writeSimpleEpub = [&](const QString &fileName, const QString &title,
                                   const QList<QPair<QString, QString>> &chapters) {
            const QString path = dir.filePath(fileName);
            QByteArray manifest, spine;
            QList<QPair<QString, QByteArray>> files;
            for (int i = 0; i < chapters.size(); ++i) {
                const QByteArray id = "c" + QByteArray::number(i + 1);
                const QByteArray href = id + ".xhtml";
                manifest += "    <item id=\"" + id + "\" href=\"" + href + "\" media-type=\"application/xhtml+xml\"/>\n";
                spine += "<itemref idref=\"" + id + "\"/>";
                files.append({QStringLiteral("OEBPS/") + QString::fromLatin1(href),
                              TestBooks::chapter(chapters[i].first.toUtf8(),
                                                 "<p>" + chapters[i].second.toUtf8() + "</p>")});
            }
            const QByteArray opf =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                "    <dc:title>" + title.toUtf8() + "</dc:title><dc:creator>Researcher</dc:creator><dc:language>en</dc:language>\n"
                "  </metadata>\n"
                "  <manifest>\n" + manifest + "  </manifest>\n"
                "  <spine>" + spine + "</spine>\n"
                "</package>";
            files.prepend({QStringLiteral("OEBPS/content.opf"), opf});
            return TestBooks::write(path, TestBooks::epub(files)) ? path : QString();
        };

        const QString book1Path = writeSimpleEpub(
            QStringLiteral("main.epub"), QStringLiteral("Attention Mechanisms"),
            {{QStringLiteral("Introduction"), QStringLiteral("Self-attention computes scaled dot products across query tokens.")},
             {QStringLiteral("Appendix"), QStringLiteral("Secret Appendix Proof of Lemma Nine: eigenvalues decay exponentially.")}});
        const QString book2Path = writeSimpleEpub(
            QStringLiteral("shelf_peer.epub"), QStringLiteral("Variational Bounds"),
            {{QStringLiteral("Chapter 1"), QStringLiteral("Variational inference uses the ELBO objective to bound log-likelihood cleanly.")}});
        const QString book3Path = writeSimpleEpub(
            QStringLiteral("outside.epub"), QStringLiteral("Outside Unrelated"),
            {{QStringLiteral("Chapter 1"), QStringLiteral("Outside-only ELBO sentinel SECRET_OUTSIDE_SHELF_TOKEN should never leak.")}});
        QVERIFY(!book1Path.isEmpty() && !book2Path.isEmpty() && !book3Path.isEmpty());

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreSettings = qScopeGuard([&] {
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai/preset"));
        });

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int book1Id = library.addBookFile(book1Path);
        const int book2Id = library.addBookFile(book2Path);
        const int book3Id = library.addBookFile(book3Path);
        QVERIFY(book1Id > 0 && book2Id > 0 && book3Id > 0);

        const int shelfId = library.createCollection(QStringLiteral("ML Papers"));
        QVERIFY(shelfId > 0);
        QVERIFY(library.addToCollection(book1Id, shelfId));
        QVERIFY(library.addToCollection(book2Id, shelfId));
        // book3Id is intentionally NOT in "ML Papers".

        FakeServer server;
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        LocalModels::Server cfg;
        cfg.kind = LocalModels::Ollama;
        cfg.url = QUrl(server.origin());
        local.configure(cfg, QStringLiteral("qwen3.5:4b"), QStringLiteral("nomic-embed-text"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        // 1. PassageIndex::rankLibrary() and searchLibrary() enforce `bookIds` scoping.
        server.routes.insert("/api/embed",
                             {200, "application/json", "{\"embeddings\":[[1.0,0.0,0.0]]}"});
        {
            PassageIndex pIndex(&services, library.database().connectionName());
            auto embedBook = [&](int id, const QString &text) {
                bool done = false;
                pIndex.build(id, {{0, 0, text.size(), text}}, nullptr, [&](const QString &err) {
                    QVERIFY2(err.isEmpty(), qPrintable(err));
                    done = true;
                });
                QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
            };
            embedBook(book1Id, QStringLiteral("Self-attention scaled dot products."));
            embedBook(book2Id, QStringLiteral("Variational inference ELBO bound."));
            embedBook(book3Id, QStringLiteral("Outside-only ELBO sentinel."));

            const QSet<int> shelfSet{book1Id, book2Id};
            const QList<PassageIndex::LibraryHit> ranked =
                PassageIndex::rankLibrary(library.database().databaseName(), QStringLiteral("local/nomic-embed-text"),
                                          {1.0f, 0.0f, 0.0f}, 10, 0.25, shelfSet);
            QCOMPARE(ranked.size(), 2);
            for (const auto &h : ranked)
                QVERIFY(shelfSet.contains(h.bookId));

            QList<PassageIndex::LibraryHit> searched;
            bool searchDone = false;
            pIndex.searchLibrary(QStringLiteral("ELBO"), 10,
                                 [&](const QList<PassageIndex::LibraryHit> &hits) {
                                     searched = hits;
                                     searchDone = true;
                                 },
                                 shelfSet);
            QTRY_VERIFY_WITH_TIMEOUT(searchDone, 5000);
            QCOMPARE(searched.size(), 2);
            for (const auto &h : searched)
                QVERIFY(shelfSet.contains(h.bookId));
        }

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(book1Id));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});

        Companion companion(&services, &reader, library.database().connectionName());
        QCOMPARE(companion.shelfLabel(), QStringLiteral("ML Papers"));

        // 2. "upto" scope enforces spoiler guard (never sends Ch 1 Appendix while reader is on Ch 0).
        companion.setReadingMode(QStringLiteral("fiction"));
        QCOMPARE(companion.scope(), QStringLiteral("upto"));
        server.requests.clear();
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"Scaled dot products.\"},\"done\":true}\n"});
        companion.ask(QStringLiteral("What about Lemma Nine?"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QByteArray uptoBody;
        for (const auto &r : server.requests)
            if (r.path == "/api/chat")
                uptoBody = r.body;
        QVERIFY(!uptoBody.isEmpty());
        QVERIFY(!uptoBody.contains("Secret Appendix Proof of Lemma Nine"));

        // 3. "book" scope accesses later chapters / appendix while reader is still on Ch 0.
        companion.setReadingMode(QStringLiteral("study"));
        QCOMPARE(companion.scope(), QStringLiteral("book"));
        companion.newConversation();
        server.requests.clear();
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"Eigenvalues decay exponentially.\"},\"done\":true}\n"});
        companion.ask(QStringLiteral("Explain Secret Appendix Proof of Lemma Nine"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QByteArray bookBody;
        for (const auto &r : server.requests)
            if (r.path == "/api/chat")
                bookBody = r.body;
        QVERIFY(bookBody.contains("Secret Appendix Proof of Lemma Nine"));

        // 4. "shelf" scope searches strictly inside "ML Papers" (includes Book 2, excludes Book 3).
        companion.setScope(QStringLiteral("shelf"));
        QCOMPARE(companion.scope(), QStringLiteral("shelf"));
        companion.newConversation();
        server.requests.clear();
        server.responses.append({200, "application/x-ndjson",
                                 "{\"message\":{\"content\":\"ELBO bounds log-likelihood [Variational Bounds, Ch. 1].\"},\"done\":true}\n"});
        companion.ask(QStringLiteral("How does the ELBO objective work across this shelf?"));
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QByteArray shelfBody;
        for (const auto &r : server.requests)
            if (r.path == "/api/chat")
                shelfBody = r.body;
        QVERIFY(shelfBody.contains("Variational Bounds"));
        QVERIFY(!shelfBody.contains("SECRET_OUTSIDE_SHELF_TOKEN"));

        // 5. activateCitation() emits openBookPassage only for cross-book citations.
        QSignalSpy jumpSpy(&companion, &Companion::openBookPassage);
        companion.activateCitation(book1Id, 0, 0, QStringLiteral("Self-attention"));
        QCOMPARE(jumpSpy.count(), 0);
        companion.activateCitation(book2Id, 0, 12, QStringLiteral("Variational inference uses the ELBO"));
        QCOMPARE(jumpSpy.count(), 1);
        QCOMPARE(jumpSpy.first().at(0).toInt(), book2Id);
        QCOMPARE(jumpSpy.first().at(1).toInt(), 0);
        QCOMPARE(jumpSpy.first().at(2).toString(), QStringLiteral("Variational inference uses the ELBO"));
    }

    void wave4OfflineLatexMathAndSsrfPreservation() {
        QTemporaryDir dir;
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        Companion companion(&services, &reader, library.database().connectionName());

        // 1. Inline ($...$, \(...\)) and display ($$...$$, \[...\]) math: Greek, operators, \mathbb, \mathcal, \sqrt, sup/sub, \frac.
        const QString inlineMath =
            Companion::cleanMarkdown(QStringLiteral("Let $\\theta \\in \\mathbb{R}^d$ minimize $\\mathcal{L}(x_i) = \\sqrt{a + b}$ and \\(\\alpha \\to \\beta\\)."));
        QVERIFY(inlineMath.contains(QStringLiteral("class=\"math-inline\"")));
        QVERIFY(inlineMath.contains(QStringLiteral("θ")));
        QVERIFY(inlineMath.contains(QStringLiteral("ℝ")));
        QVERIFY(inlineMath.contains(QStringLiteral("<sup>d</sup>")));
        QVERIFY(inlineMath.contains(QStringLiteral("ℒ")));
        QVERIFY(inlineMath.contains(QStringLiteral("<sub>i</sub>")));
        QVERIFY(inlineMath.contains(QStringLiteral("√")));
        QVERIFY(inlineMath.contains(QStringLiteral("→")));
        QVERIFY(companion.hasMath(QStringLiteral("Check $\\nabla \\mathcal{L}$")));

        const QString displayMath =
            Companion::cleanMarkdown(QStringLiteral("Formula:\n$$\\sum_{i=1}^n \\frac{x_i}{n} \\le \\int_0^1 f(t) dt$$\nAlso:\n\\[a \\times b\\]"));
        QVERIFY(displayMath.contains(QStringLiteral("class=\"math-block\"")));
        QVERIFY(displayMath.contains(QStringLiteral("∑")));
        QVERIFY(displayMath.contains(QStringLiteral("≤")));
        QVERIFY(displayMath.contains(QStringLiteral("∫")));
        QVERIFY(displayMath.contains(QStringLiteral("×")));

        const QString rich = companion.formatDisplayHtml(
            QStringLiteral("**Result**: $\\alpha + \\beta$\n\n$$\\frac{1}{2}$$"), QStringLiteral("#222633"));
        QVERIFY(rich.contains(QStringLiteral("α")));
        QVERIFY(rich.contains(QStringLiteral("#222633")));

        // 2. Currency preservation ($5 to $10 stays literal plain text, not math).
        const QString currency = QStringLiteral("Tickets cost $5 to $10, or $12.50 at the door.");
        QVERIFY(!companion.hasMath(currency));
        const QString cleanedCurrency = Companion::cleanMarkdown(currency);
        QCOMPARE(cleanedCurrency, currency);

        // 3. Fenced code block preservation (LaTeX inside ```latex ... ``` stays verbatim).
        const QString fenced = QStringLiteral("```latex\n$\\alpha + \\beta$\n```");
        const QString cleanedFence = Companion::cleanMarkdown(fenced);
        QVERIFY(!cleanedFence.contains(QStringLiteral("class=\"math-inline\"")));
        QVERIFY(cleanedFence.contains(QStringLiteral("$\\alpha + \\beta$")));

        // 4. Zero-click SSRF preservation (outside AND inside math delimiters!).
        const QString hostile = QStringLiteral(
            "Text <img src=\"http://evil.invalid/a.png\"> ![leak](https://evil.invalid/b.png) "
            "<script>alert(1)</script><link rel=\"stylesheet\" href=\"http://evil.invalid/c.css\">"
            "<iframe src=\"http://evil.invalid/d\"></iframe> "
            "Math $\\alpha <img src=\"http://evil.invalid/m.png\"> + \\beta$ "
            "$$![nested](http://evil.invalid/n.png) + \\theta$$");
        const QString safe = Companion::cleanMarkdown(hostile);
        QVERIFY(!safe.contains(QStringLiteral("<img"), Qt::CaseInsensitive));
        QVERIFY(!safe.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!safe.contains(QStringLiteral("<link"), Qt::CaseInsensitive));
        QVERIFY(!safe.contains(QStringLiteral("<iframe"), Qt::CaseInsensitive));
        QVERIFY(!safe.contains(QStringLiteral("evil.invalid")));
        QVERIFY(!safe.contains(QStringLiteral("![")));
        QVERIFY(safe.contains(QStringLiteral("α")));
        QVERIFY(safe.contains(QStringLiteral("θ")));
    }

    void wave4VisualSnipEntryAndPrivacyBudgetInvariants() {
        QTemporaryDir dir;
        const QString pdfPath = dir.filePath(QStringLiteral("eq.pdf"));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Equations", "A. Researcher")));

        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreSettings = qScopeGuard([&] {
            QSettings s(settingsPath, QSettings::IniFormat);
            s.remove(QStringLiteral("ai/preset"));
            s.remove(QStringLiteral("ai/budget"));
        });

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        FakeServer localServer;
        FakeServer cloudServer;
        GeminiProvider gemini(nullptr, cloudServer.endpoint());
        MemoryKeyStore keys;
        keys.save(QStringLiteral("gemini"), QStringLiteral("test-cloud-key"));
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        LocalModels::Server cfg;
        cfg.kind = LocalModels::Ollama;
        cfg.url = QUrl(localServer.origin());
        local.configure(cfg, QStringLiteral("qwen2.5vl:7b"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        Companion companion(&services, &reader, library.database().connectionName());
        const QString b64 = QString::fromLatin1(png(40, 24).toBase64());

        // 1. "ask" with empty prompt attaches pending snip & emits openAskRequested() without network call.
        QSignalSpy openAskSpy(&companion, &Companion::openAskRequested);
        companion.askAboutSnipBase64(b64, QStringLiteral("ask"), QString(), 0);
        QVERIFY(companion.hasPendingSnip());
        QVERIFY(companion.pendingSnipUrl().startsWith(QStringLiteral("data:image/png;base64,")));
        QCOMPARE(companion.pendingSnipPage(), 0);
        QCOMPARE(openAskSpy.count(), 1);
        QCOMPARE(localServer.requests.size(), 0);

        // Sending ask() consumes pending snip, posts image to /v1/chat/completions, and restricts snipUrl strictly to user turn.
        localServer.responses.append({200, "text/event-stream",
                                      "data: {\"choices\":[{\"delta\":{\"content\":\"Solved: $x = 2$.\"}}]}\n\n"
                                      "data: [DONE]\n\n"});
        companion.ask(QStringLiteral("Solve for x"));
        QVERIFY(!companion.hasPendingSnip());
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        QCOMPARE(localServer.requests.size(), 1);
        QCOMPARE(localServer.requests.first().path, QByteArray("/v1/chat/completions"));
        QCOMPARE(companion.conversation().size(), 2);
        QVERIFY(companion.conversation()[0].toMap().value(QStringLiteral("fromUser")).toBool());
        QVERIFY(companion.conversation()[0].toMap().value(QStringLiteral("snipUrl")).toString().startsWith(QStringLiteral("data:image/png;base64,")));
        QVERIFY(!companion.conversation()[1].toMap().value(QStringLiteral("fromUser")).toBool());
        QVERIFY(companion.conversation()[1].toMap().value(QStringLiteral("snipUrl")).toString().isEmpty());

        // 2. Direct snip actions: "explain", "latex", "table".
        for (const QString &action : {QStringLiteral("explain"), QStringLiteral("latex"), QStringLiteral("table")}) {
            localServer.responses.append({200, "text/event-stream",
                                          "data: {\"choices\":[{\"delta\":{\"content\":\"Answer for " + action.toUtf8() + "\"}}]}\n\n"
                                          "data: [DONE]\n\n"});
            companion.askAboutSnipBase64(b64, action, QString(), 0);
            QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
            QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
        }
        QCOMPARE(localServer.requests.size(), 4);

        // 3. Invalid image payload fails cleanly without sending a request.
        companion.askAboutSnipBase64(QStringLiteral("not-valid-png!!!"), QStringLiteral("explain"), QString(), 0);
        QVERIFY(!companion.problem().isEmpty());
        QCOMPARE(localServer.requests.size(), 4);

        // 4. Strict "local" invariant: when local server rejects images (HTTP 400), NEVER fall back to Gemini even with key set.
        localServer.responses.append({400, "application/json", "{\"error\":\"vision unsupported\"}"});
        companion.askAboutSnipBase64(b64, QStringLiteral("explain"), QString(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QCOMPARE(companion.problem(), LocalModels::visionUnsupportedMessage());
        QCOMPARE(cloudServer.requests.size(), 0);

        // 5. $0 cloud budget invariant: switching to "cloud" with $0 budget blocks outbound cloud snip & ask calls.
        services.setPreset(QStringLiteral("cloud"));
        services.setBudget(0.0);
        companion.clearProblem();
        companion.askAboutSnipBase64(b64, QStringLiteral("explain"), QString(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
        QVERIFY(!companion.problem().isEmpty());
        QCOMPARE(cloudServer.requests.size(), 0);
    }

    void wave4PdfRegionSnipClampingAndThreadSafety() {
        QTemporaryDir dir;
        const QString pdfPath = dir.filePath(QStringLiteral("rendered.pdf"));
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setResolution(72);
            QPainter painter(&writer);
            painter.fillRect(QRect(40, 60, 460, 140), QColor(QStringLiteral("#224488")));
            painter.setPen(Qt::white);
            painter.drawText(QRect(60, 100, 400, 60), QStringLiteral("Equation 1: E = mc^2"));
            writer.newPage();
            painter.fillRect(QRect(40, 400, 460, 140), QColor(QStringLiteral("#884422")));
            painter.end();
        }

        PdfBook pdf(pdfPath);
        QVERIFY2(pdf.open(), qPrintable(pdf.errorString()));
        QCOMPARE(pdf.pageCount(), 2);

        // Valid normalized rect renders crisp non-blank PNG matching target width.
        const QByteArray regionPng = pdf.renderRegionPng(0, 0.05, 0.05, 0.85, 0.25, 700);
        QVERIFY(!regionPng.isEmpty());
        const QImage decoded = QImage::fromData(regionPng, "PNG");
        QVERIFY(!decoded.isNull());
        QVERIFY(decoded.width() >= 600 && decoded.width() <= 800);
        QVERIFY(decoded.height() > 50);

        // Partially out-of-range rects clamp cleanly into [0..1] and still produce a valid PNG.
        const QByteArray clampedPng = pdf.renderRegionPng(0, -0.2, -0.1, 0.6, 0.5, 400);
        QVERIFY(!clampedPng.isEmpty());
        QVERIFY(!QImage::fromData(clampedPng, "PNG").isNull());

        // Invalid / degenerate / NaN / Inf / out-of-bounds page rects return empty safely.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        QVERIFY(pdf.renderRegionPng(-1, 0.1, 0.1, 0.5, 0.5).isEmpty());
        QVERIFY(pdf.renderRegionPng(2, 0.1, 0.1, 0.5, 0.5).isEmpty());
        QVERIFY(pdf.renderRegionPng(0, 1.5, 0.1, 0.2, 0.2).isEmpty());
        QVERIFY(pdf.renderRegionPng(0, 0.2, 0.2, 0.0, 0.2).isEmpty());
        QVERIFY(pdf.renderRegionPng(0, 0.2, 0.2, -0.3, 0.2).isEmpty());
        QVERIFY(pdf.renderRegionPng(0, 0.2, 0.2, 0.0001, 0.2).isEmpty());
        QVERIFY(pdf.renderRegionPng(0, nan, 0.2, 0.3, 0.3).isEmpty());
        QVERIFY(pdf.renderRegionPng(0, 0.2, inf, 0.3, 0.3).isEmpty());

        // Reader::snipPdfRegionBase64() returns valid base64 PNG for open PDF and empty when closed.
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.snipPdfRegionBase64(0, 0.1, 0.1, 0.5, 0.3).isEmpty());
        QVERIFY(reader.openBook(bookId));
        const QString b64 = reader.snipPdfRegionBase64(0, 0.05, 0.05, 0.85, 0.25);
        QVERIFY(!b64.isEmpty());
        QVERIFY(!QImage::fromData(QByteArray::fromBase64(b64.toLatin1()), "PNG").isNull());
        QVERIFY(reader.snipPdfRegionBase64(99, 0.1, 0.1, 0.5, 0.3).isEmpty());

        // Thread safety: concurrent renderRegionPng() calls on a shared PdfBook stay race-free.
        QList<QFuture<QByteArray>> futures;
        for (int i = 0; i < 6; ++i) {
            const int page = i % 2;
            const double y = 0.05 + 0.05 * i;
            futures.append(QtConcurrent::run([&pdf, page, y] {
                return pdf.renderRegionPng(page, 0.05, y, 0.7, 0.25, 320);
            }));
        }
        for (auto &f : futures) {
            const QByteArray bytes = f.result();
            QVERIFY(!bytes.isEmpty());
            QVERIFY(!QImage::fromData(bytes, "PNG").isNull());
        }
    }

    void wave4PdfLinksDestYAndPeekDestination() {
        // Build a 2-page PDF:
        // - Page 0 has two internal links to Page 1:
        //   1) /XYZ 0 100 1 (on 300pt height -> ~0.67 down from top)
        //   2) /Fit (unspecified Y -> destY == 0.0)
        // - Page 1 has distinct top, middle, and bottom lines.
        auto makePeekPdf = [] {
            QByteArray out = "%PDF-1.4\n";
            QList<qsizetype> offsets;
            auto object = [&](const QByteArray &body) {
                offsets.append(out.size());
                out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
            };
            object("<< /Type /Catalog /Pages 2 0 R >>");                                       // 1
            object("<< /Type /Pages /Kids [3 0 R 8 0 R] /Count 2 >>");                         // 2
            object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 300] /Contents 4 0 R "
                   "/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R 7 0 R] >>");         // 3 (page 0)
            const QByteArray s1 = "BT /F1 10 Tf 20 260 Td (See Reference [1] and Figure 1) Tj ET";
            object("<< /Length " + QByteArray::number(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");  // 4
            object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");                               // 5
            object("<< /Type /Annot /Subtype /Link /Rect [20 245 120 270] /A << /S /GoTo /D [8 0 R /XYZ 0 100 1] >> >>");  // 6
            object("<< /Type /Annot /Subtype /Link /Rect [130 245 220 270] /A << /S /GoTo /D [8 0 R /Fit] >> >>");         // 7
            object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 300] /Contents 9 0 R "
                   "/Resources << /Font << /F1 5 0 R >> >> >>");                                            // 8 (page 1)
            const QByteArray s2 =
                "BT /F1 10 Tf 20 275 Td (Top Figure 1 Architecture Diagram) Tj ET\n"
                "BT /F1 10 Tf 20 150 Td (Middle Lemma Proof Statement) Tj ET\n"
                "BT /F1 10 Tf 20 50 Td (Bottom Reference [1] Vaswani et al.) Tj ET";
            object("<< /Length " + QByteArray::number(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");  // 9
            const qsizetype xref = out.size();
            out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
            for (const qsizetype off : offsets)
                out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
            out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
            out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
            return out;
        };

        QTemporaryDir dir;
        const QString pdfPath = dir.filePath(QStringLiteral("peek.pdf"));
        QVERIFY(TestBooks::write(pdfPath, makePeekPdf()));

        PdfBook pdf(pdfPath);
        QVERIFY2(pdf.open(), qPrintable(pdf.errorString()));
        const QList<PdfLinkEntry> links = pdf.pageLinks(0);
        QCOMPARE(links.size(), 2);
        bool sawPositioned = false;
        bool sawDefaultZero = false;
        for (const PdfLinkEntry &link : links) {
            QCOMPARE(link.page, 1);
            QVERIFY(link.destY >= 0.0 && link.destY <= 1.0);
            if (link.destY > 0.5 && link.destY < 0.85)
                sawPositioned = true;
            if (qFuzzyIsNull(link.destY))
                sawDefaultZero = true;
        }
        QVERIFY(sawPositioned);
        QVERIFY(sawDefaultZero);

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.peekPdfDestination(0, 0.0).isEmpty());
        QVERIFY(reader.openBook(bookId));

        // Reader::pdfLinks() exposes destY cleanly to QML.
        const QVariantList qmlLinks = reader.pdfLinks(0);
        QCOMPARE(qmlLinks.size(), 2);
        QVERIFY(qmlLinks.first().toMap().contains(QStringLiteral("destY")));

        // Top-strip fallback when destY < 0 captures top line & valid data URL image.
        const QVariantMap topPeek = reader.peekPdfDestination(1, -1.0);
        QCOMPARE(topPeek.value(QStringLiteral("page")).toInt(), 1);
        QCOMPARE(topPeek.value(QStringLiteral("pageLabel")).toString(), QStringLiteral("2"));
        const QString topUrl = topPeek.value(QStringLiteral("imageDataUrl")).toString();
        QVERIFY(topUrl.startsWith(QStringLiteral("data:image/png;base64,")));
        QVERIFY(!QImage::fromData(QByteArray::fromBase64(topUrl.mid(22).toLatin1()), "PNG").isNull());
        QVERIFY2(topPeek.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Top Figure 1")),
                 qPrintable(topPeek.value(QStringLiteral("excerpt")).toString()));

        // Bottom destY band extracts the bottom reference text rather than top text.
        const QVariantMap bottomPeek = reader.peekPdfDestination(1, 0.85);
        QCOMPARE(bottomPeek.value(QStringLiteral("page")).toInt(), 1);
        QVERIFY2(bottomPeek.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Bottom Reference")),
                 qPrintable(bottomPeek.value(QStringLiteral("excerpt")).toString()));
        QVERIFY(!bottomPeek.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Top Figure 1")));

        // Out-of-range destination pages return empty map.
        QVERIFY(reader.peekPdfDestination(-1, 0.0).isEmpty());
        QVERIFY(reader.peekPdfDestination(2, 0.5).isEmpty());
        QVERIFY(reader.peekPdfDestination(99, -1.0).isEmpty());
    }

    void wave4PdfAcademicIdExtractionAndCitations() {
        // 1. PdfBook::extractDoi() edge cases (trailing punctuation vs balanced parens).
        QCOMPARE(PdfBook::extractDoi(QStringLiteral("Published at doi: 10.1145/3292500.3330701.")),
                 QStringLiteral("10.1145/3292500.3330701"));
        QCOMPARE(PdfBook::extractDoi(QStringLiteral("(see https://doi.org/10.1000/xyz.123),")),
                 QStringLiteral("10.1000/xyz.123"));
        QCOMPARE(PdfBook::extractDoi(QStringLiteral("DOI: 10.1002/(SICI)1097-4571(1999);")),
                 QStringLiteral("10.1002/(SICI)1097-4571(1999)"));
        QVERIFY(PdfBook::extractDoi(QStringLiteral("No identifier in this sentence.")).isEmpty());

        // 2. PdfBook::extractArxivId() edge cases (prefix, case, URL, DataCite DOI).
        QCOMPARE(PdfBook::extractArxivId(QStringLiteral("Preprint arXiv:2305.14314v2 [cs.LG]")),
                 QStringLiteral("arXiv:2305.14314v2"));
        QCOMPARE(PdfBook::extractArxivId(QStringLiteral("ARXIV: 1706.03762")),
                 QStringLiteral("arXiv:1706.03762"));
        QCOMPARE(PdfBook::extractArxivId(QStringLiteral("Available at https://arxiv.org/pdf/2401.00001v1")),
                 QStringLiteral("arXiv:2401.00001v1"));
        QCOMPARE(PdfBook::extractArxivId(QStringLiteral("DOI: 10.48550/arXiv.2303.08774")),
                 QStringLiteral("arXiv:2303.08774"));
        QVERIFY(PdfBook::extractArxivId(QStringLiteral("Plain book text.")).isEmpty());

        // 3. End-to-end PDF scan on Reader::openBook() + cleanup on Reader::close().
        QTemporaryDir dir;
        const QString pdfPath = dir.filePath(QStringLiteral("academic.pdf"));
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setResolution(72);
            writer.setTitle(QStringLiteral("Attention & Scaling 100%"));
            writer.setCreator(QStringLiteral("Test"));
            QPainter p(&writer);
            p.drawText(QRect(20, 60, 550, 120),
                       QStringLiteral("arXiv:1706.03762v7  DOI: 10.1145/1234567.8901234."));
            p.end();
        }
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(!reader.isPdf());
        QVERIFY(reader.formatCitation(QStringLiteral("bibtex")).isEmpty());

        QVERIFY(reader.openBook(bookId));
        QVERIFY(reader.isPdf());
        QCOMPARE(reader.detectedArxiv(), QStringLiteral("arXiv:1706.03762v7"));
        QCOMPARE(reader.detectedDoi(), QStringLiteral("10.1145/1234567.8901234"));
        const QString openBib = reader.formatCitation(QStringLiteral("bibtex"));
        QVERIFY(openBib.startsWith(QStringLiteral("@article{")));
        QVERIFY(openBib.contains(QStringLiteral("doi = {10.1145/1234567.8901234}")));
        QVERIFY(openBib.contains(QStringLiteral("eprint = {1706.03762v7}")));
        QVERIFY(openBib.contains(QStringLiteral("Attention \\& Scaling 100\\%")));

        reader.close();
        QVERIFY(!reader.isPdf());
        QVERIFY(reader.detectedDoi().isEmpty());
        QVERIFY(reader.detectedArxiv().isEmpty());

        // 4. Reader::formatCitationFor() BibTeX (@article / @misc / @book) & APA 7th matrix.
        const QString bibArticle = Reader::formatCitationFor(
            {{QStringLiteral("title"), QStringLiteral("The Rate of $O(n)$ & 50% #1")},
             {QStringLiteral("author"), QStringLiteral("Élodie Müller and Jean-Luc Picard")},
             {QStringLiteral("year"), 2024},
             {QStringLiteral("doi"), QStringLiteral("https://doi.org/10.1000/test.42")},
             {QStringLiteral("format"), QStringLiteral("pdf")}},
            QStringLiteral("bibtex"));
        QVERIFY(bibArticle.startsWith(QStringLiteral("@article{muller2024rate,")));
        QVERIFY(bibArticle.contains(QStringLiteral("author = {Müller, Élodie and Picard, Jean-Luc}")));
        QVERIFY(bibArticle.contains(QStringLiteral("\\$O(n)\\$ \\& 50\\% \\#1")));
        QVERIFY(bibArticle.contains(QStringLiteral("doi = {10.1000/test.42}")));

        const QString bibArxiv = Reader::formatCitationFor(
            {{QStringLiteral("title"), QStringLiteral("QLoRA: Efficient Finetuning")},
             {QStringLiteral("author"), QStringLiteral("Tim Dettmers")},
             {QStringLiteral("arxiv"), QStringLiteral("arXiv:2305.14314")},
             {QStringLiteral("format"), QStringLiteral("pdf")}},
            QStringLiteral("bibtex"));
        QVERIFY(bibArxiv.startsWith(QStringLiteral("@misc{dettmers2023qlora,")));
        QVERIFY(bibArxiv.contains(QStringLiteral("year = {2023}")));
        QVERIFY(bibArxiv.contains(QStringLiteral("eprint = {2305.14314}")));
        QVERIFY(bibArxiv.contains(QStringLiteral("archivePrefix = {arXiv}")));
        QVERIFY(bibArxiv.contains(QStringLiteral("url = {https://arxiv.org/abs/2305.14314}")));

        const QString bibBook = Reader::formatCitationFor(
            {{QStringLiteral("title"), QStringLiteral("The Lantern Keeper")},
             {QStringLiteral("author"), QStringLiteral("Ada Marsh")},
             {QStringLiteral("publisher"), QStringLiteral("Harbor Books")},
             {QStringLiteral("isbn"), QStringLiteral("978-0-00-000000-2")},
             {QStringLiteral("year"), 1911},
             {QStringLiteral("format"), QStringLiteral("epub")}},
            QStringLiteral("bibtex"));
        QVERIFY(bibBook.startsWith(QStringLiteral("@book{marsh1911lantern,")));
        QVERIFY(bibBook.contains(QStringLiteral("publisher = {Harbor Books}")));
        QVERIFY(bibBook.contains(QStringLiteral("isbn = {978-0-00-000000-2}")));

        // APA 7th: 1 author + arXiv, 2 authors + DOI, 3 authors, editor-only, no-author + (n.d.).
        QCOMPARE(Reader::formatCitationFor(
                     {{QStringLiteral("title"), QStringLiteral("QLoRA")},
                      {QStringLiteral("author"), QStringLiteral("Tim Dettmers")},
                      {QStringLiteral("arxiv"), QStringLiteral("arXiv:2305.14314")}},
                     QStringLiteral("apa")),
                 QStringLiteral("Dettmers, T. (2023). QLoRA. arXiv preprint arXiv:2305.14314. https://arxiv.org/abs/2305.14314"));
        QCOMPARE(Reader::formatCitationFor(
                     {{QStringLiteral("title"), QStringLiteral("Joint Work")},
                      {QStringLiteral("author"), QStringLiteral("Ada Lovelace; Alan Turing")},
                      {QStringLiteral("year"), 1950},
                      {QStringLiteral("doi"), QStringLiteral("10.1000/turing")}},
                     QStringLiteral("apa")),
                 QStringLiteral("Lovelace, A., & Turing, A. (1950). Joint Work. https://doi.org/10.1000/turing"));
        QCOMPARE(Reader::formatCitationFor(
                     {{QStringLiteral("title"), QStringLiteral("Three Minds")},
                      {QStringLiteral("author"), QStringLiteral("Ada Lovelace; Alan Turing; Grace Hopper")},
                      {QStringLiteral("year"), 1955}},
                     QStringLiteral("apa")),
                 QStringLiteral("Lovelace, A., Turing, A., & Hopper, G. (1955). Three Minds."));
        QCOMPARE(Reader::formatCitationFor(
                     {{QStringLiteral("title"), QStringLiteral("Collected Papers")},
                      {QStringLiteral("editor"), QStringLiteral("Tom Reed")},
                      {QStringLiteral("year"), 2001}},
                     QStringLiteral("apa")),
                 QStringLiteral("Reed, T. (Ed.). (2001). Collected Papers."));
        QCOMPARE(Reader::formatCitationFor(
                     {{QStringLiteral("title"), QStringLiteral("Anonymous Treatise?")}},
                     QStringLiteral("apa")),
                 QStringLiteral("Anonymous Treatise? (n.d.)."));
        QVERIFY(Reader::formatCitationFor({{QStringLiteral("title"), QStringLiteral("X")}}, QStringLiteral("chicago")).isEmpty());
    }

    void wave4McpProtocolKillSwitchToolsAndResources() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath(QStringLiteral("lantern.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

        // Multi-page PDF with distinct text on pages 1 and 2.
        const QString pdfPath = dir.filePath(QStringLiteral("quantum.pdf"));
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setResolution(72);
            writer.setTitle(QStringLiteral("Quantum Circuits Primer"));
            QPainter p(&writer);
            p.drawText(QRect(20, 60, 550, 120),
                       QStringLiteral("Page one introduces Hadamard gates and quantum entanglement clearly."));
            writer.newPage();
            p.drawText(QRect(20, 60, 550, 120),
                       QStringLiteral("Page two derives Bell state fidelity bounds with concise proofs."));
            p.end();
        }
        const QString hiddenPath = dir.filePath(QStringLiteral("hidden.epub"));
        QVERIFY(TestBooks::write(hiddenPath, TestBooks::epub3(adobeObfuscate(fontPayload()))));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int epubId = library.addBookFile(epubPath);
        const int pdfId = library.addBookFile(pdfPath);
        const int hiddenId = library.addBookFile(hiddenPath);
        QVERIFY(epubId > 0 && pdfId > 0 && hiddenId > 0);
        library.setHidden(hiddenId, true);

        const int colId = library.createCollection(QStringLiteral("Physics"));
        QVERIFY(colId > 0);
        QVERIFY(library.addToCollection(pdfId, colId));

        Marks marks(&library);
        marks.setBook(epubId);
        const int markId = marks.addHighlight(0, 12, 36, QStringLiteral("lamp's light & the sea"),
                                              QStringLiteral("yellow"), QStringLiteral("Chapter One"));
        QVERIFY(markId > 0);
        marks.setNote(markId, QStringLiteral("Opening harbor motif"));

        const QString settingsIni = dir.filePath(QStringLiteral("mcp-settings.ini"));
        const QString indexDb = dir.filePath(QStringLiteral("index.sqlite"));
        LibraryIndex libIndex(indexDb);
        QVERIFY(libIndex.available());

        // Set up fake local embedder so we can exercise BOTH lexical and semantic paths of search_passages.
        const QString appSettings =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreSettings = qScopeGuard([&] {
            QSettings s(appSettings, QSettings::IniFormat);
            s.remove(QStringLiteral("ai/preset"));
        });
        FakeServer embedServer;
        embedServer.routes.insert("/api/embed",
                                  {200, "application/json", "{\"embeddings\":[[0.0,1.0,0.0]]}"});
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        LocalModels local;
        LocalModels::Server lcfg;
        lcfg.kind = LocalModels::Ollama;
        lcfg.url = QUrl(embedServer.origin());
        local.configure(lcfg, QStringLiteral("qwen3.5:4b"), QStringLiteral("nomic-embed-text"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        McpServer server(&library, &libIndex, &services, {settingsIni, indexDb});

        auto rpc = [&](const QJsonValue &id, const QString &method, const QJsonObject &params = {}) {
            QJsonObject req{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}};
            if (!id.isUndefined())
                req.insert(QStringLiteral("id"), id);
            if (!params.isEmpty())
                req.insert(QStringLiteral("params"), params);
            return server.handleRequest(req);
        };

        // 1. Handshake, framing, notifications, ping, and protocol error codes.
        const QJsonObject initOk = rpc(1, QStringLiteral("initialize"), {{QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")}});
        QCOMPARE(initOk.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(),
                 QStringLiteral("2025-03-26"));
        QCOMPARE(initOk.value(QStringLiteral("result")).toObject().value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("clothbound"));

        const QJsonObject initFallback = rpc(2, QStringLiteral("initialize"), {{QStringLiteral("protocolVersion"), QStringLiteral("1999-01-01")}});
        QCOMPARE(initFallback.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(),
                 QStringLiteral("2024-11-05"));

        // Notification without id produces empty QJsonObject (no stdout reply).
        QVERIFY(rpc(QJsonValue::Undefined, QStringLiteral("notifications/initialized")).isEmpty());

        const QJsonObject pingRes = rpc(3, QStringLiteral("ping"));
        QVERIFY(pingRes.contains(QStringLiteral("result")));
        QCOMPARE(rpc(4, QStringLiteral("nonexistent/method")).value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(),
                 -32601);
        const QJsonObject badRpc = server.handleRequest({{QStringLiteral("jsonrpc"), QStringLiteral("1.0")},
                                                         {QStringLiteral("id"), 5},
                                                         {QStringLiteral("method"), QStringLiteral("ping")}});
        QCOMPARE(badRpc.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32600);

        // 2. tools/list advertises all 6 strictly read-only tools.
        const QJsonArray tools = rpc(6, QStringLiteral("tools/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
        QCOMPARE(tools.size(), 6);
        QStringList toolNames;
        for (const QJsonValue &tv : tools) {
            const QJsonObject t = tv.toObject();
            toolNames.append(t.value(QStringLiteral("name")).toString());
            const QJsonObject ann = t.value(QStringLiteral("annotations")).toObject();
            QVERIFY(ann.value(QStringLiteral("readOnlyHint")).toBool());
            QVERIFY(!ann.value(QStringLiteral("destructiveHint")).toBool());
        }
        for (const QString &expected : {QStringLiteral("list_books"), QStringLiteral("search_library"),
                                        QStringLiteral("search_passages"), QStringLiteral("get_book_outline"),
                                        QStringLiteral("read_passage"), QStringLiteral("list_marks")}) {
            QVERIFY2(toolNames.contains(expected), qPrintable(expected));
        }

        // 3. Kill-switch: ai/mcpEnabled=false blocks tools/call & resources/read with -32001, re-enabling restores immediately.
        QVERIFY(server.isEnabled());
        {
            QSettings s(settingsIni, QSettings::IniFormat);
            s.setValue(QStringLiteral("ai/mcpEnabled"), false);
            s.sync();
        }
        QVERIFY(!server.isEnabled());
        const QJsonObject blockedTool = rpc(7, QStringLiteral("tools/call"),
                                            {{QStringLiteral("name"), QStringLiteral("list_books")},
                                             {QStringLiteral("arguments"), QJsonObject{}}});
        QCOMPARE(blockedTool.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32001);
        QVERIFY(blockedTool.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("Settings")));
        const QJsonObject blockedRes = rpc(8, QStringLiteral("resources/read"),
                                           {{QStringLiteral("uri"), QStringLiteral("clothbound://library/books")}});
        QCOMPARE(blockedRes.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32001);
        {
            QSettings s(settingsIni, QSettings::IniFormat);
            s.setValue(QStringLiteral("ai/mcpEnabled"), true);
            s.sync();
        }
        QVERIFY(server.isEnabled());

        auto callTool = [&](int id, const QString &name, const QJsonObject &args = {}) {
            return rpc(id, QStringLiteral("tools/call"),
                       {{QStringLiteral("name"), name}, {QStringLiteral("arguments"), args}})
                .value(QStringLiteral("result"))
                .toObject();
        };

        // 4. list_books (filtering by format, query, collection name & id, hidden book excluded).
        const QJsonObject allBooks = callTool(10, QStringLiteral("list_books")).value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(allBooks.value(QStringLiteral("count")).toInt(), 2);
        const QJsonObject pdfOnly = callTool(11, QStringLiteral("list_books"), {{QStringLiteral("format"), QStringLiteral("pdf")}})
                                        .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(pdfOnly.value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(pdfOnly.value(QStringLiteral("books")).toArray()[0].toObject().value(QStringLiteral("id")).toInt(), pdfId);
        const QJsonObject byColName = callTool(12, QStringLiteral("list_books"), {{QStringLiteral("collection"), QStringLiteral("physics")}})
                                          .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(byColName.value(QStringLiteral("count")).toInt(), 1);
        const QJsonObject byColId = callTool(13, QStringLiteral("list_books"), {{QStringLiteral("collection"), QString::number(colId)}})
                                        .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(byColId.value(QStringLiteral("count")).toInt(), 1);

        // 5. search_library (whole-library & book_id scoped FTS5).
        const QJsonObject ftsAll = callTool(14, QStringLiteral("search_library"), {{QStringLiteral("query"), QStringLiteral("Hadamard")}})
                                       .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(ftsAll.value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(ftsAll.value(QStringLiteral("results")).toArray()[0].toObject().value(QStringLiteral("book_id")).toInt(), pdfId);
        QCOMPARE(ftsAll.value(QStringLiteral("results")).toArray()[0].toObject().value(QStringLiteral("page")).toInt(), 1);

        const QJsonObject ftsScopedMiss = callTool(15, QStringLiteral("search_library"),
                                                   {{QStringLiteral("query"), QStringLiteral("Hadamard")},
                                                    {QStringLiteral("book_id"), epubId}})
                                              .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(ftsScopedMiss.value(QStringLiteral("count")).toInt(), 0);

        // 6. search_passages: lexical FTS5+BM25 fallback first (no vectors yet), then semantic path after building PassageIndex.
        const QJsonObject lexPassages = callTool(16, QStringLiteral("search_passages"),
                                                 {{QStringLiteral("query"), QStringLiteral("Bell state fidelity")},
                                                  {QStringLiteral("collection"), QStringLiteral("Physics")}})
                                            .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(lexPassages.value(QStringLiteral("mode")).toString(), QStringLiteral("lexical"));
        QVERIFY(lexPassages.value(QStringLiteral("count")).toInt() >= 1);
        QCOMPARE(lexPassages.value(QStringLiteral("passages")).toArray()[0].toObject().value(QStringLiteral("book_id")).toInt(), pdfId);

        {
            PassageIndex pi(&services, library.database().connectionName());
            bool built = false;
            pi.build(pdfId, {{0, 0, 60, QStringLiteral("Page one introduces Hadamard gates and quantum entanglement.")}},
                     nullptr, [&](const QString &err) {
                         QVERIFY2(err.isEmpty(), qPrintable(err));
                         built = true;
                     });
            QTRY_VERIFY_WITH_TIMEOUT(built, 5000);
        }
        const QJsonObject semPassages = callTool(17, QStringLiteral("search_passages"),
                                                 {{QStringLiteral("query"), QStringLiteral("entanglement")},
                                                  {QStringLiteral("book_id"), pdfId},
                                                  {QStringLiteral("limit"), 1}})
                                            .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(semPassages.value(QStringLiteral("mode")).toString(), QStringLiteral("semantic"));
        QCOMPARE(semPassages.value(QStringLiteral("count")).toInt(), 1);

        // 7. get_book_outline (EPUB TOC/spine/page_map & PDF pages) + error for missing book.
        const QJsonObject epubOutline = callTool(18, QStringLiteral("get_book_outline"), {{QStringLiteral("book_id"), epubId}})
                                            .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(epubOutline.value(QStringLiteral("chapters")).toInt(), 2);
        QVERIFY(!epubOutline.value(QStringLiteral("toc")).toArray().isEmpty());
        QVERIFY(!epubOutline.value(QStringLiteral("spine")).toArray().isEmpty());
        QVERIFY(!epubOutline.value(QStringLiteral("page_map")).toArray().isEmpty());

        const QJsonObject pdfOutline = callTool(19, QStringLiteral("get_book_outline"), {{QStringLiteral("book_id"), pdfId}})
                                           .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(pdfOutline.value(QStringLiteral("pages")).toInt(), 2);
        QVERIFY(callTool(20, QStringLiteral("get_book_outline"), {{QStringLiteral("book_id"), 99999}}).value(QStringLiteral("isError")).toBool());

        // 8. read_passage (EPUB chapter, PDF multi-page span, offset pagination, citation header, out-of-range error).
        const QJsonObject epubRead = callTool(21, QStringLiteral("read_passage"),
                                              {{QStringLiteral("book_id"), epubId},
                                               {QStringLiteral("chapter"), 0},
                                               {QStringLiteral("offset"), 5},
                                               {QStringLiteral("max_chars"), 999999}});
        QVERIFY(!epubRead.value(QStringLiteral("isError")).toBool());
        const QJsonObject epubStruct = epubRead.value(QStringLiteral("structuredContent")).toObject();
        QVERIFY(epubStruct.value(QStringLiteral("citation")).toString().startsWith(QStringLiteral("[Citation:")));
        QCOMPARE(epubStruct.value(QStringLiteral("offset")).toInt(), 5);
        QVERIFY(epubStruct.value(QStringLiteral("text")).toString().contains(QStringLiteral("storm")));

        const QJsonObject pdfRead = callTool(22, QStringLiteral("read_passage"),
                                             {{QStringLiteral("book_id"), pdfId},
                                              {QStringLiteral("page"), 1},
                                              {QStringLiteral("page_end"), 2}});
        const QJsonObject pdfStruct = pdfRead.value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(pdfStruct.value(QStringLiteral("page")).toInt(), 1);
        QCOMPARE(pdfStruct.value(QStringLiteral("page_end")).toInt(), 2);
        QVERIFY(pdfStruct.value(QStringLiteral("text")).toString().contains(QStringLiteral("Hadamard")));
        QVERIFY(pdfStruct.value(QStringLiteral("text")).toString().contains(QStringLiteral("Bell state")));
        QVERIFY(callTool(23, QStringLiteral("read_passage"), {{QStringLiteral("book_id"), pdfId}, {QStringLiteral("page"), 50}})
                    .value(QStringLiteral("isError")).toBool());

        // 9. list_marks (book-scoped & cross-book + query filter).
        const QJsonObject bookMarks = callTool(24, QStringLiteral("list_marks"),
                                               {{QStringLiteral("book_id"), epubId}, {QStringLiteral("query"), QStringLiteral("harbor")}})
                                          .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(bookMarks.value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(bookMarks.value(QStringLiteral("marks")).toArray()[0].toObject().value(QStringLiteral("note")).toString(),
                 QStringLiteral("Opening harbor motif"));
        const QJsonObject allMarks = callTool(25, QStringLiteral("list_marks")).value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(allMarks.value(QStringLiteral("count")).toInt(), 1);

        // 10. resources/{list,templates/list,read} + unknown URI -32002.
        const QJsonArray resList = rpc(26, QStringLiteral("resources/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("resources")).toArray();
        QVERIFY(resList.size() >= 5);  // library/books + 2 visible books * (outline + marks)
        const QJsonArray resTemplates = rpc(27, QStringLiteral("resources/templates/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("resourceTemplates")).toArray();
        QCOMPARE(resTemplates.size(), 2);

        for (const QString &uri : {QStringLiteral("clothbound://library/books"),
                                   QStringLiteral("clothbound://book/%1/outline").arg(epubId),
                                   QStringLiteral("clothbound://book/%1/marks").arg(epubId)}) {
            const QJsonObject readRes = rpc(28, QStringLiteral("resources/read"), {{QStringLiteral("uri"), uri}});
            const QJsonArray contents = readRes.value(QStringLiteral("result")).toObject().value(QStringLiteral("contents")).toArray();
            QCOMPARE(contents.size(), 1);
            QCOMPARE(contents[0].toObject().value(QStringLiteral("uri")).toString(), uri);
            QVERIFY(QJsonDocument::fromJson(contents[0].toObject().value(QStringLiteral("text")).toString().toUtf8()).isObject());
        }
        const QJsonObject badUri = rpc(29, QStringLiteral("resources/read"), {{QStringLiteral("uri"), QStringLiteral("clothbound://book/99999/outline")}});
        QCOMPARE(badUri.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32002);
    }

    void wave4CliSearchPassageAndMcpStdioPurity() {
        QVERIFY(isCliCommand(QStringLiteral("mcp")));
        QVERIFY(isCliCommand(QStringLiteral("search")));
        QVERIFY(isCliCommand(QStringLiteral("passage")));

        const QString clothboundBin = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../clothbound"));
        if (!QFileInfo::exists(clothboundBin))
            QSKIP("build/clothbound binary not built yet");

        QTemporaryDir dir;
        QDir(dir.path()).mkpath(QStringLiteral("config"));
        QDir(dir.path()).mkpath(QStringLiteral("data"));
        QDir(dir.path()).mkpath(QStringLiteral("cache"));
        const QString epubPath = dir.filePath(QStringLiteral("lantern.epub"));
        const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setResolution(72);
            writer.setTitle(QStringLiteral("Spectral Graph Theory"));
            QPainter p(&writer);
            p.drawText(QRect(20, 60, 550, 120), QStringLiteral("Laplacian eigenvalues bound graph expansion tightly."));
            writer.newPage();
            p.drawText(QRect(20, 60, 550, 120), QStringLiteral("Cheeger inequality relates conductance to spectral gap."));
            p.end();
        }

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("DISPLAY"));
        env.remove(QStringLiteral("WAYLAND_DISPLAY"));
        env.remove(QStringLiteral("QT_QPA_PLATFORM"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), dir.filePath(QStringLiteral("config")));
        env.insert(QStringLiteral("XDG_DATA_HOME"), dir.filePath(QStringLiteral("data")));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), dir.filePath(QStringLiteral("cache")));

        auto runSub = [&](const QStringList &args, const QByteArray &stdinBytes = {}) {
            QProcess proc;
            proc.setProcessEnvironment(env);
            proc.start(clothboundBin, args);
            if (!proc.waitForStarted(5000))
                return std::tuple<int, QByteArray, QByteArray>{-1, {}, QByteArray("failed to start")};
            if (!stdinBytes.isEmpty())
                proc.write(stdinBytes);
            proc.closeWriteChannel();
            if (!proc.waitForFinished(10000)) {
                proc.kill();
                proc.waitForFinished(2000);
                return std::tuple<int, QByteArray, QByteArray>{-2, proc.readAllStandardOutput(), proc.readAllStandardError()};
            }
            return std::tuple<int, QByteArray, QByteArray>{proc.exitCode(), proc.readAllStandardOutput(), proc.readAllStandardError()};
        };

        // 1. Add both books headlessly.
        auto [addCode, addOut, addErr] = runSub({QStringLiteral("add"), epubPath, pdfPath, QStringLiteral("--json")});
        QCOMPARE(addCode, 0);
        QVERIFY2(addErr.isEmpty(), addErr.constData());
        const QJsonObject addJson = QJsonDocument::fromJson(addOut).object();
        QCOMPARE(addJson.value(QStringLiteral("books")).toInt(), 2);

        // 2. clothbound search --json (whole library & --book scoped).
        auto [sCode, sOut, sErr] = runSub({QStringLiteral("search"), QStringLiteral("Cheeger"), QStringLiteral("--json")});
        QCOMPARE(sCode, 0);
        QVERIFY2(sErr.isEmpty(), sErr.constData());
        const QJsonObject sJson = QJsonDocument::fromJson(sOut).object();
        QCOMPARE(sJson.value(QStringLiteral("count")).toInt(), 1);
        const QJsonObject firstHit = sJson.value(QStringLiteral("results")).toArray()[0].toObject();
        const int pdfId = firstHit.value(QStringLiteral("book_id")).toInt();
        QVERIFY(pdfId > 0);
        QCOMPARE(firstHit.value(QStringLiteral("page")).toInt(), 2);

        // 3. clothbound passage --page / --page-end --json + invalid book/page exit 1.
        auto [pCode, pOut, pErr] = runSub({QStringLiteral("passage"), QString::number(pdfId),
                                           QStringLiteral("--page"), QStringLiteral("1"),
                                           QStringLiteral("--page-end"), QStringLiteral("2"),
                                           QStringLiteral("--json")});
        QCOMPARE(pCode, 0);
        QVERIFY2(pErr.isEmpty(), pErr.constData());
        const QJsonObject pJson = QJsonDocument::fromJson(pOut).object();
        QVERIFY(pJson.value(QStringLiteral("citation")).toString().startsWith(QStringLiteral("[Citation:")));
        QVERIFY(pJson.value(QStringLiteral("text")).toString().contains(QStringLiteral("Laplacian")));
        QVERIFY(pJson.value(QStringLiteral("text")).toString().contains(QStringLiteral("Cheeger")));

        auto [badCode, badOut, badErr] = runSub({QStringLiteral("passage"), QStringLiteral("99999"), QStringLiteral("--page"), QStringLiteral("1")});
        Q_UNUSED(badOut);
        QCOMPARE(badCode, 1);
        QVERIFY(!badErr.isEmpty());

        // 4. Live subprocess `clothbound mcp` stdio purity check: 0 stderr bytes, pure newline-delimited JSON-RPC stdout.
        const QByteArray mcpStdin =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_books\",\"arguments\":{}}}\n";
        auto [mCode, mOut, mErr] = runSub({QStringLiteral("mcp")}, mcpStdin);
        QCOMPARE(mCode, 0);
        QCOMPARE(mErr.size(), qsizetype(0));
        const QList<QByteArray> lines = mOut.split('\n');
        QList<QJsonObject> replies;
        for (const QByteArray &line : lines) {
            if (line.trimmed().isEmpty())
                continue;
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            QVERIFY2(err.error == QJsonParseError::NoError && doc.isObject(), line.constData());
            replies.append(doc.object());
        }
        QCOMPARE(replies.size(), 3);
        QCOMPARE(replies[0].value(QStringLiteral("id")).toInt(), 1);
        QCOMPARE(replies[0].value(QStringLiteral("result")).toObject().value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("clothbound"));
        QCOMPARE(replies[1].value(QStringLiteral("id")).toInt(), 2);
        QCOMPARE(replies[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray().size(), 6);
        QCOMPARE(replies[2].value(QStringLiteral("id")).toInt(), 3);
        QCOMPARE(replies[2].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 2);
    }

    // --- Wave 5 regression tests: SFX & PDF line bands, Slim ListenBar,
    //     Companion voice/resize, Library collections & shelves -------------

    void wave5PdfLineBandsAndRealPageTurnSfx() {
        // 1. PdfBook::mergeLineRects(): jagged word heights on one visual line -> 1 uniform band.
        {
            const QList<QRectF> jagged = {
                QRectF(0.100, 0.200, 0.060, 0.020),  // "The"
                QRectF(0.170, 0.205, 0.030, 0.011),  // "in"
                QRectF(0.210, 0.204, 0.040, 0.017),  // "by"
                QRectF(0.255, 0.212, 0.008, 0.005),  // "."
            };
            const QList<QRectF> out = PdfBook::mergeLineRects(jagged);
            QCOMPARE(out.size(), 1);
            QVERIFY(qAbs(out[0].left() - 0.100) < 1e-6);
            QVERIFY(qAbs(out[0].right() - 0.263) < 1e-6);
            QVERIFY(out[0].height() >= 0.020);
        }

        // 2. Lowercase-only wrap line matches height of full-height body lines.
        {
            const QList<QRectF> wrapped = {
                QRectF(0.10, 0.200, 0.78, 0.020),  // Full line 1
                QRectF(0.10, 0.232, 0.78, 0.020),  // Full line 2
                QRectF(0.10, 0.268, 0.22, 0.011),  // Lowercase wrap line ("in a sum")
            };
            const QList<QRectF> out = PdfBook::mergeLineRects(wrapped);
            QCOMPARE(out.size(), 3);
            QVERIFY2(qAbs(out[2].height() - out[0].height()) < 0.001,
                     qPrintable(QStringLiteral("wrap=%1 full=%2").arg(out[2].height()).arg(out[0].height())));
        }

        // 3. Two-column academic gutters (0.04 and 0.06) are never bridged.
        {
            const QList<QRectF> twoCol = {
                // Row 1: gutter = 0.50 - 0.46 = 0.04 (body h=0.012 -> maxWordGap=0.036)
                QRectF(0.08, 0.200, 0.17, 0.012),
                QRectF(0.26, 0.200, 0.20, 0.012),
                QRectF(0.50, 0.200, 0.18, 0.012),
                QRectF(0.69, 0.200, 0.19, 0.012),
                // Row 2: gutter = 0.51 - 0.45 = 0.06
                QRectF(0.08, 0.226, 0.37, 0.012),
                QRectF(0.51, 0.226, 0.37, 0.012),
            };
            const QList<QRectF> out = PdfBook::mergeLineRects(twoCol);
            QCOMPARE(out.size(), 4);
            for (const QRectF &band : out) {
                const bool crossesCenterGutter = band.left() < 0.46 && band.right() > 0.50;
                QVERIFY2(!crossesCenterGutter, qPrintable(QStringLiteral("Bridged gutter: [%1..%2]").arg(band.left()).arg(band.right())));
            }
        }

        // 4. Oversized heading preserves its taller height above body lines.
        {
            const QList<QRectF> withHeading = {
                QRectF(0.10, 0.100, 0.50, 0.042),  // Section heading
                QRectF(0.10, 0.180, 0.78, 0.018),
                QRectF(0.10, 0.210, 0.78, 0.018),
                QRectF(0.10, 0.240, 0.78, 0.018),
            };
            const QList<QRectF> out = PdfBook::mergeLineRects(withHeading);
            QCOMPARE(out.size(), 4);
            QVERIFY(out[0].height() > out[1].height() * 1.8);
        }

        // 5. Out-of-order tokens on the same visual line unite cleanly.
        {
            const QList<QRectF> outOfOrder = {
                QRectF(0.30, 0.30, 0.10, 0.018),
                QRectF(0.10, 0.30, 0.08, 0.018),
                QRectF(0.19, 0.30, 0.10, 0.018),
            };
            const QList<QRectF> out = PdfBook::mergeLineRects(outOfOrder);
            QCOMPARE(out.size(), 1);
            QVERIFY(qAbs(out[0].left() - 0.10) < 1e-6);
            QVERIFY(qAbs(out[0].right() - 0.40) < 1e-6);
        }

        // 6. Degenerate / NaN / Inf / empty rects dropped safely.
        {
            const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
            const qreal inf = std::numeric_limits<qreal>::infinity();
            const QList<QRectF> invalidOnly = {
                QRectF(nan, 0.1, 0.1, 0.02),
                QRectF(0.1, inf, 0.1, 0.02),
                QRectF(0.1, 0.1, 0.0, 0.02),
                QRectF(0.1, 0.1, 0.1, -0.02),
            };
            QVERIFY(PdfBook::mergeLineRects(invalidOnly).isEmpty());
            QList<QRectF> mixed = invalidOnly;
            mixed.append(QRectF(-0.05, 0.40, 0.30, 0.02));
            const QList<QRectF> out = PdfBook::mergeLineRects(mixed);
            QCOMPARE(out.size(), 1);
            QVERIFY(out[0].left() >= 0.0 && out[0].right() <= 1.0);
        }

        // 7. Tight consecutive lines stay vertically non-overlapping (zero double-alpha seams).
        {
            const QList<QRectF> tight = {
                QRectF(0.10, 0.200, 0.75, 0.019),
                QRectF(0.10, 0.218, 0.75, 0.019),
                QRectF(0.10, 0.236, 0.75, 0.019),
            };
            const QList<QRectF> out = PdfBook::mergeLineRects(tight);
            QCOMPARE(out.size(), 3);
            QVERIFY2(out[0].bottom() < out[1].top(),
                     qPrintable(QStringLiteral("line0.bottom=%1 line1.top=%2").arg(out[0].bottom()).arg(out[1].top())));
            QVERIFY2(out[1].bottom() < out[2].top(),
                     qPrintable(QStringLiteral("line1.bottom=%1 line2.top=%2").arg(out[1].bottom()).arg(out[2].top())));
        }

        // 8. Reader::playPageTurnDryRun(): single signature page_turn.wav loads cleanly,
        //    consecutive forward turns are consistent (c0 == c1, clipIndex == 0), 85 ms throttle
        //    blocks immediate repeats, backward gain is ~0.82x, and forward peak lies in -14..-18 dBFS.
        {
            QTemporaryDir dir;
            Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
            QVERIFY(library.open());
            BookScheme scheme;
            Reader reader(&library, &scheme);

            auto peak16 = [](const QByteArray &pcm) {
                const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
                const qsizetype n = pcm.size() / 2;
                int p = 0;
                for (qsizetype i = 0; i < n; ++i)
                    p = qMax(p, std::abs(int(s[i])));
                return p;
            };

            QByteArray c0, c1, b0, throttled;
            QVERIFY(reader.playPageTurnDryRun(1, &c0));
            QVERIFY(!reader.playPageTurnDryRun(1, &throttled));
            QVERIFY(throttled.isEmpty());
            QTest::qWait(95);
            QVERIFY(reader.playPageTurnDryRun(1, &c1));
            QTest::qWait(95);
            QVERIFY(reader.playPageTurnDryRun(-1, &b0));

            QVERIFY(c0.size() >= 18000);
            QCOMPARE(c1, c0);
            QCOMPARE(b0.size(), c0.size());
            QVERIFY(b0 != c0);

            const int pkFwd = peak16(c0);
            const int pkBwd = peak16(b0);
            QVERIFY(pkFwd > 0 && pkBwd > 0);
            const double dbfs = 20.0 * std::log10(double(pkFwd) / 32768.0);
            QVERIFY2(dbfs >= -18.0 && dbfs <= -14.0,
                     qPrintable(QStringLiteral("clip peak dBFS=%1 out of [-18,-14]").arg(dbfs)));
            const double bwdRatio = double(pkBwd) / double(pkFwd);
            QVERIFY2(bwdRatio >= 0.79 && bwdRatio <= 0.85,
                     qPrintable(QStringLiteral("backward gain ratio=%1 expected ~0.82").arg(bwdRatio)));
        }

        // 9. AppearancePanel.qml exposes Page-turn sound toggle bound to readingSettings.pageTurnSound.
        {
            const QString srcRoot = QFINDTESTDATA("../src");
            QFile apFile(srcRoot + QStringLiteral("/ui/reader/AppearancePanel.qml"));
            QVERIFY(apFile.open(QIODevice::ReadOnly));
            const QString apQml = QString::fromUtf8(apFile.readAll());
            QVERIFY(apQml.contains(QStringLiteral("qsTr(\"Page-turn sound\")")));
            QVERIFY(apQml.contains(QStringLiteral("readingSettings.pageTurnSound")));
            QVERIFY(apQml.contains(QStringLiteral("reader.playPageTurn(1)")));
        }
    }

    void wave5SlimListenBarAndToastClearance() {
        QTemporaryDir dir;
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        VoiceCatalog voices(&services, &cache, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("voices.json")));
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        Theme theme;

        QQmlEngine engine;
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("ai"), &services);
        engine.rootContext()->setContextProperty(QStringLiteral("voices"), &voices);
        engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);

        // 1. Live ListenBar.qml instantiation: resting 44px capsule -> expanded 86px hover state.
        QQmlComponent barComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/audio/ListenBar.qml")));
        QVERIFY2(barComp.status() == QQmlComponent::Ready, qPrintable(barComp.errorString()));
        std::unique_ptr<QObject> barObj(barComp.createWithInitialProperties(
            {{QStringLiteral("availableWidth"), 1280.0},
             {QStringLiteral("look"), QVariantMap{{QStringLiteral("dark"), false}}}}));
        QVERIFY2(barObj != nullptr, qPrintable(barComp.errorString()));

        QCOMPARE(barObj->property("showBookInfo").toBool(), false);
        QCOMPARE(barObj->property("animateSize").toBool(), true);
        QCOMPARE(barObj->property("expanded").toBool(), false);
        QCOMPARE(qRound(barObj->property("height").toDouble()), 44);
        QCOMPARE(qRound(barObj->property("restingTargetWidth").toDouble()), 368);
        QCOMPARE(qRound(barObj->property("width").toDouble()), 368);

        // Unfollowing widens the resting capsule cleanly to make room for the Follow chip.
        narration.setFollowing(false);
        QCOMPARE(qRound(barObj->property("restingTargetWidth").toDouble()), 412);
        QCOMPARE(qRound(barObj->property("expandedTargetWidth").toDouble()), 556);
        narration.setFollowing(true);
        QCOMPARE(qRound(barObj->property("restingTargetWidth").toDouble()), 368);
        QCOMPARE(qRound(barObj->property("expandedTargetWidth").toDouble()), 544);

        // Expanding transitions to 86px height & 544px width, clamped cleanly by availableWidth.
        barObj->setProperty("hoverExpanded", true);
        QCOMPARE(barObj->property("expanded").toBool(), true);
        QCOMPARE(qRound(barObj->property("expandedTargetHeight").toDouble()), 86);
        QTRY_COMPARE_WITH_TIMEOUT(qRound(barObj->property("height").toDouble()), 86, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(qRound(barObj->property("width").toDouble()), 544, 1000);

        barObj->setProperty("availableWidth", 460.0);
        QTRY_VERIFY_WITH_TIMEOUT(barObj->property("width").toDouble() <= 436.5, 1000);
        barObj->setProperty("hoverExpanded", false);
        QTRY_COMPARE_WITH_TIMEOUT(qRound(barObj->property("height").toDouble()), 44, 1000);

        // 2. Dynamic Toast.bottomClearance above ListenBar while narration.active matches Main.qml.
        const QString harnessQml = QStringLiteral(R"QML(
import QtQuick
import "ui/audio"
import "ui/components"
Item {
    width: 1280; height: 800
    property bool readerSurfaceVisible: true
    property bool narrationActive: false
    property alias bar: bar
    property alias toast: toast
    ListenBar {
        id: bar
        availableWidth: parent.width
        look: ({ dark: false })
    }
    Toast {
        id: toast
        bottomClearance: !parent.readerSurfaceVisible ? 28
                       : parent.narrationActive
                         ? Math.round(bar.height) + 28
                         : 28
    }
}
)QML");
        QQmlComponent harnessComp(&engine);
        harnessComp.setData(harnessQml.toUtf8(), QUrl::fromLocalFile(srcRoot + QStringLiteral("/Harness.qml")));
        QVERIFY2(harnessComp.status() == QQmlComponent::Ready, qPrintable(harnessComp.errorString()));
        std::unique_ptr<QObject> harnessObj(harnessComp.create());
        QVERIFY2(harnessObj != nullptr, qPrintable(harnessComp.errorString()));
        QObject *hBar = harnessObj->property("bar").value<QObject *>();
        QObject *hToast = harnessObj->property("toast").value<QObject *>();
        QVERIFY(hBar && hToast);

        QCOMPARE(qRound(hToast->property("bottomClearance").toDouble()), 28);
        harnessObj->setProperty("narrationActive", true);
        QCOMPARE(qRound(hToast->property("bottomClearance").toDouble()), 72);  // 44 + 28
        hBar->setProperty("hoverExpanded", true);
        QTRY_COMPARE_WITH_TIMEOUT(qRound(hToast->property("bottomClearance").toDouble()), 114, 1000);  // 86 + 28
        harnessObj->setProperty("readerSurfaceVisible", false);
        QCOMPARE(qRound(hToast->property("bottomClearance").toDouble()), 28);

        // 3. Main.qml & EpubReader.qml retain exact dynamic Toast & bottom clearance wiring.
        QFile mainFile(srcRoot + QStringLiteral("/ui/Main.qml"));
        QVERIFY(mainFile.open(QIODevice::ReadOnly));
        const QString mainQml = QString::fromUtf8(mainFile.readAll());
        QVERIFY(mainQml.contains(QStringLiteral("readerLoader.item.listenBar.height")));
        QVERIFY(mainQml.contains(QStringLiteral("bottomClearance:")));

        QFile readerFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
        QVERIFY(readerFile.open(QIODevice::ReadOnly));
        const QString readerQml = QString::fromUtf8(readerFile.readAll());
        QVERIFY(readerQml.contains(QStringLiteral("animateSize: !view.companionResizing")));
        QVERIFY(readerQml.contains(QStringLiteral("showBookInfo: false")));
    }

    void wave5CompanionResponseSpeechAndResize() {
        // 1. Narration::cleanSpeechText() coverage.
        {
            const QString mdLinks = Narration::cleanSpeechText(
                QStringLiteral("![Architecture](fig1.png) Read [Clothbound](https://clothbound.app/docs) now."));
            QVERIFY2(mdLinks.contains(QStringLiteral("Architecture")) && mdLinks.contains(QStringLiteral("Clothbound")),
                     qPrintable(mdLinks));
            QVERIFY(!mdLinks.contains(QStringLiteral("https://")));
            QVERIFY(!mdLinks.contains(QStringLiteral("](")));

            const QString cites = Narration::cleanSpeechText(
                QStringLiteral("Transformers scale well [1] across benchmarks [2, 3] and chapters [p. 4] [Ch. 3]."));
            QVERIFY2(!cites.contains(QStringLiteral("[1]")) && !cites.contains(QStringLiteral("[2, 3]")) &&
                         !cites.contains(QStringLiteral("[p. 4]")) && !cites.contains(QStringLiteral("[Ch. 3]")),
                     qPrintable(cites));
            QVERIFY(cites.contains(QStringLiteral("Transformers scale well")));

            const QString formatting = Narration::cleanSpeechText(
                QStringLiteral("## Summary\n> A **bold**, *italic*, and _quiet_ line.\n- First item\n"));
            QVERIFY2(formatting.contains(QStringLiteral("Summary")) &&
                         formatting.contains(QStringLiteral("A bold, italic, and quiet line.")) &&
                         formatting.contains(QStringLiteral("First item")),
                     qPrintable(formatting));
            QVERIFY(!formatting.contains(QStringLiteral("##")) && !formatting.contains(QStringLiteral("**")) &&
                    !formatting.contains(QLatin1Char('>')));

            const QString code = Narration::cleanSpeechText(
                QStringLiteral("```python\nscore = calc_norm(x)\n```\nCall `snake_case_fn` directly."));
            QVERIFY2(code.contains(QStringLiteral("score = calc_norm(x)")) &&
                         code.contains(QStringLiteral("snake_case_fn")),
                     qPrintable(code));
            QVERIFY(!code.contains(QLatin1Char('`')));

            const QString html = Narration::cleanSpeechText(
                QStringLiteral("H<sup>2</sup>O &amp; <span class=\"note\">pure</span>&mdash;clear."));
            QVERIFY2(html.contains(QStringLiteral("H2O & pure\u2014clear.")), qPrintable(html));
            QVERIFY(!html.contains(QStringLiteral("<sup>")) && !html.contains(QStringLiteral("<span")));

            const QString latex = Narration::cleanSpeechText(
                QStringLiteral("Using $\\frac{a}{b}$, $\\alpha + \\nabla f = \\sqrt{x}$, and bare \\frac{1}{2} \\alpha."));
            QVERIFY2(latex.contains(QStringLiteral("a over b")) && latex.contains(QStringLiteral("alpha")) &&
                         latex.contains(QStringLiteral("gradient")) && latex.contains(QStringLiteral("square root of x")) &&
                         latex.contains(QStringLiteral("1 over 2")),
                     qPrintable(latex));
            QVERIFY(!latex.contains(QStringLiteral("\\frac")) && !latex.contains(QStringLiteral("\\alpha")) &&
                    !latex.contains(QStringLiteral("\\nabla")) && !latex.contains(QStringLiteral("\\sqrt")) &&
                    !latex.contains(QLatin1Char('$')));

            // Currency guard: "$5 to $10" must stay verbatim.
            const QString currency = Narration::cleanSpeechText(QStringLiteral("Expect costs of $5 to $10 per month."));
            QVERIFY2(currency.contains(QStringLiteral("$5 to $10")), qPrintable(currency));

            // Markup-only text collapses to empty.
            QVERIFY(Narration::cleanSpeechText(QStringLiteral("---\n***\n   \n")).isEmpty());
        }

        // 2. Narration::speakResponse() / stopResponse() runtime behavior in headless mode.
        QTemporaryDir dir;
        const QString epubPath = dir.filePath(QStringLiteral("companion-speech.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;  // No Gemini key saved -> tests missing voice & local fallback.
        AiServices services(&gemini, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);

        // 2a. Missing-voice friendly prompt.
        {
            QSignalSpy probSpy(&narration, &Narration::responseSpeechProblem);
            narration.speakResponse(QStringLiteral("Here is a concise answer."), QStringLiteral("ask-0"));
            QCOMPARE(probSpy.count(), 1);
            const QString msg = probSpy.first().first().toString();
            QVERIFY2(msg.contains(QStringLiteral("Choose or download a voice first (P)")), qPrintable(msg));
            QCOMPARE(narration.problem(), msg);
            QVERIFY(!narration.speakingResponse());
        }

        // 2b. Local voice fallback when m_voice is empty + fast-start multi-sentence chunking.
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);

        const QString longReply = QStringLiteral(
            "Scaled dot-product attention stabilizes softmax gradients across large key dimensions. "
            "Without dividing by the square root of the key width, dot products grow linearly in variance and saturate the exponential curve. "
            "Each query vector attends simultaneously across all key vectors in a single dense matrix multiplication. "
            "Multi-head projection lets distinct subspaces track syntax, positional distance, and long-range semantic references in parallel. "
            "Residual connections and layer normalization then keep gradients well-conditioned through every encoder and decoder block. "
            "This combination replaces sequential recurrence while preserving strong modeling fidelity.");

        narration.speakResponse(longReply, QStringLiteral("ask-1"));
        QVERIFY(narration.speakingResponse());
        QCOMPARE(narration.speakingResponseId(), QStringLiteral("ask-1"));
        QVERIFY2(narration.m_responseVoice.startsWith(QStringLiteral("builtin:")), qPrintable(narration.m_responseVoice));
        QVERIFY2(narration.m_responseChunks.size() >= 2, qPrintable(QString::number(narration.m_responseChunks.size())));
        QVERIFY2(narration.m_responseChunks.first().text.size() <= 220,
                 qPrintable(QString::number(narration.m_responseChunks.first().text.size())));
        QTRY_VERIFY_WITH_TIMEOUT(!narration.responseBuffering() &&
                                     !narration.m_responseChunks.isEmpty() &&
                                     narration.m_responseChunks.first().state == Narration::ResponseChunk::Complete,
                                 5000);

        // 2c. Same-id toggle turns playback off; invoking another id switches cleanly.
        narration.speakResponse(longReply, QStringLiteral("ask-1"));
        QVERIFY(!narration.speakingResponse());
        QVERIFY(narration.speakingResponseId().isEmpty());

        narration.speakResponse(QStringLiteral("First item reply."), QStringLiteral("ask-2"));
        QVERIFY(narration.speakingResponse());
        QCOMPARE(narration.speakingResponseId(), QStringLiteral("ask-2"));
        narration.speakResponse(QStringLiteral("Recap summary reply."), QStringLiteral("recap"));
        QVERIFY(narration.speakingResponse());
        QCOMPARE(narration.speakingResponseId(), QStringLiteral("recap"));
        narration.stopResponse();
        QVERIFY(!narration.speakingResponse());

        // 2d. Coexistence with book narration: pauses book cleanly, resume() restores book & stops response.
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.start(0, 0);
        narration.addBlocks(0, R"([{"start":0,"end":42,"text":"The harbour lamp glowed through the storm.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() && narration.m_clips[0].state == Narration::Clip::Complete, 5000);
        QVERIFY(narration.active() && narration.playing());

        narration.speakResponse(QStringLiteral("Answering while book narration pauses."), QStringLiteral("ask-3"));
        QVERIFY(narration.active());
        QVERIFY(!narration.playing());
        QVERIFY(narration.m_bookPlayerDirty);
        QVERIFY(narration.speakingResponse());
        QCOMPARE(narration.speakingResponseId(), QStringLiteral("ask-3"));

        narration.resume();
        QVERIFY(!narration.speakingResponse());
        QVERIFY(narration.playing());
        QVERIFY(!narration.m_bookPlayerDirty);
        QVERIFY(narration.m_clips[0].queued);
        narration.stop();

        // 2e. reader.close() cleanly stops response speech in headless mode.
        narration.speakResponse(QStringLiteral("Reading selection explanation aloud."), QStringLiteral("selection"));
        QVERIFY(narration.speakingResponse());
        reader.close();
        QVERIFY(!narration.speakingResponse());
        QVERIFY(narration.speakingResponseId().isEmpty());

        // 3. CompanionPanel.qml resize bounds, ui/companionWidth persistence, and SpeakButton wiring.
        const QString srcRoot = QFINDTESTDATA("../src");
        QFile compFile(srcRoot + QStringLiteral("/ui/companion/CompanionPanel.qml"));
        QVERIFY(compFile.open(QIODevice::ReadOnly));
        const QString compQml = QString::fromUtf8(compFile.readAll());
        QVERIFY(compQml.contains(QStringLiteral("property int companionWidth: 384")));
        QVERIFY(compQml.contains(QStringLiteral("readonly property int minWidth: 320")));
        QVERIFY(compQml.contains(QStringLiteral("readonly property bool resizing: resizeDrag.active")));
        QVERIFY(compQml.contains(QStringLiteral("component SpeakButton:")));
    }

    void wave5LibraryCollectionsRecentAndSidebarIcons() {
        QTemporaryDir dir;
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));

        auto makePdf = [](const QString &path, const QString &title, const QString &body) {
            QPdfWriter writer(path);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setResolution(72);
            writer.setTitle(title);
            QPainter p(&writer);
            p.drawText(QRect(30, 60, 520, 120), body);
            p.end();
        };

        const QString existingPdf = dir.filePath(QStringLiteral("alpha_archive.pdf"));
        const QString freshPdf = dir.filePath(QStringLiteral("zebra_paper.pdf"));
        const QString epubPath = dir.filePath(QStringLiteral("lantern.epub"));
        makePdf(existingPdf, QStringLiteral("Alpha Archive"), QStringLiteral("Existing PDF paper already in library."));
        makePdf(freshPdf, QStringLiteral("Zebra Paper"), QStringLiteral("Brand new research PDF added straight to Research."));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

        int colId = 0;
        int existingId = 0;
        int freshId = 0;
        int epubId = 0;

        {
            Library library(dataDir, cacheDir);
            QVERIFY(library.open());
            existingId = library.addBookFile(existingPdf);
            epubId = library.addBookFile(epubPath);
            QVERIFY(existingId > 0 && epubId > 0);

            colId = library.createCollection(QStringLiteral("Research"));
            QVERIFY(colId > 0);
            library.works()->setCollectionId(colId);
            QCOMPARE(library.works()->count(), 0);

            // 1. Re-adding an already-in-library PDF while viewing Research tags it immediately (0 -> 1) and emits booksAdded.
            QSignalSpy addedSpy(&library, &Library::booksAdded);
            library.addUrls({QUrl::fromLocalFile(existingPdf)});
            QCOMPARE(library.works()->count(), 1);
            QVERIFY(library.isInCollection(existingId, colId));
            QCOMPARE(addedSpy.count(), 1);
            QCOMPARE(addedSpy[0][0].toInt(), existingId);
            QCOMPARE(addedSpy[0][1].toInt(), 1);
            QCOMPARE(addedSpy[0][3].toInt(), colId);

            // 2. Adding a brand-new PDF while viewing Research scans, tags, updates works() live, and emits booksAdded.
            library.addUrls({QUrl::fromLocalFile(freshPdf)});
            QTRY_COMPARE_WITH_TIMEOUT(library.works()->count(), 2, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(addedSpy.count() >= 2, 5000);
            freshId = library.idForPath(QFileInfo(freshPdf).canonicalFilePath());
            QVERIFY(freshId > 0);
            QVERIFY(library.isInCollection(freshId, colId));
            QCOMPARE(addedSpy.last()[3].toInt(), colId);

            // Set explicit added_at timestamps to exercise 14-day window and newest-first ordering across reopen.
            const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
            QSqlQuery q(library.database());
            QVERIFY(q.exec(QStringLiteral("UPDATE books SET added_at = %1 WHERE id = %2").arg(nowSec - 3600).arg(freshId)));
            QVERIFY(q.exec(QStringLiteral("UPDATE books SET added_at = %1 WHERE id = %2").arg(nowSec - 3LL * 86400).arg(epubId)));
            QVERIFY(q.exec(QStringLiteral("UPDATE books SET added_at = %1 WHERE id = %2").arg(nowSec - 20LL * 86400).arg(existingId)));
        }

        // 3. Reopen SQLite library: collection memberships persist, shelfCounts & recent shelf behave accurately.
        {
            Library reopened(dataDir, cacheDir);
            QVERIFY(reopened.open());
            QCOMPARE(reopened.schemaVersion(), 4);
            QVERIFY(reopened.isInCollection(existingId, colId));
            QVERIFY(reopened.isInCollection(freshId, colId));

            const QVariantMap counts = reopened.shelfCounts();
            QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 1);
            QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 2);
            QCOMPARE(counts.value(QStringLiteral("recent")).toInt(), 2);

            // statusFilter == "recent": 14-day window, strictly newest-first regardless of sortOrder == "title".
            reopened.works()->setSortOrder(QStringLiteral("title"));
            reopened.works()->setStatusFilter(QStringLiteral("recent"));
            QCOMPARE(reopened.works()->count(), 2);
            QCOMPARE(reopened.works()->get(0).value(QStringLiteral("bookId")).toInt(), freshId);
            QCOMPARE(reopened.works()->get(1).value(QStringLiteral("bookId")).toInt(), epubId);

            // Format shelves match shelfCounts["epub"] and shelfCounts["pdf"].
            reopened.works()->clearFilters();
            reopened.works()->setFormatFilter(QStringLiteral("epub"));
            QCOMPARE(reopened.works()->count(), counts.value(QStringLiteral("epub")).toInt());
            reopened.works()->setFormatFilter(QStringLiteral("pdf"));
            QCOMPARE(reopened.works()->count(), counts.value(QStringLiteral("pdf")).toInt());
        }

        // 4. LibrarySidebar.qml icon check (Icons.download for Downloaded, Icons.headphones for Audiobooks)
        //    and live component compilation.
        {
            const QString srcRoot = QFINDTESTDATA("../src");
            QFile sidebarFile(srcRoot + QStringLiteral("/ui/library/LibrarySidebar.qml"));
            QVERIFY(sidebarFile.open(QIODevice::ReadOnly));
            const QString qml = QString::fromUtf8(sidebarFile.readAll());
            const qsizetype dlPos = qml.indexOf(QStringLiteral("qsTr(\"Downloaded\")"));
            const qsizetype abPos = qml.indexOf(QStringLiteral("qsTr(\"Audiobooks\")"));
            QVERIFY(dlPos > 0 && abPos > dlPos);
            const QString dlBlock = qml.mid(dlPos, 120);
            const QString abBlock = qml.mid(abPos, 120);
            QVERIFY2(dlBlock.contains(QStringLiteral("glyph: Icons.download")), qPrintable(dlBlock));
            QVERIFY2(abBlock.contains(QStringLiteral("glyph: Icons.headphones")), qPrintable(abBlock));
            QVERIFY2(qml.contains(QStringLiteral("readonly property string title: qsTr(\"Settings\")")),
                     "LibrarySidebar footer must be labeled Settings");
            QVERIFY2(qml.contains(QStringLiteral("path: Icons.gear")),
                     "LibrarySidebar Settings footer must use Icons.gear");
            QVERIFY2(qml.contains(QStringLiteral("onClicked: side.settingsRequested()")),
                     "LibrarySidebar Settings footer must emit settingsRequested()");

            QFile detailsFile(srcRoot + QStringLiteral("/ui/library/BookDetails.qml"));
            QVERIFY(detailsFile.open(QIODevice::ReadOnly));
            const QString detailsQml = QString::fromUtf8(detailsFile.readAll());
            QVERIFY(detailsQml.contains(QStringLiteral("text: \"BibTeX\"")));
            QVERIFY(detailsQml.contains(QStringLiteral("text: \"APA\"")));
            QVERIFY(detailsQml.contains(QStringLiteral("shelfPill.member ? theme.accentSoft")));

            QFile shelfFile(srcRoot + QStringLiteral("/ui/library/ContinueShelf.qml"));
            QVERIFY(shelfFile.open(QIODevice::ReadOnly));
            const QString shelfQml = QString::fromUtf8(shelfFile.readAll());
            QVERIFY(shelfQml.contains(QStringLiteral("book.format === \"pdf\"")));
            QVERIFY(shelfQml.contains(QStringLiteral("qsTr(\"Page %1 of %2\")")));

            QFile libViewFile(srcRoot + QStringLiteral("/ui/library/LibraryView.qml"));
            QVERIFY(libViewFile.open(QIODevice::ReadOnly));
            const QString libViewQml = QString::fromUtf8(libViewFile.readAll());
            QVERIFY(libViewQml.contains(QStringLiteral("onSettingsRequested: win.showSettings()")));
            QVERIFY(libViewQml.contains(QStringLiteral("qsTr(\"Open any book while the rest arrive.\")")));

            QFile readerFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
            QVERIFY(readerFile.open(QIODevice::ReadOnly));
            const QString readerQml = QString::fromUtf8(readerFile.readAll());
            QVERIFY(readerQml.contains(QStringLiteral("key: \"X\", what: qsTr(\"Snip\")")));
            QVERIFY(readerQml.contains(QStringLiteral("key: \"Ctrl K\", what: qsTr(\"Find\")")));
        }
    }

    void smoothWheelScrollPhysicsAndLibraryWiring() {
        Theme theme;
        QQmlEngine engine;
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

        // 1. Standalone SmoothWheel.qml compiles via QQmlComponent and computeWheelTarget() math is exact.
        QQmlComponent swComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/components/SmoothWheel.qml")));
        QVERIFY2(swComp.status() == QQmlComponent::Ready, qPrintable(swComp.errorString()));
        std::unique_ptr<QObject> swObj(swComp.create());
        QVERIFY2(swObj != nullptr, qPrintable(swComp.errorString()));
        QCOMPARE(swObj->property("stepSize").toDouble(), 135.0);
        QCOMPARE(swObj->property("pixelMultiplier").toDouble(), 1.25);

        auto compute = [&](double curY, double activeY, bool gliding, double minY, double maxY,
                           double angleY, double pixelY) -> double {
            QVariant ret;
            const bool ok = QMetaObject::invokeMethod(
                swObj.get(), "computeWheelTarget",
                Q_RETURN_ARG(QVariant, ret),
                Q_ARG(QVariant, curY), Q_ARG(QVariant, activeY), Q_ARG(QVariant, gliding),
                Q_ARG(QVariant, minY), Q_ARG(QVariant, maxY),
                Q_ARG(QVariant, angleY), Q_ARG(QVariant, pixelY));
            Q_ASSERT(ok);
            return ret.toDouble();
        };

        // Single discrete wheel notch down (-120) advances by +stepSize (135 px).
        QCOMPARE(compute(0.0, 0.0, false, 0.0, 1000.0, -120.0, 0.0), 135.0);
        // Consecutive notch mid-glide accumulates onto activeTargetY (135 -> 270), even while curY is still at 40.
        QCOMPARE(compute(40.0, 135.0, true, 0.0, 1000.0, -120.0, 0.0), 270.0);
        // Reversing direction mid-glide pivots immediately from currentY instead of overshooting forward.
        QCOMPARE(compute(100.0, 400.0, true, 0.0, 1000.0, 120.0, 0.0), 0.0);
        // Trackpad pixelDelta moves directly by -pixelDeltaY * pixelMultiplier, ignoring stale activeTargetY.
        QCOMPARE(compute(200.0, 600.0, true, 0.0, 1000.0, -60.0, -40.0), 250.0);
        // Top & bottom clamping, including negative minY (GridView/ListView header originY).
        QCOMPARE(compute(950.0, 950.0, false, 0.0, 1000.0, -120.0, 0.0), 1000.0);
        QCOMPARE(compute(30.0, 30.0, false, 0.0, 1000.0, 120.0, 0.0), 0.0);
        QCOMPARE(compute(-260.0, -260.0, false, -260.0, 540.0, -120.0, 0.0), -125.0);
        QCOMPARE(compute(-200.0, -200.0, false, -260.0, 540.0, 120.0, 0.0), -260.0);
        // Zero vertical scroll room stays pinned at minY.
        QCOMPARE(compute(0.0, 0.0, false, 0.0, 0.0, -120.0, 0.0), 0.0);

        // 2. Live QQuickWindow + Flickable + GridView with REAL QWheelEvent delivery:
        //    proves reparenting off contentItem onto viewport (even when contentWidth == 0),
        //    discrete glide accumulation, direct pixelDelta, negative originY header bounds,
        //    Ctrl+wheel passthrough, external reset cancellation, and reducedMotion.
        const QString harnessQml = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "ui/components"
Item {
    id: harnessRoot
    width: 600
    height: 400
    property alias flick: flick
    property alias wheel: sw
    property alias grid: grid
    property alias gridWheel: gridSw
    property int ctrlWheelCount: 0

    Flickable {
        id: flick
        anchors.fill: parent
        // Intentionally omit contentWidth (like LibrarySidebar / BookDetails / CompanionPanel)
        // to verify SmoothWheel reparents onto the Flickable viewport instead of 0-wide contentItem.
        contentHeight: 2000
        boundsBehavior: Flickable.StopAtBounds
        pixelAligned: true
        ScrollBar.vertical: ScrollBar {}
        Item {
            parent: flick
            anchors.fill: parent
            WheelHandler {
                acceptedModifiers: Qt.ControlModifier
                onWheel: harnessRoot.ctrlWheelCount++
            }
        }
        SmoothWheel {
            id: sw
            stepSize: 165
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        visible: false
        cellWidth: 150
        cellHeight: 200
        model: 40
        boundsBehavior: Flickable.StopAtBounds
        pixelAligned: true
        header: Item { width: grid.width; height: 220 }
        delegate: Item { width: 150; height: 200 }
        ScrollBar.vertical: ScrollBar {}
        SmoothWheel {
            id: gridSw
            stepSize: 165
        }
    }
}
)QML");
        QQmlComponent harnessComp(&engine);
        harnessComp.setData(harnessQml.toUtf8(), QUrl::fromLocalFile(srcRoot + QStringLiteral("/SmoothWheelHarness.qml")));
        QVERIFY2(harnessComp.status() == QQmlComponent::Ready, qPrintable(harnessComp.errorString()));
        std::unique_ptr<QQuickWindow> win(new QQuickWindow);
        win->resize(600, 400);
        std::unique_ptr<QObject> rootObj(harnessComp.create());
        QVERIFY2(rootObj != nullptr, qPrintable(harnessComp.errorString()));
        QQuickItem *rootItem = qobject_cast<QQuickItem *>(rootObj.get());
        QVERIFY(rootItem != nullptr);
        rootItem->setParentItem(win->contentItem());
        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win.get()));

        QQuickItem *flickItem = rootObj->property("flick").value<QQuickItem *>();
        QQuickItem *wheelItem = rootObj->property("wheel").value<QQuickItem *>();
        QQuickItem *gridItem = rootObj->property("grid").value<QQuickItem *>();
        QQuickItem *gridWheelItem = rootObj->property("gridWheel").value<QQuickItem *>();
        QVERIFY(flickItem && wheelItem && gridItem && gridWheelItem);

        // SmoothWheel automatically resolved Flickable & GridView and reparented onto viewport.
        QCOMPARE(wheelItem->property("target").value<QObject *>(), flickItem);
        QCOMPARE(wheelItem->parentItem(), flickItem);
        QCOMPARE(wheelItem->width(), 600.0);
        QCOMPARE(wheelItem->height(), 400.0);
        QCOMPARE(gridWheelItem->property("target").value<QObject *>(), gridItem);
        QCOMPARE(gridWheelItem->parentItem(), gridItem);

        auto sendWheel = [&](QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            const QPointF pos(300.0, 200.0);
            QWheelEvent ev(pos, pos, pixelDelta, angleDelta, Qt::NoButton, mods, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(win.get(), &ev);
        };

        // Two rapid REAL mouse-wheel notches accumulate to 330px (never double-scrolled by Flickable).
        sendWheel(QPoint(0, 0), QPoint(0, -120));
        QCOMPARE(wheelItem->property("gliding").toBool(), true);
        QCOMPARE(wheelItem->property("activeTargetY").toDouble(), 165.0);
        sendWheel(QPoint(0, 0), QPoint(0, -120));
        QCOMPARE(wheelItem->property("activeTargetY").toDouble(), 330.0);
        QTRY_VERIFY_WITH_TIMEOUT(!wheelItem->property("gliding").toBool(), 1000);
        QVERIFY2(qAbs(flickItem->property("contentY").toDouble() - 330.0) <= 1.0,
                 qPrintable(QString::number(flickItem->property("contentY").toDouble())));

        // Real trackpad pixelDelta cancels any running glide immediately and moves directly by -pixelDelta.y * 1.25.
        sendWheel(QPoint(0, 0), QPoint(0, -120));
        QCOMPARE(wheelItem->property("gliding").toBool(), true);
        const double beforePixel = flickItem->property("contentY").toDouble();
        sendWheel(QPoint(0, -40), QPoint(0, 0));
        QCOMPARE(wheelItem->property("gliding").toBool(), false);
        QVERIFY2(qAbs(flickItem->property("contentY").toDouble() - (beforePixel + 50.0)) <= 1.0,
                 qPrintable(QString::number(flickItem->property("contentY").toDouble())));

        // Ctrl+Wheel passes through untouched (leaves contentY stable, reaches outer Ctrl WheelHandler).
        const double beforeCtrl = flickItem->property("contentY").toDouble();
        sendWheel(QPoint(0, 0), QPoint(0, -120), Qt::ControlModifier);
        QCOMPARE(flickItem->property("contentY").toDouble(), beforeCtrl);
        QCOMPARE(rootObj->property("ctrlWheelCount").toInt(), 1);

        // External contentY reset (like positionViewAtBeginning) cancels an in-flight glide cleanly.
        sendWheel(QPoint(0, 0), QPoint(0, -120));
        QCOMPARE(wheelItem->property("gliding").toBool(), true);
        flickItem->setProperty("contentY", 0.0);
        QCOMPARE(wheelItem->property("gliding").toBool(), false);
        QCOMPARE(flickItem->property("contentY").toDouble(), 0.0);

        // reducedMotion jumps instantaneously and clamps cleanly at top (0) and bottom (1600).
        theme.setReducedMotion(true);
        sendWheel(QPoint(0, 0), QPoint(0, -120));
        QCOMPARE(wheelItem->property("gliding").toBool(), false);
        QCOMPARE(flickItem->property("contentY").toDouble(), 165.0);
        sendWheel(QPoint(0, 0), QPoint(0, 2400));
        QCOMPARE(flickItem->property("contentY").toDouble(), 0.0);
        sendWheel(QPoint(0, 0), QPoint(0, -24000));
        QCOMPARE(flickItem->property("contentY").toDouble(), 1600.0);

        // GridView with a 220px header (negative originY): scrolls down by exact stepSize and back to header top.
        flickItem->setVisible(false);
        gridItem->setVisible(true);
        QMetaObject::invokeMethod(gridItem, "positionViewAtBeginning");
        const double gridOriginY = gridItem->property("originY").toDouble();
        QVERIFY2(gridOriginY <= -200.0, qPrintable(QString::number(gridOriginY)));
        QCOMPARE(gridItem->property("contentY").toDouble(), gridOriginY);
        sendWheel(QPoint(0, 0), QPoint(0, -120));
        QVERIFY2(qAbs(gridItem->property("contentY").toDouble() - (gridOriginY + 165.0)) <= 1.0,
                 qPrintable(QStringLiteral("contentY=%1 expected=%2").arg(gridItem->property("contentY").toDouble()).arg(gridOriginY + 165.0)));
        sendWheel(QPoint(0, 0), QPoint(0, 240));
        QVERIFY2(qAbs(gridItem->property("contentY").toDouble() - gridOriginY) <= 1.0,
                 qPrintable(QStringLiteral("contentY=%1 expected=%2").arg(gridItem->property("contentY").toDouble()).arg(gridOriginY)));

        // When no vertical scroll room exists (contentHeight <= height), handleWheelDelta returns false.
        gridItem->setVisible(false);
        flickItem->setVisible(true);
        flickItem->setProperty("contentHeight", 300.0);
        flickItem->setProperty("contentY", 0.0);
        QVariant handled;
        QVERIFY(QMetaObject::invokeMethod(wheelItem, "handleWheelDelta", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, -120.0), Q_ARG(QVariant, 0.0)));
        QCOMPARE(handled.toBool(), false);
        rootItem->setParentItem(nullptr);

        // 3. CoverImage.qml compiles cleanly with async decoding and rounded-mask guards.
        QQmlComponent coverComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CoverImage.qml")));
        QVERIFY2(coverComp.status() == QQmlComponent::Ready, qPrintable(coverComp.errorString()));

        // 4. LibraryView.qml wires SmoothWheel + flick tuning on both grid and listView,
        //    and all app scroll views wire SmoothWheel consistently.
        auto readQml = [&](const QString &rel) -> QString {
            QFile f(srcRoot + QLatin1Char('/') + rel);
            if (!f.open(QIODevice::ReadOnly))
                return {};
            return QString::fromUtf8(f.readAll());
        };
        const QString libQml = readQml(QStringLiteral("ui/library/LibraryView.qml"));
        const qsizetype gridPos = libQml.indexOf(QStringLiteral("id: grid"));
        const qsizetype listPos = libQml.indexOf(QStringLiteral("id: listView"));
        QVERIFY(gridPos > 0 && listPos > gridPos);
        const QString gridSection = libQml.mid(gridPos, listPos - gridPos);
        const QString listSection = libQml.mid(listPos, 900);
        QVERIFY(gridSection.contains(QStringLiteral("SmoothWheel { stepSize: 165 }")));
        QVERIFY(gridSection.contains(QStringLiteral("maximumFlickVelocity: 4500")));
        QVERIFY(gridSection.contains(QStringLiteral("flickDeceleration: 2200")));
        QVERIFY(gridSection.contains(QStringLiteral("pixelAligned: true")));
        QVERIFY(listSection.contains(QStringLiteral("SmoothWheel { stepSize: 115 }")));
        QVERIFY(listSection.contains(QStringLiteral("maximumFlickVelocity: 4500")));
        QVERIFY(listSection.contains(QStringLiteral("flickDeceleration: 2200")));
        QVERIFY(listSection.contains(QStringLiteral("pixelAligned: true")));

        QVERIFY(readQml(QStringLiteral("ui/library/LibrarySidebar.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 105 }")));
        QVERIFY(readQml(QStringLiteral("ui/settings/SettingsView.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 120 }")));
        QVERIFY(readQml(QStringLiteral("ui/audio/VoicePanel.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 110 }")));
        QVERIFY(readQml(QStringLiteral("ui/reader/ContentsPanel.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 110 }")));
        QVERIFY(readQml(QStringLiteral("ui/reader/MarksPanel.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 110 }")));
        const QString compQml = readQml(QStringLiteral("ui/companion/CompanionPanel.qml"));
        QVERIFY(compQml.count(QStringLiteral("SmoothWheel { stepSize: 115 }")) >= 4);
    }

    void articlesShelfAndDesktopV040Wiring() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));

        // 1. Articles shelf detection across all 3 sources, epub split, sourceUrl, and schema v4 invariant.
        {
            Library library(dataDir, cacheDir);
            QVERIFY(library.open());
            QCOMPARE(library.schemaVersion(), 4);

            const QString bookEpub = dir.filePath(QStringLiteral("regular-novel.epub"));
            const QString settingArtEpub = dir.filePath(QStringLiteral("setting-article.epub"));
            const QString subjectArtEpub = dir.filePath(QStringLiteral("subject-article.epub"));
            const QString dirArtEpub = dataDir + QStringLiteral("/articles/dir-article.epub");
            const QString samplePdf = dir.filePath(QStringLiteral("paper.pdf"));

            auto writeSimpleEpub = [](const QString &path, const QByteArray &title, const QByteArray &author) {
                const QByteArray opf =
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">"
                    "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                    "<dc:identifier id=\"uid\">urn:test:" + title + "</dc:identifier>"
                    "<dc:title>" + title + "</dc:title>"
                    "<dc:creator>" + author + "</dc:creator>"
                    "<dc:language>en</dc:language>"
                    "</metadata>"
                    "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                    "<spine><itemref idref=\"c\"/></spine></package>";
                return TestBooks::write(
                    path, TestBooks::epub({{"OEBPS/content.opf", opf},
                                           {"OEBPS/c.xhtml", TestBooks::chapter(title, "<p>Body prose.</p>")}}));
            };

            QVERIFY(writeSimpleEpub(bookEpub, "Regular Novel", "Ada Lovelace"));
            QVERIFY(writeSimpleEpub(settingArtEpub, "Setting Essay", "Grace Hopper"));
            QVERIFY(TestBooks::write(samplePdf, TestBooks::pdf("Research Paper", "Alan Turing")));

            EpubArticle art;
            art.title = QStringLiteral("Subject Article");
            art.author = QStringLiteral("Alan Kay");
            art.sourceUrl = QStringLiteral("https://example.org/subject-article");
            art.subjects = {QStringLiteral("Clothbound Article")};
            art.bodyXhtml = QStringLiteral("<p>Readable article prose packaged offline.</p>");
            QString writeErr;
            QVERIFY2(EpubWriter::writeArticle(subjectArtEpub, art, &writeErr), qPrintable(writeErr));

            QVERIFY(QDir().mkpath(dataDir + QStringLiteral("/articles")));
            QVERIFY(writeSimpleEpub(dirArtEpub, "Folder Article", "Donald Knuth"));

            const int idBook = library.addBookFile(bookEpub);
            const int idSettingArt = library.addBookFile(settingArtEpub);
            const int idSubjectArt = library.addBookFile(subjectArtEpub);
            const int idDirArt = library.addBookFile(dirArtEpub);
            const int idPdf = library.addBookFile(samplePdf);
            QVERIFY(idBook > 0 && idSettingArt > 0 && idSubjectArt > 0 && idDirArt > 0 && idPdf > 0);

            library.setBookSetting(idSettingArt, QStringLiteral("content_kind"), QStringLiteral("article"));
            library.setBookSetting(idSettingArt, QStringLiteral("source_url"), QStringLiteral("https://example.org/essay"));
            library.setBookSetting(idSubjectArt, QStringLiteral("source_url"), QStringLiteral("https://example.org/subject-article"));
            library.works()->setGrouped(false);

            QVariantMap counts = library.shelfCounts();
            QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 5);
            QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 1);
            QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 1);
            QCOMPARE(counts.value(QStringLiteral("articles")).toInt(), 3);

            // Articles shelf shows all 3 articles and marks ArticleRole == true.
            library.works()->setStatusFilter(QStringLiteral("articles"));
            QCOMPARE(library.works()->count(), 3);
            for (int row = 0; row < library.works()->count(); ++row)
                QCOMPARE(library.works()->get(row).value(QStringLiteral("article")).toBool(), true);

            // Books (epub) shelf excludes articles so Books and Articles stay mutually distinct.
            library.works()->clearFilters();
            library.works()->setFormatFilter(QStringLiteral("epub"));
            QCOMPARE(library.works()->count(), 1);
            QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), idBook);
            QCOMPARE(library.works()->get(0).value(QStringLiteral("article")).toBool(), false);

            // Library::toMap() & Library::book() expose isArticle & sourceUrl.
            const QVariantMap artMap = library.book(idSettingArt);
            QCOMPARE(artMap.value(QStringLiteral("isArticle")).toBool(), true);
            QCOMPARE(artMap.value(QStringLiteral("sourceUrl")).toString(), QStringLiteral("https://example.org/essay"));
            const QVariantMap regularMap = library.book(idBook);
            QCOMPARE(regularMap.value(QStringLiteral("isArticle")).toBool(), false);
            QCOMPARE(regularMap.value(QStringLiteral("sourceUrl")).toString(), QString());

            // Clearing content_kind on setting-only article moves it back into Books (epub).
            library.setBookSetting(idSettingArt, QStringLiteral("content_kind"), QString());
            counts = library.shelfCounts();
            QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 2);
            QCOMPARE(counts.value(QStringLiteral("articles")).toInt(), 2);
            library.setBookSetting(idSettingArt, QStringLiteral("content_kind"), QStringLiteral("article"));
            QCOMPARE(library.shelfCounts().value(QStringLiteral("articles")).toInt(), 3);
        }

        // 2. Persistence across reopen preserves schemaVersion() == 4, Articles shelf, and sourceUrl.
        {
            Library reopened(dataDir, cacheDir);
            QVERIFY(reopened.open());
            QCOMPARE(reopened.schemaVersion(), 4);
            const QVariantMap counts = reopened.shelfCounts();
            QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 5);
            QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 1);
            QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 1);
            QCOMPARE(counts.value(QStringLiteral("articles")).toInt(), 3);

            reopened.works()->setGrouped(false);
            reopened.works()->setStatusFilter(QStringLiteral("articles"));
            QCOMPARE(reopened.works()->count(), 3);
            const int idSettingArt = reopened.idForPath(dir.filePath(QStringLiteral("setting-article.epub")));
            QVERIFY(idSettingArt > 0);
            QCOMPARE(reopened.book(idSettingArt).value(QStringLiteral("isArticle")).toBool(), true);
            QCOMPARE(reopened.book(idSettingArt).value(QStringLiteral("sourceUrl")).toString(),
                     QStringLiteral("https://example.org/essay"));
        }

        // 3. Live QML smoke test for AddUrlSheet.qml, LibraryView.qml (with Online section & sheets),
        //    and BookDetails.qml (with Source URL row & Web article label) — zero runtime QML warnings.
        {
            Library library(dataDir, cacheDir);
            QVERIFY(library.open());
            BookScheme scheme;
            Reader reader(&library, &scheme);
            GeminiProvider gemini;
            MemoryKeyStore keys;
            AiServices ai(&gemini, &keys, library.database().connectionName());
            LibraryFinder finder;
            PortalFilePicker picker;
            Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
            Translator translator;
            WebImporter webImporter(&library, dataDir);
            OpdsClient opds(&library, &keys, dataDir, dir.filePath(QStringLiteral("opds.ini")));

            QQmlEngine engine;
            const QString srcRoot = QFINDTESTDATA("../src");
            QVERIFY(QFileInfo::exists(srcRoot));
            engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
            engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
            engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
            engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
            engine.rootContext()->setContextProperty(QStringLiteral("libraryFinder"), &finder);
            engine.rootContext()->setContextProperty(QStringLiteral("picker"), &picker);
            engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
            engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
            engine.rootContext()->setContextProperty(QStringLiteral("webImporter"), &webImporter);
            engine.rootContext()->setContextProperty(QStringLiteral("opds"), &opds);

            QList<QQmlError> warnings;
            QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                             [&](const QList<QQmlError> &errs) { warnings.append(errs); });

            QQmlComponent addUrlComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/AddUrlSheet.qml")));
            QVERIFY2(addUrlComp.status() == QQmlComponent::Ready, qPrintable(addUrlComp.errorString()));
            std::unique_ptr<QObject> addUrlObj(addUrlComp.create());
            QVERIFY2(addUrlObj != nullptr, qPrintable(addUrlComp.errorString()));
            QMetaObject::invokeMethod(addUrlObj.get(), "show", Q_ARG(QVariant, QStringLiteral("https://arxiv.org/abs/2301.00001")),
                                      Q_ARG(QVariant, 0));
            QCOMPARE(addUrlObj->property("open").toBool(), true);
            QCOMPARE(addUrlObj->property("detectedKind").toString(), QStringLiteral("arxiv"));
            QMetaObject::invokeMethod(addUrlObj.get(), "close");
            QCOMPARE(addUrlObj->property("open").toBool(), false);

            QQmlComponent libComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/LibraryView.qml")));
            QVERIFY2(libComp.status() == QQmlComponent::Ready, qPrintable(libComp.errorString()));
            std::unique_ptr<QObject> libObj(libComp.create());
            QVERIFY2(libObj != nullptr, qPrintable(libComp.errorString()));
            QCOMPARE(libObj->property("addUrlOpen").toBool(), false);
            QCOMPARE(libObj->property("catalogsOpen").toBool(), false);
            QMetaObject::invokeMethod(libObj.get(), "openAddUrl", Q_ARG(QVariant, QString()));
            QCOMPARE(libObj->property("addUrlOpen").toBool(), true);
            QCOMPARE(libObj->property("typing").toBool(), true);
            QMetaObject::invokeMethod(libObj.get(), "closeAddUrl");
            QCOMPARE(libObj->property("addUrlOpen").toBool(), false);
            QMetaObject::invokeMethod(libObj.get(), "openCatalogs");
            QCOMPARE(libObj->property("catalogsOpen").toBool(), true);
            QCOMPARE(libObj->property("typing").toBool(), true);
            QMetaObject::invokeMethod(libObj.get(), "closeCatalogs");
            QCOMPARE(libObj->property("catalogsOpen").toBool(), false);

            const int idSettingArt = library.idForPath(dir.filePath(QStringLiteral("setting-article.epub")));
            QMetaObject::invokeMethod(libObj.get(), "showDetails", Q_ARG(QVariant, 0));
            QQmlComponent detComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/BookDetails.qml")));
            QVERIFY2(detComp.status() == QQmlComponent::Ready, qPrintable(detComp.errorString()));
            std::unique_ptr<QObject> detObj(detComp.create());
            QVERIFY2(detObj != nullptr, qPrintable(detComp.errorString()));
            QMetaObject::invokeMethod(detObj.get(), "show", Q_ARG(QVariant, idSettingArt), Q_ARG(QVariant, QString()));
            QCoreApplication::processEvents();
            QMetaObject::invokeMethod(detObj.get(), "close");

            QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
        }

        // 4. CLI `clothbound add <url>` routes http(s):// and arxiv: arguments through WebImporter.
        {
            QVERIFY(cliSummary().contains(QStringLiteral("add <folder|book|url>")));
            // Private/loopback URL is rejected by UrlGuard (proving WebImporter ran instead of local file check).
            QCOMPARE(runCli({QStringLiteral("clothbound"), QStringLiteral("add"), QStringLiteral("--json"),
                             QStringLiteral("http://127.0.0.1/blocked.epub")}),
                     1);
        }
    }

    void pdfPresetViewModes() {
        QTemporaryDir dir;
        const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Preset Modes", "Ada Marsh")));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(pdfPath);
        QVERIFY(bookId > 0);

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));
        QCOMPARE(reader.format(), QStringLiteral("pdf"));

        Marks marks(&library);
        BookSearch bookSearch(&reader);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        Theme theme;

        const QVariantMap mockReadingSettings{
            {QStringLiteral("pageTurnSound"), true},
            {QStringLiteral("pdfFit"), QStringLiteral("page")},
        };

        QQmlEngine engine;
        engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
        engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
        engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
        engine.rootContext()->setContextProperty(QStringLiteral("readingSettings"), mockReadingSettings);

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &list) { warnings.append(list); });

        // 1. Live PdfSurface.qml fit modes: Fit page, Fit width (+ live resize/Companion dock), Actual, and Custom zoom.
        QQmlComponent surfComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml")));
        QVERIFY2(surfComp.status() == QQmlComponent::Ready, qPrintable(surfComp.errorString()));
        const QVariantMap look{
            {QStringLiteral("bg"), QStringLiteral("#FAF6EE")},
            {QStringLiteral("fg"), QStringLiteral("#26221D")},
            {QStringLiteral("muted"), QStringLiteral("#787066")},
            {QStringLiteral("line"), QStringLiteral("#E2DCD0")},
            {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
            {QStringLiteral("link"), QStringLiteral("#8A5A12")},
            {QStringLiteral("dark"), false},
        };
        std::unique_ptr<QObject> surfObj(surfComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 760.0},
            {QStringLiteral("look"), look},
        }));
        QVERIFY2(surfObj != nullptr, qPrintable(surfComp.errorString()));

        QVariantMap styleMap{
            {QStringLiteral("layout"), QStringLiteral("single")},
            {QStringLiteral("pdfCrop"), false},
            {QStringLiteral("pdfOriginalColors"), false},
            {QStringLiteral("pdfFit"), QStringLiteral("page")},
            {QStringLiteral("marginTop"), 60.0},
            {QStringLiteral("marginBottom"), 76.0},
            {QStringLiteral("animate"), false},
        };
        const QVariantMap posMap{{QStringLiteral("chapter"), 0}};
        QMetaObject::invokeMethod(surfObj.get(), "open",
                                  Q_ARG(QVariant, reader.manifest()),
                                  Q_ARG(QVariant, posMap),
                                  Q_ARG(QVariant, styleMap));
        QCoreApplication::processEvents();

        // Default Fit page: fits inside 1000x760 with zero panning and 1.0 effectiveZoom.
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("page"));
        QVERIFY(qAbs(surfObj->property("effectiveZoom").toDouble() - 1.0) < 0.02);
        QCOMPARE(surfObj->property("canPanX").toBool(), false);
        QCOMPARE(surfObj->property("canPanY").toBool(), false);
        const double fitPageW = surfObj->property("activeDisplayWidth").toDouble();
        QVERIFY2(fitPageW > 200.0 && fitPageW < 700.0, qPrintable(QString::number(fitPageW)));

        // Fit width: fills roomWidth (1000 - 48 = 952), zero horizontal overflow, vertical scroll active.
        QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("width")));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("width"));
        QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - 952.0) < 1.5,
                 qPrintable(QString::number(surfObj->property("activeDisplayWidth").toDouble())));
        QCOMPARE(surfObj->property("canPanX").toBool(), false);
        QCOMPARE(surfObj->property("canPanY").toBool(), true);
        QCOMPARE(surfObj->property("zoomedIn").toBool(), true);
        const double widthScale = surfObj->property("effectiveZoom").toDouble();
        QVERIFY2(widthScale > 1.3, qPrintable(QString::number(widthScale)));

        // Live auto-refit when window tiles or Companion drawer docks (1000 -> 680 px).
        surfObj->setProperty("width", 680.0);
        QCoreApplication::processEvents();
        QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - 632.0) < 1.5,
                 qPrintable(QString::number(surfObj->property("activeDisplayWidth").toDouble())));
        QCOMPARE(surfObj->property("canPanX").toBool(), false);
        surfObj->setProperty("width", 1000.0);
        QCoreApplication::processEvents();

        // Actual (1:1 print scale): 96/72 logical pixels per PDF point.
        QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("actual")));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("actual"));
        const QVariantMap pts = reader.pdfPageSize(0);
        const double expectedActualW = pts.value(QStringLiteral("width")).toDouble() * (96.0 / 72.0);
        QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - expectedActualW) < 1.5,
                 qPrintable(QStringLiteral("got=%1 expected=%2")
                                .arg(surfObj->property("activeDisplayWidth").toDouble())
                                .arg(expectedActualW)));

        // Nudging zoom from Fit width transitions smoothly to custom without jumping back to 1.0x.
        QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("width")));
        QCoreApplication::processEvents();
        const double beforeNudge = surfObj->property("effectiveZoom").toDouble();
        QMetaObject::invokeMethod(surfObj.get(), "nudgeZoom", Q_ARG(QVariant, 1.15));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("custom"));
        QVERIFY2(surfObj->property("effectiveZoom").toDouble() > beforeNudge * 1.10,
                 qPrintable(QStringLiteral("before=%1 after=%2")
                                .arg(beforeNudge)
                                .arg(surfObj->property("effectiveZoom").toDouble())));

        // Unrelated setStyle() update (same pdfFit) preserves active custom zoom.
        styleMap[QStringLiteral("pdfFit")] = QStringLiteral("width");
        styleMap[QStringLiteral("pdfOriginalColors")] = true;
        QMetaObject::invokeMethod(surfObj.get(), "setStyle", Q_ARG(QVariant, styleMap));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("custom"));

        // Explicit pdfFit change in setStyle() snaps to the new preset.
        styleMap[QStringLiteral("pdfFit")] = QStringLiteral("page");
        QMetaObject::invokeMethod(surfObj.get(), "setStyle", Q_ARG(QVariant, styleMap));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("page"));

        // Scroll layout also honors Fit width with zero horizontal overflow.
        styleMap[QStringLiteral("layout")] = QStringLiteral("scroll");
        styleMap[QStringLiteral("pdfFit")] = QStringLiteral("width");
        QMetaObject::invokeMethod(surfObj.get(), "setStyle", Q_ARG(QVariant, styleMap));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("width"));
        QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - 952.0) < 1.5,
                 qPrintable(QString::number(surfObj->property("activeDisplayWidth").toDouble())));
        QCOMPARE(surfObj->property("canPanX").toBool(), false);

        // 1b. Two-page spread Fit width: next()/prev() (Left/Right arrows) turn immediately on a single press
        //     without vertically shifting first, while scrollVertical() scrolls tall pages cleanly.
        {
            const QString multiPath = dir.filePath(QStringLiteral("multi.pdf"));
            {
                QPdfWriter writer(multiPath);
                writer.setPageSize(QPageSize(QPageSize::Letter));
                writer.setResolution(72);
                QPainter p(&writer);
                for (int i = 0; i < 5; ++i) {
                    if (i > 0)
                        writer.newPage();
                    p.drawText(QPointF(72, 100), QStringLiteral("Page %1").arg(i + 1));
                }
            }
            const int multiId = library.addBookFile(multiPath);
            QVERIFY(multiId > 0);
            QVERIFY(reader.openBook(multiId));
            QCOMPARE(reader.chapterCount(), 5);

            surfObj->setProperty("width", 1280.0);
            surfObj->setProperty("height", 800.0);
            QVariantMap spreadStyle{
                {QStringLiteral("layout"), QStringLiteral("double")},
                {QStringLiteral("pdfCrop"), false},
                {QStringLiteral("pdfOriginalColors"), false},
                {QStringLiteral("pdfFit"), QStringLiteral("width")},
                {QStringLiteral("marginTop"), 60.0},
                {QStringLiteral("marginBottom"), 76.0},
                {QStringLiteral("animate"), false},
            };
            const QVariantMap startPos{{QStringLiteral("chapter"), 0}};
            QMetaObject::invokeMethod(surfObj.get(), "open",
                                      Q_ARG(QVariant, reader.manifest()),
                                      Q_ARG(QVariant, startPos),
                                      Q_ARG(QVariant, spreadStyle));
            QCoreApplication::processEvents();
            QCOMPARE(surfObj->property("spread").toBool(), true);
            QCOMPARE(surfObj->property("current").toInt(), 0);
            QCOMPARE(surfObj->property("canPanY").toBool(), true);

            // Single press of next() turns from cover [0] -> spread [1, 2] -> spread [3, 4].
            QMetaObject::invokeMethod(surfObj.get(), "next");
            QCoreApplication::processEvents();
            QCOMPARE(surfObj->property("current").toInt(), 1);
            QCOMPARE(surfObj->property("canPanY").toBool(), true);
            QMetaObject::invokeMethod(surfObj.get(), "next");
            QCoreApplication::processEvents();
            QCOMPARE(surfObj->property("current").toInt(), 3);

            // Single press of prev() turns straight back [3, 4] -> [1, 2] -> [0].
            QMetaObject::invokeMethod(surfObj.get(), "prev");
            QCoreApplication::processEvents();
            QCOMPARE(surfObj->property("current").toInt(), 1);
            QMetaObject::invokeMethod(surfObj.get(), "prev");
            QCoreApplication::processEvents();
            QCOMPARE(surfObj->property("current").toInt(), 0);

            // scrollVertical(1) scrolls vertically down the tall page without turning pages.
            QVariant scrolled;
            QMetaObject::invokeMethod(surfObj.get(), "scrollVertical",
                                      Q_RETURN_ARG(QVariant, scrolled), Q_ARG(QVariant, 1));
            QCOMPARE(scrolled.toBool(), true);
            QCOMPARE(surfObj->property("current").toInt(), 0);

            // Switch back to original single-page PDF for remaining per-book checks.
            QVERIFY(reader.openBook(bookId));
        }

        // 2. Per-book appearance override persists pdfFit cleanly across Reader reload.
        reader.saveBookAppearance(QVariantMap{
            {QStringLiteral("pdfFit"), QStringLiteral("width")},
            {QStringLiteral("pdfCrop"), true},
        });
        QVERIFY(reader.bookAppearanceOverride());
        QCOMPARE(reader.bookAppearance().value(QStringLiteral("pdfFit")).toString(), QStringLiteral("width"));
        Reader reloaded(&library, &scheme);
        QVERIFY(reloaded.openBook(bookId));
        QVERIFY(reloaded.bookAppearanceOverride());
        QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("pdfFit")).toString(), QStringLiteral("width"));

        // 3. AppearancePanel.qml instantiates cleanly for PDFs with Fit presets and emits pdfFitChosen.
        QQmlComponent apComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/AppearancePanel.qml")));
        QVERIFY2(apComp.status() == QQmlComponent::Ready, qPrintable(apComp.errorString()));
        const QVariantMap apSettings{
            {QStringLiteral("theme"), QStringLiteral("paper")},
            {QStringLiteral("font"), QStringLiteral("Literata")},
            {QStringLiteral("fontScale"), 1.15},
            {QStringLiteral("fontWeight"), 400},
            {QStringLiteral("lineHeight"), 1.62},
            {QStringLiteral("paragraphSpacing"), 0.0},
            {QStringLiteral("margins"), QStringLiteral("normal")},
            {QStringLiteral("justify"), true},
            {QStringLiteral("hyphenate"), true},
            {QStringLiteral("dropCap"), true},
            {QStringLiteral("layout"), QStringLiteral("auto")},
            {QStringLiteral("animate"), true},
            {QStringLiteral("publisher"), false},
            {QStringLiteral("pdfCrop"), true},
            {QStringLiteral("pdfOriginalColors"), false},
            {QStringLiteral("pdfFit"), QStringLiteral("width")},
        };
        std::unique_ptr<QObject> apObj(apComp.createWithInitialProperties({
            {QStringLiteral("height"), 760.0},
            {QStringLiteral("look"), look},
            {QStringLiteral("settings"), apSettings},
            {QStringLiteral("pdf"), true},
            {QStringLiteral("pdfSurface"), QVariant::fromValue(surfObj.get())},
            {QStringLiteral("bookOverride"), false},
        }));
        QVERIFY2(apObj != nullptr, qPrintable(apComp.errorString()));
        QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));

        // 4. Bookmark dog-ear stays clear of Companion button (52 px tool reserve) and Companion C toggles cleanly.
        {
            QFile erFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
            QVERIFY(erFile.open(QIODevice::ReadOnly));
            const QString erQml = QString::fromUtf8(erFile.readAll());
            QVERIFY(erQml.contains(QStringLiteral("anchors.rightMargin: 52")));
            QVERIFY(erQml.contains(QStringLiteral("companionPanel.open ? companionPanel.close() : showCompanion(\"ask\")")));

            QFile cpFile(srcRoot + QStringLiteral("/ui/companion/CompanionPanel.qml"));
            QVERIFY(cpFile.open(QIODevice::ReadOnly));
            const QString cpQml = QString::fromUtf8(cpFile.readAll());
            QVERIFY(cpQml.contains(QStringLiteral("function show(which, focusInput)")));
            QVERIFY(cpQml.contains(QStringLiteral("if (focusInput && !choosing && tab === \"ask\")")));
        }
    }

    void cacheDirectoryDeletionSelfHeals() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        const QString audioDir = cacheDir + QStringLiteral("/audio");
        const QString indexPath = cacheDir + QStringLiteral("/index.sqlite");
        const QString epubPath = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(24, 36))));

        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        const QString coverFile = library.record(bookId).cover;
        QVERIFY(!coverFile.isEmpty());
        QVERIFY(QFileInfo::exists(library.coverDirectory() + QLatin1Char('/') + coverFile));

        AudioCache cache(audioDir, library.database().connectionName());
        const QString key1 = AudioCache::keyFor("builtin", "m", "v", "", "First cached line.");
        QByteArray pcm(4800, '\x10');
        {
            QSignalSpy storedSpy(&cache, &AudioCache::stored);
            cache.store(key1, pcm, 24000, {});
            QTRY_VERIFY_WITH_TIMEOUT(storedSpy.count() >= 1, 5000);
        }
        QVERIFY(!cache.lookup(key1).isEmpty());

        LibraryIndex index(indexPath);
        QVERIFY(index.available());
        QVERIFY(LibraryIndex::indexBook(indexPath, {bookId, epubPath, QStringLiteral("epub"), 100}));
        QCOMPARE(index.bookCount(), 1);

        // Simulate the user accidentally deleting ~/.cache/clothbound while the app is running!
        QVERIFY(QDir(cacheDir).removeRecursively());
        QVERIFY(!QFileInfo::exists(cacheDir));

        // 1. AudioCache misses cleanly, prunes stale stats, recreates dir, and stores new clips.
        QVERIFY(cache.lookup(key1).isEmpty());
        cache.refreshStats();
        QCOMPARE(cache.clipCount(), 0);
        const QString key2 = AudioCache::keyFor("builtin", "m", "v", "", "Recovered after cache wipe.");
        {
            QSignalSpy storedSpy(&cache, &AudioCache::stored);
            cache.store(key2, pcm, 24000, {});
            QTRY_VERIFY_WITH_TIMEOUT(storedSpy.count() >= 1, 5000);
        }
        QVERIFY(!cache.lookup(key2).isEmpty());

        // 2. LibraryIndex detaches from unlinked inode, recreates index.sqlite, and indexes cleanly.
        QCOMPARE(index.bookCount(), 0);
        QVERIFY(QFileInfo::exists(indexPath));
        QVERIFY(LibraryIndex::indexBook(indexPath, {bookId, epubPath, QStringLiteral("epub"), 100}));
        QCOMPARE(index.bookCount(), 1);

        // 3. Library::rescan() notices missing cached cover file and automatically re-extracts it.
        QVERIFY(!QFileInfo::exists(library.coverDirectory() + QLatin1Char('/') + coverFile));
        QSignalSpy scanDone(&library, &Library::scanFinished);
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(scanDone.count() >= 1, 5000);
        QVERIFY(QFileInfo::exists(library.coverDirectory() + QLatin1Char('/') + library.record(bookId).cover));
    }

    void trayIconAndShortcutToggleWiring() {
        TrayIcon tray;
        QCOMPARE(tray.playing(), false);
        const TrayMenuLayoutItem pausedLayout = tray.buildMenuLayout(0, 1);
        QCOMPARE(pausedLayout.children.size(), 6);
        QCOMPARE(pausedLayout.children.first().variant().value<TrayMenuLayoutItem>().properties.value("label").toString(),
                 QStringLiteral("Play"));
        tray.setPlaying(true);
        const TrayMenuLayoutItem playingLayout = tray.buildMenuLayout(0, 1);
        QCOMPARE(playingLayout.children.first().variant().value<TrayMenuLayoutItem>().properties.value("label").toString(),
                 QStringLiteral("Pause"));

        QSignalSpy actSpy(&tray, &TrayIcon::activated);
        QSignalSpy toggleSpy(&tray, &TrayIcon::togglePlayRequested);
        QSignalSpy nextSpy(&tray, &TrayIcon::nextRequested);
        QSignalSpy prevSpy(&tray, &TrayIcon::previousRequested);
        QSignalSpy quitSpy(&tray, &TrayIcon::quitRequested);
        tray.handleActivate();
        tray.handleMenuEvent(1, QStringLiteral("clicked"));
        tray.handleMenuEvent(2, QStringLiteral("clicked"));
        tray.handleMenuEvent(3, QStringLiteral("clicked"));
        tray.handleMenuEvent(5, QStringLiteral("clicked"));
        tray.handleMenuEvent(6, QStringLiteral("clicked"));
        QCOMPARE(actSpy.count(), 2);
        QCOMPARE(toggleSpy.count(), 1);
        QCOMPARE(nextSpy.count(), 1);
        QCOMPARE(prevSpy.count(), 1);
        QCOMPARE(quitSpy.count(), 1);

        const QString srcRoot = QFINDTESTDATA("../src");
        QFile mainFile(srcRoot + QStringLiteral("/ui/Main.qml"));
        QVERIFY(mainFile.open(QIODevice::ReadOnly));
        const QString mainQml = QString::fromUtf8(mainFile.readAll());
        QVERIFY(!mainQml.contains(QStringLiteral("Qt.labs.platform")));
        QVERIFY(mainQml.contains(QStringLiteral("sequence: \"Ctrl+Q\"")));
        QVERIFY(mainQml.contains(QStringLiteral("sequence: \"Ctrl+W\"")));
        QVERIFY(mainQml.contains(QStringLiteral("function quitApp()")));
        QVERIFY(mainQml.contains(QStringLiteral("close.accepted = true")));
        QVERIFY(!mainQml.contains(QStringLiteral("close.accepted = false")));

        // Verify live Ctrl+Q and Ctrl+W ApplicationShortcuts fire unambiguously on every Linux theme.
        {
            QQmlEngine qmlEngine;
            QQmlComponent comp(&qmlEngine);
            comp.setData(QByteArrayLiteral(R"QML(
import QtQuick
Item {
    id: scRoot
    width: 200; height: 200
    focus: true
    property int quitHits: 0
    property int closeHits: 0
    property int ambiguousHits: 0
    Shortcut {
        sequence: "Ctrl+Q"
        context: Qt.ApplicationShortcut
        onActivated: scRoot.quitHits++
        onActivatedAmbiguously: scRoot.ambiguousHits++
    }
    Shortcut {
        sequence: "Ctrl+W"
        context: Qt.ApplicationShortcut
        onActivated: scRoot.closeHits++
        onActivatedAmbiguously: scRoot.ambiguousHits++
    }
}
)QML"),
                         QUrl::fromLocalFile(srcRoot + QStringLiteral("/QuitShortcutHarness.qml")));
            QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
            std::unique_ptr<QQuickWindow> win(new QQuickWindow);
            win->resize(200, 200);
            std::unique_ptr<QObject> obj(comp.create());
            QVERIFY(obj != nullptr);
            auto *item = qobject_cast<QQuickItem *>(obj.get());
            QVERIFY(item != nullptr);
            item->setParentItem(win->contentItem());
            win->show();
            win->requestActivate();
            QVERIFY(QTest::qWaitForWindowExposed(win.get()));
            QCoreApplication::processEvents();
            QTest::keyClick(win.get(), Qt::Key_Q, Qt::ControlModifier);
            QTest::keyClick(win.get(), Qt::Key_W, Qt::ControlModifier);
            QCOMPARE(obj->property("quitHits").toInt(), 1);
            QCOMPARE(obj->property("closeHits").toInt(), 1);
            QCOMPARE(obj->property("ambiguousHits").toInt(), 0);
            item->setParentItem(nullptr);
        }

        QFile erFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
        QVERIFY(erFile.open(QIODevice::ReadOnly));
        const QString erQml = QString::fromUtf8(erFile.readAll());
        QVERIFY(erQml.contains(QStringLiteral("appearance.open ? appearance.close() : view.showAppearance()")));
        QVERIFY(erQml.contains(QStringLiteral("contents.open ? contents.close() : view.showContents()")));
        QVERIFY(erQml.contains(QStringLiteral("marksPanel.open ? marksPanel.close() : view.showMarks()")));
        QVERIFY(erQml.contains(QStringLiteral("voicePanel.open ? voicePanel.close() : view.showVoices()")));
    }

    void pdfPagesRawCacheAndDownscaleReuse() {
        const QString token = QStringLiteral("test-pdf-reuse");
        const QString id = token + QStringLiteral("/0?bg=%23111111&fg=%23eeeeee");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("dummy.pdf"));
        {
            QPdfWriter w(pdfPath);
            w.setPageSize(QPageSize(QPageSize::A4));
            QPainter p(&w);
            p.drawText(QRect(40, 40, 200, 50), QStringLiteral("Hello PDF"));
        }
        auto book = std::make_shared<PdfBook>(pdfPath);
        QVERIFY(book->open());
        PdfPages::add(token, book);

        QImage hi(1408, 2000, QImage::Format_RGB32);
        hi.fill(QColor(0x11, 0x11, 0x11));
        const QString hiKey = PdfPages::makeCacheKey(id, QSize(1408, 0));
        PdfPages::storeCached(token, id, 1408, hiKey, hi);

        // Narrowing (e.g. Companion docking: 1408 -> 1024) reuses the cached hi-res image.
        const QImage reuse = PdfPages::cachedDownscaleMatch(id, QSize(1024, 0));
        QCOMPARE(reuse.width(), 1408);
        // Zooming in (1408 -> 1600) must NOT reuse a smaller image.
        QVERIFY(PdfPages::cachedDownscaleMatch(id, QSize(1600, 0)).isNull());
        // Raw cache stores & clears cleanly with remove(token).
        const QString rawKey = PdfPages::makeRawKey(token, 0, QSize(1408, 0));
        PdfPages::storeRaw(token, rawKey, hi);
        QCOMPARE(PdfPages::cachedRaw(rawKey).width(), 1408);

        PdfPages::remove(token);
        QVERIFY(PdfPages::cached(hiKey).isNull());
        QVERIFY(PdfPages::cachedRaw(rawKey).isNull());
        QVERIFY(PdfPages::cachedDownscaleMatch(id, QSize(1024, 0)).isNull());
    }

    void smoothWheelTrackpadAccelerationAndKineticCoast() {
        Theme theme;
        QQmlEngine engine;
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

        // 1. Pure velocity-gain and kinetic coast math.
        QQmlComponent swComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/components/SmoothWheel.qml")));
        QVERIFY2(swComp.status() == QQmlComponent::Ready, qPrintable(swComp.errorString()));
        std::unique_ptr<QObject> swObj(swComp.create());
        QVERIFY2(swObj != nullptr, qPrintable(swComp.errorString()));

        auto gainFor = [&](double speed) -> double {
            QVariant ret;
            QMetaObject::invokeMethod(swObj.get(), "trackpadGainForSpeed", Q_RETURN_ARG(QVariant, ret), Q_ARG(QVariant, speed));
            return ret.toDouble();
        };
        QCOMPARE(gainFor(150.0), 1.0);
        QVERIFY(gainFor(1200.0) > 1.8);
        QVERIFY(gainFor(2500.0) >= 2.75);

        auto coastPlan = [&](double curY, double vel, double minY, double maxY) -> QVariantMap {
            QVariant ret;
            QMetaObject::invokeMethod(swObj.get(), "computeCoastPlan", Q_RETURN_ARG(QVariant, ret),
                                      Q_ARG(QVariant, curY), Q_ARG(QVariant, vel),
                                      Q_ARG(QVariant, minY), Q_ARG(QVariant, maxY));
            return ret.toMap();
        };
        // Below threshold -> zero coast.
        QCOMPARE(coastPlan(100.0, 150.0, 0.0, 6000.0).value(QStringLiteral("duration")).toInt(), 0);
        // Brisk downward flick -> substantial smooth coast distance (reaches across long views effortlessly).
        const QVariantMap brisk = coastPlan(100.0, 3600.0, 0.0, 6000.0);
        QVERIFY2(brisk.value(QStringLiteral("distance")).toDouble() > 1500.0,
                 qPrintable(QString::number(brisk.value(QStringLiteral("distance")).toDouble())));
        QVERIFY(brisk.value(QStringLiteral("duration")).toInt() >= 220);
        // Clamping near bottom shortens duration proportionally rather than crawling.
        const QVariantMap nearBottom = coastPlan(5800.0, 4500.0, 0.0, 6000.0);
        QCOMPARE(nearBottom.value(QStringLiteral("targetY")).toDouble(), 6000.0);
        QVERIFY(nearBottom.value(QStringLiteral("duration")).toInt() < 350);

        // 2. Live QQuickWindow trackpad flick -> accelerated tracking + kinetic coast on Qt::ScrollEnd,
        //    immediate stop when fingers touch back down, and zero double-coast on native ScrollMomentum.
        const QString harnessQml = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "ui/components"
Item {
    width: 600
    height: 400
    property alias flick: flick
    property alias wheel: sw
    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: 8000
        boundsBehavior: Flickable.StopAtBounds
        pixelAligned: true
        ScrollBar.vertical: ScrollBar {}
        SmoothWheel {
            id: sw
            stepSize: 165
        }
    }
}
)QML");
        QQmlComponent harnessComp(&engine);
        harnessComp.setData(harnessQml.toUtf8(), QUrl::fromLocalFile(srcRoot + QStringLiteral("/SmoothWheelTrackpadHarness.qml")));
        QVERIFY2(harnessComp.status() == QQmlComponent::Ready, qPrintable(harnessComp.errorString()));
        std::unique_ptr<QQuickWindow> win(new QQuickWindow);
        win->resize(600, 400);
        std::unique_ptr<QObject> rootObj(harnessComp.create());
        QVERIFY2(rootObj != nullptr, qPrintable(harnessComp.errorString()));
        QQuickItem *rootItem = qobject_cast<QQuickItem *>(rootObj.get());
        QVERIFY(rootItem != nullptr);
        rootItem->setParentItem(win->contentItem());
        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win.get()));

        QQuickItem *flickItem = rootObj->property("flick").value<QQuickItem *>();
        QQuickItem *wheelItem = rootObj->property("wheel").value<QQuickItem *>();
        QVERIFY(flickItem && wheelItem);

        auto invokeWheel = [&](double ay, double py, int phase, bool isTouchPad) -> bool {
            QVariant ret;
            QMetaObject::invokeMethod(wheelItem, "handleWheelEvent", Q_RETURN_ARG(QVariant, ret),
                                      Q_ARG(QVariant, ay), Q_ARG(QVariant, py),
                                      Q_ARG(QVariant, phase), Q_ARG(QVariant, isTouchPad));
            return ret.toBool();
        };

        // Simulate a brisk 6-frame two-finger flick (14 px per ~10 ms = ~1750 px/s raw).
        QVERIFY(invokeWheel(0.0, -14.0, int(Qt::ScrollBegin), true));
        for (int i = 0; i < 5; ++i) {
            QTest::qWait(10);
            QVERIFY(invokeWheel(0.0, -14.0, int(Qt::ScrollUpdate), true));
        }
        const double atLiftY = flickItem->property("contentY").toDouble();
        // Adaptive velocity gain should already exceed flat 1.25x (6 * 14 * 1.25 = 105 px).
        QVERIFY2(atLiftY > 140.0, qPrintable(QStringLiteral("atLiftY=%1").arg(atLiftY)));
        // Lift fingers -> ScrollEnd immediately kicks off kinetic coasting.
        QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollEnd), true));
        QCOMPARE(wheelItem->property("coasting").toBool(), true);
        const double coastTarget = wheelItem->property("activeTargetY").toDouble();
        QVERIFY2(coastTarget > atLiftY + 800.0,
                 qPrintable(QStringLiteral("coastTarget=%1 atLiftY=%2").arg(coastTarget).arg(atLiftY)));

        // Putting fingers back down (ScrollBegin) catches and stops the coast instantly.
        QTest::qWait(40);
        QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollBegin), true));
        QCOMPARE(wheelItem->property("coasting").toBool(), false);
        QCOMPARE(wheelItem->property("gliding").toBool(), false);

        // Native compositor ScrollMomentum passes through directly without launching synthetic coasting.
        const double beforeMom = flickItem->property("contentY").toDouble();
        QVERIFY(invokeWheel(0.0, -20.0, int(Qt::ScrollMomentum), true));
        QCOMPARE(wheelItem->property("coasting").toBool(), false);
        QVERIFY2(qAbs(flickItem->property("contentY").toDouble() - (beforeMom + 25.0)) <= 1.0,
                 qPrintable(QString::number(flickItem->property("contentY").toDouble())));
        QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollEnd), true));
        QCOMPARE(wheelItem->property("coasting").toBool(), false);

        // Reduced motion disables kinetic coasting.
        theme.setReducedMotion(true);
        QVERIFY(invokeWheel(0.0, -16.0, int(Qt::ScrollBegin), true));
        for (int i = 0; i < 4; ++i) {
            QTest::qWait(10);
            QVERIFY(invokeWheel(0.0, -16.0, int(Qt::ScrollUpdate), true));
        }
        QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollEnd), true));
        QCOMPARE(wheelItem->property("coasting").toBool(), false);
        rootItem->setParentItem(nullptr);

        // 3. PdfSurface.qml attaches SmoothWheel to both continuous scroll pageList and zoomed pan view.
        QFile pdfFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml"));
        QVERIFY(pdfFile.open(QIODevice::ReadOnly));
        const QString pdfQml = QString::fromUtf8(pdfFile.readAll());
        QVERIFY(pdfQml.contains(QStringLiteral("SmoothWheel { stepSize: 165 }")));
        QVERIFY(pdfQml.contains(QStringLiteral("enabled: surface.canPanY")));
    }

    // === fix-build-qa tests ===
    void mprisMetadataAndForcedAsserts() {
        // Verify Q_ASSERT is never silently compiled out in test builds.
#if defined(QT_NO_DEBUG) && !defined(QT_FORCE_ASSERTS)
#error "Q_ASSERT must remain enabled in clothbound_tests builds"
#endif
        int assertProbe = 0;
        Q_ASSERT((++assertProbe) == 1);
        QCOMPARE(assertProbe, 1);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString epubPath = dir.filePath(QStringLiteral("mpris_book.epub"));
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:title>MPRIS Test Book</dc:title>"
            "<dc:creator>Ada Lovelace</dc:creator>"
            "<dc:language>en</dc:language></metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/></spine>\n"
            "</package>";
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub({
            {"OEBPS/content.opf", opf},
            {"OEBPS/c1.xhtml", TestBooks::chapter("Chapter One", "<p>Hello MPRIS.</p>")},
        })));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        keys.save(QStringLiteral("gemini"), QStringLiteral("test-key"));
        AiServices services(&gemini, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);

        Mpris mpris(&narration, &reader, &library);
        auto *root = mpris.findChild<MprisRoot *>();
        auto *player = mpris.findChild<MprisPlayer *>();
        QVERIFY(root != nullptr);
        QVERIFY(player != nullptr);

        // Root adaptor basics and Raise signal.
        QCOMPARE(root->identity(), QStringLiteral("Clothbound"));
        QCOMPARE(root->desktopEntry(), QStringLiteral("clothbound"));
        QCOMPARE(root->yes(), true);
        QCOMPARE(root->no(), false);
        QSignalSpy raised(&mpris, &Mpris::raiseRequested);
        root->Raise();
        QCOMPARE(raised.count(), 1);

        // Inactive narration reports Stopped and empty metadata.
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Stopped"));
        QCOMPARE(player->playbackStatus(), QStringLiteral("Stopped"));
        QVERIFY(mpris.metadata().isEmpty());
        QCOMPARE(player->minimumRate(), 0.75);
        QCOMPARE(player->maximumRate(), 2.0);

        // Active narration exposes book/chapter metadata and honors player controls.
        narration.setVoice(QStringLiteral("sulafat"), QStringLiteral("Sulafat"));
        narration.start(0, 0);
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Playing"));
        const QVariantMap meta = mpris.metadata();
        QCOMPARE(meta.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path(),
                 QStringLiteral("/app/clothbound/book%1/chapter0").arg(bookId));
        QCOMPARE(meta.value(QStringLiteral("xesam:album")).toString(), QStringLiteral("MPRIS Test Book"));
        QVERIFY(!meta.value(QStringLiteral("xesam:title")).toString().isEmpty());
        QCOMPARE(meta.value(QStringLiteral("xesam:artist")).toStringList(),
                 QStringList{QStringLiteral("Ada Lovelace")});

        player->setRate(1.25);
        QVERIFY(qFuzzyCompare(player->rate(), 1.25));
        player->setVolume(0.6);
        QVERIFY(qFuzzyCompare(player->volume(), 0.6));

        // Per MPRIS2 spec, Rate <= 0 pauses playback.
        player->setRate(0.0);
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Paused"));
        player->Play();
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Playing"));
        player->Pause();
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Paused"));
        player->Stop();
        QCOMPARE(mpris.playbackStatus(), QStringLiteral("Stopped"));
        QVERIFY(mpris.metadata().isEmpty());
    }
    // === end fix-build-qa tests ===

    // === fix-reader-ui tests ===
    void fixReaderUiReaderJsRecoveryClobberAndPerf() {
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (node.isEmpty())
            QSKIP("node executable not available");

        const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
        QVERIFY(QFileInfo::exists(readerJsPath));

        const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;
let fakeNow = 1000;
let nextTimerId = 1;
const timers = new Map();

function fakeSetTimeout(fn, delay) {
    const id = nextTimerId++;
    timers.set(id, { fn, at: fakeNow + Math.max(0, Number(delay) || 0) });
    return id;
}
function fakeClearTimeout(id) { timers.delete(id); }
function advanceTimers(ms) {
    fakeNow += ms;
    while (true) {
        let earliestId = null, earliest = null;
        for (const [id, entry] of timers.entries()) {
            if (entry.at <= fakeNow && (!earliest || entry.at < earliest.at)) {
                earliestId = id;
                earliest = entry;
            }
        }
        if (!earliest) break;
        timers.delete(earliestId);
        earliest.fn();
    }
}

let autoRaf = true;
let nextRafId = 1;
const rafQueue = new Map();
function fakeRaf(cb) {
    const id = nextRafId++;
    rafQueue.set(id, cb);
    if (autoRaf) {
        queueMicrotask(() => {
            const fn = rafQueue.get(id);
            if (!fn) return;
            rafQueue.delete(id);
            fakeNow += 120;
            fn(fakeNow);
        });
    }
    return id;
}
function fakeCancelRaf(id) { rafQueue.delete(id); }
function stepRaf(deltaMs) {
    fakeNow += deltaMs;
    const batch = [...rafQueue.entries()];
    rafQueue.clear();
    for (const [, fn] of batch) fn(fakeNow);
}

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        willChange: '', colorScheme: '', left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}
function makeClassList() {
    const s = new Set();
    return { add(c) { s.add(c); }, remove(c) { s.delete(c); }, contains(c) { return s.has(c); } };
}

let getImageDataCalls = 0;
let badgeCreations = 0;
const frameModes = new Map(); // url -> 'ok' | 'error' | 'hang' | 'nullDoc' | 'clobber' | 'image' | 'multipage'
const createdDocs = new Map();

function makeDoc(ownerFrame, url, mode) {
    const listeners = new Map();
    const headChildren = [];
    const byId = new Map();
    if (mode === 'clobber') {
        byId.set('cb-typography', { localName: 'div', id: 'cb-typography', textContent: 'PRESERVE_TYPO' });
        byId.set('cb-layout', { localName: 'p', id: 'cb-layout', textContent: 'PRESERVE_LAYOUT' });
    }
    const textNode = { nodeType: 3, data: 'Clothbound chapter text for testing recovery and perf.', parentElement: null };
    const bodyEl = {
        childNodes: [textNode],
        lastElementChild: {
            getClientRects() { return [{ left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }]; },
            getBoundingClientRect() { return { left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }; },
        },
    };
    textNode.parentElement = bodyEl;
    const pages = mode === 'multipage' ? 2 : 1;
    const docEl = {
        style: makeStyle(),
        get scrollWidth() { return pages * (parseFloat(ownerFrame.style.width) || 680); },
        scrollHeight: 692,
        scrollLeft: 0,
        scrollTop: 0,
        scrollTo(opts) {
            if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
            if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
            for (const fn of (listeners.get('scroll') || [])) fn();
        },
        appendChild() {},
    };
    const imgAttrs = new Map();
    const mockImg = {
        complete: true, naturalWidth: 16, naturalHeight: 16,
        src: 'clothbound://book/1/fig.png', currentSrc: 'clothbound://book/1/fig.png',
        hasAttribute(k) { return imgAttrs.has(k); },
        setAttribute(k, v) { imgAttrs.set(k, v); },
        getAttribute(k) { return imgAttrs.get(k); },
    };
    const doc = {
        URL: url,
        _headChildren: headChildren,
        _byId: byId,
        _mockImg: mockImg,
        documentElement: docEl,
        head: {
            appendChild(el) {
                headChildren.push(el);
                if (el.id && !byId.has(el.id)) byId.set(el.id, el);
            },
        },
        body: bodyEl,
        scrollingElement: docEl,
        fonts: { ready: Promise.resolve() },
        images: mode === 'image' ? [mockImg] : [],
        defaultView: {
            getComputedStyle() { return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start', whiteSpace: 'normal' }; },
            CSS: { highlights: new Map() },
            Highlight: class { constructor(...r) { this.ranges = r; this.priority = 0; } },
        },
        getElementById(id) { return byId.get(id) || null; },
        createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
        querySelector() { return null; },
        querySelectorAll() { return []; },
        addEventListener(type, fn) {
            if (!listeners.has(type)) listeners.set(type, []);
            listeners.get(type).push(fn);
        },
        removeEventListener() {},
        createTreeWalker() {
            let done = false;
            return { nextNode() { if (done) return null; done = true; return textNode; } };
        },
        createRange() {
            return {
                selectNodeContents() {},
                setStart() {},
                setEnd() {},
                getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
                getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
            };
        },
        getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
    };
    createdDocs.set(url, doc);
    return doc;
}

function makeFrame(id) {
    const f = {
        id,
        style: makeStyle(),
        classList: makeClassList(),
        contentDocument: null,
        contentWindow: null,
        onload: null,
        onerror: null,
        _src: 'about:blank',
        setAttribute() {},
        removeAttribute() {},
        getBoundingClientRect() { return { left: 160, top: 54, width: parseFloat(this.style.width) || 680, height: 692 }; },
    };
    Object.defineProperty(f, 'src', {
        get() { return this._src; },
        set(v) {
            this._src = v;
            if (!v || v === 'about:blank') {
                this.contentDocument = null;
                this.contentWindow = null;
                return;
            }
            const mode = frameModes.get(v) || 'ok';
            if (mode === 'hang') {
                this.contentDocument = null;
                this.contentWindow = null;
                return;
            }
            if (mode === 'error') {
                this.contentDocument = null;
                this.contentWindow = null;
                const cb = this.onerror;
                if (typeof cb === 'function') queueMicrotask(() => cb());
                return;
            }
            if (mode === 'nullDoc') {
                this.contentDocument = null;
                this.contentWindow = null;
                const cb = this.onload;
                if (typeof cb === 'function') queueMicrotask(() => cb());
                return;
            }
            const doc = makeDoc(this, v, mode);
            this.contentDocument = doc;
            this.contentWindow = doc.defaultView;
            const cb = this.onload;
            if (typeof cb === 'function') queueMicrotask(() => cb());
        },
    });
    return f;
}

const stageEl = {
    clientWidth: 1200, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 1200, height: 800 }; },
};
const viewportEl = { style: makeStyle(), classList: makeClassList() };
const bookFrame = makeFrame('book');
const backFrame = makeFrame('book-back');
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'viewport') return viewportEl;
        if (id === 'book') return bookFrame;
        if (id === 'book-back') return backFrame;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement(tag) {
        if (tag === 'canvas') {
            return {
                width: 32, height: 32,
                getContext() {
                    return {
                        clearRect() {},
                        drawImage() {},
                        getImageData(_x, _y, w, h) {
                            ++getImageDataCalls;
                            const data = new Uint8ClampedArray(w * h * 4);
                            // Half transparent, half dark monochrome pixels -> data-cb-lineart.
                            for (let i = 0; i < w * h; ++i) {
                                data[i * 4 + 0] = 20;
                                data[i * 4 + 1] = 20;
                                data[i * 4 + 2] = 20;
                                data[i * 4 + 3] = (i % 2 === 0) ? 0 : 255;
                            }
                            return { data };
                        },
                    };
                },
            };
        }
        if (tag === 'button') {
            ++badgeCreations;
            return {
                tagName: 'BUTTON', type: '', className: '', title: '', innerHTML: '',
                style: makeStyle(),
                setAttribute() {}, addEventListener() {}, remove() {},
            };
        }
        return { tagName: tag.toUpperCase(), id: '', style: makeStyle(), setAttribute() {}, remove() {} };
    },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

const logs = [];
let lastLoc = null;
const bridge = {
    surfaceLoaded() {},
    relocated(loc) { lastLoc = loc; },
    notifyPageTurn() {},
    log(msg) { logs.push(String(msg)); },
};

const sandbox = {
    console, Promise, URL, Uint8ClampedArray,
    setTimeout: fakeSetTimeout, clearTimeout: fakeClearTimeout,
    performance: { now: () => fakeNow },
    requestAnimationFrame: fakeRaf, cancelAnimationFrame: fakeCancelRaf,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1200, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class { observe() {} },
    matchMedia: () => ({ matches: false }),
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;
const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

const makeStyleCfg = dark => ({
    theme: { bg: dark ? '#1e1d1b' : '#fbfaf6', fg: dark ? '#dcd5c8' : '#23221f', link: '#8a6a2a',
             muted: '#6f6b63', popover: '#fff', border: 'rgba(0,0,0,0.12)', selection: '#cde',
             search: '#ff0', found: '#fa0', markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f',
             markPink: '#f0f', speaking: '#eef', dark },
    font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.0,
    margin: 0.1, measure: 30, justify: false, hyphenate: false, layout: 'single', singleColumn: false,
    animate: true, reducedMotion: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
    listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
});
const makeBook = (session, href) => ({
    session, base: `clothbound://book/${session}/`, language: 'en', rtl: false,
    spine: [{ href, linear: true }], pageTargets: [],
});

(async () => {
    // 1. onerror recovery: state.queue does NOT deadlock; next open() succeeds.
    frameModes.set('clothbound://book/1/bad.xhtml', 'error');
    frameModes.set('clothbound://book/2/good.xhtml', 'ok');
    await clothbound.open(makeBook(1, 'bad.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.ok(logs.some(m => m.includes('Failed to load chapter')), 'onerror should log error');
    await clothbound.open(makeBook(2, 'good.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(lastLoc.session, 2, 'queue recovered after onerror');

    // 2. 10s safety timeout recovery: hanging iframe times out and unblocks state.queue.
    logs.length = 0;
    frameModes.set('clothbound://book/3/hang.xhtml', 'hang');
    frameModes.set('clothbound://book/4/after-timeout.xhtml', 'ok');
    const pHang = clothbound.open(makeBook(3, 'hang.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    await Promise.resolve();
    advanceTimers(10001);
    await pHang;
    assert.ok(logs.some(m => m.includes('Timed out loading chapter')), 'timeout should log error');
    await clothbound.open(makeBook(4, 'after-timeout.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(lastLoc.session, 4, 'queue recovered after timeout');

    // 3. contentDocument null check recovery.
    logs.length = 0;
    frameModes.set('clothbound://book/5/null.xhtml', 'nullDoc');
    frameModes.set('clothbound://book/6/after-null.xhtml', 'ok');
    await clothbound.open(makeBook(5, 'null.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.ok(logs.some(m => m.includes('has no document')), 'null contentDocument should log error');
    await clothbound.open(makeBook(6, 'after-null.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(lastLoc.session, 6, 'queue recovered after null contentDocument');

    // 4. Generation/session cancellation: opening Book B cancels hanging Book A immediately with ZERO error log.
    logs.length = 0;
    frameModes.set('clothbound://book/7/hang-cancel.xhtml', 'hang');
    frameModes.set('clothbound://book/8/fresh.xhtml', 'ok');
    const pCancelled = clothbound.open(makeBook(7, 'hang-cancel.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    await Promise.resolve();
    const pFresh = clothbound.open(makeBook(8, 'fresh.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    await Promise.all([pCancelled, pFresh]);
    assert.strictEqual(lastLoc.session, 8, 'book 8 opened cleanly without waiting for timeout');
    assert.strictEqual(logs.length, 0, 'cancelled superseded load must not log an error');

    // 5. styleElement() DOM clobbering protection: non-<style> elements with cb-typography/cb-layout stay untouched.
    frameModes.set('clothbound://book/9/clobber.xhtml', 'clobber');
    await clothbound.open(makeBook(9, 'clobber.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    const clobDoc = createdDocs.get('clothbound://book/9/clobber.xhtml');
    assert.strictEqual(clobDoc.getElementById('cb-typography').textContent, 'PRESERVE_TYPO');
    assert.strictEqual(clobDoc.getElementById('cb-layout').textContent, 'PRESERVE_LAYOUT');
    const stylesAfterOpen = clobDoc._headChildren.filter(e => e.localName === 'style');
    assert.strictEqual(stylesAfterOpen.length, 2, 'two real <style> elements injected into head');
    await clothbound.setStyle(makeStyleCfg(true));
    assert.strictEqual(clobDoc.getElementById('cb-typography').textContent, 'PRESERVE_TYPO');
    assert.strictEqual(clobDoc._headChildren.filter(e => e.localName === 'style').length, 2, 'style updates reuse elements');

    // 6. classifyImages() skipped in light theme and runs when switching to dark theme.
    getImageDataCalls = 0;
    frameModes.set('clothbound://book/10/img.xhtml', 'image');
    await clothbound.open(makeBook(10, 'img.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(getImageDataCalls, 0, 'light mode must skip synchronous getImageData()');
    await clothbound.setStyle(makeStyleCfg(true));
    assert.strictEqual(getImageDataCalls, 1, 'dark mode switch classifies raster images once');
    const imgDoc = createdDocs.get('clothbound://book/10/img.xhtml');
    assert.strictEqual(imgDoc._mockImg.hasAttribute('data-cb-lineart'), true);

    // 7. layoutMarginNotes() paused during turnTo() RAF animation, runs once on settle.
    frameModes.set('clothbound://book/11/notes.xhtml', 'multipage');
    await clothbound.open(makeBook(11, 'notes.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    clothbound.marks([{ id: 1, start: 0, end: 10, color: 'yellow', noted: true, note: 'Margin note' }]);
    await Promise.resolve();
    badgeCreations = 0;
    autoRaf = false;
    const pTurn = clothbound.next();
    await Promise.resolve();
    // Step 3 intermediate RAF frames mid-turn: scroll fires every frame, but 0 badges created mid-turn!
    stepRaf(40);
    stepRaf(40);
    stepRaf(40);
    assert.strictEqual(badgeCreations, 0, 'layoutMarginNotes must pause during turnTo RAF animation');
    // Finish turn and flush the settled scheduleMarginNotes RAF once.
    stepRaf(140);
    await pTurn;
    stepRaf(16);
    assert.strictEqual(badgeCreations, 1, 'layoutMarginNotes runs once after turnTo settles');

    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");

        QProcess proc;
        proc.start(node, {QStringLiteral("-e"), harness, readerJsPath});
        QVERIFY2(proc.waitForFinished(10000), "node fix-reader-ui reader.js harness timed out");
        const QByteArray stderrOut = proc.readAllStandardError();
        QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                 stderrOut.isEmpty() ? "node fix-reader-ui reader.js harness failed" : stderrOut.constData());
    }

    void fixReaderUiQmlSafetyLifecyclePanningAndPerf() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        const QString pdfPath = dir.filePath(QStringLiteral("pan.pdf"));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("<b>Injected Title</b>", "Ada Marsh")));

        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(pdfId > 0);

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(pdfId));
        Marks marks(&library);
        BookSearch bookSearch(&reader);
        LibraryIndex libraryIndex(cacheDir + QStringLiteral("/index.sqlite"));
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices ai(&gemini, &keys, library.database().connectionName());
        AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&ai, &audioCache, &reader, &library);
        narration.m_player->setHeadless(true);
        Companion companion(&ai, &reader, library.database().connectionName());
        Illustrator illustrator(&ai, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("pictures")));
        LocalSetup localSetup(&ai);
        Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
        Translator translator;

        QQmlEngine engine;
        engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
        engine.rootContext()->setContextProperty(QStringLiteral("libraryIndex"), &libraryIndex);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
        engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
        engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
        engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
        engine.rootContext()->setContextProperty(QStringLiteral("companion"), &companion);
        engine.rootContext()->setContextProperty(QStringLiteral("illustrator"), &illustrator);
        engine.rootContext()->setContextProperty(QStringLiteral("localSetup"), &localSetup);
        engine.rootContext()->setContextProperty(QStringLiteral("settingsUrl"),
                                                 QUrl::fromLocalFile(dir.filePath(QStringLiteral("settings.ini"))).toString());

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &errs) { warnings.append(errs); });

        // 1. Safe-by-default Text.PlainText on Label, ToolTip, Toast.
        {
            QQmlComponent styleComp(&engine);
            styleComp.setData(QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "ui/components"
Item {
    property alias lbl: lbl
    property alias tip: tip
    property alias toast: toast
    Label { id: lbl; text: "<img src=x>hello" }
    ToolTip { id: tip; text: "<img src=x>tip" }
    Toast { id: toast }
}
)QML"),
                              QUrl::fromLocalFile(srcRoot + QStringLiteral("/PlainTextCheck.qml")));
            QVERIFY2(styleComp.status() == QQmlComponent::Ready, qPrintable(styleComp.errorString()));
            std::unique_ptr<QObject> obj(styleComp.create());
            QVERIFY(obj != nullptr);
            QObject *lbl = obj->property("lbl").value<QObject *>();
            QObject *tip = obj->property("tip").value<QObject *>();
            QVERIFY(lbl && tip);
            QCOMPARE(lbl->property("textFormat").toInt(), int(Qt::PlainText));
            QObject *tipContent = tip->property("contentItem").value<QObject *>();
            QVERIFY(tipContent != nullptr);
            QCOMPARE(tipContent->property("textFormat").toInt(), int(Qt::PlainText));
        }

        // 2. SmoothWheel coast distance clamped to slope-matched cap (zero 3.67x velocity surge).
        {
            QQmlComponent swComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/components/SmoothWheel.qml")));
            QVERIFY2(swComp.status() == QQmlComponent::Ready, qPrintable(swComp.errorString()));
            std::unique_ptr<QObject> swObj(swComp.create());
            QVERIFY(swObj != nullptr);
            QVariant ret;
            QVERIFY(QMetaObject::invokeMethod(swObj.get(), "computeCoastPlan", Q_RETURN_ARG(QVariant, ret),
                                              Q_ARG(QVariant, 0.0), Q_ARG(QVariant, 9000.0),
                                              Q_ARG(QVariant, 0.0), Q_ARG(QVariant, 50000.0)));
            const QVariantMap maxPlan = ret.toMap();
            const double dist = maxPlan.value(QStringLiteral("distance")).toDouble();
            const double durSec = maxPlan.value(QStringLiteral("duration")).toDouble() / 1000.0;
            QVERIFY(durSec > 0.0);
            const double initVel = (3.0 * dist) / durSec;
            QVERIFY2(initVel <= 9050.0, qPrintable(QStringLiteral("initial velocity surged to %1 px/s").arg(initVel)));
        }

        // 3. CoverImage gates effectiveSource on visible (real QQuickWindow visibility).
        {
            QQmlComponent covComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CoverImage.qml")));
            QVERIFY2(covComp.status() == QQmlComponent::Ready, qPrintable(covComp.errorString()));
            std::unique_ptr<QQuickWindow> covWin(new QQuickWindow);
            covWin->resize(200, 240);
            std::unique_ptr<QObject> covObj(covComp.createWithInitialProperties({
                {QStringLiteral("visible"), false},
                {QStringLiteral("width"), 120.0},
                {QStringLiteral("height"), 180.0},
                {QStringLiteral("source"), QStringLiteral("file:///tmp/nonexistent-cover.png")},
            }));
            QVERIFY(covObj != nullptr);
            auto *covItem = qobject_cast<QQuickItem *>(covObj.get());
            QVERIFY(covItem != nullptr);
            covItem->setParentItem(covWin->contentItem());
            covWin->show();
            QVERIFY(QTest::qWaitForWindowExposed(covWin.get()));
            QCOMPARE(covObj->property("effectiveSource").toString(), QString());
            covItem->setVisible(true);
            QVERIFY(!covObj->property("effectiveSource").toString().isEmpty());
            covItem->setParentItem(nullptr);
        }

        // 4. BookDetails does not flash visible when window widens while closed.
        {
            QQmlComponent detComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/BookDetails.qml")));
            QVERIFY2(detComp.status() == QQmlComponent::Ready, qPrintable(detComp.errorString()));
            std::unique_ptr<QObject> detObj(detComp.createWithInitialProperties({
                {QStringLiteral("width"), 1000.0},
                {QStringLiteral("height"), 760.0},
            }));
            QVERIFY(detObj != nullptr);
            QCOMPARE(detObj->property("visible").toBool(), false);
            detObj->setProperty("width", 1360.0);
            QCoreApplication::processEvents();
            QCOMPARE(detObj->property("visible").toBool(), false);
            QCOMPARE(detObj->property("slideProgress").toDouble(), 0.0);
        }

        // 5. NoteEditor bounds long notes inside Flickable so buttons stay on-screen.
        {
            const int hlId = marks.addHighlight(0, 0, 12, QStringLiteral("Short quote"), QStringLiteral("yellow"), QStringLiteral("P1"));
            QString longNote;
            for (int i = 0; i < 60; ++i)
                longNote += QStringLiteral("Line %1 of a very long margin note that must scroll.\n").arg(i);
            marks.setNote(hlId, longNote);

            QQmlComponent noteHostComp(&engine);
            noteHostComp.setData(QByteArrayLiteral(R"QML(
import QtQuick
import "ui/reader"
Item {
    width: 720; height: 520
    property alias editor: ed
    NoteEditor {
        id: ed
        look: ({ dark: false, bg: "#fff", fg: "#111", muted: "#666", popover: "#fff", line: "#ddd", link: "#00f" })
    }
}
)QML"),
                                 QUrl::fromLocalFile(srcRoot + QStringLiteral("/NoteEditorHost.qml")));
            QVERIFY2(noteHostComp.status() == QQmlComponent::Ready, qPrintable(noteHostComp.errorString()));
            std::unique_ptr<QObject> hostObj(noteHostComp.create());
            QVERIFY(hostObj != nullptr);
            QObject *ed = hostObj->property("editor").value<QObject *>();
            QVERIFY(ed != nullptr);
            QMetaObject::invokeMethod(ed, "show", Q_ARG(QVariant, hlId));
            QCoreApplication::processEvents();
            QVERIFY2(ed->property("height").toDouble() <= 472.5,
                     qPrintable(QString::number(ed->property("height").toDouble())));
        }

        // 6. PdfSurface proportional Ctrl+Wheel zoom & Fit-to-Width canPan.
        {
            const QVariantMap look{
                {QStringLiteral("bg"), QStringLiteral("#FAF6EE")},
                {QStringLiteral("fg"), QStringLiteral("#26221D")},
                {QStringLiteral("muted"), QStringLiteral("#787066")},
                {QStringLiteral("line"), QStringLiteral("#E2DCD0")},
                {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
                {QStringLiteral("link"), QStringLiteral("#8A5A12")},
                {QStringLiteral("dark"), false},
            };
            QQmlComponent surfComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml")));
            QVERIFY2(surfComp.status() == QQmlComponent::Ready, qPrintable(surfComp.errorString()));
            std::unique_ptr<QObject> surfObj(surfComp.createWithInitialProperties({
                {QStringLiteral("width"), 1000.0},
                {QStringLiteral("height"), 760.0},
                {QStringLiteral("look"), look},
            }));
            QVERIFY(surfObj != nullptr);
            QVariantMap styleMap{
                {QStringLiteral("layout"), QStringLiteral("single")},
                {QStringLiteral("pdfCrop"), false},
                {QStringLiteral("pdfOriginalColors"), false},
                {QStringLiteral("pdfFit"), QStringLiteral("width")},
                {QStringLiteral("marginTop"), 60.0},
                {QStringLiteral("marginBottom"), 76.0},
                {QStringLiteral("animate"), false},
            };
            const QVariantMap posMap{{QStringLiteral("chapter"), 0}};
            QMetaObject::invokeMethod(surfObj.get(), "open",
                                      Q_ARG(QVariant, reader.manifest()),
                                      Q_ARG(QVariant, posMap),
                                      Q_ARG(QVariant, styleMap));
            QCoreApplication::processEvents();
            QCOMPARE(surfObj->property("canPanX").toBool(), false);
            QCOMPARE(surfObj->property("canPanY").toBool(), true);
            QCOMPARE(surfObj->property("canPan").toBool(), true);
            const double z0 = surfObj->property("effectiveZoom").toDouble();
            QMetaObject::invokeMethod(surfObj.get(), "zoomByWheel", Q_ARG(QVariant, 15.0));
            const double zSmall = surfObj->property("effectiveZoom").toDouble();
            QVERIFY2(zSmall > z0 && zSmall < z0 * 1.03,
                     qPrintable(QStringLiteral("z0=%1 zSmall=%2").arg(z0).arg(zSmall)));
        }

        // 7. CommandPalette classifies builtin:/studio: offline voices & clears cachedBooks on close().
        {
            QQmlComponent cpComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/CommandPalette.qml")));
            QVERIFY2(cpComp.status() == QQmlComponent::Ready, qPrintable(cpComp.errorString()));
            std::unique_ptr<QObject> cpObj(cpComp.createWithInitialProperties({
                {QStringLiteral("width"), 1000.0},
                {QStringLiteral("height"), 760.0},
            }));
            QVERIFY(cpObj != nullptr);
            QVariant sub;
            narration.setVoice(QStringLiteral("builtin:kokoro:af_heart"), QStringLiteral("Heart"));
            QVERIFY(QMetaObject::invokeMethod(cpObj.get(), "voiceSubtitle", Q_RETURN_ARG(QVariant, sub)));
            QVERIFY2(sub.toString().contains(QStringLiteral("on this computer")), qPrintable(sub.toString()));
            narration.setVoice(QStringLiteral("studio:kokoro"), QStringLiteral("Studio Voice"));
            QVERIFY(QMetaObject::invokeMethod(cpObj.get(), "voiceSubtitle", Q_RETURN_ARG(QVariant, sub)));
            QVERIFY2(sub.toString().contains(QStringLiteral("on this computer")), qPrintable(sub.toString()));
            QMetaObject::invokeMethod(cpObj.get(), "show");
            QVERIFY(!cpObj->property("cachedBooks").toList().isEmpty());
            QMetaObject::invokeMethod(cpObj.get(), "close");
            QVERIFY(cpObj->property("cachedBooks").toList().isEmpty());
        }

        // 8. CompanionPanel instantiates cleanly with virtualized ListView, throttled stream, and zero QML warnings.
        {
            QQmlComponent compComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/companion/CompanionPanel.qml")));
            QVERIFY2(compComp.status() == QQmlComponent::Ready, qPrintable(compComp.errorString()));
            std::unique_ptr<QObject> compObj(compComp.createWithInitialProperties({
                {QStringLiteral("height"), 760.0},
                {QStringLiteral("look"), QVariantMap{{QStringLiteral("dark"), false}}},
            }));
            QVERIFY(compObj != nullptr);
            QCoreApplication::processEvents();
        }

        // 9. Wiring checks for Main.qml defaultSleep, EpubReader.qml teardown/subframes/lifecycle,
        //    VoicePanel.qml lazy Loader, PdfSurface.qml mipmap/canPan, LibraryView.qml dialog MouseArea.
        auto readSrc = [&](const QString &rel) -> QString {
            QFile f(srcRoot + QLatin1Char('/') + rel);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        const QString mainQml = readSrc(QStringLiteral("ui/Main.qml"));
        QVERIFY(mainQml.contains(QStringLiteral("id: listenSettings")));
        QVERIFY(mainQml.contains(QStringLiteral("property string defaultSleep: \"off\"")));
        QVERIFY(mainQml.contains(QStringLiteral("win.applyDefaultSleep()")));

        const QString erQml = readSrc(QStringLiteral("ui/reader/EpubReader.qml"));
        QVERIFY(erQml.contains(QStringLiteral("!request.isMainFrame && (u === \"about:blank\" || u === \"about:srcdoc\")")));
        QVERIFY(erQml.contains(QStringLiteral("(reader.open && !view.pdf)")));
        QVERIFY(erQml.contains(QStringLiteral("closeWebSurface()")));

        const QString pdfQml = readSrc(QStringLiteral("ui/reader/PdfSurface.qml"));
        QVERIFY(pdfQml.contains(QStringLiteral("mipmap: true")));
        QVERIFY(pdfQml.contains(QStringLiteral("panning = surface.canPan &&")));

        const QString vpQml = readSrc(QStringLiteral("ui/audio/VoicePanel.qml"));
        QVERIFY(vpQml.contains(QStringLiteral("id: designLoader")));
        QVERIFY(vpQml.contains(QStringLiteral("active: delWrap.isDesign")));

        const QString cpQml = readSrc(QStringLiteral("ui/companion/CompanionPanel.qml"));
        QVERIFY(cpQml.contains(QStringLiteral("id: talkModel")));
        QVERIFY(cpQml.contains(QStringLiteral("id: streamThrottle")));
        QVERIFY(cpQml.contains(QStringLiteral("onClosed: item = null")));

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
    }
    // === end fix-reader-ui tests ===
    // === fix-core tests ===
    void fixCoreMigrationRewritesPathsAndSafeCopy() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
        const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
        const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
        const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
        const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
        const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

        QVERIFY(QDir().mkpath(legacyCfg));
        QVERIFY(QDir().mkpath(legacyData + QStringLiteral("/articles")));
        QVERIFY(QDir().mkpath(legacyData + QStringLiteral("/downloads")));
        QVERIFY(QDir().mkpath(legacyData + QStringLiteral("/watched")));
        QVERIFY(QDir().mkpath(legacyCache));

        write(legacyCfg + QStringLiteral("/settings.ini"), "[reading]\ntheme=paper\n");
        const QString articleOld = legacyData + QStringLiteral("/articles/saved.epub");
        const QString opdsOld = legacyData + QStringLiteral("/downloads/opds.epub");
        const QString watchedOld = legacyData + QStringLiteral("/watched");
        QVERIFY(TestBooks::write(articleOld, TestBooks::epub2({})));
        QVERIFY(TestBooks::write(opdsOld, TestBooks::epub2({})));

        int articleId = 0;
        int opdsId = 0;
        {
            Library legacyLib(legacyData, legacyCache);
            QVERIFY(legacyLib.open());
            articleId = legacyLib.addBookFile(articleOld);
            opdsId = legacyLib.addBookFile(opdsOld);
            QVERIFY(articleId > 0 && opdsId > 0);
            legacyLib.addFolder(watchedOld);
            QTRY_VERIFY(!legacyLib.scanning());
        }
        const QString legacyIndexPath = legacyCache + QStringLiteral("/index.sqlite");
        {
            LibraryIndex legacyIdx(legacyIndexPath);
            if (legacyIdx.available()) {
                QVERIFY(LibraryIndex::indexBook(legacyIndexPath, {articleId, articleOld, QStringLiteral("epub"), 100}));
                QVERIFY(LibraryIndex::indexBook(legacyIndexPath, {opdsId, opdsOld, QStringLiteral("epub"), 100}));
            }
        }

        const Migration::Result res = Migration::run({legacyCfg, newCfg, legacyData, newData, legacyCache, newCache});
        QVERIFY(res.configMigrated);
        QVERIFY(res.dataMigrated);
        QVERIFY(res.cacheMigrated);

        const QString articleNew = newData + QStringLiteral("/articles/saved.epub");
        const QString opdsNew = newData + QStringLiteral("/downloads/opds.epub");
        const QString watchedNew = newData + QStringLiteral("/watched");
        QVERIFY(QFileInfo::exists(articleNew));
        QVERIFY(QFileInfo::exists(opdsNew));

        {
            Library migratedLib(newData, newCache);
            QVERIFY(migratedLib.open());
            QCOMPARE(migratedLib.record(articleId).path, articleNew);
            QCOMPARE(migratedLib.record(opdsId).path, opdsNew);
            QVERIFY(migratedLib.folders().contains(watchedNew));

            // Rescan must NOT mark migrated articles/OPDS downloads as missing.
            QSignalSpy scanDone(&migratedLib, &Library::scanFinished);
            migratedLib.rescan();
            QTRY_VERIFY_WITH_TIMEOUT(scanDone.count() >= 1, 5000);
            QCOMPARE(migratedLib.record(articleId).missing, false);
            QCOMPARE(migratedLib.record(opdsId).missing, false);
            QCOMPARE(migratedLib.bookCount(), 2);
        }

        // Verify index.sqlite indexed.path rows were rewritten too.
        const QString newIndexPath = newCache + QStringLiteral("/index.sqlite");
        if (QFileInfo::exists(newIndexPath)) {
            const QString conn = QStringLiteral("fix-core-mig-idx-check");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(newIndexPath);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("SELECT book, path FROM indexed ORDER BY book")));
                QHash<int, QString> paths;
                while (q.next())
                    paths.insert(q.value(0).toInt(), q.value(1).toString());
                QCOMPARE(paths.value(articleId), articleNew);
                QCOMPARE(paths.value(opdsId), opdsNew);
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }

        // Verify cross-FS safe copy: preserves relative symlinks, skips FIFOs,
        // and copies WAL-mode SQLite databases cleanly without tearing.
        const QString copySrc = dir.filePath(QStringLiteral("copy-src"));
        const QString copyDst = dir.filePath(QStringLiteral("copy-dst"));
        QVERIFY(QDir().mkpath(copySrc));
        write(copySrc + QStringLiteral("/target.txt"), "hello-link");
        QVERIFY(QFile::link(QStringLiteral("target.txt"), copySrc + QStringLiteral("/rel.link")));
        QProcess::execute(QStringLiteral("mkfifo"), {copySrc + QStringLiteral("/pipe.fifo")});

        const QString walDb = copySrc + QStringLiteral("/sample.sqlite");
        {
            const QString conn = QStringLiteral("fix-core-wal-src");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(walDb);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode = WAL")));
                QVERIFY(q.exec(QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY, val TEXT)")));
                QVERIFY(q.exec(QStringLiteral("INSERT INTO items VALUES (1, 'from-wal')")));
                // Keep DB open during copy to exercise live WAL checkpoint/VACUUM INTO.
                QVERIFY(Migration::copyDirectorySafely(copySrc, copyDst));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }
        // Delete source tree and confirm relative symlink in destination still resolves.
        QVERIFY(QDir(copySrc).removeRecursively());
        QFile copiedLink(copyDst + QStringLiteral("/rel.link"));
        QVERIFY(copiedLink.open(QIODevice::ReadOnly));
        QCOMPARE(copiedLink.readAll(), QByteArray("hello-link"));
        QVERIFY(!QFileInfo::exists(copyDst + QStringLiteral("/pipe.fifo")));
        QVERIFY(!QFileInfo::exists(copyDst + QStringLiteral("/sample.sqlite-wal")));
        {
            const QString conn = QStringLiteral("fix-core-wal-dst");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(copyDst + QStringLiteral("/sample.sqlite"));
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("SELECT val FROM items WHERE id = 1")));
                QVERIFY(q.next());
                QCOMPARE(q.value(0).toString(), QStringLiteral("from-wal"));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }
    }

    void fixCoreZipArchiveHardeningLimitsAndOverlap() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // 1. Overlapping ZIP bomb: two central-directory entries aliasing the same local offset.
        {
            const QByteArray payload = TestBooks::rawDeflate("repeated payload");
            const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>("repeated payload"), 16));
            const QByteArray nameA("a.txt"), nameB("b.txt");
            QByteArray out, cd;
            TestBooks::put32(out, 0x04034b50);
            TestBooks::put16(out, 20);
            TestBooks::put16(out, 0x0800);
            TestBooks::put16(out, 8);
            TestBooks::put32(out, 0);
            TestBooks::put32(out, crc);
            TestBooks::put32(out, quint32(payload.size()));
            TestBooks::put32(out, 16);
            TestBooks::put16(out, quint16(nameA.size()));
            TestBooks::put16(out, 0);
            out.append(nameA);
            out.append(payload);

            const quint32 cdOff = quint32(out.size());
            auto addCd = [&](const QByteArray &name) {
                TestBooks::put32(cd, 0x02014b50);
                TestBooks::put16(cd, 20);
                TestBooks::put16(cd, 20);
                TestBooks::put16(cd, 0x0800);
                TestBooks::put16(cd, 8);
                TestBooks::put32(cd, 0);
                TestBooks::put32(cd, crc);
                TestBooks::put32(cd, quint32(payload.size()));
                TestBooks::put32(cd, 16);
                TestBooks::put16(cd, quint16(name.size()));
                TestBooks::put16(cd, 0);
                TestBooks::put16(cd, 0);
                TestBooks::put16(cd, 0);
                TestBooks::put16(cd, 0);
                TestBooks::put32(cd, 0);
                TestBooks::put32(cd, 0);  // Both alias offset 0!
                cd.append(name);
            };
            addCd(nameA);
            addCd(nameB);
            out.append(cd);
            TestBooks::put32(out, 0x06054b50);
            TestBooks::put16(out, 0);
            TestBooks::put16(out, 0);
            TestBooks::put16(out, 2);
            TestBooks::put16(out, 2);
            TestBooks::put32(out, quint32(cd.size()));
            TestBooks::put32(out, cdOff);
            TestBooks::put16(out, 0);

            const QString overlapPath = dir.filePath(QStringLiteral("overlap.zip"));
            QVERIFY(TestBooks::write(overlapPath, out));
            ZipArchive zip(overlapPath);
            QVERIFY(!zip.open());
            QVERIFY2(zip.errorString().contains(QStringLiteral("overlapping")), qPrintable(zip.errorString()));
        }

        // 2. Entry-count cap via ZIP64 EOCD declaring > kMaxEntryCount entries.
        {
            QByteArray z64 = TestBooks::zip({{"one.txt", "first", true}}, true);
            const QByteArray sig("\x50\x4b\x06\x06", 4);
            const qsizetype pos = z64.indexOf(sig);
            QVERIFY(pos > 0 && pos + 56 <= z64.size());
            const quint64 tooMany = ZipArchive::kMaxEntryCount + 1;
            for (int b = 0; b < 8; ++b)
                z64[pos + 32 + b] = char((tooMany >> (b * 8)) & 0xff);
            const QString manyPath = dir.filePath(QStringLiteral("many.zip"));
            QVERIFY(TestBooks::write(manyPath, z64));
            ZipArchive zip(manyPath);
            QVERIFY(!zip.open());
            QVERIFY2(zip.errorString().contains(QStringLiteral("too many")), qPrintable(zip.errorString()));
        }

        // 3. Cumulative per-archive inflation budget.
        {
            const QByteArray chunk(64 * 1024, 'A');
            const QString budgetPath = dir.filePath(QStringLiteral("budget.zip"));
            QVERIFY(TestBooks::write(budgetPath, TestBooks::zip({{"one.txt", chunk, true}, {"two.txt", chunk, true}})));
            ZipArchive zip(budgetPath);
            zip.setMaxArchiveInflateBytes(96 * 1024);
            QVERIFY(zip.open());
            QCOMPARE(zip.read(QStringLiteral("one.txt")), chunk);
            QString err;
            QVERIFY(zip.read(QStringLiteral("two.txt"), &err).isEmpty());
            QVERIFY2(err.contains(QStringLiteral("too large")), qPrintable(err));
        }
    }

    void fixCoreLibraryFutureSchemaAndTargetedSavePosition() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        auto makeNamedEpub = [](const QByteArray &title, const QByteArray &body) {
            return TestBooks::epub({
                {"OEBPS/content.opf",
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">"
                 "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title + "</dc:title></metadata>"
                 "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                 "<spine><itemref idref=\"c\"/></spine></package>"},
                {"OEBPS/c.xhtml", TestBooks::chapter(title, body)},
            });
        };
        const QString bookA = dir.filePath(QStringLiteral("book-a.epub"));
        const QString bookB = dir.filePath(QStringLiteral("book-b.epub"));
        QVERIFY(TestBooks::write(bookA, makeNamedEpub("Alpha Book", "<p>First distinct story.</p>")));
        QVERIFY(TestBooks::write(bookB, makeNamedEpub("Beta Book", "<p>Second distinct story.</p>")));

        // 1. savePosition emits targeted bookUpdated/bookChanged and updates
        //    WorksModel in place without booksChanged/readingStatsChanged/rebuild storms.
        {
            Library library(dataDir, cacheDir);
            QVERIFY(library.open());
            const int idA = library.addBookFile(bookA);
            const int idB = library.addBookFile(bookB);
            QVERIFY(idA > 0 && idB > 0);
            library.markOpened(idA);
            library.markOpened(idB);
            QTest::qWait(100);

            // Activate a status filter ("reading") that previously forced O(N log N)
            // rebuild() on every single savePosition() call.
            library.works()->setStatusFilter(QStringLiteral("reading"));
            QCOMPARE(library.works()->count(), 2);

            QSignalSpy booksSpy(&library, &Library::booksChanged);
            QSignalSpy statsSpy(&library, &Library::readingStatsChanged);
            QSignalSpy colsSpy(&library, &Library::collectionsChanged);
            QSignalSpy updatedSpy(&library, &Library::bookUpdated);
            QSignalSpy bookSpy(&library, &Library::bookChanged);
            QSignalSpy resetSpy(library.works(), &QAbstractItemModel::modelReset);
            QSignalSpy dataSpy(library.works(), &QAbstractItemModel::dataChanged);

            library.savePosition(idA, QStringLiteral(R"({"chapter":1,"offset":42})"), 0.35);
            QTest::qWait(100);

            QCOMPARE(booksSpy.count(), 0);
            QCOMPARE(statsSpy.count(), 0);
            QCOMPARE(colsSpy.count(), 0);
            QCOMPARE(updatedSpy.count(), 1);
            QCOMPARE(updatedSpy.first().first().toInt(), idA);
            QCOMPARE(bookSpy.count(), 1);
            QCOMPARE(resetSpy.count(), 0);
            QCOMPARE(dataSpy.count(), 1);
            const QModelIndex topLeft = dataSpy.first().at(0).toModelIndex();
            const QModelIndex bottomRight = dataSpy.first().at(1).toModelIndex();
            QCOMPARE(topLeft.row(), bottomRight.row());
            QCOMPARE(library.works()->data(topLeft, WorksModel::BookIdRole).toInt(), idA);

            // Recording actual reading time DOES emit readingStatsChanged().
            library.addReadingTime(idA, 60, 1200);
            QCOMPARE(statsSpy.count(), 1);

            // 2. flushPending() isolates failures per book via nested savepoints:
            //    rejecting one book via a DB trigger must NOT discard its sibling.
            const QString scanFolder = dir.filePath(QStringLiteral("scan-batch"));
            QVERIFY(QDir().mkpath(scanFolder));
            const QString goodBook = scanFolder + QStringLiteral("/good.epub");
            const QString badBook = scanFolder + QStringLiteral("/poison.epub");
            QVERIFY(TestBooks::write(goodBook, makeNamedEpub("Good Sibling", "<p>Good content.</p>")));
            QVERIFY(TestBooks::write(badBook, makeNamedEpub("Poison Sibling", "<p>Blocked by trigger.</p>")));
            {
                QSqlQuery trig(library.database());
                QVERIFY(trig.exec(QStringLiteral(
                    "CREATE TRIGGER block_poison BEFORE INSERT ON books WHEN NEW.path LIKE '%poison.epub' "
                    "BEGIN SELECT RAISE(ABORT, 'simulated failure'); END")));
            }
            QSignalSpy scanDone(&library, &Library::scanFinished);
            library.addFolder(scanFolder);
            QTRY_VERIFY_WITH_TIMEOUT(scanDone.count() >= 1, 5000);
            QVERIFY(library.idForPath(goodBook) > 0);
            QCOMPARE(library.idForPath(badBook), 0);
            {
                QSqlQuery dropTrig(library.database());
                QVERIFY(dropTrig.exec(QStringLiteral("DROP TRIGGER block_poison")));
            }
        }

        // 3. Future schema version (> kLatestVersion) must fail closed on open().
        {
            const QString conn = QStringLiteral("fix-core-future-schema");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(dataDir + QStringLiteral("/library.sqlite"));
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 999")));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);

            Library futureLib(dataDir, cacheDir);
            QVERIFY(!futureLib.open());
            QVERIFY2(futureLib.errorString().contains(QStringLiteral("999")), qPrintable(futureLib.errorString()));
        }
    }

    void fixCoreLibraryIndexRebuildCancelAndIndexedPassage() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString idxPath = dir.filePath(QStringLiteral("index.sqlite"));
        const QString epub1 = dir.filePath(QStringLiteral("one.epub"));
        const QString epub2 = dir.filePath(QStringLiteral("two.epub"));
        auto makeSearchEpub = [](const QByteArray &title, const QByteArray &body) {
            return TestBooks::epub({
                {"OEBPS/content.opf",
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">"
                 "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title + "</dc:title></metadata>"
                 "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                 "<spine><itemref idref=\"c\"/></spine></package>"},
                {"OEBPS/c.xhtml", TestBooks::chapter(title, body)},
            });
        };
        QVERIFY(TestBooks::write(epub1, makeSearchEpub("One", "<p>The amber lantern glowed across the harbor.</p>")));
        QVERIFY(TestBooks::write(epub2, makeSearchEpub("Two", "<p>A second amber lantern guided the ship home.</p>")));

        // Pre-cancelled indexBook returns false immediately without writing rows.
        std::atomic<bool> preCancel{true};
        QVERIFY(!LibraryIndex::indexBook(idxPath, {1, epub1, QStringLiteral("epub"), 10}, &preCancel));

        LibraryIndex index(idxPath);
        if (!index.available())
            QSKIP("SQLite without FTS5");
        QCOMPARE(index.bookCount(), 0);

        const QList<LibraryIndex::Source> sources{
            {1, epub1, QStringLiteral("epub"), 10},
            {2, epub2, QStringLiteral("epub"), 20},
        };
        index.update(sources);
        // Trigger back-to-back rebuilds while indexing is active; per-run cancel
        // tokens + 1-thread pool must prevent dual chains or stale rows.
        index.rebuild();
        index.rebuild();
        QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 10000);
        QCOMPARE(index.bookCount(), 2);
        QCOMPARE(index.search(QStringLiteral("lantern")).size(), 2);
        QVERIFY(index.passage(1, 0, 0, 30).contains(QStringLiteral("lantern"), Qt::CaseInsensitive));

        // Confirm chapter_rows mapping exists for O(log N) lookups and deletes.
        const QString conn = QStringLiteral("fix-core-chapter-rows-check");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(idxPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT count(*) FROM chapter_rows")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 2);
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        // Removing all books via batched update() clears both indexed and chapter_rows.
        index.update({});
        QCOMPARE(index.bookCount(), 0);
        QVERIFY(index.passage(1, 0, 0, 30).isEmpty());
    }

    void fixCorePdfPagesReleaseAndSystemCheckNullGuard() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("release.pdf"));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Release Test", "Author")));
        auto book = std::make_shared<PdfBook>(pdfPath);
        QVERIFY(book->open());

        const QString tok = QStringLiteral("fix-core-pdf-release");
        PdfPages::add(tok, book);
        {
            PdfPages pages;
            QCOMPARE(pages.m_pool.maxThreadCount(), 1);
            QCOMPARE(pages.m_pool.expiryTimeout(), -1);

            std::unique_ptr<QQuickImageResponse> resp(
                pages.requestImageResponse(tok + QStringLiteral("/0?bg=%23ffffff&fg=%23000000"), QSize(180, 240)));
            QSignalSpy finishedSpy(resp.get(), &QQuickImageResponse::finished);
            QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 8000);
            QVERIFY(pages.m_worker->book != nullptr);
            QCOMPARE(pages.m_worker->token, tok);

            // remove() must asynchronously release worker's QPdfDocument on its affine thread.
            PdfPages::remove(tok);
            QVERIFY(pages.m_pool.waitForDone(5000));
            QVERIFY(pages.m_worker->book == nullptr);
            QVERIFY(pages.m_worker->token.isEmpty());
        }

        // SystemCheck must handle null m_ai cleanly without dereferencing.
        SystemCheck nullAiCheck(nullptr, nullptr);
        const QVariantList verdicts = nullAiCheck.judge(0.0, 0.0, 0.0);
        QVERIFY(!verdicts.isEmpty());
        nullAiCheck.run();
        QTRY_VERIFY_WITH_TIMEOUT(!nullAiCheck.running(), 5000);
        QVERIFY(!nullAiCheck.recommendation().isEmpty());

        // Instance lock file enforces single-instance exclusion and releases on destruction.
        {
            Instance first;
            QVERIFY(first.claim());
            Instance second;
            QVERIFY(!second.claim());
        }
        {
            Instance afterRelease;
            QVERIFY(afterRelease.claim());
        }
    }
    // === end fix-core tests ===
    // === fix-audio tests ===
#ifndef Q_MOC_RUN
private:
    struct FixAudioCastReply : public Ai::Reply {
        explicit FixAudioCastReply(QObject *parent = nullptr) : Ai::Reply(parent) {}
        bool aborted = false;
        void abort() override {
            aborted = true;
            finish({Ai::Error::Cancelled, QStringLiteral("Cancelled.")});
        }
        void succeed(const QString &json) {
            m_text = json;
            finish();
        }
    };
    struct FixAudioCastProvider : public Ai::Provider, public Ai::TextModel {
        explicit FixAudioCastProvider(QObject *parent = nullptr) : Ai::Provider(parent) {}
        QList<QPointer<FixAudioCastReply>> replies;
        QString id() const override { return QStringLiteral("manual"); }
        QString name() const override { return QStringLiteral("Manual"); }
        QString privacyNote() const override { return {}; }
        bool isLocal() const override { return true; }
        bool needsKey() const override { return false; }
        void setKey(const QString &) override {}
        bool hasKey() const override { return true; }
        Ai::Reply *check() override { return new FixAudioCastReply(this); }
        Ai::TextModel *text() override { return this; }
        Ai::Reply *generate(const Ai::TextRequest &) override {
            auto *r = new FixAudioCastReply(this);
            replies.append(r);
            return r;
        }
        QString textModelName(bool) const override { return QStringLiteral("manual-fast"); }
    };
#endif

private slots:
    void fixAudioScopedCastCancelDoesNotDeadlockSecondCaller() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath(QStringLiteral("cast-scope.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        FixAudioCastProvider provider;
        MemoryKeyStore keys;
        AiServices services(&provider, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        VoiceCatalog catalog(&services, &cache, &reader, library.database().connectionName(),
                             dir.filePath(QStringLiteral("voices.json")));
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);

        Cast cast(&services, &cache, &reader, &library, &catalog, &packs);
        cast.setNarrator(QStringLiteral("builtin:kokoro:bm_george"));
        cast.setEnabled(true);
        QVERIFY(cast.available());

        // 1. Cancelling caller A's scope settles A's callback without aborting or deadlocking caller B.
        QObject scopeA;
        QObject scopeB;
        const QList<SpeechBlock> blocksA{block(0, QStringLiteral("\"Hello there,\" said Alice."))};
        const QList<SpeechBlock> blocksB{block(0, QStringLiteral("\"Good morning,\" replied Bob."))};

        bool settledA = false;
        QMap<int, Cast::Line> resultA;
        bool settledB = false;
        QMap<int, Cast::Line> resultB;

        cast.ask(0, blocksA, &scopeA, [&](const QMap<int, Cast::Line> &lines) {
            settledA = true;
            resultA = lines;
        });
        cast.ask(1, blocksB, &scopeB, [&](const QMap<int, Cast::Line> &lines) {
            settledB = true;
            resultB = lines;
        });
        QCOMPARE(provider.replies.size(), 2);
        QPointer<FixAudioCastReply> replyA = provider.replies[0];
        QPointer<FixAudioCastReply> replyB = provider.replies[1];
        QVERIFY(replyA && replyB);

        cast.cancel(&scopeA);
        QVERIFY(replyA->aborted);
        QVERIFY(settledA);
        QVERIFY(resultA.isEmpty());
        QVERIFY(!replyB->aborted);
        QVERIFY(!settledB);

        replyB->succeed(QStringLiteral("{\"lines\":[{\"block\":0,\"speaker\":\"Bob\",\"gender\":\"male\"}]}"));
        QVERIFY(settledB);
        QCOMPARE(resultB.value(0).speaker, QStringLiteral("Bob"));

        // 2. Narration::stop() does not orphan or deadlock an active Download's cast request.
        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.setCast(&cast);

        Download download(&services, &cache, &reader, &narration);
        download.setCast(&cast);
        download.start(0, 0);
        const qsizetype beforeReplies = provider.replies.size();
        download.addBlocks(
            0, QStringLiteral("[{\"start\":0,\"end\":28,\"text\":\"\\\"Watch the tide,\\\" said Mara.\",\"kind\":\"text\"}]"));
        QCOMPARE(provider.replies.size(), beforeReplies + 1);
        QPointer<FixAudioCastReply> dlReply = provider.replies.last();
        QVERIFY(dlReply && !dlReply->aborted);

        narration.stop();
        QVERIFY(!dlReply->aborted);
        dlReply->succeed(QStringLiteral("{\"lines\":[{\"block\":0,\"speaker\":\"Mara\",\"gender\":\"female\"}]}"));
        QTRY_VERIFY_WITH_TIMEOUT(download.finished(), 10000);
        QVERIFY2(download.problem().isEmpty(), qPrintable(download.problem()));
    }

    void fixAudioNarrationHoldSamplesReleasesOnShortClipsAndSleepTrim() {
        QTemporaryDir dir;
        const QByteArray twoChOpf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>Short Story</dc:title>\n"
            "    <dc:creator>Test Author</dc:creator>\n"
            "    <dc:language>en-GB</dc:language>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
            "</package>";
        const QString epubPath = dir.filePath(QStringLiteral("short.epub"));
        QVERIFY(TestBooks::write(
            epubPath,
            TestBooks::epub({
                {"OEBPS/content.opf", twoChOpf},
                {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Chapter one sentence.</p>")},
                {"OEBPS/c2.xhtml", TestBooks::chapter("Ch 2", "<p>Final short line.</p>")},
            })));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());

        // 1. Slow-CPU head-start (>= 2.0s hold) must release on a final chapter shorter than 2.0s.
        const auto model = packs.model(QStringLiteral("builtin:kokoro:bm_george"));
        QVERIFY(model.has_value());
        local.noteSpeed(model->key, 0.5);

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.start(1, 0);  // final chapter (chapter 1 of 2)
        narration.addBlocks(1, R"([{"start":0,"end":17,"text":"Final short line.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() &&
                                     narration.m_clips[0].state == Narration::Clip::Complete,
                                 5000);
        // Clip is ~0.82s (< 2.0s head-start floor), yet head-start must release so playback never deadlocks.
        QVERIFY(narration.m_clips[0].samples < 24000 * 2);
        QCOMPARE(narration.m_holdSamples, qint64(0));
        QVERIFY(!narration.preparing());
        QCOMPARE(narration.preparedPercent(), 100);
        QVERIFY(narration.m_player->isPlaying());
        narration.stop();

        // 2. sleepAtEndOfChapter() trims already-queued next-chapter clips and positions resume at Ch 2 start.
        local.noteSpeed(model->key, 5.0);
        local.noteSpeed(model->key, 5.0);
        local.noteSpeed(model->key, 5.0);
        narration.start(0, 0);
        narration.addBlocks(0, R"([{"start":0,"end":21,"text":"Chapter one sentence.","kind":"text"}])");
        narration.addBlocks(1, R"([{"start":0,"end":17,"text":"Final short line.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(narration.m_clips.size() == 2 &&
                                     narration.m_clips[0].state == Narration::Clip::Complete &&
                                     narration.m_clips[1].state == Narration::Clip::Complete &&
                                     narration.m_clips[0].queued && narration.m_clips[1].queued,
                                 5000);
        QVERIFY(narration.m_player->hasTrack(narration.m_clips[1].id));

        narration.sleepAtEndOfChapter();
        QVERIFY(narration.m_clips[0].queued && narration.m_player->hasTrack(narration.m_clips[0].id));
        QVERIFY(!narration.m_clips[1].queued && !narration.m_player->hasTrack(narration.m_clips[1].id));

        emit narration.m_player->drained();
        QVERIFY(!narration.playing());
        QVERIFY(!narration.sleepAtChapterEnd());
        QCOMPARE(narration.m_current, 1);
        QCOMPARE(narration.m_currentSample, qint64(0));

        narration.resume();
        QVERIFY(narration.playing());
        QVERIFY(narration.m_clips[1].queued && narration.m_player->hasTrack(narration.m_clips[1].id));
        narration.stop();
    }

    void fixAudioNarrationQueueBlockedDuringResponseAndPauseSleepClear() {
        QTemporaryDir dir;
        const QString epubPath = dir.filePath(QStringLiteral("interleave.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(bookId));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));
        narration.start(0, 0);
        narration.addBlocks(0, R"([{"start":0,"end":28,"text":"Book passage before speech.","kind":"text"}])");
        QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() &&
                                     narration.m_clips[0].state == Narration::Clip::Complete,
                                 5000);
        const int bookClipId = narration.m_clips[0].id;

        // Start Companion response speech: book audio must not inject into m_player.
        narration.speakResponse(QStringLiteral("Companion answer spoken cleanly."), QStringLiteral("resp-1"));
        QVERIFY(narration.speakingResponse());
        QVERIFY(narration.m_bookPlayerDirty);
        QVERIFY(!narration.playing());
        QVERIFY(!narration.m_player->hasTrack(bookClipId));

        narration.m_clips[0].queued = false;
        narration.m_queued = -1;
        narration.queue();
        narration.pump();
        QVERIFY(!narration.m_clips[0].queued);
        QVERIFY(!narration.m_player->hasTrack(bookClipId));

        // MPRIS Pause() path (Narration::pause()) must stop active Companion response speech.
        narration.pause();
        QVERIFY(!narration.speakingResponse());
        QVERIFY(narration.speakingResponseId().isEmpty());

        // Sleep timer expiry (finishSleep()) must also stop active Companion response speech.
        narration.speakResponse(QStringLiteral("Second companion answer."), QStringLiteral("resp-2"));
        QVERIFY(narration.speakingResponse());
        narration.finishSleep();
        QVERIFY(!narration.speakingResponse());
        QVERIFY(narration.speakingResponseId().isEmpty());

        // Extreme Seek/SetPosition values must not overflow int64.
        narration.resume();
        narration.seekByUs(std::numeric_limits<qint64>::max());
        QVERIFY(narration.m_currentSample >= 0);
        narration.seekByUs(std::numeric_limits<qint64>::min());
        QCOMPARE(narration.m_currentSample, qint64(0));
        narration.seekToUs(std::numeric_limits<qint64>::max());
        QVERIFY(narration.m_currentSample >= 0);
        narration.stop();
    }

    void fixAudioCacheClearStoreRaceAndWavValidation() {
        QTemporaryDir dir;
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());

        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        const QByteArray pcmA(24000 * 2 / 4, '\x11');
        const QByteArray pcmB(24000 * 2 / 4, '\x22');

        cache.store(QStringLiteral("race-key-1"), pcmA, 24000);
        cache.store(QStringLiteral("race-key-2"), pcmA, 24000);
        QTRY_VERIFY_WITH_TIMEOUT(cache.clipCount() == 2 && !cache.hasPendingStores(), 10000);

        // Immediately store the same key right after clear() while async trash deletion runs.
        cache.clear();
        QVERIFY(cache.clearing());
        cache.store(QStringLiteral("race-key-1"), pcmB, 24000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.clearing() && !cache.hasPendingStores(), 10000);

        const QString survived = cache.lookupPath(QStringLiteral("race-key-1"));
        QVERIFY2(!survived.isEmpty() && QFileInfo::exists(survived), "Newly stored clip was deleted by async clear()");
        QVERIFY(cache.lookupPath(QStringLiteral("race-key-2")).isEmpty());
        QCOMPARE(cache.clipCount(), 1);

        // Repeated clear() calls serialize cleanly and never delete subsequent store().
        cache.clear();
        cache.clear();
        cache.store(QStringLiteral("race-key-1"), pcmB, 24000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.clearing() && !cache.hasPendingStores(), 10000);
        QVERIFY(!cache.lookupPath(QStringLiteral("race-key-1")).isEmpty());

        // WAV header validation in decode(): reject corrupt non-RIFF WAV, resample valid 16 kHz WAV -> 24 kHz.
        const QString badWav = dir.filePath(QStringLiteral("bad.wav"));
        write(badWav, QByteArray("<html>502 Bad Gateway error page that exceeds 44 bytes easily</html>"));
        bool badDone = false;
        bool badOk = true;
        cache.decode(badWav, 24000, this, [&](const QByteArray &, bool ok) {
            badDone = true;
            badOk = ok;
        });
        QTRY_VERIFY_WITH_TIMEOUT(badDone, 5000);
        QVERIFY(!badOk);

        const QString goodWav = dir.filePath(QStringLiteral("good16k.wav"));
        QByteArray pcm16k(1600 * 2, '\0');  // 0.1 s at 16 kHz
        auto *s16 = reinterpret_cast<qint16 *>(pcm16k.data());
        for (int i = 0; i < 1600; ++i)
            s16[i] = qint16(6000 * std::sin(2.0 * M_PI * 440.0 * i / 16000.0));
        write(goodWav, Speech::wav(pcm16k, 16000));
        bool goodDone = false;
        bool goodOk = false;
        QByteArray decoded;
        cache.decode(goodWav, 24000, this, [&](const QByteArray &pcm, bool ok) {
            goodDone = true;
            goodOk = ok;
            decoded = pcm;
        });
        QTRY_VERIFY_WITH_TIMEOUT(goodDone, 5000);
        QVERIFY(goodOk);
        QVERIFY2(qAbs(decoded.size() - 2400 * 2) <= 8, qPrintable(QString::number(decoded.size())));
    }

    void fixAudioAudiobookSnapshotsMetadataAcrossBookSwitch() {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not installed");

        QTemporaryDir dir;
        const QString musicDir = dir.filePath(QStringLiteral("music"));
        QVERIFY(QDir().mkpath(musicDir));
        const QString userDirsPath = QString::fromUtf8(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/user-dirs.dirs");
        const bool hadUserDirs = QFile::exists(userDirsPath);
        QByteArray prevUserDirs;
        if (hadUserDirs) {
            QFile f(userDirsPath);
            if (f.open(QIODevice::ReadOnly))
                prevUserDirs = f.readAll();
        }
        auto restoreUserDirs = qScopeGuard([&] {
            if (hadUserDirs)
                write(userDirsPath, prevUserDirs);
            else
                QFile::remove(userDirsPath);
        });
        write(userDirsPath, QStringLiteral("XDG_MUSIC_DIR=\"%1\"\n").arg(musicDir).toUtf8());

        auto makeEpub = [&](const QString &fileName, const QByteArray &title, const QByteArray &author) {
            const QByteArray opf =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                "    <dc:title>" + title + "</dc:title>\n"
                "    <dc:creator>" + author + "</dc:creator>\n"
                "    <dc:language>en-GB</dc:language>\n"
                "    <meta name=\"cover\" content=\"cover-image\"/>\n"
                "  </metadata>\n"
                "  <manifest>\n"
                "    <item id=\"cover-image\" href=\"images/cover.png\" media-type=\"image/png\"/>\n"
                "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                "  </manifest>\n"
                "  <spine><itemref idref=\"c1\"/></spine>\n"
                "</package>";
            const QString path = dir.filePath(fileName);
            const bool ok = TestBooks::write(
                path, TestBooks::epub({
                          {"OEBPS/content.opf", opf},
                          {"OEBPS/images/cover.png", png(32, 48)},
                          {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Opening line.</p><p>Second line.</p>")},
                      }));
            return ok ? path : QString();
        };

        const QString pathA = makeEpub(QStringLiteral("alpha.epub"), "Alpha Voyage", "Author Alpha");
        const QString pathB = makeEpub(QStringLiteral("beta.epub"), "Beta Chronicle", "Author Beta");
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int idA = library.addBookFile(pathA);
        const int idB = library.addBookFile(pathB);
        QVERIFY(idA > 0 && idB > 0 && idA != idB);

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY(reader.openBook(idA));

        qunsetenv("GEMINI_API_KEY");
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<ToneEngine>(); });
        services.setLocalSpeech(&local);
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());

        Narration narration(&services, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        narration.setVoice(QStringLiteral("builtin:kokoro:bm_george"), QStringLiteral("George"));

        Download download(&services, &cache, &reader, &narration);
        download.start(0, 0);
        // Include a scene break so Audiobook::save() runs async silence prep before startMux().
        download.addBlocks(0, R"([
            {"start":0,"end":13,"text":"Opening line.","kind":"text"},
            {"start":14,"end":19,"text":"* * *","kind":"break"},
            {"start":20,"end":32,"text":"Second line.","kind":"text"}
        ])");
        QTRY_VERIFY_WITH_TIMEOUT(download.finished(), 15000);

        Audiobook audiobook(&cache, &download, &reader, &library);
        audiobook.save();
        QVERIFY(audiobook.busy());
        // Switch reader to Book B immediately while async prep is in flight!
        QVERIFY(reader.openBook(idB));
        QCOMPARE(reader.bookId(), idB);

        QTRY_VERIFY_WITH_TIMEOUT(!audiobook.busy(), 15000);
        QVERIFY2(audiobook.problem().isEmpty(), qPrintable(audiobook.problem()));
        QVERIFY2(audiobook.saved().endsWith(QStringLiteral("Alpha Voyage.m4b")), qPrintable(audiobook.saved()));
        QVERIFY(QFileInfo(audiobook.saved()).size() > 1000);
        QCOMPARE(library.bookSetting(idA, QStringLiteral("audiobook_saved")), QStringLiteral("1"));
        QVERIFY(library.bookSetting(idB, QStringLiteral("audiobook_saved")).isEmpty());

        const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
        if (!ffprobe.isEmpty()) {
            QProcess probe;
            probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                                  QStringLiteral("format_tags=title,artist"), QStringLiteral("-of"),
                                  QStringLiteral("default=noprint_wrappers=1"), audiobook.saved()});
            QVERIFY(probe.waitForFinished(5000));
            const QByteArray out = probe.readAllStandardOutput();
            QVERIFY2(out.contains("TAG:title=Alpha Voyage"), out.constData());
            QVERIFY2(out.contains("TAG:artist=Author Alpha"), out.constData());
        }
    }

    void fixAudioVoicePacksCachedIsInstalledAndSpeechPlayerChunks() {
        QTemporaryDir dir;
        const QString voicesRoot = dir.filePath(QStringLiteral("voices"));
        fakeInstall(voicesRoot, QStringLiteral("engine-arm"));
        fakeInstall(voicesRoot, QStringLiteral("kokoro-fp32"));

        VoicePacks packs(voicesRoot, nullptr, QStringLiteral("aarch64"), packsManifest(QStringLiteral("http://x")));
        QVERIFY(packs.isInstalled(QStringLiteral("engine")));
        QVERIFY(packs.isInstalled(QStringLiteral("kokoro-v1.0")));

        // Delete ONLY the sentinel inside the subdirectory without altering voicesRoot's directory entries:
        // cached m_installedPacks serves isInstalled() from RAM without re-statting pack marker files.
        QVERIFY(QFile::remove(voicesRoot + QStringLiteral("/kokoro-fp32/.clothbound-installed")));
        QVERIFY(packs.isInstalled(QStringLiteral("kokoro-v1.0")));
        QVERIFY(packs.voice(QStringLiteral("builtin:kokoro:af_heart")).value(QStringLiteral("installed")).toBool());

        // Async remove() updates cache & removes target directory path immediately without blocking.
        packs.remove(QStringLiteral("kokoro-v1.0"));
        QVERIFY(!packs.isInstalled(QStringLiteral("kokoro-v1.0")));
        QVERIFY(!QFileInfo::exists(voicesRoot + QStringLiteral("/kokoro-fp32")));

        // SpeechPlayer chunked appendAudio() handles odd-byte splits without shifting PCM samples.
        SpeechPlayer player(24000);
        player.setHeadless(true);
        player.addTrack(7);
        QByteArray pcm(2000 * 2, Qt::Uninitialized);
        auto *samples = reinterpret_cast<qint16 *>(pcm.data());
        for (int i = 0; i < 2000; ++i)
            samples[i] = qint16(i - 1000);
        player.appendAudio(7, pcm.left(5));
        player.appendAudio(7, pcm.mid(5, 7));
        player.appendAudio(7, pcm.mid(12));
        player.finishTrack(7);
        QCOMPARE(player.trackSamples(7), qint64(2000));
        QVERIFY(player.trackComplete(7));
    }
    // === fix-ai-mcp tests ===

    void fixAiMcpCompanionSanitizerAndSpoilerGuards() {
        QTemporaryDir dir;
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        Companion companion(&services, &reader, library.database().connectionName());

        // 1. Forged <div class="math-block"> and <span class="math-inline"> MUST NEVER bypass sanitization.
        const QString forgedBlock = QStringLiteral(
            "<div class=\"math-block\"><a href=\"http://evil.invalid/phish\">Click</a>"
            "<img src=\"http://evil.invalid/beacon.png\"></div>"
            "<span class=\"math-inline\"><a href=\"http://evil.invalid/in\">Link</a></span>"
            " Real math: $x + 1$");
        const QString html = companion.formatDisplayHtml(forgedBlock, QStringLiteral("#1f2430"));
        QVERIFY2(!html.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("href="), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("beacon.png")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("<div class=\"math-block\"")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("&lt;div")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("x")), qPrintable(html));

        // Forged math wrapper with currency ($5 to $10) must NOT trick hasMath().
        QVERIFY(!companion.hasMath(QStringLiteral("<div class=\"math-block\">fake</div> $5 to $10")));

        // 2. Doubly/triply nested malformed <img> tags and unclosed backtick desyncs are neutralized.
        const QString nestedImg =
            QStringLiteral("Hello <i<i<i<img >mg >mg >mg src=\"http://evil.invalid/nested.png\"> world");
        const QString cleanedNested = Companion::cleanMarkdown(nestedImg);
        QVERIFY2(!cleanedNested.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(cleanedNested));
        QVERIFY2(!cleanedNested.contains(QStringLiteral("evil.invalid")), qPrintable(cleanedNested));

        const QString loneTick = QStringLiteral("Odd ` <a href=\"http://evil.invalid/x\">phish</a>");
        const QString cleanedTick = Companion::cleanMarkdown(loneTick);
        QVERIFY2(!cleanedTick.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(cleanedTick));
        QVERIFY2(cleanedTick.contains(QStringLiteral("&lt;a ")), qPrintable(cleanedTick));

        // 3. Companion::textUpTo() treats offset <= 0 on PDFs as full current page (never left(0)).
        const QString pdfPath = dir.filePath(QStringLiteral("pages.pdf"));
        {
            QPdfWriter writer(pdfPath);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setResolution(72);
            QPainter p(&writer);
            p.drawText(QRect(20, 60, 550, 120), QStringLiteral("Page zero has the brass astrolabe theorem."));
            writer.newPage();
            p.drawText(QRect(20, 60, 550, 120), QStringLiteral("Page one hides the secret spoiler epilogue."));
            p.end();
        }
        const QList<Companion::Chapter> pdfPage0Unlimited = Companion::textUpTo(pdfPath, QStringLiteral("pdf"), 0, 0, -1);
        QCOMPARE(pdfPage0Unlimited.size(), 1);
        QVERIFY2(pdfPage0Unlimited.first().text.contains(QStringLiteral("brass astrolabe")),
                 qPrintable(pdfPage0Unlimited.first().text));
        QVERIFY(!pdfPage0Unlimited.first().text.contains(QStringLiteral("secret spoiler")));

        const QList<Companion::Chapter> pdfPage0Limited = Companion::textUpTo(pdfPath, QStringLiteral("pdf"), 0, 0, 4000);
        QCOMPARE(pdfPage0Limited.size(), 1);
        QVERIFY(pdfPage0Limited.first().text.contains(QStringLiteral("brass astrolabe")));

        // Explicit positive offset on PDF still slices cleanly.
        const QList<Companion::Chapter> pdfSliced = Companion::textUpTo(pdfPath, QStringLiteral("pdf"), 0, 9, -1);
        QCOMPARE(pdfSliced.size(), 1);
        QCOMPARE(pdfSliced.first().text.size(), qsizetype(9));

        // 4. Backward chapter jump hides future-chapter Ask turns, keeps them out of prompts,
        //    and cancels in-flight Ask requests.
        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restorePreset = qScopeGuard([&] {
            QSettings(settingsPath, QSettings::IniFormat).remove(QStringLiteral("ai"));
        });
        const QString epubPath = dir.filePath(QStringLiteral("spoiler-thread.epub"));
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>Fog Harbor</dc:title><dc:creator>A. Writer</dc:creator><dc:language>en</dc:language>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "    <item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
            "</package>";
        QVERIFY(TestBooks::write(
            epubPath,
            TestBooks::epub({
                {"OEBPS/content.opf", opf},
                {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Opening chapter about the brass lantern on the pier.</p>")},
                {"OEBPS/c2.xhtml", TestBooks::chapter("Ch 2", "<p>Chapter two reveals the SECRET_KILLER_IDENTITY at midnight.</p>")},
            })));
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        QVERIFY(reader.openBook(bookId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});

        FakeServer localSrv;
        LocalModels local;
        LocalModels::Server cfg;
        cfg.kind = LocalModels::Ollama;
        cfg.url = QUrl(localSrv.origin());
        local.configure(cfg, QStringLiteral("qwen3.5:4b"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        Companion comp(&services, &reader, library.database().connectionName());
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"The brass lantern sits on the pier.\"},\"done\":true}\n"});
        comp.ask(QStringLiteral("What is on the pier?"));
        QTRY_VERIFY_WITH_TIMEOUT(!comp.busy(), 5000);
        QCOMPARE(comp.conversation().size(), 2);

        // Advance to chapter 1 and ask a chapter-1 question.
        reader.relocated({{"chapter", 1}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"It reveals SECRET_KILLER_IDENTITY.\"},\"done\":true}\n"});
        comp.ask(QStringLiteral("What is revealed in chapter two?"));
        QTRY_VERIFY_WITH_TIMEOUT(!comp.busy(), 5000);
        QCOMPARE(comp.conversation().size(), 4);

        // Jump backward to chapter 0 -> chapter-1 turns vanish from visible conversation and next prompt!
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});
        QCOMPARE(comp.conversation().size(), 2);

        localSrv.requests.clear();
        localSrv.responses.append({200, "application/x-ndjson",
                                   "{\"message\":{\"content\":\"Only the brass lantern so far.\"},\"done\":true}\n"});
        comp.ask(QStringLiteral("Remind me what we know."));
        QTRY_VERIFY_WITH_TIMEOUT(!comp.busy(), 5000);
        QVERIFY(!localSrv.requests.isEmpty());
        QVERIFY2(!localSrv.requests.last().body.contains("SECRET_KILLER_IDENTITY"),
                 localSrv.requests.last().body.constData());

        // In-flight ask() is cancelled when jumping backward to an earlier chapter.
        QTcpServer hangSrv;
        QVERIFY(hangSrv.listen(QHostAddress::LocalHost));
        QList<QPointer<QTcpSocket>> held;
        connect(&hangSrv, &QTcpServer::newConnection, &hangSrv, [&hangSrv, &held] {
            while (QTcpSocket *s = hangSrv.nextPendingConnection())
                held.append(s);
        });
        LocalModels::Server hangCfg;
        hangCfg.kind = LocalModels::Ollama;
        hangCfg.url = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(hangSrv.serverPort()));
        local.configure(hangCfg, QStringLiteral("qwen3.5:4b"));
        reader.relocated({{"chapter", 1}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});
        comp.ask(QStringLiteral("Hanging chapter-two question"));
        QVERIFY(comp.busy());
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});
        QVERIFY(!comp.busy());
        for (const QPointer<QTcpSocket> &s : std::as_const(held))
            if (s)
                s->close();
        hangSrv.close();
    }

    void fixAiMcpServerLockdownAndFraming() {
        QTemporaryDir dir;
        const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
        const QString appSettings =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restoreEnv = qScopeGuard([&] {
            if (origEnvKey.isEmpty())
                qunsetenv("GEMINI_API_KEY");
            else
                qputenv("GEMINI_API_KEY", origEnvKey);
            QSettings(appSettings, QSettings::IniFormat).remove(QStringLiteral("ai"));
        });

        // Build two EPUBs (one with hostile %3/%4 placeholders in its title).
        auto makeBook = [&](const QString &fileName, const QString &title, const QString &bodyText) {
            const QString path = dir.filePath(fileName);
            const QByteArray opf =
                QStringLiteral(
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                    "    <dc:title>%1</dc:title><dc:creator>Author Name</dc:creator><dc:language>en</dc:language>\n"
                    "  </metadata>\n"
                    "  <manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
                    "  <spine><itemref idref=\"c1\"/></spine>\n"
                    "</package>")
                    .arg(title)
                    .toUtf8();
            const bool ok = TestBooks::write(
                path, TestBooks::epub({{"OEBPS/content.opf", opf},
                                       {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>" + bodyText.toUtf8() + "</p>")}}));
            return ok ? path : QString();
        };

        const QString book1Path = makeBook(QStringLiteral("b1.epub"), QStringLiteral("Report %3 %4 Title"),
                                           QStringLiteral("Quantum decoherence limits qubit coherence time."));
        const QString book2Path = makeBook(QStringLiteral("b2.epub"), QStringLiteral("Second Book"),
                                           QStringLiteral("Stellar nucleosynthesis forges heavy elements."));
        QVERIFY(!book1Path.isEmpty() && !book2Path.isEmpty());

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int b1 = library.addBookFile(book1Path);
        const int b2 = library.addBookFile(book2Path);
        QVERIFY(b1 > 0 && b2 > 0);

        const QString settingsIni = dir.filePath(QStringLiteral("mcp.ini"));
        const QString indexDb = dir.filePath(QStringLiteral("index.sqlite"));
        LibraryIndex libIndex(indexDb);
        QVERIFY(libIndex.available());

        // 1. resources/list is gated by isEnabled() (blocks with -32001 when disabled).
        {
            QSettings s(settingsIni, QSettings::IniFormat);
            s.setValue(QStringLiteral("ai/mcpEnabled"), false);
            s.sync();
        }
        McpServer gatedServer(&library, &libIndex, nullptr, {settingsIni, indexDb});
        const QJsonObject resListBlocked = gatedServer.handleRequest({
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 1},
            {QStringLiteral("method"), QStringLiteral("resources/list")},
        });
        QCOMPARE(resListBlocked.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32001);
        {
            QSettings s(settingsIni, QSettings::IniFormat);
            s.setValue(QStringLiteral("ai/mcpEnabled"), true);
            s.sync();
        }

        // 2. Read-only guarantee: search_library finds unindexed books via lexical fallback
        //    WITHOUT writing any rows to index.sqlite.
        const QJsonObject searchRes = gatedServer.handleRequest({
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 2},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("search_library")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("query"), QStringLiteral("decoherence")}}},
            }},
        }).value(QStringLiteral("result")).toObject();
        QCOMPARE(searchRes.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(libIndex.bookCount(), 0);  // ZERO writes to index.sqlite!

        // 3. Chained .arg() immunity + untrusted-data boundary markers in read_passage.
        const QJsonObject readRes = gatedServer.handleRequest({
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 3},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("read_passage")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("book_id"), b1}, {QStringLiteral("chapter"), 0}}},
            }},
        }).value(QStringLiteral("result")).toObject();
        const QString citation = readRes.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("citation")).toString();
        QVERIFY2(citation.contains(QStringLiteral("Report %3 %4 Title")), qPrintable(citation));
        const QString displayText = readRes.value(QStringLiteral("content")).toArray()[0].toObject().value(QStringLiteral("text")).toString();
        QVERIFY(displayText.contains(QStringLiteral("<<<UNTRUSTED_BOOK_CONTENT>>>")));
        QVERIFY(displayText.contains(QStringLiteral("<<<END_UNTRUSTED_BOOK_CONTENT>>>")));

        // 4. Local-only guarantee: even with GEMINI_API_KEY set in env and passage_vectors present,
        //    MCP search_passages NEVER sends queries to Gemini cloud or writes ai_usage.
        qputenv("GEMINI_API_KEY", "test-env-cloud-key");
        FakeServer cloudSrv;
        GeminiProvider gemini(nullptr, cloudSrv.endpoint());
        MemoryKeyStore keys;
        AiServices cloudAi(&gemini, &keys, library.database().connectionName());
        cloudAi.setPreset(QStringLiteral("cloud"));
        {
            QSqlQuery q(library.database());
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
                "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))")));
            const QList<float> v{1.0f, 0.0f, 0.0f};
            q.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors VALUES (?, ?, ?, ?)"));
            q.addBindValue(b1);
            q.addBindValue(QStringLiteral("gemini/gemini-embedding-2"));
            q.addBindValue(QStringLiteral("0:0:45:aa"));
            q.addBindValue(QByteArray(reinterpret_cast<const char *>(v.constData()), v.size() * qsizetype(sizeof(float))));
            QVERIFY(q.exec());
        }
        McpServer cloudGuardServer(&library, &libIndex, &cloudAi, {settingsIni, indexDb});
        const QJsonObject noCloudRes = cloudGuardServer.handleRequest({
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 4},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("search_passages")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("query"), QStringLiteral("decoherence")}}},
            }},
        }).value(QStringLiteral("result")).toObject();
        QCOMPARE(cloudSrv.requests.size(), 0);
        QCOMPARE(noCloudRes.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("mode")).toString(),
                 QStringLiteral("lexical"));
        QCOMPARE(cloudAi.spentThisMonth(), 0.0);

        // 5. allowedBooks is passed directly to PassageIndex::searchLibrary() in toolSearchPassages().
        FakeServer localEmbedSrv;
        localEmbedSrv.routes.insert("/api/embed", {200, "application/json", "{\"embeddings\":[[1.0,0.0,0.0]]}"});
        LocalModels localEmbed;
        LocalModels::Server lcfg;
        lcfg.kind = LocalModels::Ollama;
        lcfg.url = QUrl(localEmbedSrv.origin());
        localEmbed.configure(lcfg, {}, QStringLiteral("nomic-embed-text"));
        {
            // Seed 15 decoy chapters in b1 with score 1.0 and 1 chapter in b2 with score 0.6.
            QSqlQuery q(library.database());
            const QList<float> highV{1.0f, 0.0f, 0.0f};
            const QList<float> midV{0.6f, 0.8f, 0.0f};
            for (int ch = 0; ch < 15; ++ch) {
                q.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors VALUES (?, ?, ?, ?)"));
                q.addBindValue(b1);
                q.addBindValue(QStringLiteral("local/nomic-embed-text"));
                q.addBindValue(QStringLiteral("%1:0:40:aa").arg(ch));
                q.addBindValue(QByteArray(reinterpret_cast<const char *>(highV.constData()), highV.size() * qsizetype(sizeof(float))));
                QVERIFY(q.exec());
            }
            q.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors VALUES (?, ?, ?, ?)"));
            q.addBindValue(b2);
            q.addBindValue(QStringLiteral("local/nomic-embed-text"));
            q.addBindValue(QStringLiteral("0:0:45:bb"));
            q.addBindValue(QByteArray(reinterpret_cast<const char *>(midV.constData()), midV.size() * qsizetype(sizeof(float))));
            QVERIFY(q.exec());
        }
        McpServer localSemServer(&library, &libIndex, nullptr, {settingsIni, indexDb});
        localSemServer.setLocalEmbedder(&localEmbed);
        const QJsonObject scopedSem = localSemServer.handleRequest({
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 5},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("search_passages")},
                {QStringLiteral("arguments"), QJsonObject{
                    {QStringLiteral("query"), QStringLiteral("elements")},
                    {QStringLiteral("book_id"), b2},
                    {QStringLiteral("limit"), 1},
                }},
            }},
        }).value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(scopedSem.value(QStringLiteral("mode")).toString(), QStringLiteral("semantic"));
        QCOMPARE(scopedSem.value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(scopedSem.value(QStringLiteral("passages")).toArray()[0].toObject().value(QStringLiteral("book_id")).toInt(), b2);

        // 6. Bounded fread() framing: rejects embedded NUL bytes and oversized frames without corrupting subsequent frames.
        QByteArray frames;
        frames += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
        frames += QByteArray("{\"jsonrpc\":\"2.0\",") + '\0' + QByteArray("\"id\":2,\"method\":\"ping\"}\n");
        frames += QByteArray(300, 'x') + '\n';  // exceeds test cap of 128 bytes
        frames += "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}\n";

        std::FILE *inFile = std::tmpfile();
        std::FILE *outFile = std::tmpfile();
        QVERIFY(inFile && outFile);
        QCOMPARE(std::fwrite(frames.constData(), 1, size_t(frames.size()), inFile), size_t(frames.size()));
        std::rewind(inFile);

        McpServer::runStdioLoop(
            inFile, outFile,
            [&gatedServer](const QJsonObject &req) { return gatedServer.handleRequest(req); },
            128);

        std::rewind(outFile);
        QByteArray outBytes;
        char buf[1024];
        while (const size_t n = std::fread(buf, 1, sizeof(buf), outFile))
            outBytes.append(buf, qsizetype(n));
        std::fclose(inFile);
        std::fclose(outFile);

        QList<QJsonObject> replies;
        for (const QByteArray &line : outBytes.split('\n')) {
            if (line.trimmed().isEmpty())
                continue;
            const QJsonDocument d = QJsonDocument::fromJson(line);
            QVERIFY2(d.isObject(), line.constData());
            replies.append(d.object());
        }
        QCOMPARE(replies.size(), 4);
        QCOMPARE(replies[0].value(QStringLiteral("id")).toInt(), 1);
        QVERIFY(replies[0].contains(QStringLiteral("result")));
        QCOMPARE(replies[1].value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32700);
        QCOMPARE(replies[2].value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32600);
        QCOMPARE(replies[3].value(QStringLiteral("id")).toInt(), 4);
        QVERIFY(replies[3].contains(QStringLiteral("result")));
    }

    void fixAiMcpBudgetReservationPricingAndPassageIndex() {
        QTemporaryDir dir;
        const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
        const QString appSettings =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
        auto restore = qScopeGuard([&] {
            if (origEnvKey.isEmpty())
                qunsetenv("GEMINI_API_KEY");
            else
                qputenv("GEMINI_API_KEY", origEnvKey);
            QSettings(appSettings, QSettings::IniFormat).remove(QStringLiteral("ai"));
        });
        qunsetenv("GEMINI_API_KEY");

        // 1. Unknown cloud model fallback pricing (non-zero for rotated Gemini models, $0 for local).
        Ai::Usage u;
        u.inputTokens = 100000;
        u.outputTokens = 10000;
        const QDate today(2026, 11, 25);
        QVERIFY(AiUsageLedger::cost(QStringLiteral("gemini-9-future-pro"), u, today, false) > 0.0);
        QVERIFY(AiUsageLedger::cost(QStringLiteral("gemini-5-flash-tts-preview"), u, today, false) > 0.0);
        QVERIFY(AiUsageLedger::cost(QStringLiteral("gemini-embedding-9"), u, today, false) > 0.0);
        QCOMPARE(AiUsageLedger::cost(QStringLiteral("a-local-model"), u, today, false), 0.0);
        QVERIFY(AiUsageLedger::costForProvider(QStringLiteral("gemini"), QStringLiteral("unrecognized-cloud-id"), u, today, false) > 0.0);
        QCOMPARE(AiUsageLedger::costForProvider(QStringLiteral("local"), QStringLiteral("gemini-4-hydrogen"), u, today, false), 0.0);

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());

        // 2. In-flight spend reservation blocks concurrent budget overshoot & releases on cancel/finish.
        QTcpServer hangSrv;
        QVERIFY(hangSrv.listen(QHostAddress::LocalHost));
        QList<QPointer<QTcpSocket>> held;
        connect(&hangSrv, &QTcpServer::newConnection, &hangSrv, [&hangSrv, &held] {
            while (QTcpSocket *s = hangSrv.nextPendingConnection())
                held.append(s);
        });
        GeminiProvider gemini(nullptr, QStringLiteral("http://127.0.0.1:%1/v1beta").arg(hangSrv.serverPort()));
        gemini.setKey(QStringLiteral("k"));
        MemoryKeyStore keys;
        AiServices services(&gemini, &keys, library.database().connectionName());
        services.setUnlimitedBudget(false);
        services.setBudget(1.00);
        QVERIFY(services.fitsBudget(0.70));

        Ai::TextRequest req;
        req.feature = QStringLiteral("ask");
        req.messages = {{true, QStringLiteral("hi")}};
        std::unique_ptr<Ai::Reply> inFlight(gemini.generate(req));
        services.track(inFlight.get(), false, 0.70);
        QVERIFY(qAbs(services.reservedBudget() - 0.70) < 1e-6);
        QVERIFY(!services.fitsBudget(0.50));  // 0.70 reserved + 0.50 > $1.00 cap!
        QVERIFY(services.fitsBudget(0.25));   // 0.70 reserved + 0.25 <= $1.00

        inFlight->abort();
        QTRY_VERIFY(inFlight->isFinished());
        QCOMPARE(services.reservedBudget(), 0.0);
        QVERIFY(services.fitsBudget(0.50));  // released after cancel!
        for (const QPointer<QTcpSocket> &s : std::as_const(held))
            if (s)
                s->close();
        hangSrv.close();

        // 3. Audio-only usage rows are recorded and billed (not dropped when input/output/thought == 0).
        class AudioOnlyReply : public Ai::Reply {
        public:
            explicit AudioOnlyReply(QObject *parent) : Ai::Reply(parent) {
                m_model = QStringLiteral("gemini-3.8-flash-tts");
                m_feature = QStringLiteral("listen");
                m_usage.audioTokens = 50000;
            }
            void abort() override { finish({Ai::Error::Cancelled, {}}); }
            void emitDone() { finish(); }
        };
        AudioOnlyReply audioReply(&gemini);
        services.track(&audioReply, false, 0.01);
        audioReply.emitDone();
        QVERIFY(services.spentThisMonth() > 0.40);  // 50k audio tokens @ $9/1M = $0.45

        // 4. PRAGMA data_version invalidates m_spentCache when a sibling DB connection writes ai_usage.
        const double beforeSibling = services.spentThisMonth();
        {
            const QString extConn = QStringLiteral("sibling-cli-usage-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                QSqlDatabase extDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), extConn);
                extDb.setDatabaseName(library.database().databaseName());
                QVERIFY(extDb.open());
                QSqlQuery ins(extDb);
                ins.prepare(QStringLiteral(
                    "INSERT INTO ai_usage (at, provider, model, feature, input, output, cached, thought, audio, flex, cost) "
                    "VALUES (?, 'gemini', 'gemini-4-hydrogen', 'ask', 100, 100, 0, 0, 0, 0, 2.50)"));
                ins.addBindValue(QDateTime::currentSecsSinceEpoch());
                QVERIFY(ins.exec());
                extDb.close();
            }
            QSqlDatabase::removeDatabase(extConn);
        }
        QVERIFY(services.spentThisMonth() >= beforeSibling + 2.49);

        // 5. PassageIndex schema index, book_id IN (>500 chunking), partial vectors, and orphan cleanup.
        const QString epubPath = dir.filePath(QStringLiteral("idx.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));
        const int realBookId = library.addBookFile(epubPath);
        QVERIFY(realBookId > 0);

        FakeServer localSrv;
        LocalModels local;
        LocalModels::Server lcfg;
        lcfg.kind = LocalModels::Ollama;
        lcfg.url = QUrl(localSrv.origin());
        local.configure(lcfg, QStringLiteral("qwen3.5:4b"), QStringLiteral("nomic-embed-text"));
        services.setLocalModels(&local);
        services.setPreset(QStringLiteral("local"));

        PassageIndex pIndex(&services, library.database().connectionName());
        {
            QSqlQuery idxCheck(library.database());
            QVERIFY(idxCheck.exec(QStringLiteral(
                "SELECT 1 FROM sqlite_master WHERE type='index' AND name='idx_passage_vectors_model_book'")));
            QVERIFY(idxCheck.next());
        }

        // Embed only passage 0 of a 2-passage book -> search() still runs semantic/hybrid (no all-or-nothing drop!).
        const QList<PassageIndex::Passage> twoPassages{
            {0, 0, 20, QStringLiteral("Orbital mechanics.")},
            {0, 25, 48, QStringLiteral("Unembedded passage.")},
        };
        localSrv.responses.append({200, "application/json", "{\"embeddings\":[[1.0,0.0,0.0]]}"});
        QString buildErr = QStringLiteral("unset");
        pIndex.build(realBookId, {twoPassages[0]}, nullptr, [&](const QString &e) { buildErr = e; });
        QTRY_COMPARE(buildErr, QString());
        QCOMPARE(pIndex.indexed(realBookId), 1);

        localSrv.responses.append({200, "application/json", "{\"embeddings\":[[1.0,0.0,0.0]]}"});
        QList<PassageIndex::Hit> partialHits;
        bool usedMeaning = false;
        bool searchDone = false;
        pIndex.search(realBookId, twoPassages, QStringLiteral("spaceflight"), 2,
                      [&](const QList<PassageIndex::Hit> &hits, bool byMeaning) {
                          partialHits = hits;
                          usedMeaning = byMeaning;
                          searchDone = true;
                      });
        QTRY_VERIFY_WITH_TIMEOUT(searchDone, 5000);
        QVERIFY(usedMeaning);
        QVERIFY(!partialHits.isEmpty());
        QCOMPARE(partialHits.first().passage.text, QStringLiteral("Orbital mechanics."));

        // book_id IN chunking handles > 500 IDs cleanly.
        QSet<int> hugeBookSet;
        for (int id = 1000; id < 1560; ++id)
            hugeBookSet.insert(id);
        hugeBookSet.insert(realBookId);
        const QList<PassageIndex::LibraryHit> chunkedHits = PassageIndex::rankLibrary(
            library.database().databaseName(), QStringLiteral("local/nomic-embed-text"),
            {1.0f, 0.0f, 0.0f}, 5, 0.25, hugeBookSet);
        QCOMPARE(chunkedHits.size(), 1);
        QCOMPARE(chunkedHits.first().bookId, realBookId);

        // Orphan vector pruning + AFTER DELETE ON books trigger.
        {
            QSqlQuery insOrphan(library.database());
            const QList<float> v{1.0f, 0.0f, 0.0f};
            insOrphan.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, ?, ?, ?)"));
            insOrphan.addBindValue(99999);
            insOrphan.addBindValue(QStringLiteral("local/nomic-embed-text"));
            insOrphan.addBindValue(QStringLiteral("0:0:10:zz"));
            insOrphan.addBindValue(QByteArray(reinterpret_cast<const char *>(v.constData()), v.size() * qsizetype(sizeof(float))));
            QVERIFY(insOrphan.exec());
        }
        QCOMPARE(pIndex.pruneOrphans(), 1);
        {
            QSqlQuery delBook(library.database());
            delBook.prepare(QStringLiteral("DELETE FROM books WHERE id = ?"));
            delBook.addBindValue(realBookId);
            QVERIFY(delBook.exec());
            QCOMPARE(pIndex.indexed(realBookId), 0);
        }

        // 6. PassageIndex::embedNext() re-checks budget before each cloud batch.
        {
            FakeServer cloudEmbedSrv;
            QJsonArray batchVecs;
            for (int i = 0; i < 32; ++i)
                batchVecs.append(QJsonObject{{QStringLiteral("values"), QJsonArray{1.0, 0.0, 0.0}}});
            cloudEmbedSrv.responses.append(
                {200, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("embeddings"), batchVecs}}).toJson(QJsonDocument::Compact)});
            cloudEmbedSrv.responses.append(
                {200, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("embeddings"), batchVecs}}).toJson(QJsonDocument::Compact)});

            GeminiProvider batchGemini(nullptr, cloudEmbedSrv.endpoint());
            MemoryKeyStore batchKeys;
            batchKeys.save(QStringLiteral("gemini"), QStringLiteral("k"));
            Library batchLib(dir.filePath(QStringLiteral("data_batch")), dir.filePath(QStringLiteral("cache_batch")));
            QVERIFY(batchLib.open());
            const int batchBookId = batchLib.addBookFile(epubPath);
            QVERIFY(batchBookId > 0);
            AiServices batchAi(&batchGemini, &batchKeys, batchLib.database().connectionName());
            batchAi.setPreset(QStringLiteral("cloud"));
            batchAi.setUnlimitedBudget(false);

            // 40 passages = 2 batches (32 + 8). Initial budget ($0.01) passes upfront build() check;
            // lowering budget while batch 1 is in flight must halt batch 2 via embedNext() re-check.
            QList<PassageIndex::Passage> many;
            const QString filler(2000, QLatin1Char('a'));  // ~500 tokens each -> 32 * 500 = 16000 tokens ($0.0032), 8 * 500 = 4000 ($0.0008)
            for (int i = 0; i < 40; ++i)
                many.append({0, i * 2100, i * 2100 + 2000, QStringLiteral("P%1 %2").arg(i).arg(filler)});
            batchAi.setBudget(0.01);

            PassageIndex batchIndex(&batchAi, batchLib.database().connectionName());
            QString batchErr = QStringLiteral("unset");
            batchIndex.build(
                batchBookId, many,
                [&](double) { batchAi.setBudget(0.0036); },  // tightened mid-flight before batch 2
                [&](const QString &err) { batchErr = err; });
            QTRY_VERIFY_WITH_TIMEOUT(batchErr != QLatin1String("unset"), 5000);
            QVERIFY2(batchErr.contains(QStringLiteral("budget"), Qt::CaseInsensitive), qPrintable(batchErr));
            QCOMPARE(cloudEmbedSrv.requests.size(), 1);
            QCOMPARE(batchIndex.indexed(batchBookId), 32);
        }
    }

    void fixAiMcpSseLocalModelsAndWavValidation() {
        // 1. SseParser strips UTF-8 BOM split across 1-byte chunks and caps oversized frames.
        {
            SseParser sse;
            QVERIFY(sse.feed(QByteArray("\xEF", 1)).isEmpty());
            QVERIFY(sse.feed(QByteArray("\xBB", 1)).isEmpty());
            QVERIFY(sse.feed(QByteArray("\xBF", 1)).isEmpty());
            const auto ev = sse.feed("event: delta\ndata: split-bom\n\n");
            QCOMPARE(ev.size(), 1);
            QCOMPARE(ev[0].name, QByteArray("delta"));
            QCOMPARE(ev[0].data, QByteArray("split-bom"));
            QVERIFY(!sse.overflowed());

            SseParser capped;
            const QByteArray bigChunk(SseParser::kMaxBufferBytes + 16, 'a');
            QVERIFY(capped.feed(bigChunk).isEmpty());
            QVERIFY(capped.overflowed());
        }

        // 2. LocalModels ServerReply: rejects premature clean EOF without [DONE] / "done":true,
        //    and flushes split-tail SSE delivered right before EOF.
        {
            FakeServer server;
            LocalModels local;
            LocalModels::Server cfg;
            cfg.kind = LocalModels::LlamaCpp;
            cfg.url = QUrl(server.origin());
            local.configure(cfg, QStringLiteral("qwen"));

            // Truncated SSE stream without [DONE] -> Offline error (not silent truncation!).
            server.responses.append({200, "text/event-stream",
                                     "data: {\"choices\":[{\"delta\":{\"content\":\"Halfway\"}}]}\n\n"});
            Ai::TextRequest req;
            req.stream = true;
            req.messages = {{true, QStringLiteral("Hi")}};
            std::unique_ptr<Ai::Reply> truncSse(local.generate(req));
            QTRY_VERIFY_WITH_TIMEOUT(truncSse->isFinished(), 5000);
            QCOMPARE(truncSse->error().kind, Ai::Error::Offline);

            // Split-tail SSE stream where final event + [DONE] arrive cleanly at EOF.
            server.responses.append({200, "text/event-stream",
                                     "data: {\"choices\":[{\"delta\":{\"content\":\"Complete\"}}]}\n\n"
                                     "data: [DONE]"});
            std::unique_ptr<Ai::Reply> tailSse(local.generate(req));
            QTRY_VERIFY_WITH_TIMEOUT(tailSse->isFinished(), 5000);
            QVERIFY2(!tailSse->error(), qPrintable(tailSse->error().message));
            QCOMPARE(tailSse->text(), QStringLiteral("Complete"));

            // Truncated Ollama NDJSON without "done":true -> Offline error.
            cfg.kind = LocalModels::Ollama;
            local.configure(cfg, QStringLiteral("qwen3.5:4b"));
            server.responses.append({200, "application/x-ndjson",
                                     "{\"message\":{\"content\":\"Cut off\"},\"done\":false}\n"});
            std::unique_ptr<Ai::Reply> truncNd(local.generate(req));
            QTRY_VERIFY_WITH_TIMEOUT(truncNd->isFinished(), 5000);
            QCOMPARE(truncNd->error().kind, Ai::Error::Offline);
        }

        // 3. GeminiProvider::pcmFromWav() validates RIFF/WAVE + PCM 16-bit and downmixes stereo.
        {
            auto le32 = [](quint32 v) {
                QByteArray b(4, Qt::Uninitialized);
                b[0] = char(v & 0xff);
                b[1] = char((v >> 8) & 0xff);
                b[2] = char((v >> 16) & 0xff);
                b[3] = char((v >> 24) & 0xff);
                return b;
            };
            auto le16 = [](quint16 v) {
                QByteArray b(2, Qt::Uninitialized);
                b[0] = char(v & 0xff);
                b[1] = char((v >> 8) & 0xff);
                return b;
            };

            // Reject HTML, JSON, MP3, Ogg, and non-WAV in strict mode.
            QVERIFY(GeminiProvider::pcmFromWav("<html><body>502 Bad Gateway</body></html>").isEmpty());
            QVERIFY(GeminiProvider::pcmFromWav("{\"error\":\"upstream\"}").isEmpty());
            QVERIFY(GeminiProvider::pcmFromWav("ID3\x03\x00\x00\x00\x00\x00\x00").isEmpty());
            QVERIFY(GeminiProvider::pcmFromWav(QByteArray("\xFF\xFB\x90\x00\x00\x00", 6)).isEmpty());
            QVERIFY(GeminiProvider::pcmFromWav("OggS\x00\x02").isEmpty());
            QVERIFY(GeminiProvider::pcmFromWav("raw bytes", nullptr, true).isEmpty());

            // Reject non-PCM WAV (audioFormat=3 float) and 24-bit WAV.
            const QByteArray floatWav = "RIFF" + le32(44) + "WAVE" + "fmt " + le32(16) + le16(3) + le16(1) +
                                        le32(24000) + le32(96000) + le16(4) + le16(32) + "data" + le32(8) + QByteArray(8, '\0');
            QVERIFY(GeminiProvider::pcmFromWav(floatWav).isEmpty());

            const QByteArray bit24Wav = "RIFF" + le32(42) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(1) +
                                        le32(24000) + le32(72000) + le16(3) + le16(24) + "data" + le32(6) + QByteArray(6, '\0');
            QVERIFY(GeminiProvider::pcmFromWav(bit24Wav).isEmpty());

            // Valid 16-bit stereo WAV downmixes cleanly to mono: (1000 + 3000) / 2 = 2000.
            QByteArray stereoData;
            stereoData += le16(1000) + le16(3000);
            stereoData += le16(quint16(qint16(-400))) + le16(quint16(qint16(-200)));
            const QByteArray stereoWav = "RIFF" + le32(36 + quint32(stereoData.size())) + "WAVE" + "fmt " + le32(16) +
                                         le16(1) + le16(2) + le32(48000) + le32(192000) + le16(4) + le16(16) +
                                         "data" + le32(quint32(stereoData.size())) + stereoData;
            int rate = 0;
            const QByteArray mono = GeminiProvider::pcmFromWav(stereoWav, &rate, true);
            QCOMPARE(rate, 48000);
            QCOMPARE(mono.size(), qsizetype(4));
            const qint16 s0 = qint16(quint8(mono[0]) | (quint8(mono[1]) << 8));
            const qint16 s1 = qint16(quint8(mono[2]) | (quint8(mono[3]) << 8));
            QCOMPARE(s0, qint16(2000));
            QCOMPARE(s1, qint16(-300));
        }
    }

    // === wave-2 integration tests ===
    void opdsCoverProviderWiring() {
        const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
        QVERIFY(!srcRoot.isEmpty());

        QFile mainFile(srcRoot + QStringLiteral("/app/main.cpp"));
        QVERIFY(mainFile.open(QIODevice::ReadOnly));
        const QString mainCpp = QString::fromUtf8(mainFile.readAll());
        const qsizetype opdsPos = mainCpp.indexOf(QStringLiteral("OpdsClient opds("));
        const qsizetype enginePos = mainCpp.indexOf(QStringLiteral("QQmlApplicationEngine engine;"));
        const qsizetype providerPos = mainCpp.indexOf(
            QStringLiteral("engine.addImageProvider(QStringLiteral(\"opdscover\"), new OpdsCoverProvider(&opds));"));
        QVERIFY(opdsPos > 0);
        QVERIFY(enginePos > opdsPos);
        QVERIFY(providerPos > enginePos);

        QFile catalogsFile(srcRoot + QStringLiteral("/ui/library/CatalogsSheet.qml"));
        QVERIFY(catalogsFile.open(QIODevice::ReadOnly));
        const QString catalogsQml = QString::fromUtf8(catalogsFile.readAll());
        QVERIFY(catalogsQml.count(QStringLiteral("opds.coverImageUrl(")) >= 2);
        QVERIFY(!catalogsQml.contains(QStringLiteral("source: entryWrap.modelData.thumbnailUrl")));
        QVERIFY(!catalogsQml.contains(QStringLiteral("source: navRow.isBookNav ? (entryWrap.modelData.thumbnailUrl")));
    }
    // === end wave-2 integration tests ===

    // === a-plus-arch tests ===
    void aPlusArchMathFormatterUnitAndFuzz() {
        // 1. Direct unit coverage for pure MathFormatter namespace APIs and Companion wrapper parity.
        const QString sample = QStringLiteral(
            "Let $\\theta \\in \\mathbb{R}^d$ satisfy $$\\sum_{i=1}^n \\frac{x_i}{n} \\le \\sqrt[3]{a + b}$$.");
        QVERIFY(MathFormatter::hasMath(sample));
        QCOMPARE(MathFormatter::cleanMarkdown(sample), Companion::cleanMarkdown(sample));
        const QString formatted = MathFormatter::formatLatexInMarkdown(QStringLiteral("Value $x_i^2$ and <b>raw</b>"));
        QVERIFY(formatted.contains(QStringLiteral("class=\"math-inline\"")));
        QVERIFY(formatted.contains(QStringLiteral("<sub>i</sub>")));
        QVERIFY(formatted.contains(QStringLiteral("<sup><span style=\"font-style:normal;\">2</span></sup>")));
        QVERIFY(formatted.contains(QStringLiteral("&lt;b&gt;raw&lt;/b&gt;")));

        // 2. CommonMark backtick-fence desync resistance (4-tick fence containing 3-tick line, or info-string fake close).
        const QString fenceDesync1 = QStringLiteral(
            "````\n"
            "```\n"
            "````\n"
            "<a href=\"javascript:alert(1)\">phish</a><img src=\"http://evil.invalid/a.png\">");
        const QString fenceDesync2 = QStringLiteral(
            "```\n"
            "``` fakeinfo\n"
            "```\n"
            "<a href=\"javascript:alert(2)\">phish2</a><script>alert(2)</script>");
        for (const QString &desync : {fenceDesync1, fenceDesync2}) {
            const QString cleaned = MathFormatter::cleanMarkdown(desync);
            QVERIFY2(!cleaned.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(cleaned));
            QVERIFY2(!cleaned.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(cleaned));
            QVERIFY2(!cleaned.contains(QStringLiteral("<script"), Qt::CaseInsensitive), qPrintable(cleaned));
            const QString rich = MathFormatter::formatDisplayHtml(desync, QStringLiteral("#222633"));
            QVERIFY2(!rich.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("<script"), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive), qPrintable(rich));
        }

        // 3. Deep brace nesting terminates cleanly in O(N) without blowing up output or stack,
        //    and CSS injection via mathBlockBg is rejected.
        const QString deepBraces = QStringLiteral("$") + QString(40, QLatin1Char('{')) + QStringLiteral("x<img src=1>") +
                                   QString(40, QLatin1Char('}')) + QStringLiteral("$");
        const QString deepHtml = MathFormatter::formatDisplayHtml(
            deepBraces + QStringLiteral("\n\n$$a+b$$"),
            QStringLiteral("red; background-image:url(javascript:alert(1))"));
        QVERIFY2(!deepHtml.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(deepHtml));
        QVERIFY2(!deepHtml.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive), qPrintable(deepHtml));
        QVERIFY2(deepHtml.contains(QStringLiteral("rgba(128,128,128,0.12)")), qPrintable(deepHtml));

        // 4. Structured javascript: / link / tag escape vectors must produce ZERO javascript: anywhere in RichText.
        const QStringList jsVectors = {
            QStringLiteral("[click](javascript:alert(1))"),
            QStringLiteral("[click](  JaVaScRiPt:alert(1)  )"),
            QStringLiteral("[click](javascript:void(0))"),
            QStringLiteral("[click](<javascript:alert(1)>)"),
            QStringLiteral("[ref][1]\n\n[1]: javascript:alert(1)"),
            QStringLiteral("![alt](javascript:alert(1))"),
            QStringLiteral("<a href=\"javascript:alert(1)\">click</a>"),
            QStringLiteral("<a href='javascript:alert(1)'>click</a>"),
            QStringLiteral("<a href=javascript:alert(1)>click</a>"),
            QStringLiteral("<img src=\"javascript:alert(1)\">"),
            QStringLiteral("<iframe src=\"javascript:alert(1)\"></iframe>"),
            QStringLiteral("<svg onload=\"javascript:alert(1)\"></svg>"),
            QStringLiteral("<div class=\"math-block\"><a href=\"javascript:alert(1)\">x</a></div> $$x+1$$"),
            QStringLiteral("<span class=\"math-inline\"><img src=\"javascript:alert(1)\"></span> $x$"),
            QStringLiteral("`<a href=\"javascript:alert(1)\">code</a>`"),
            QStringLiteral("```\n<a href=\"javascript:alert(1)\">fenced</a>\n```"),
        };
        for (const QString &vec : jsVectors) {
            const QString rich = MathFormatter::formatDisplayHtml(vec, QStringLiteral("#1f2430"));
            QVERIFY2(!rich.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("<script"), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("<a\t"), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("<a\n"), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("href="), Qt::CaseInsensitive), qPrintable(rich));
            QVERIFY2(!rich.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive), qPrintable(rich));
        }

        // 5. Seeded deterministic fuzz-mutation of 550 adversarial HTML/LaTeX/Markdown inputs.
        quint64 rng = 0xC107B00D5EED0001ULL;
        auto nextU64 = [&rng]() -> quint64 {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            return rng * 2685821657736338717ULL;
        };
        const QStringList atoms = {
            QStringLiteral("<img src=\"http://evil.invalid/x.png\">"),
            QStringLiteral("<IMG/SRC=x ONERROR=alert(1)>"),
            QStringLiteral("<i<i<img >mg >mg src=http://evil.invalid/n.png>"),
            QStringLiteral("<script>alert(1)</script>"),
            QStringLiteral("<scr<script>ipt>alert(1)</script>"),
            QStringLiteral("<a href=\"javascript:alert(1)\">click</a>"),
            QStringLiteral("<A HREF=\"http://evil.invalid\">link</A>"),
            QStringLiteral("[click](javascript:alert(1))"),
            QStringLiteral("![leak](https://evil.invalid/i.png)"),
            QStringLiteral("!![nest](http://evil.invalid/j.png)"),
            QStringLiteral("<svg onload=alert(1)>"),
            QStringLiteral("<iframe src=\"javascript:alert(1)\"></iframe>"),
            QStringLiteral("<div class=\"math-block\">"),
            QStringLiteral("<span class=\"math-inline\">"),
            QStringLiteral("</div>"),
            QStringLiteral("</span>"),
            QStringLiteral("$\\frac{a}{b}$"),
            QStringLiteral("$$\\sum_{i=1}^n x_i^2$$"),
            QStringLiteral("\\(\\alpha + \\beta\\)"),
            QStringLiteral("\\[\\sqrt[3]{x + y}\\]"),
            QStringLiteral("$\\text{<img src=x>}$"),
            QStringLiteral("$5 to $10"),
            QStringLiteral("`"),
            QStringLiteral("``"),
            QStringLiteral("\n```\n"),
            QStringLiteral("\n````\n"),
            QStringLiteral("\n``` latex\n"),
            QStringLiteral("{"),
            QStringLiteral("}"),
            QStringLiteral("^"),
            QStringLiteral("_"),
            QStringLiteral("\\"),
            QStringLiteral("> "),
            QStringLiteral("\n\n"),
        };
        const QStringList bgCandidates = {
            QStringLiteral("#1f2430"),
            QStringLiteral("rgba(128,128,128,0.12)"),
            QStringLiteral("red; background:url(javascript:alert(1))"),
            QStringLiteral("\"><img src=x>"),
            QString(),
        };
        static const QRegularExpression anyTagRx(QStringLiteral("<[^>]*>"));
        static const QRegularExpression anchorTagRx(QStringLiteral("<a(?:\\s|/|>|$)"),
                                                    QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression badTagAttrRx(
            QStringLiteral("(?:javascript:|\\b(?:href|src|background|onerror|onload)\\s*=)"),
            QRegularExpression::CaseInsensitiveOption);

        constexpr int kFuzzCases = 550;
        for (int iter = 0; iter < kFuzzCases; ++iter) {
            QString input;
            const int parts = 3 + int(nextU64() % 10);
            for (int p = 0; p < parts; ++p)
                input += atoms[qsizetype(nextU64() % quint64(atoms.size()))];
            // Apply deterministic splice/mutation operations on half the cases.
            const int mutOps = int(nextU64() % 4);
            for (int m = 0; m < mutOps && !input.isEmpty(); ++m) {
                const int op = int(nextU64() % 4);
                const qsizetype pos = qsizetype(nextU64() % quint64(input.size()));
                if (op == 0) {
                    const QString extra = atoms[qsizetype(nextU64() % quint64(atoms.size()))];
                    input.insert(pos, extra);
                } else if (op == 1 && input.size() > 4) {
                    const qsizetype delLen = 1 + qsizetype(nextU64() % qMin<quint64>(8, quint64(input.size() - pos)));
                    input.remove(pos, delLen);
                } else if (op == 2) {
                    const QString chars = QStringLiteral("<>`$\\{}^_[]()\"'!\n");
                    input.insert(pos, chars[qsizetype(nextU64() % quint64(chars.size()))]);
                } else {
                    input.insert(pos, QStringLiteral("CBMATH0000000000000000B0Z"));
                }
            }

            const QString bg = bgCandidates[qsizetype(nextU64() % quint64(bgCandidates.size()))];
            const QString html = MathFormatter::formatDisplayHtml(input, bg);
            (void)MathFormatter::cleanMarkdown(input);
            (void)MathFormatter::hasMath(input);

            QVERIFY2(!html.contains(QStringLiteral("<img"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("iter %1 leaked <img: %2").arg(iter).arg(html)));
            QVERIFY2(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("iter %1 leaked <script: %2").arg(iter).arg(html)));
            QVERIFY2(!anchorTagRx.match(html).hasMatch(),
                     qPrintable(QStringLiteral("iter %1 leaked <a>: %2").arg(iter).arg(html)));
            QVERIFY2(!html.contains(QStringLiteral("<a href"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("iter %1 leaked <a href: %2").arg(iter).arg(html)));

            // Inspect every single HTML tag in RichText output for forbidden tags or javascript:/href=/src= escapes.
            auto it = anyTagRx.globalMatch(html);
            while (it.hasNext()) {
                const QString tag = it.next().captured(0);
                QVERIFY2(!badTagAttrRx.match(tag).hasMatch(),
                         qPrintable(QStringLiteral("iter %1 unsafe tag attr in %2").arg(iter).arg(tag)));
            }
        }
    }

    void aPlusArchSpeechCleanerUnitAndFuzz() {
        // 1. Direct unit tests for SpeechCleaner APIs + parity with Narration::cleanSpeechText.
        const QString complexMd = QStringLiteral(
            "### Results\n"
            "> Compute $\\binom{n}{k}$, $\\sqrt[3]{x}$, $\\sqrt[4]{y}$, $\\sum_{i=1}^n x_i^2$, "
            "$\\int_0^1 t^3 dt$, and $\\vec{v} + \\hat{u} + \\bar{w} + \\tilde{z}$.\n"
            "| Metric | Score |\n| --- | --- |\n| BLEU | 42.1 |\n"
            "See [Paper](https://example.test/p) [1, 2] [p. 14] and costs of $5 to $10.");
        const QString spoken = SpeechCleaner::cleanSpeechText(complexMd);
        QCOMPARE(spoken, Narration::cleanSpeechText(complexMd));
        QVERIFY2(spoken.contains(QStringLiteral("n choose k")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("cube root of x")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("4th root of y")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("sum from i=1 to n")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("squared")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("cubed")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("vector v")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("u hat")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("w bar")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("z tilde")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("BLEU, 42.1")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("$5 to $10")), qPrintable(spoken));
        QVERIFY(!spoken.contains(QStringLiteral("https://")));
        QVERIFY(!spoken.contains(QStringLiteral("[1, 2]")));
        QVERIFY(!spoken.contains(QStringLiteral("[p. 14]")));

        // 2. cleanLatexForSpeech directly on display/inline/bare commands & inline code preservation.
        const QString latexOnly = SpeechCleaner::cleanLatexForSpeech(
            QStringLiteral("$$\\frac{a}{b}$$ and `\\frac{raw}{code}` with \\nabla f"));
        QVERIFY2(latexOnly.contains(QStringLiteral("a over b")), qPrintable(latexOnly));
        QVERIFY2(latexOnly.contains(QStringLiteral("`\\frac{raw}{code}`")), qPrintable(latexOnly));
        QVERIFY2(latexOnly.contains(QStringLiteral("gradient")), qPrintable(latexOnly));

        // 3. splitOverlongSentence respects maxChars and splits cleanly at clause/word boundaries.
        const QString longSent = QStringLiteral(
            "First clause with enough words to exceed the limit, second clause with more words; "
            "third clause continuing onwards without stopping until the very end of the sentence.");
        const QStringList parts = SpeechCleaner::splitOverlongSentence(longSent, 60);
        QVERIFY(parts.size() >= 3);
        for (const QString &part : parts) {
            QVERIFY(!part.isEmpty());
            QVERIFY2(part.size() <= 60, qPrintable(part));
        }
        QVERIFY(SpeechCleaner::splitOverlongSentence(QStringLiteral("   "), 60).isEmpty());

        // 4. Deterministic fuzz stress test (250 adversarial inputs incl. deep brace nesting).
        quint64 rng = 0x59EEC4C1EA4E0002ULL;
        auto nextU64 = [&rng]() -> quint64 {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            return rng * 2685821657736338717ULL;
        };
        const QStringList pieces = {
            QStringLiteral("$$\\frac{1}{2}$$"), QStringLiteral("$x_i^2$"), QStringLiteral("\\(\\alpha\\)"),
            QStringLiteral("\\[\\sqrt[3]{z}\\]"), QStringLiteral("\\nabla"), QStringLiteral("`code`"),
            QStringLiteral("**bold**"), QStringLiteral("<b>tag</b>"), QStringLiteral("[1, 2]"),
            QStringLiteral("[link](http://x)"), QStringLiteral("$5 to $10"), QStringLiteral("{{{{{"),
            QStringLiteral("}}}}}"), QStringLiteral("^^^"), QStringLiteral("___"), QStringLiteral("\\\\"),
        };
        for (int iter = 0; iter < 250; ++iter) {
            QString s;
            const int count = 2 + int(nextU64() % 12);
            for (int k = 0; k < count; ++k)
                s += pieces[qsizetype(nextU64() % quint64(pieces.size()))];
            if (iter % 10 == 0)
                s = QStringLiteral("$") + QString(36, QLatin1Char('{')) + s + QString(36, QLatin1Char('}')) + QStringLiteral("$");
            const QString out = SpeechCleaner::cleanSpeechText(s);
            const QStringList split = SpeechCleaner::splitOverlongSentence(out, 120);
            for (const QString &chunk : split)
                QVERIFY(chunk.size() <= 120);
        }
    }

    void aPlusArchSseParserFuzz() {
        quint64 rng = 0x55E9A45E4F022003ULL;
        auto nextU64 = [&rng]() -> quint64 {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            return rng * 2685821657736338717ULL;
        };

        // 1. 300 deterministic valid/edge-case SSE streams: arbitrary chunk splits must equal single-shot parse.
        const QList<QByteArray> eols = {"\n", "\r\n", "\r"};
        const QList<QByteArray> names = {"", "delta", "message", "content_block_delta", "done"};
        const QList<QByteArray> payloads = {
            "{\"text\":\"hello\"}",
            "line with: colons: inside",
            "",
            "[DONE]",
            "utf8-\xC3\xA9-\xE2\x80\x94-\xF0\x9F\x93\x96",
            " leading-space-preserved",
        };
        for (int iter = 0; iter < 300; ++iter) {
            QByteArray stream;
            if (nextU64() % 3 == 0)
                stream.append("\xEF\xBB\xBF", 3);
            const int numEvents = 1 + int(nextU64() % 8);
            for (int e = 0; e < numEvents; ++e) {
                const QByteArray eol = eols[qsizetype(nextU64() % quint64(eols.size()))];
                if (nextU64() % 3 == 0)
                    stream += ": keepalive comment" + eol;
                const QByteArray evName = names[qsizetype(nextU64() % quint64(names.size()))];
                if (!evName.isEmpty())
                    stream += "event: " + evName + eol;
                const int dataLines = 1 + int(nextU64() % 4);
                for (int d = 0; d < dataLines; ++d) {
                    const QByteArray p = payloads[qsizetype(nextU64() % quint64(payloads.size()))];
                    stream += (nextU64() % 4 == 0 ? "data:" : "data: ") + p + eol;
                }
                if (nextU64() % 4 == 0)
                    stream += "id: 42" + eol;
                // Occasionally omit final blank line to exercise finish() flush.
                if (e + 1 < numEvents || nextU64() % 2 == 0)
                    stream += eol;
            }

            SseParser whole;
            const QList<SseParser::Event> expected = whole.feed(stream) + whole.finish();
            QVERIFY(!whole.overflowed());

            SseParser chunked;
            QList<SseParser::Event> actual;
            qsizetype pos = 0;
            while (pos < stream.size()) {
                if (nextU64() % 5 == 0)
                    actual += chunked.feed(QByteArray());
                const qsizetype step = 1 + qsizetype(nextU64() % 17);
                actual += chunked.feed(stream.mid(pos, step));
                pos += step;
            }
            actual += chunked.finish();
            QVERIFY(!chunked.overflowed());
            QCOMPARE(actual.size(), expected.size());
            for (qsizetype i = 0; i < expected.size(); ++i) {
                QCOMPARE(actual[i].name, expected[i].name);
                QCOMPARE(actual[i].data, expected[i].data);
            }
        }

        // 2. 300 deterministic adversarial / mutated binary chunk streams: no crash, UB, or runaway buffer.
        const QByteArray alphabet = QByteArrayLiteral("event:data:\r\n \t\0\xEF\xBB\xBF\xFF{}[]\"a1");
        for (int iter = 0; iter < 300; ++iter) {
            SseParser parser;
            const int numChunks = 1 + int(nextU64() % 24);
            for (int c = 0; c < numChunks; ++c) {
                const qsizetype len = qsizetype(nextU64() % 64);
                QByteArray chunk(len, Qt::Uninitialized);
                for (qsizetype i = 0; i < len; ++i)
                    chunk[i] = alphabet[qsizetype(nextU64() % quint64(alphabet.size()))];
                (void)parser.feed(chunk);
            }
            (void)parser.finish();
        }

        // 3. Multi-chunk overflow cap (> kMaxBufferBytes without delimiter) sets overflowed() cleanly without OOM.
        {
            SseParser capped;
            const QByteArray block(65536, 'x');
            const int chunksNeeded = int(SseParser::kMaxBufferBytes / block.size()) + 2;
            for (int i = 0; i < chunksNeeded; ++i)
                (void)capped.feed(block);
            QVERIFY(capped.overflowed());
            QVERIFY(capped.feed("data: after-overflow\n\n").isEmpty());
            QVERIFY(capped.finish().isEmpty());
        }
    }

    void aPlusArchSpeechResampleAntiAliasingAndMicroFade() {
        // 1. Speech::resample() downsampling anti-aliasing (48 kHz -> 24 kHz and 44.1 kHz -> 24 kHz).
        auto measureRms = [](const QVector<float> &buf, qsizetype trim) -> double {
            const qsizetype start = qMin(trim, buf.size() / 4);
            const qsizetype end = qMax(start + 1, buf.size() - start);
            double sumSq = 0.0;
            for (qsizetype i = start; i < end; ++i)
                sumSq += double(buf[i]) * double(buf[i]);
            return std::sqrt(sumSq / double(end - start));
        };
        auto makeSine = [](int rate, double freqHz, int numSamples) -> QVector<float> {
            QVector<float> buf(numSamples);
            for (int n = 0; n < numSamples; ++n)
                buf[n] = float(std::sin(2.0 * M_PI * freqHz * double(n) / double(rate)));
            return buf;
        };

        // 48 kHz -> 24 kHz (target Nyquist = 12 kHz):
        // Passband 1 kHz sine wave must be preserved accurately (~0 dB).
        const QVector<float> pass48 = makeSine(48000, 1000.0, 24000);
        const QVector<float> pass24 = Speech::resample(pass48, 48000, 24000);
        QCOMPARE(pass24.size(), qsizetype(12000));
        const double passDb = 20.0 * std::log10(measureRms(pass24, 64) / measureRms(pass48, 128));
        QVERIFY2(std::abs(passDb) < 0.25, qPrintable(QStringLiteral("1 kHz passband gain: %1 dB").arg(passDb)));

        // Stopband sine waves above 12 kHz Nyquist (15 kHz, 16 kHz, 18 kHz) must be attenuated by > 25 dB.
        for (const double stopFreq : {15000.0, 16000.0, 18000.0}) {
            const QVector<float> stop48 = makeSine(48000, stopFreq, 24000);
            const QVector<float> stop24 = Speech::resample(stop48, 48000, 24000);
            const double rmsIn = measureRms(stop48, 128);
            const double rmsOut = measureRms(stop24, 64);
            const double attenDb = -20.0 * std::log10(qMax(rmsOut, 1e-12) / rmsIn);
            QVERIFY2(attenDb > 25.0,
                     qPrintable(QStringLiteral("48->24 kHz %1 Hz attenuation only %2 dB").arg(stopFreq).arg(attenDb)));
        }

        // Non-integer ratio 44.1 kHz -> 24 kHz: 1 kHz passband preserved, 16/18 kHz attenuated > 25 dB.
        const QVector<float> pass44 = makeSine(44100, 1000.0, 22050);
        const QVector<float> pass44To24 = Speech::resample(pass44, 44100, 24000);
        const double pass44Db = 20.0 * std::log10(measureRms(pass44To24, 64) / measureRms(pass44, 128));
        QVERIFY2(std::abs(pass44Db) < 0.25, qPrintable(QStringLiteral("44.1->24 passband gain: %1 dB").arg(pass44Db)));

        for (const double stopFreq : {16000.0, 18000.0}) {
            const QVector<float> stop44 = makeSine(44100, stopFreq, 22050);
            const QVector<float> stop44To24 = Speech::resample(stop44, 44100, 24000);
            const double attenDb = -20.0 * std::log10(qMax(measureRms(stop44To24, 64), 1e-12) / measureRms(stop44, 128));
            QVERIFY2(attenDb > 25.0,
                     qPrintable(QStringLiteral("44.1->24 kHz %1 Hz attenuation only %2 dB").arg(stopFreq).arg(attenDb)));
        }

        // Unity DC gain on downsampling.
        const QVector<float> dc48(4800, 0.75f);
        const QVector<float> dc24 = Speech::resample(dc48, 48000, 24000);
        QCOMPARE(dc24.size(), qsizetype(2400));
        for (qsizetype i = 0; i < dc24.size(); ++i)
            QVERIFY2(std::abs(dc24[i] - 0.75f) < 1e-4f, qPrintable(QStringLiteral("DC drift at %1: %2").arg(i).arg(dc24[i])));

        // 2. LocalWorker::run() 2 ms micro-fade-out eliminates DC-offset clicks at sentence boundaries.
        class DcEngine : public TtsEngine {
        public:
            QString prepare(const VoicePacks::Model &) override { return {}; }
            int sampleRate() const override { return 24000; }
            QVector<float> speak(const QString &, int, const std::atomic_bool &stop) override {
                if (stop)
                    return {};
                return QVector<float>(400, 0.8f);
            }
        };

        QTemporaryDir dir;
        VoicePacks packs(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                         packsManifest(QStringLiteral("http://x")));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("engine-arm"));
        fakeInstall(dir.filePath(QStringLiteral("voices")), QStringLiteral("kokoro-fp32"));
        LocalSpeech local(&packs, nullptr, [](const QString &) { return std::make_unique<DcEngine>(); });

        Ai::SpeechRequest req;
        req.voice = QStringLiteral("builtin:kokoro:bm_george");
        req.sentences = {QStringLiteral("Hello world.")};
        std::unique_ptr<Ai::Reply> reply(local.synthesize(req));
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
        QVERIFY(!reply->error());

        const QByteArray pcm = reply->audio();
        const qsizetype totalSamples = pcm.size() / 2;
        QCOMPARE(totalSamples, qsizetype(400 + qsizetype(0.32 * 24000)));
        const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
        const qint16 expectedDc = qint16(std::lround(0.8f * 32767.0f));
        QVERIFY2(std::abs(s[100] - expectedDc) <= 2, qPrintable(QString::number(s[100])));
        QVERIFY2(std::abs(s[351] - expectedDc) <= 2, qPrintable(QString::number(s[351])));
        // Last 48 samples [352..399] fade monotonically down to 0 before silence padding [400..].
        for (qsizetype i = 352; i < 400; ++i)
            QVERIFY2(s[i] <= s[i - 1],
                     qPrintable(QStringLiteral("Non-monotonic fade at %1: %2 > %3").arg(i).arg(s[i]).arg(s[i - 1])));
        QCOMPARE(s[399], qint16(0));
        QCOMPARE(s[400], qint16(0));
        // Maximum sample-to-sample step across the fade and boundary into silence is smooth (no abrupt click).
        int maxStep = 0;
        for (qsizetype i = 352; i <= 405; ++i)
            maxStep = qMax(maxStep, std::abs(int(s[i]) - int(s[i - 1])));
        QVERIFY2(maxStep < 1000, qPrintable(QStringLiteral("Abrupt boundary click step: %1").arg(maxStep)));
    }
    // === end a-plus-arch tests ===

    // === a-plus-core tests ===
    void aPlusCoreBookSchemeCspAndAsync();
    void aPlusCoreMarksAnchoringHealingAndUndo();
    void aPlusCoreTranslatorFastPathAndPlurals();
    void aPlusCoreTokenizerAndCoverProviderGc();
    void aPlusCoreParserFuzzStress();
    // === end a-plus-core tests ===

    // === a-plus-craft tests ===
    void aPlusCraftBridgeAccessibilityAndReducedMotion() {
        const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
        QVERIFY(!srcRoot.isEmpty());
        auto readSrc = [&](const QString &rel) -> QString {
            QFile f(srcRoot + QLatin1Char('/') + rel);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };

        // 1. EpubReader.qml: zero new Function / eval / brittle string replace; typed dispatch + keyboard reachability.
        const QString erQml = readSrc(QStringLiteral("ui/reader/EpubReader.qml"));
        QVERIFY(!erQml.isEmpty());
        QVERIFY2(!erQml.contains(QStringLiteral("new Function")), "EpubReader.qml must not use new Function");
        QVERIFY2(!erQml.contains(QStringLiteral("eval(")), "EpubReader.qml must not use eval()");
        QVERIFY2(!erQml.contains(QStringLiteral("code.replace(\"clothbound.marks(\"")),
                 "EpubReader.qml must not use brittle clothbound.marks string replace");
        QVERIFY(erQml.contains(QStringLiteral("function surfaceOpen(")));
        QVERIFY(erQml.contains(QStringLiteral("function surfaceFound(")));
        QVERIFY(erQml.contains(QStringLiteral("function surfaceMarks(")));
        QVERIFY(erQml.contains(QStringLiteral("function surfaceGoTo(")));
        QVERIFY(erQml.contains(QStringLiteral("readonly property bool chromeFocused:")));
        QVERIFY(erQml.contains(QStringLiteral("readonly property bool readingViewportFocused:")));
        QVERIFY(erQml.contains(QStringLiteral("tabFocusable: view.chromeShown")));
        QVERIFY(erQml.contains(QStringLiteral("enabled: view.keys && view.readingViewportFocused")));

        // 2. Accessibility roles & descriptive names across primary interactive surfaces.
        const QString bookCardQml = readSrc(QStringLiteral("ui/library/BookCard.qml"));
        QVERIFY(bookCardQml.contains(QStringLiteral("Accessible.role: Accessible.ListItem")));
        QVERIFY(bookCardQml.contains(QStringLiteral("Accessible.name:")));

        const QString libraryViewQml = readSrc(QStringLiteral("ui/library/LibraryView.qml"));
        QVERIFY(libraryViewQml.contains(QStringLiteral("Accessible.role: Accessible.ListItem")));
        QVERIFY(libraryViewQml.contains(QStringLiteral("Accessible.name:")));

        const QString continueShelfQml = readSrc(QStringLiteral("ui/library/ContinueShelf.qml"));
        QVERIFY(continueShelfQml.contains(QStringLiteral("Accessible.role: Accessible.Button")));
        QVERIFY(continueShelfQml.count(QStringLiteral("Accessible.name:")) >= 3);

        const QString cmdPaletteQml = readSrc(QStringLiteral("ui/CommandPalette.qml"));
        QVERIFY(cmdPaletteQml.contains(QStringLiteral("Accessible.role: Accessible.ListItem")));
        QVERIFY(cmdPaletteQml.contains(QStringLiteral("Accessible.name:")));

        const QString companionQml = readSrc(QStringLiteral("ui/companion/CompanionPanel.qml"));
        QVERIFY(companionQml.contains(QStringLiteral("Accessible.role: Accessible.EditableText")));
        QVERIFY(companionQml.contains(QStringLiteral("Accessible.name: questionPlaceholder.text")));

        const QString listenOfferQml = readSrc(QStringLiteral("ui/audio/ListenOffer.qml"));
        QVERIFY(listenOfferQml.contains(QStringLiteral("FocusScope {")));
        QVERIFY(listenOfferQml.contains(QStringLiteral("Keys.onEscapePressed:")));
        QVERIFY(listenOfferQml.contains(QStringLiteral("Accessible.role: Accessible.Dialog")));

        // 3. Reduced motion across views, ListenBar sleepMoon gating, and Clothbound style controls.
        const QString skeletonQml = readSrc(QStringLiteral("ui/components/Skeleton.qml"));
        QVERIFY(skeletonQml.contains(QStringLiteral("!theme.reducedMotion")));

        const QString welcomeQml = readSrc(QStringLiteral("ui/Welcome.qml"));
        QVERIFY(welcomeQml.contains(QStringLiteral("theme.reducedMotion ? 0 : 150")));

        QVERIFY(cmdPaletteQml.contains(QStringLiteral("theme.reducedMotion ? 0 : 140")));

        const QString bookDetailsQml = readSrc(QStringLiteral("ui/library/BookDetails.qml"));
        QVERIFY(bookDetailsQml.contains(QStringLiteral("theme.reducedMotion")));

        const QString listenBarQml = readSrc(QStringLiteral("ui/audio/ListenBar.qml"));
        QVERIFY(listenBarQml.contains(QStringLiteral("running: narration.sleepFading && expandedCol.visible && !theme.reducedMotion")));
        QVERIFY(!listenBarQml.contains(QStringLiteral("bar.visible && !theme.reducedMotion")));
        QVERIFY(listenBarQml.contains(QStringLiteral("enabled: !theme.reducedMotion")));

        for (const QString &ctrl : {QStringLiteral("Button.qml"), QStringLiteral("Popup.qml"),
                                    QStringLiteral("Menu.qml"), QStringLiteral("ToolTip.qml"),
                                    QStringLiteral("Slider.qml"), QStringLiteral("ProgressBar.qml"),
                                    QStringLiteral("ScrollBar.qml")}) {
            const QString qml = readSrc(QStringLiteral("ui/style/Clothbound/") + ctrl);
            QVERIFY2(qml.contains(QStringLiteral("theme.reducedMotion")), qPrintable(ctrl));
        }

        // 4. Live ListenOffer FocusScope instantiation + Esc dismissal (zero QML warnings).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices ai(&gemini, &keys, library.database().connectionName());
        AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&ai, &audioCache, &reader, &library);
        narration.m_player->setHeadless(true);
        VoicePacks voicePacks(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                              packsManifest(QStringLiteral("http://127.0.0.1/")));
        LocalSpeech localSpeech(&voicePacks);
        VoiceCatalog voices(&ai, &audioCache, &reader, library.database().connectionName(),
                            dir.filePath(QStringLiteral("voices-cache.json")));
        Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
        Translator translator;

        QQmlEngine engine;
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
        engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
        engine.rootContext()->setContextProperty(QStringLiteral("voicePacks"), &voicePacks);
        engine.rootContext()->setContextProperty(QStringLiteral("localSpeech"), &localSpeech);
        engine.rootContext()->setContextProperty(QStringLiteral("voices"), &voices);
        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent offerComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/audio/ListenOffer.qml")));
        QVERIFY2(offerComp.status() == QQmlComponent::Ready, qPrintable(offerComp.errorString()));
        QQuickWindow win;
        win.resize(900, 640);
        std::unique_ptr<QObject> offerObj(offerComp.create());
        QVERIFY(offerObj != nullptr);
        auto *offerItem = qobject_cast<QQuickItem *>(offerObj.get());
        QVERIFY(offerItem != nullptr);
        QVERIFY(offerItem->isFocusScope());
        offerItem->setParentItem(win.contentItem());
        QSignalSpy closedSpy(offerItem, SIGNAL(closed()));
        QVERIFY(QMetaObject::invokeMethod(offerItem, "show"));
        QCOMPARE(offerItem->property("open").toBool(), true);
        QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(offerItem, &escEvent);
        QCOMPARE(offerItem->property("open").toBool(), false);
        QCOMPARE(closedSpy.count(), 1);
        QVERIFY2(warnings.isEmpty(),
                 qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
    }

    void aPlusCraftHotPathsCommandPaletteCatalogsPdfAndReader() {
        const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
        QVERIFY(!srcRoot.isEmpty());
        auto readSrc = [&](const QString &rel) -> QString {
            QFile f(srcRoot + QLatin1Char('/') + rel);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };

        // 1. CatalogsSheet scroll preservation + PdfSurface O(1) indexAt & drag-select coalescer + single-open Reader::openPdf.
        const QString catalogsQml = readSrc(QStringLiteral("ui/library/CatalogsSheet.qml"));
        QVERIFY(catalogsQml.contains(QStringLiteral("function syncFeed()")));
        QVERIFY(catalogsQml.contains(QStringLiteral("model: sheet.feedEntries")));
        QVERIFY(catalogsQml.contains(QStringLiteral("Qt.callLater(restoreScroll)")));

        const QString pdfSurfaceQml = readSrc(QStringLiteral("ui/reader/PdfSurface.qml"));
        QVERIFY(pdfSurfaceQml.contains(QStringLiteral("pageList.indexAt(")));
        QVERIFY2(!pdfSurfaceQml.contains(QStringLiteral("for (let i = 0; i < pageList.count")),
                 "PdfSurface.qml must not scan pageList linearly");
        QVERIFY(pdfSurfaceQml.contains(QStringLiteral("id: dragSelectTimer")));
        QVERIFY(pdfSurfaceQml.contains(QStringLiteral("function flushPendingSelect()")));
        QVERIFY(pdfSurfaceQml.contains(QStringLiteral("function applySelection(")));

        const QString readerCpp = readSrc(QStringLiteral("reader/reader.cpp"));
        QVERIFY2(!readerCpp.contains(QStringLiteral("PdfBook own(path)")),
                 "Reader::openPdf() must reuse m_pdf instead of opening redundant PdfBook instances");

        // 2. Live CommandPalette pre-normalized search cache + diacritic-insensitive matching.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        auto makeSimpleEpub = [](const QByteArray &title, const QByteArray &author, const QByteArray &body) {
            const QByteArray opf =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                "    <dc:title>" + title + "</dc:title>\n"
                "    <dc:creator>" + author + "</dc:creator>\n"
                "    <dc:language>en</dc:language>\n"
                "  </metadata>\n"
                "  <manifest>\n"
                "    <item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                "  </manifest>\n"
                "  <spine><itemref idref=\"ch1\"/></spine>\n"
                "</package>";
            return TestBooks::epub({
                {QStringLiteral("OEBPS/content.opf"), opf},
                {QStringLiteral("OEBPS/ch1.xhtml"), TestBooks::chapter(title, body)},
            });
        };
        const QString epub1 = dir.filePath(QStringLiteral("miserables.epub"));
        const QString epub2 = dir.filePath(QStringLiteral("uber.epub"));
        QVERIFY(TestBooks::write(epub1, makeSimpleEpub("Les Misérables", "Victor Hugo",
                                                       "<p>Jean Valjean walked on.</p>")));
        QVERIFY(TestBooks::write(epub2, makeSimpleEpub("Über die Liebe", "Rainer Maria Rilke",
                                                       "<p>Ein ruhiger Abend.</p>")));

        // Build a minimal 2-page PDF embedding a DOI so we also test m_pdf citation reuse & unified init.
        auto makeDoiPdf = []() {
            QByteArray out = "%PDF-1.4\n";
            QList<qsizetype> offsets;
            auto object = [&](const QByteArray &body) {
                offsets.append(out.size());
                out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
            };
            object("<< /Type /Catalog /Pages 2 0 R >>");
            object("<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>");
            object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R "
                   "/Resources << /Font << /F1 5 0 R >> >> >>");
            const QByteArray s1 = "BT /F1 11 Tf 36 340 Td (Attention Is All You Need DOI: 10.1145/9876543.2109876) Tj ET";
            object("<< /Length " + QByteArray::number(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");
            object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
            object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 7 0 R "
                   "/Resources << /Font << /F1 5 0 R >> >> >>");
            const QByteArray s2 = "BT /F1 11 Tf 36 340 Td (Second page of the paper with body text.) Tj ET";
            object("<< /Length " + QByteArray::number(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");
            const qsizetype xref = out.size();
            out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
            for (const qsizetype off : offsets)
                out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
            out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
                   QByteArray::number(xref) + "\n%%EOF\n";
            return out;
        };
        const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
        QVERIFY(TestBooks::write(pdfPath, makeDoiPdf()));

        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        QVERIFY(library.addBookFile(epub1) > 0);
        QVERIFY(library.addBookFile(epub2) > 0);
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(pdfId > 0);

        BookScheme scheme;
        Reader reader(&library, &scheme);
        QSignalSpy lengthsSpy(&reader, &Reader::lengthsChanged);
        QVERIFY(reader.openBook(pdfId));
        QTRY_VERIFY_WITH_TIMEOUT(lengthsSpy.count() >= 1, 5000);
        QCOMPARE(library.chapterLengths(pdfId).size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!library.bookSetting(pdfId, QStringLiteral("pdf_crop")).isEmpty(), 5000);

        // Remove the PDF file on disk while it remains open in Reader: formatBookCitation()
        // called with only `path` (and id=0) must still enrich the DOI from the already-open m_pdf.
        QVERIFY(QFile::remove(pdfPath));
        const QString bibtex = reader.formatBookCitation(
            QVariantMap{
                {QStringLiteral("id"), 0},
                {QStringLiteral("title"), QStringLiteral("Attention Is All You Need")},
                {QStringLiteral("author"), QStringLiteral("Ashish Vaswani")},
                {QStringLiteral("format"), QStringLiteral("pdf")},
                {QStringLiteral("path"), pdfPath},
            },
            QStringLiteral("bibtex"));
        QVERIFY2(bibtex.contains(QStringLiteral("10.1145/9876543.2109876")), qPrintable(bibtex));

        // CommandPalette live pre-normalized search test.
        Marks marks(&library);
        BookSearch bookSearch(&reader);
        LibraryIndex libraryIndex(cacheDir + QStringLiteral("/index.sqlite"));
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices ai(&gemini, &keys, library.database().connectionName());
        AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&ai, &audioCache, &reader, &library);
        narration.m_player->setHeadless(true);
        Companion companion(&ai, &reader, library.database().connectionName());
        Illustrator illustrator(&ai, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("pictures")));
        LocalSetup localSetup(&ai);
        Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
        Translator translator;

        QQmlEngine engine;
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
        engine.rootContext()->setContextProperty(QStringLiteral("libraryIndex"), &libraryIndex);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
        engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
        engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
        engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
        engine.rootContext()->setContextProperty(QStringLiteral("companion"), &companion);
        engine.rootContext()->setContextProperty(QStringLiteral("illustrator"), &illustrator);
        engine.rootContext()->setContextProperty(QStringLiteral("localSetup"), &localSetup);

        QQmlComponent cpComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/CommandPalette.qml")));
        QVERIFY2(cpComp.status() == QQmlComponent::Ready, qPrintable(cpComp.errorString()));
        std::unique_ptr<QObject> cpObj(cpComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 760.0},
        }));
        QVERIFY(cpObj != nullptr);
        QMetaObject::invokeMethod(cpObj.get(), "show");
        const QVariantList cached = cpObj->property("cachedBooks").toList();
        QVERIFY(cached.size() >= 2);
        bool sawMiserables = false;
        bool sawUber = false;
        for (const QVariant &v : cached) {
            const QVariantMap entry = v.toMap();
            const QString normTitle = entry.value(QStringLiteral("normTitle")).toString();
            QVERIFY(!entry.value(QStringLiteral("book")).toMap().isEmpty());
            QVERIFY(!entry.value(QStringLiteral("normFull")).toString().isEmpty());
            if (normTitle == QStringLiteral("les miserables"))
                sawMiserables = true;
            if (normTitle == QStringLiteral("uber die liebe"))
                sawUber = true;
        }
        QVERIFY(sawMiserables);
        QVERIFY(sawUber);

        QMetaObject::invokeMethod(cpObj.get(), "search", Q_ARG(QVariant, QStringLiteral("miserables")));
        const QVariantList results = cpObj->property("results").toList();
        bool matchedBook = false;
        for (const QVariant &r : results) {
            const QVariantMap row = r.toMap();
            if (row.value(QStringLiteral("kind")).toString() == QStringLiteral("book") &&
                row.value(QStringLiteral("title")).toString() == QStringLiteral("Les Misérables")) {
                matchedBook = true;
                break;
            }
        }
        QVERIFY(matchedBook);
        QMetaObject::invokeMethod(cpObj.get(), "close");
        QVERIFY(cpObj->property("cachedBooks").toList().isEmpty());

        const QString cpQml = readSrc(QStringLiteral("ui/CommandPalette.qml"));
        QCOMPARE(cpQml.count(QStringLiteral(".normalize(\"NFD\")")), 1);
        QVERIFY(cpQml.contains(QStringLiteral("function buildBookCache()")));
        QVERIFY(cpQml.contains(QStringLiteral("function fuzzyScoreNormalized(")));
    }

    void aPlusCraftReaderJsSwipeQueueAndScrollQuiet() {
        const QString nodeBin = QStandardPaths::findExecutable(QStringLiteral("node"));
        QVERIFY2(!nodeBin.isEmpty(), "node executable required for reader.js verification");

        const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
        QVERIFY(QFileInfo::exists(readerJsPath));

        const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;
let wheelHandler = null;
let fakeNow = 1000;
let nextTimerId = 1;
const timers = new Map();

function fakeSetTimeout(fn, delay) {
    const id = nextTimerId++;
    timers.set(id, { fn, at: fakeNow + Math.max(0, Number(delay) || 0) });
    return id;
}
function fakeClearTimeout(id) { timers.delete(id); }
function advanceTimers(ms) {
    fakeNow += ms;
    while (true) {
        let earliestId = null, earliest = null;
        for (const [id, entry] of timers.entries()) {
            if (entry.at <= fakeNow && (!earliest || entry.at < earliest.at)) {
                earliestId = id;
                earliest = entry;
            }
        }
        if (!earliest) break;
        timers.delete(earliestId);
        earliest.fn();
    }
}

let nextRafId = 1;
const rafQueue = new Map();
function fakeRaf(cb) {
    const id = nextRafId++;
    rafQueue.set(id, cb);
    queueMicrotask(() => {
        const fn = rafQueue.get(id);
        if (!fn) return;
        rafQueue.delete(id);
        fakeNow += 120;
        fn(fakeNow);
    });
    return id;
}
function fakeCancelRaf(id) { rafQueue.delete(id); }

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        willChange: '', colorScheme: '', left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}
function makeClassList() {
    const s = new Set();
    return { add(c) { s.add(c); }, remove(c) { s.delete(c); }, contains(c) { return s.has(c); } };
}

function makeDoc(ownerFrame, url, pages) {
    const headChildren = [];
    const byId = new Map();
    const textNode = { nodeType: 3, data: 'Chapter text for swipe serialization and scroll quiet tests.', parentElement: null };
    const bodyEl = {
        childNodes: [textNode],
        lastElementChild: {
            getClientRects() { return [{ left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }]; },
            getBoundingClientRect() { return { left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }; },
        },
    };
    textNode.parentElement = bodyEl;
    const docEl = {
        style: makeStyle(),
        get scrollWidth() { return pages * (parseFloat(ownerFrame.style.width) || 680); },
        scrollHeight: 600,
        clientHeight: 600,
        scrollLeft: 0,
        scrollTop: 0,
        scrollTo(opts) {
            if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
            if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
        },
        appendChild() {},
    };
    return {
        URL: url,
        documentElement: docEl,
        head: { appendChild(el) { headChildren.push(el); if (el.id) byId.set(el.id, el); } },
        body: bodyEl,
        scrollingElement: docEl,
        fonts: { ready: Promise.resolve() },
        images: [],
        defaultView: {
            getComputedStyle() { return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start', whiteSpace: 'normal' }; },
            CSS: { highlights: new Map() },
            Highlight: class { constructor(...r) { this.ranges = r; this.priority = 0; } },
        },
        getElementById(id) { return byId.get(id) || null; },
        createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
        querySelector() { return null; },
        querySelectorAll() { return []; },
        addEventListener() {},
        removeEventListener() {},
        createTreeWalker() {
            let done = false;
            return { nextNode() { if (done) return null; done = true; return textNode; } };
        },
        createRange() {
            return {
                selectNodeContents() {}, setStart() {}, setEnd() {},
                getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
                getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
            };
        },
        getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
    };
}

let deferredResolve = null;
const pageCounts = new Map([
    ['clothbound://book/1/ch0.xhtml', 2],
    ['clothbound://book/1/ch1.xhtml', 1],
    ['clothbound://book/1/ch2.xhtml', 1],
]);

function makeFrame(id) {
    const f = {
        id, style: makeStyle(), classList: makeClassList(),
        contentDocument: null, contentWindow: null, onload: null, onerror: null, _src: 'about:blank',
        setAttribute() {}, removeAttribute() {},
        getBoundingClientRect() { return { left: 160, top: 54, width: parseFloat(this.style.width) || 680, height: 692 }; },
    };
    Object.defineProperty(f, 'src', {
        get() { return this._src; },
        set(v) {
            this._src = v;
            if (!v || v === 'about:blank') { this.contentDocument = null; this.contentWindow = null; return; }
            const finish = () => {
                const doc = makeDoc(this, v, pageCounts.get(v) || 1);
                this.contentDocument = doc;
                this.contentWindow = doc.defaultView;
                if (typeof this.onload === 'function') this.onload();
            };
            if (deferredResolve === 'armed') {
                deferredResolve = finish;
            } else {
                queueMicrotask(finish);
            }
        },
    });
    return f;
}

const stageEl = {
    clientWidth: 1200, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 1200, height: 800 }; },
};
const viewportEl = { style: makeStyle(), classList: makeClassList() };
const bookFrame = makeFrame('book');
const backFrame = makeFrame('book-back');
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'viewport') return viewportEl;
        if (id === 'book') return bookFrame;
        if (id === 'book-back') return backFrame;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement() { return { style: makeStyle(), setAttribute() {}, remove() {}, addEventListener() {} }; },
    addEventListener(type, fn) {
        if (type === 'DOMContentLoaded') domReady = fn;
        if (type === 'wheel') wheelHandler = fn;
    },
};

const bridge = {
    surfaceLoaded() {},
    relocated() {},
    tapped() {},
    notifyPageTurn() {},
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console, URL, Map, Set, WeakMap, Promise, Math, Number, String, Array, Object, JSON, Error, RegExp,
    document: documentMock,
    window: { innerWidth: 1200, innerHeight: 800 },
    performance: { now() { return fakeNow; } },
    setTimeout: fakeSetTimeout,
    clearTimeout: fakeClearTimeout,
    requestAnimationFrame: fakeRaf,
    cancelAnimationFrame: fakeCancelRaf,
    matchMedia() { return { matches: false }; },
    ResizeObserver: class { observe() {} },
    QWebChannel: class { constructor(_t, cb) { cb({ objects: { bridge } }); } },
    qt: { webChannelTransport: {} },
    NodeFilter: { SHOW_TEXT: 4 },
    Hyphenation: { load: async () => false },
    fetch() { return Promise.resolve({ ok: true, text: async () => '' }); },
    getComputedStyle() { return { fontSize: '18px' }; },
};
sandbox.globalThis = sandbox;
sandbox.window.document = documentMock;
vm.createContext(sandbox);
vm.runInContext(source + '\nthis.__cb = clothbound;', sandbox);
domReady();
const cb = sandbox.__cb;

const book = {
    session: 1,
    base: 'clothbound://book/1/',
    language: 'en',
    spine: [
        { href: 'ch0.xhtml', linear: true },
        { href: 'ch1.xhtml', linear: true },
        { href: 'ch2.xhtml', linear: true },
    ],
};
const pagedStyle = {
    theme: { bg: '#fff', fg: '#111', link: '#00f', muted: '#666', popover: '#fff', border: '#ccc', dark: false },
    font: 'Literata', fontScale: 1, lineHeight: 1.5, margin: 0.08, measure: 38, marginTop: 54, marginBottom: 54,
    justify: false, hyphenate: false, layout: 'single', singleColumn: true, animate: true, reducedMotion: false,
};

async function flushMicrotasks(n = 8) {
    for (let i = 0; i < n; ++i) await Promise.resolve();
}

(async () => {
    await cb.open(book, { chapter: 0, offset: 0 }, pagedStyle);
    let dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 0);
    assert.strictEqual(dbg.page, 0);
    assert.strictEqual(dbg.pages, 2);

    // 1. Mid-swipe goTo() cancels pending swipeEnd so stale swipe never snaps back to ch0.
    wheelHandler({ deltaMode: 0, deltaX: 240, deltaY: 0, preventDefault() {} });
    await cb.goTo({ chapter: 2, offset: 0, instant: true });
    advanceTimers(150);
    await flushMicrotasks();
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 2, 'stale swipe timer must not override goTo(2)');

    // 2. Horizontal swipe past threshold settles via state.queue and serializes cleanly with queued next().
    await cb.goTo({ chapter: 0, offset: 0, instant: true });
    fakeNow += 500;
    wheelHandler({ deltaMode: 0, deltaX: 260, deltaY: 0, preventDefault() {} });
    advanceTimers(100);
    const queuedNext = cb.next();
    await flushMicrotasks(16);
    await queuedNext;
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 1, 'swipe -> page 1 followed by queued next() must reach chapter 1');
    assert.strictEqual(dbg.page, 0);

    // 3. Swipe start is ignored while a queued chapter navigation is actively in flight (state.navBusy > 0).
    await cb.goTo({ chapter: 0, offset: 0, instant: true });
    fakeNow += 500;
    deferredResolve = 'armed';
    const pendingNav = cb.goTo({ chapter: 2, offset: 0, instant: true });
    await flushMicrotasks(4);
    assert.strictEqual(typeof deferredResolve, 'function');
    wheelHandler({ deltaMode: 0, deltaX: 300, deltaY: 0, preventDefault() {} });
    const resolveLoad = deferredResolve;
    deferredResolve = null;
    resolveLoad();
    await pendingNav;
    advanceTimers(150);
    await flushMicrotasks();
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 2, 'wheel swipe during active navigation must be ignored');

    // 4. Scroll-mode wheelQuietUntil >= 550ms blocks accidental momentum multi-chapter skip.
    const scrollStyle = Object.assign({}, pagedStyle, { layout: 'scroll', animate: false });
    await cb.setStyle(scrollStyle);
    await cb.goTo({ chapter: 0, offset: 0, instant: true });
    fakeNow += 700;
    wheelHandler({ deltaMode: 0, deltaX: 0, deltaY: 80, preventDefault() {} });
    await flushMicrotasks(12);
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 1, 'bottom-edge scroll should advance to chapter 1');

    // Momentum wheel 400ms later is inside 550ms cooldown -> must stay on chapter 1.
    fakeNow += 400;
    wheelHandler({ deltaMode: 0, deltaX: 0, deltaY: 80, preventDefault() {} });
    await flushMicrotasks(12);
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 1, 'scroll momentum within 550ms must not double-skip to chapter 2');

    // Intentional wheel after 550ms cooldown -> advances to chapter 2.
    fakeNow += 200;
    wheelHandler({ deltaMode: 0, deltaX: 0, deltaY: 80, preventDefault() {} });
    await flushMicrotasks(12);
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 2, 'scroll wheel after 550ms cooldown should advance to chapter 2');

    process.stdout.write('OK\n');
})().catch(err => {
    console.error(err && err.stack || err);
    process.exit(1);
});
)JS");

        QProcess proc;
        proc.start(nodeBin, {QStringLiteral("-e"), harness, readerJsPath});
        QVERIFY2(proc.waitForFinished(10000), "node a-plus-craft reader.js harness timed out");
        const QByteArray stderrOut = proc.readAllStandardError();
        QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                 stderrOut.isEmpty() ? "node a-plus-craft reader.js harness failed" : stderrOut.constData());
        QCOMPARE(proc.readAllStandardOutput().trimmed(), QByteArray("OK"));
    }
    // === end a-plus-craft tests ===

    // === final-audit-polish tests ===
    void finalAuditPolishRegressions() {
        // 1. MathFormatter CommonMark tilde fences (~~~) & backslash-escaped backticks (\`)
        //    must never desync raw HTML escaping or math extraction.
        const QString tildePayload = QStringLiteral("~~~\n```\n~~~\nhello <a href=\"http://evil.invalid\">LINK</a>");
        const QString tildeClean = MathFormatter::cleanMarkdown(tildePayload);
        QVERIFY2(!tildeClean.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(tildeClean));
        QVERIFY2(tildeClean.contains(QStringLiteral("&lt;a href=")), qPrintable(tildeClean));
        const QString tildeHtml = MathFormatter::formatDisplayHtml(tildePayload, QStringLiteral("rgba(0,0,0,0.1)"));
        QVERIFY2(!tildeHtml.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(tildeHtml));

        const QString escapedTickPayload = QStringLiteral("\\`<a href=\"http://evil.invalid\">LINK</a>`");
        const QString escapedTickClean = MathFormatter::cleanMarkdown(escapedTickPayload);
        QVERIFY2(!escapedTickClean.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(escapedTickClean));
        QVERIFY2(escapedTickClean.contains(QStringLiteral("&lt;a href=")), qPrintable(escapedTickClean));

        const QString escapedBackslashCode = QStringLiteral("\\\\`<a href=\"http://evil.invalid\">CODE</a>` <a href=\"http://evil.invalid\">OUT</a>");
        const QString escapedBsClean = MathFormatter::cleanMarkdown(escapedBackslashCode);
        QVERIFY2(escapedBsClean.contains(QStringLiteral("&lt;a href=\"http://evil.invalid\"&gt;OUT")), qPrintable(escapedBsClean));
        const QString escapedBsHtml = MathFormatter::formatDisplayHtml(escapedBackslashCode, QStringLiteral("rgba(0,0,0,0.1)"));
        QVERIFY2(!escapedBsHtml.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(escapedBsHtml));

        const QString tildeMath = QStringLiteral("~~~latex\n$x^2$\n~~~\nOutside $y^2$.");
        const QString tildeMathOut = MathFormatter::formatLatexInMarkdown(tildeMath);
        QVERIFY(tildeMathOut.contains(QStringLiteral("~~~latex\n$x^2$\n~~~")));
        QVERIFY(tildeMathOut.contains(QStringLiteral("class=\"math-inline\"")));

        // 2. McpServer::readPassageFromIndexReadOnly uses chapter_rows B-tree JOIN first
        //    and falls back cleanly to chapter_text when chapter_rows is absent.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString epubPath = dir.filePath(QStringLiteral("mcp-rows.epub"));
        QVERIFY(TestBooks::write(epubPath, TestBooks::epub({
            {"OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>MCP Rows</dc:title></metadata>"
             "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
             "<spine><itemref idref=\"c1\"/></spine></package>"},
            {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Original fallback passage text for verification.</p>")},
        })));
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        const QString indexDb = dir.filePath(QStringLiteral("index.sqlite"));
        const QString settingsIni = dir.filePath(QStringLiteral("mcp.ini"));
        LibraryIndex idx(indexDb);
        QVERIFY(idx.available());
        QVERIFY(LibraryIndex::indexBook(indexDb, {bookId, epubPath, QStringLiteral("epub"), 1}));

        // Point chapter_rows(bookId, 0) to a distinct FTS rowid whose chapter_text.chapter is 99
        // to prove the fast chapter_rows JOIN runs before the unindexed chapter_text scan.
        const QString conn = QStringLiteral("final-audit-mcp-rows");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(indexDb);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO chapter_text (book, chapter, body) VALUES (%1, 99, 'Fast chapter_rows indexed passage hit.')")
                               .arg(bookId)));
            const qint64 fastRowId = q.lastInsertId().toLongLong();
            QVERIFY(fastRowId > 0);
            QVERIFY(q.exec(QStringLiteral("UPDATE chapter_rows SET fts_rowid = %1 WHERE book = %2 AND chapter = 0")
                               .arg(fastRowId)
                               .arg(bookId)));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        McpServer server(&library, nullptr, nullptr, {settingsIni, indexDb});
        const QString fastHit = server.extractPassageText(bookId, 0, 0, 80);
        QCOMPARE(fastHit, QStringLiteral("Fast chapter_rows indexed passage hit."));

        // Drop chapter_rows (simulating pre-migration index.sqlite) and verify clean fallback.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(indexDb);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("DROP TABLE chapter_rows")));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        const QString fallbackHit = server.extractPassageText(bookId, 0, 0, 80);
        QVERIFY2(fallbackHit.contains(QStringLiteral("Original fallback passage text")), qPrintable(fallbackHit));

        // 3. Reader::openPdf rapid open+close stress exercising MainThreadRelease{std::move(book)}.
        const QString pdfPath = dir.filePath(QStringLiteral("rapid.pdf"));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Rapid Close", "Ada Marsh")));
        const int pdfId = library.addBookFile(pdfPath);
        QVERIFY(pdfId > 0);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        for (int iter = 0; iter < 8; ++iter) {
            QVERIFY(reader.openBook(pdfId));
            reader.close();
            QCoreApplication::processEvents();
        }
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
    }
    // === end final-audit-polish tests ===

    // === w5-core-lib tests ===
    void w5CoreLibNoReaderCycleAndDecoupledBookSearch();
    void w5CoreLibSchemaTelemetryAndReadOnlyOpen();
    void w5CoreLibFastEpubImportScanPerformance();
    // === end w5-core-lib tests ===
};

#ifdef CLOTHBOUND_UNIFIED_TESTS
std::unique_ptr<QObject> createOnlineIngestTests();
std::unique_ptr<QObject> createOpdsTests();
#endif

int main(int argc, char *argv[]) {
    // Always avoid GTK3 platform theme (crashes headless without a display).
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    // Hermetic XDG isolation: tests never touch ~/.config/clothbound/settings.ini
    // or collide with a live Clothbound D-Bus instance.
    QTemporaryDir testXdg;
    if (testXdg.isValid()) {
        QDir(testXdg.path()).mkpath("config");
        QDir(testXdg.path()).mkpath("data");
        QDir(testXdg.path()).mkpath("cache");
        qputenv("XDG_CONFIG_HOME", testXdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", testXdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", testXdg.filePath("cache").toUtf8());
    }
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clothbound"));
    app.setAttribute(Qt::AA_Use96Dpi, true);
    QQuickStyle::setStyle(QStringLiteral("Clothbound"));
    QTEST_SET_MAIN_SOURCE_PATH

    std::vector<std::unique_ptr<QObject>> suites;
    suites.push_back(std::make_unique<ClothboundTests>());
#ifdef CLOTHBOUND_UNIFIED_TESTS
    suites.push_back(createOnlineIngestTests());
    suites.push_back(createOpdsTests());
#endif

    // Collect non-flag positional test function filters (e.g. `./bin/test myTest`).
    QStringList filterNames;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1Char('-'))) {
            // Skip value of two-token QtTest flags like `-o file,txt`.
            if (arg == QLatin1String("-o") || arg == QLatin1String("-eventdelay") || arg == QLatin1String("-keydelay") ||
                arg == QLatin1String("-mousedelay") || arg == QLatin1String("-maxwarnings") ||
                arg == QLatin1String("-iterations") || arg == QLatin1String("-minimumvalue") ||
                arg == QLatin1String("-minimumtotal"))
                ++i;
            continue;
        }
        filterNames.append(arg.section(QLatin1Char(':'), 0, 0));
    }

    auto hasAnySlot = [](QObject *obj, const QStringList &names) {
        const QMetaObject *mo = obj->metaObject();
        for (const QString &name : names) {
            const QByteArray sig = (name + QStringLiteral("()")).toLatin1();
            if (mo->indexOfSlot(sig.constData()) >= 0)
                return true;
        }
        return false;
    };

    int rc = 0;
    bool ranAny = false;
    for (auto &suite : suites) {
        if (!filterNames.isEmpty() && !hasAnySlot(suite.get(), filterNames))
            continue;
        ranAny = true;
        rc |= QTest::qExec(suite.get(), argc, argv);
    }
    if (!ranAny && !suites.empty())
        rc = QTest::qExec(suites.front().get(), argc, argv);
    return rc;
}

// === a-plus-core tests ===
#include "online/articleextractor.h"

#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <random>
#include <thread>

void ClothboundTests::aPlusCoreBookSchemeCspAndAsync() {
    // 1. Strict CSP + nosniff on /app/ (reader.html surface) allowing its own scripts/styles,
    // clothbound: book frames, qwebchannel, and data:/blob: images without script unsafe-inline/eval.
    const auto &appHdrs = BookScheme::appHeaders();
    QCOMPARE(appHdrs.value("X-Content-Type-Options"), QByteArray("nosniff"));
    const QByteArray csp = appHdrs.value("Content-Security-Policy");
    QVERIFY(!csp.isEmpty());
    QVERIFY(csp.contains("default-src 'self'"));
    QVERIFY(csp.contains("script-src 'self'"));
    QVERIFY(!csp.contains("'unsafe-eval'"));
    QVERIFY(csp.contains("style-src 'self' 'unsafe-inline'"));
    QVERIFY(csp.contains("frame-src 'self' clothbound:"));
    QVERIFY(csp.contains("img-src") && csp.contains("data:") && csp.contains("blob:"));
    QVERIFY(csp.contains("object-src 'none'"));
    QVERIFY(csp.contains("form-action 'none'"));
    QVERIFY(csp.contains("frame-ancestors 'none'"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("async.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(24, 24))));
    auto book = std::make_shared<EpubBook>(epubPath);
    QVERIFY(book->open());

    BookScheme scheme;
    // qwebchannel.js is compiled into QtWebChannel resources and served via /app/.
    const auto qwcRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/app/qwebchannel.js")));
    QVERIFY(qwcRes.ok);
    QCOMPARE(qwcRes.headers.value("Content-Security-Policy"), csp);
    QCOMPARE(qwcRes.headers.value("X-Content-Type-Options"), QByteArray("nosniff"));

    const QString token = scheme.add(book);
    const QUrl chapterUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/text/chapter%201.xhtml").arg(token));
    const QUrl imgUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/images/cover.png").arg(token));

    // 2. Async resolution runs off the GUI thread and delivers callback on the main thread.
    QObject liveCtx;
    bool gotChapter = false;
    bool deliveredOnMainThread = false;
    BookScheme::ResolvedResource asyncChapter;
    scheme.resolveRequestAsync(chapterUrl, {}, &liveCtx, [&](BookScheme::ResolvedResource res) {
        deliveredOnMainThread = (QThread::currentThread() == QCoreApplication::instance()->thread());
        asyncChapter = std::move(res);
        gotChapter = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(gotChapter, 3000);
    QVERIFY(deliveredOnMainThread);
    QVERIFY(asyncChapter.ok);
    QCOMPARE(asyncChapter.mimeType, QByteArray("application/xhtml+xml"));
    QVERIFY(asyncChapter.headers.value("Content-Security-Policy").contains("script-src 'none'"));

    // 3. Cancelled/destroyed job contexts are safely ignored without invoking callback or crashing.
    constexpr int totalJobs = 20;
    QList<QObject *> alive;
    int aliveCallbacks = 0;
    int cancelledCallbacks = 0;
    for (int i = 0; i < totalJobs; ++i) {
        auto *ctx = new QObject;
        const bool cancelImmediately = (i % 2 == 0);
        const QUrl &target = (i % 3 == 0) ? imgUrl : chapterUrl;
        scheme.resolveRequestAsync(target, {}, ctx,
                                   [cancelImmediately, &aliveCallbacks, &cancelledCallbacks](BookScheme::ResolvedResource res) {
                                       if (cancelImmediately)
                                           ++cancelledCallbacks;
                                       else if (res.ok)
                                           ++aliveCallbacks;
                                   });
        if (cancelImmediately)
            delete ctx;
        else
            alive.append(ctx);
    }
    QTRY_COMPARE_WITH_TIMEOUT(aliveCallbacks, totalJobs / 2, 3000);
    QCoreApplication::processEvents();
    QCOMPARE(cancelledCallbacks, 0);
    qDeleteAll(alive);

    // 4. Destroying BookScheme while async work is queued/in-flight drains m_pool cleanly.
    {
        BookScheme shortLived;
        const QString t2 = shortLived.add(book);
        const QUrl u2(QStringLiteral("clothbound://local/book/%1/OEBPS/text/chapter%201.xhtml").arg(t2));
        QObject ctx2;
        for (int i = 0; i < 8; ++i)
            shortLived.resolveRequestAsync(u2, {}, &ctx2, [](BookScheme::ResolvedResource) {});
    }
    QCoreApplication::processEvents();
}

void ClothboundTests::aPlusCoreMarksAnchoringHealingAndUndo() {
    // 1. locateQuote() fast-path MUST validate prefix/suffix so a shifted hint landing on
    // an identical repeated phrase does NOT silently anchor to the wrong occurrence.
    const QString chapter =
        QStringLiteral("At dawn the harbor slept quietly. By noon the harbor roared with wind.");
    const qint64 firstHarbor = chapter.indexOf(QStringLiteral("the harbor"));
    const qint64 secondHarbor = chapter.indexOf(QStringLiteral("the harbor"), firstHarbor + 1);
    QVERIFY(firstHarbor >= 0 && secondHarbor > firstHarbor);

    const auto disamb = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                           QStringLiteral("By noon "), QStringLiteral(" roared"), firstHarbor);
    QCOMPARE(disamb.first, secondHarbor);
    QCOMPARE(disamb.second, secondHarbor + 10);

    // Matching hint AND matching context hits fast path accurately.
    const auto exact = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                          QStringLiteral("By noon "), QStringLiteral(" roared"), secondHarbor);
    QCOMPARE(exact.first, secondHarbor);
    QCOMPARE(exact.second, secondHarbor + 10);

    // Extreme hintStart does not overflow.
    const auto hugeHint = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                             QStringLiteral("At dawn "), QStringLiteral(" slept"),
                                             std::numeric_limits<qint64>::max());
    QCOMPARE(hugeHint.first, firstHarbor);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epub1 = dir.filePath(QStringLiteral("m1.epub"));
    const QString epub2 = dir.filePath(QStringLiteral("m2.epub"));
    QVERIFY(TestBooks::write(epub1, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epub2, TestBooks::epub3(adobeObfuscate(fontPayload()))));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int book1 = library.addBookFile(epub1);
    const int book2 = library.addBookFile(epub2);
    QVERIFY(book1 > 0 && book2 > 0);

    Marks marks(&library);
    marks.setBook(book1);

    // 2. Batched healChapter() emits a single model refresh + single changed() signal.
    const QString multiChapter = QStringLiteral(
        "Prefix text. First alpha quote here. Second beta quote here. Third gamma quote here. Fourth delta quote here.");
    const int m1 = marks.addHighlight(0, 0, 11, QStringLiteral("alpha quote"), QStringLiteral("yellow"),
                                      QStringLiteral("p. 1"), QStringLiteral("First "), QStringLiteral(" here."));
    const int m2 = marks.addHighlight(0, 1, 11, QStringLiteral("beta quote"), QStringLiteral("green"),
                                      QStringLiteral("p. 1"), QStringLiteral("Second "), QStringLiteral(" here."));
    const int m3 = marks.addHighlight(0, 2, 13, QStringLiteral("gamma quote"), QStringLiteral("blue"),
                                      QStringLiteral("p. 1"), QStringLiteral("Third "), QStringLiteral(" here."));
    const int m4 = marks.addBookmark(0, 3, QStringLiteral("delta quote"), QStringLiteral("p. 1"),
                                     QStringLiteral("Fourth "), QStringLiteral(" here."));
    QVERIFY(m1 > 0 && m2 > 0 && m3 > 0 && m4 > 0);

    QSignalSpy changedSpy(&marks, &Marks::changed);
    QSignalSpy resetSpy(&marks, &QAbstractItemModel::modelReset);
    QSignalSpy dataSpy(&marks, &QAbstractItemModel::dataChanged);
    QCOMPARE(marks.healChapter(0, multiChapter), 4);
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(resetSpy.count() + dataSpy.count(), 1);
    QCOMPARE(marks.healChapter(0, multiChapter), 0);
    QCOMPARE(changedSpy.count(), 1);

    // 3. Atomic Savepoint rollback in healChapter(): if SQLite aborts mid-batch, zero marks mutate.
    const QString shiftedChapter = QStringLiteral("INSERTED EXTRA TEXT BEFORE EVERYTHING. ") + multiChapter;
    const qint64 preStart1 = marks.mark(m1).value(QStringLiteral("start")).toLongLong();
    const qint64 preStart2 = marks.mark(m2).value(QStringLiteral("start")).toLongLong();
    {
        QSqlDatabase db = library.database();
        QSqlQuery trig(db);
        QVERIFY(trig.exec(QStringLiteral(
            "CREATE TEMP TRIGGER fail_second_heal BEFORE UPDATE ON annotations "
            "WHEN NEW.id = %1 BEGIN SELECT RAISE(ABORT, 'injected heal failure'); END")
                              .arg(m2)));
        QCOMPARE(marks.healChapter(0, shiftedChapter), 0);
        QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), preStart1);
        QCOMPARE(marks.mark(m2).value(QStringLiteral("start")).toLongLong(), preStart2);
        QSqlQuery check(db);
        QVERIFY(check.exec(QStringLiteral("SELECT start FROM annotations WHERE id = %1").arg(m1)));
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toLongLong(), preStart1);
        QVERIFY(trig.exec(QStringLiteral("DROP TRIGGER fail_second_heal")));
    }

    // 4. Undo/redo of Change (note/color) after healChapter() preserves freshly healed offsets!
    marks.setNote(m1, QStringLiteral("Café note"));
    marks.setColor(m1, QStringLiteral("pink"));
    QCOMPARE(marks.healChapter(0, shiftedChapter), 4);
    const qint64 healedStart1 = marks.mark(m1).value(QStringLiteral("start")).toLongLong();
    const qint64 healedEnd1 = marks.mark(m1).value(QStringLiteral("end")).toLongLong();
    QVERIFY(healedStart1 > preStart1);

    marks.undo();  // undo color -> yellow
    QCOMPARE(marks.mark(m1).value(QStringLiteral("color")).toString(), QStringLiteral("yellow"));
    QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), healedStart1);
    QCOMPARE(marks.mark(m1).value(QStringLiteral("end")).toLongLong(), healedEnd1);

    marks.undo();  // undo note -> ""
    QVERIFY(marks.mark(m1).value(QStringLiteral("note")).toString().isEmpty());
    QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), healedStart1);
    QCOMPARE(marks.mark(m1).value(QStringLiteral("end")).toLongLong(), healedEnd1);

    marks.redo();  // redo note -> "Café note"
    QCOMPARE(marks.mark(m1).value(QStringLiteral("note")).toString(), QStringLiteral("Café note"));
    QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), healedStart1);
    QCOMPARE(marks.mark(m1).value(QStringLiteral("end")).toLongLong(), healedEnd1);

    // 5. searchAll() cache serves repeated queries accurately and reflects mutations immediately.
    QCOMPARE(marks.searchAll(QStringLiteral("cafe")).size(), 1);
    QCOMPARE(marks.searchAll(QStringLiteral("cafe")).size(), 1);
    marks.setNote(m1, QStringLiteral("Updated periwinkle remark"));
    QCOMPARE(marks.searchAll(QStringLiteral("cafe")).size(), 0);
    QCOMPARE(marks.searchAll(QStringLiteral("periwinkle")).size(), 1);

    // 6. annotations.id undo-of-delete guards against rowid reuse (both in-session and external).
    const int latest = marks.addHighlight(0, 5, 15, QStringLiteral("reusable"), QStringLiteral("yellow"),
                                          QStringLiteral("p. 2"));
    marks.setNote(latest, QStringLiteral("Keep across collision"));
    marks.remove(latest);

    // Same-session add must NOT reuse the deleted mark's id.
    const int nextAdded = marks.addHighlight(0, 16, 26, QStringLiteral("brand new"), QStringLiteral("green"),
                                             QStringLiteral("p. 2"));
    QVERIFY(nextAdded > 0 && nextAdded != latest);
    marks.undo();  // remove brand new

    // Simulate an external writer (e.g. MCP / another book) stealing `latest`'s rowid in SQLite.
    {
        QSqlQuery hijack(library.database());
        hijack.prepare(QStringLiteral(
            "INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color, label, "
            "prefix, suffix, created_at, updated_at) "
            "VALUES (?, ?, 'highlight', 0, 1, 5, 'book2 text', 'book2 note', 'blue', 'Ch 1', '', '', 100, 100)"));
        hijack.addBindValue(latest);
        hijack.addBindValue(book2);
        QVERIFY(hijack.exec());
    }

    // Undo of delete detects collision on `latest`, allocates fresh id, and keeps book2's row intact.
    marks.undo();
    int restoredId = 0;
    for (int r = 0; r < marks.count(); ++r) {
        if (marks.data(marks.index(r), Marks::TextRole).toString() == QStringLiteral("reusable")) {
            restoredId = marks.data(marks.index(r), Marks::IdRole).toInt();
            break;
        }
    }
    QVERIFY(restoredId > 0 && restoredId != latest);
    QCOMPARE(marks.mark(restoredId).value(QStringLiteral("note")).toString(), QStringLiteral("Keep across collision"));

    // Subsequent undo of setNote targets the remapped `restoredId`, never book2's hijacked rowid!
    marks.undo();
    QVERIFY(marks.mark(restoredId).value(QStringLiteral("note")).toString().isEmpty());
    {
        QSqlQuery verifyBook2(library.database());
        verifyBook2.prepare(QStringLiteral("SELECT book_id, note FROM annotations WHERE id = ?"));
        verifyBook2.addBindValue(latest);
        QVERIFY(verifyBook2.exec() && verifyBook2.next());
        QCOMPARE(verifyBook2.value(0).toInt(), book2);
        QCOMPARE(verifyBook2.value(1).toString(), QStringLiteral("book2 note"));
    }
}

void ClothboundTests::aPlusCoreTranslatorFastPathAndPlurals() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString langFile = dir.filePath(QStringLiteral("language"));
    const QString i18nDir = QFileInfo(QFINDTESTDATA("../src/i18n")).canonicalFilePath();
    QVERIFY(!i18nDir.isEmpty());

    // 1. Default English with shipped en.json (3.1 KB plural rules): fast-path non-plurals, exact plurals.
    write(langFile, "en\n");
    Translator trEn(langFile, i18nDir);
    QCOMPARE(trEn.language(), QStringLiteral("en"));
    QVERIFY(!trEn.isEmpty());
    QVERIFY(trEn.translate("Main", "Save").isNull());
    QVERIFY(trEn.translate("Main", "Saved %1").isNull());
    QVERIFY(trEn.translate("Main", nullptr).isNull());
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 0), QStringLiteral("%n books"));
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 7), QStringLiteral("%n books"));
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, -1), QStringLiteral("%n books"));
    QCOMPARE(trEn.translate("Main", "%Ln marks", nullptr, 1), QStringLiteral("%Ln mark"));
    QCOMPARE(trEn.translate("Main", "%Ln marks", nullptr, 3), QStringLiteral("%Ln marks"));

    // 2. Switching to non-English and back preserves translations and re-enables English fast path.
    trEn.setLanguage(QStringLiteral("de"));
    QCOMPARE(trEn.language(), QStringLiteral("de"));
    QVERIFY(!trEn.translate("Main", "%n books", nullptr, 1).isEmpty());
    trEn.setLanguage(QStringLiteral("en"));
    QCOMPARE(trEn.language(), QStringLiteral("en"));
    QVERIFY(trEn.translate("Main", "Save").isNull());
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));

    // 3. Custom en.json with plain non-plural string override must NOT be bypassed.
    const QString customCat = dir.filePath(QStringLiteral("custom-i18n"));
    QVERIFY(QDir().mkpath(customCat));
    write(customCat + QStringLiteral("/en.json"), R"({"Hello": "Howdy", "%n books": {"one": "%n book", "other": "%n books"}})");
    Translator customEn(langFile, customCat);
    QCOMPARE(customEn.translate("Main", "Hello"), QStringLiteral("Howdy"));
    QCOMPARE(customEn.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));
    QVERIFY(customEn.translate("Main", "Unmapped").isNull());

    // 4. Lock-free concurrent stress: worker threads translate while main thread reloads languages.
    std::atomic_bool stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&trEn, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)trEn.translate("Main", "Save");
                (void)trEn.translate("Main", "%n books", nullptr, 1);
                (void)trEn.translate("Main", "%n books", nullptr, 5);
                (void)trEn.translate("Main", "Library");
            }
        });
    }
    const QStringList langs{QStringLiteral("de"), QStringLiteral("ru"), QStringLiteral("en"), QStringLiteral("fr")};
    for (int i = 0; i < 16; ++i)
        trEn.setLanguage(langs[i % langs.size()]);
    stop.store(true, std::memory_order_relaxed);
    for (auto &th : readers)
        th.join();
    trEn.setLanguage(QStringLiteral("en"));
}

void ClothboundTests::aPlusCoreTokenizerAndCoverProviderGc() {
    // 1. HtmlTokenizer::decodeEntities() zero-alloc lookup & surrogate handling.
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&nbsp;&Alpha;&omega;&mdash;&ZeroWidthSpace;&#x1F516;"),
             QStringLiteral("\u00A0\u0391\u03C9\u2014\u200B\U0001F516"));
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&; &#; &#x; &#0; &#xD800; &#x110000; &caf\u00E9;"),
             QStringLiteral("&; &#; &#x; \uFFFD \uFFFD \uFFFD &caf\u00E9;"));
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&thisEntityNameIsWayLongerThanFortyCharactersTotal; &#9999999999999999999;"),
             QStringLiteral("&thisEntityNameIsWayLongerThanFortyCharactersTotal; &#9999999999999999999;"));

    // 2. CoverProvider::pruneCoverCacheDirectory() enforces byte cap & file count cap, oldest first.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("covers"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QByteArray payload(1000, 'x');
    const QDateTime baseTime = QDateTime::fromSecsSinceEpoch(1700000000);
    for (int i = 0; i < 10; ++i) {
        const QString path = QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(payload), payload.size());
        QVERIFY(f.setFileTime(baseTime.addSecs(i * 60), QFileDevice::FileModificationTime));
        f.close();
    }

    // Over byte cap (10,000 > 5,500) -> prunes 5 oldest (c0..c4), keeps c5..c9.
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(cacheDir, 5500, 100), 5);
    for (int i = 0; i < 5; ++i)
        QVERIFY(!QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i))));
    for (int i = 5; i < 10; ++i)
        QVERIFY(QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i))));

    // Over file count cap (5 > 3) -> prunes 2 oldest (c5..c6), keeps c7..c9.
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(cacheDir, 1024 * 1024, 3), 2);
    QVERIFY(!QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c5.jpg"))));
    QVERIFY(!QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c6.jpg"))));
    for (int i = 7; i < 10; ++i)
        QVERIFY(QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i))));

    // Already within budget -> 0 removed.
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(cacheDir, 1024 * 1024, 3), 0);
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(dir.filePath(QStringLiteral("nonexistent")), 100, 1), 0);

    // 3. CoverProvider async requestImageResponse() works for missing, valid, and cancelled requests.
    CoverProvider provider;
    {
        std::unique_ptr<QQuickImageResponse> missing(
            provider.requestImageResponse(QUrl::toPercentEncoding(dir.filePath(QStringLiteral("missing.png"))), QSize(128, 128)));
        QSignalSpy doneSpy(missing.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 3000);
        QVERIFY(missing->textureFactory() == nullptr);
    }
    {
        const QString imgPath = dir.filePath(QStringLiteral("ok.png"));
        QImage img(100, 150, QImage::Format_RGB32);
        img.fill(QColor(80, 120, 160));
        QVERIFY(img.save(imgPath, "PNG"));

        const QString id = QString::fromLatin1(QUrl::toPercentEncoding(imgPath)) + QStringLiteral("?r=5&sr=3&bw=100");
        std::unique_ptr<QQuickImageResponse> okResp(provider.requestImageResponse(id, QSize(128, 128)));
        QSignalSpy doneSpy(okResp.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 3000);
        std::unique_ptr<QQuickTextureFactory> factory(okResp->textureFactory());
        QVERIFY(factory != nullptr);
    }
}

void ClothboundTests::aPlusCoreParserFuzzStress() {
    QElapsedTimer timer;
    timer.start();
    std::mt19937_64 rng(0xC107B00DuLL);

    auto mutateBytes = [&rng](QByteArray data) {
        const int ops = 1 + int(rng() % 6);
        for (int op = 0; op < ops; ++op) {
            const int kind = int(rng() % 5);
            if (kind == 0 && !data.isEmpty()) {
                // Flip random byte
                const qsizetype pos = qsizetype(rng() % quint64(data.size()));
                data[pos] = char(rng() & 0xFF);
            } else if (kind == 1 && !data.isEmpty()) {
                // Truncate
                data.truncate(qsizetype(rng() % quint64(data.size())));
            } else if (kind == 2 && data.size() < 4096) {
                // Splice random adversarial tokens
                static const QByteArray tokens[] = {
                    "<", ">", "</", "<!--", "-->", "<![CDATA[", "]]>", "<!DOCTYPE [", "<?xml ", "&amp;",
                    "&#x", "&#99999999;", "\0", "\xFF\xFE", "\xEF\xBB\xBF", "\"", "'", "{", "}", "[", "]",
                    "PK\x03\x04", "PK\x01\x02", "PK\x05\x06", "PK\x06\x06", "PK\x06\x07", "javascript:alert(1)"};
                const QByteArray &tok = tokens[rng() % (sizeof(tokens) / sizeof(tokens[0]))];
                const qsizetype pos = data.isEmpty() ? 0 : qsizetype(rng() % quint64(data.size() + 1));
                data.insert(pos, tok);
            } else if (kind == 3 && data.size() > 4) {
                // Delete a slice
                const qsizetype pos = qsizetype(rng() % quint64(data.size()));
                const qsizetype len = qsizetype(1 + rng() % quint64(qMin<qsizetype>(64, data.size() - pos)));
                data.remove(pos, len);
            } else if (data.size() < 2048) {
                // Append random bytes
                const int n = 1 + int(rng() % 32);
                for (int k = 0; k < n; ++k)
                    data.append(char(rng() & 0xFF));
            }
        }
        return data;
    };

    int totalCases = 0;

    // 1. HtmlTokenizer & Html::parse (350 adversarial + mutated cases)
    const QList<QByteArray> htmlSeeds{
        "<!DOCTYPE html [<!ENTITY x 'y'>]><html xmlns='http://www.w3.org/1999/xhtml'><body><p>Hello &amp; &mdash; &#8217;</p></body></html>",
        "<?xml version='1.0' encoding='ISO-8859-1'?><script>var a = '</scriptx>'; </script><style>body{}</style><p>caf\xE9</p>",
        "\xFF\xFE<\0p\0>\0h\0i\0<\0/\0p\0>",
        "<![CDATA[raw <tag> &amp;]]><!-- comment --!><div a='1' b=2 c/>a < b && c > d",
        QByteArray(600, '<') + "div>" + QByteArray(600, '&'),
        QByteArray("<div>").repeated(600) + "deep" + QByteArray("</div>").repeated(600),
    };
    for (int i = 0; i < 350; ++i) {
        const QByteArray input = (i < htmlSeeds.size()) ? htmlSeeds[i] : mutateBytes(htmlSeeds[rng() % htmlSeeds.size()]);
        const QString decoded = HtmlTokenizer::decodeDocument(input);
        (void)HtmlTokenizer::decodeEntities(decoded);
        auto tree = Html::parse(decoded, (i & 1) != 0);
        QVERIFY(tree != nullptr);
        (void)tree->textContent();
        (void)tree->elements(QStringLiteral("p"));
        ++totalCases;
    }

    // 2. ZipArchive (250 adversarial + mutated ZIP/Zip64 cases)
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray validZip = TestBooks::epub2(png(8, 8));
    const QList<QByteArray> zipSeeds{
        validZip,
        QByteArray(),
        QByteArray("PK\x05\x06") + QByteArray(18, '\0'),
        QByteArray("PK\x06\x07") + QByteArray(16, '\xFF') + QByteArray("PK\x05\x06") + QByteArray(18, '\xFF'),
        QByteArray("PK\x03\x04") + QByteArray(40, '\xFF') + QByteArray("PK\x01\x02") + QByteArray(46, '\xFF') +
            QByteArray("PK\x05\x06") + QByteArray(18, '\0'),
    };
    const QString zipPath = dir.filePath(QStringLiteral("fuzz.zip"));
    for (int i = 0; i < 250; ++i) {
        const QByteArray input = (i < zipSeeds.size()) ? zipSeeds[i] : mutateBytes(zipSeeds[rng() % zipSeeds.size()]);
        QFile f(zipPath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(input);
        f.close();

        ZipArchive zip(zipPath);
        zip.setMaxArchiveInflateBytes(256ull * 1024);
        if (zip.open()) {
            const QStringList names = zip.names();
            const int checkCount = qMin(int(names.size()), 4);
            for (int n = 0; n < checkCount; ++n) {
                (void)zip.size(names[n]);
                (void)zip.encrypted(names[n]);
                QString err;
                (void)zip.read(names[n], &err);
            }
        }
        ++totalCases;
    }

    // 3. OpdsParser (350 adversarial + mutated Atom / OPDS2 / OpenSearch cases)
    const QUrl opdsBase(QStringLiteral("https://catalog.example/opds/root.xml"));
    const QList<QByteArray> opdsSeeds{
        R"raw(<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><id>urn:1</id><title>Feed</title>
           <link rel="search" type="application/opensearchdescription+xml" href="/search.xml"/>
           <entry><id>e1</id><title>Book</title><author><name>Ada</name></author>
           <link rel="http://opds-spec.org/acquisition/open-access" type="application/epub+zip" href="/b.epub"/>
           <link rel="http://opds-spec.org/image" href="javascript:alert(1)"/></entry></feed>)raw",
        R"raw({"metadata":{"title":"Catalog"},"links":[{"rel":"self","href":"/opds2.json"},{"rel":"search","href":"/s?q={searchTerms}","templated":true}],
            "publications":[{"metadata":{"title":"Work","author":["Ada",{"name":"Bob"}]},"links":[{"rel":"http://opds-spec.org/acquisition","href":"/x.epub","type":"application/epub+zip"}],"images":[{"href":"/c.jpg"}]}]})raw",
        R"raw(<?xml version="1.0"?><OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
           <Url type="application/atom+xml" template="https://catalog.example/search?q={searchTerms}&amp;p={startPage?}"/></OpenSearchDescription>)raw",
        R"raw({"groups":[{"metadata":{"title":"G"},"publications":[null,123,{"metadata":[]}]}]})raw",
    };
    for (int i = 0; i < 350; ++i) {
        const QByteArray input = (i < opdsSeeds.size()) ? opdsSeeds[i] : mutateBytes(opdsSeeds[rng() % opdsSeeds.size()]);
        (void)OpdsParser::detectFormat(input);
        const OpdsFeed feed = OpdsParser::parse(input, opdsBase);
        (void)feed.toVariantMap();
        const QString tmpl = OpdsParser::parseOpenSearchTemplate(input, opdsBase);
        if (!tmpl.isEmpty())
            (void)OpdsParser::expandSearchTemplate(tmpl, QStringLiteral("test query & <tag>"));
        if ((i & 3) == 0)
            (void)OpdsParser::stripHtmlToPlainText(QString::fromUtf8(input));
        ++totalCases;
    }

    // 4. ArticleExtractor (300 adversarial + mutated HTML article cases)
    const QUrl pageUrl(QStringLiteral("https://example.org/articles/lantern"));
    const QList<QByteArray> articleSeeds{
        R"raw(<!doctype html><html lang="en"><head><title>Lantern Essay</title>
           <meta property="og:title" content="The Lantern"/><meta name="author" content="Ada Marsh"/>
           <script type="application/ld+json">{"@type":"Article","headline":"The Lantern","author":{"name":"Ada"}}</script>
           </head><body><nav>Menu</nav><article><h1>The Lantern</h1>
           <p>A long enough paragraph about optics, coastal fog, and quiet craftsmanship along the northern pier so scoring selects this article cleanly.</p>
           <img src="/img/lamp.jpg" alt="Lamp"/><iframe src="https://evil.test"></iframe><script>alert(1)</script>
           <h2>Second Section</h2><p>More prose with &mdash; entities and <a href="javascript:void(0)">bad link</a> and <a href="/ok">good link</a>.</p>
           </article></body></html>)raw",
        R"raw(<html><body><table><tr><td><div><p>Nested content</p><svg><script>x</script></svg><style>p{color:red}</style></div></td></tr></table></body></html>)raw",
        QByteArray("<div><p>").repeated(200) + "deep article" + QByteArray("</p></div>").repeated(200),
    };
    for (int i = 0; i < 300; ++i) {
        const QByteArray input = (i < articleSeeds.size()) ? articleSeeds[i] : mutateBytes(articleSeeds[rng() % articleSeeds.size()]);
        const ExtractedArticle art = ArticleExtractor::extract(input, pageUrl);
        if (!art.bodyXhtml.isEmpty()) {
            QHash<QString, QString> map;
            for (const ExtractedImage &img : art.images)
                map.insert(img.localPath, img.localPath);
            (void)ArticleExtractor::finalizeImages(art.bodyXhtml, map);
        }
        ++totalCases;
    }

    QVERIFY(totalCases >= 1250);
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QStringLiteral("Parser fuzz took %1 ms").arg(timer.elapsed())));
}
// === end a-plus-core tests ===

// === w5-core-lib tests ===
#include "library/libraryschema.h"
#include "library/readingtelemetry.h"

void ClothboundTests::w5CoreLibNoReaderCycleAndDecoupledBookSearch() {
    // 1. Strict DAG check: ZERO #include "reader/..." anywhere in src/library/.
    const QString libDir = QFINDTESTDATA("../src/library");
    QVERIFY2(QFileInfo(libDir).isDir(), qPrintable(libDir));
    int checkedFiles = 0;
    QDirIterator it(libDir, QStringList{QStringLiteral("*.h"), QStringLiteral("*.cpp")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString filePath = it.next();
        QFile f(filePath);
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(filePath));
        const QByteArray bytes = f.readAll();
        QVERIFY2(!bytes.contains("#include \"reader/") && !bytes.contains("#include <reader/"),
                 qPrintable(QStringLiteral("Forbidden reader/ include found in %1").arg(filePath)));
        ++checkedFiles;
    }
    QVERIFY(checkedFiles >= 14);

    // 2. BookSearch works cleanly with a decoupled QObject exposing meta-object
    // properties and bookChanged() signal (zero Reader dependency).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("decoupled.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

    QQmlEngine qmlEngine;
    QQmlComponent sourceComp(&qmlEngine);
    sourceComp.setData(QByteArrayLiteral(R"QML(
import QtQml
QtObject {
    property bool open: true
    property int bookId: 42
    property string format: "epub"
    property string bookPath: ""
    property int chapter: 0
    property int pageStart: 0
    property int pageEnd: 0
    signal bookChanged()
}
)QML"),
                       QUrl(QStringLiteral("inline:/DecoupledBookSource.qml")));
    QVERIFY2(sourceComp.status() == QQmlComponent::Ready, qPrintable(sourceComp.errorString()));
    std::unique_ptr<QObject> source(sourceComp.create());
    QVERIFY(source != nullptr);
    QVERIFY(source->setProperty("bookPath", epubPath));

    BookSearch search(source.get());
    search.find(QStringLiteral("storm"));
    QVERIFY(search.searching());
    QTRY_VERIFY_WITH_TIMEOUT(!search.searching(), 5000);
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.results().first().toMap().value(QStringLiteral("chapter")).toInt(), 0);
    QCOMPARE(search.results().first().toMap().value(QStringLiteral("match")).toString(), QStringLiteral("storm"));

    // Emitting bookChanged() on the decoupled QObject resets cache and clears results.
    QVERIFY(source->setProperty("open", false));
    QVERIFY(source->setProperty("bookPath", QString()));
    QVERIFY(QMetaObject::invokeMethod(source.get(), "bookChanged", Qt::DirectConnection));
    QCOMPARE(search.count(), 0);
    QVERIFY(search.query().isEmpty());

    // 3. Also supports a plain QObject exposing dynamic `path` + `format` properties.
    QObject pathOnly;
    pathOnly.setProperty("path", epubPath);
    pathOnly.setProperty("format", QStringLiteral("epub"));
    BookSearch pathSearch(&pathOnly);
    pathSearch.find(QStringLiteral("lamp"));
    QTRY_VERIFY_WITH_TIMEOUT(!pathSearch.searching(), 5000);
    QCOMPARE(pathSearch.count(), 1);
}

void ClothboundTests::w5CoreLibSchemaTelemetryAndReadOnlyOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    const QString dbPath = dataDir + QStringLiteral("/library.sqlite");

    const QString epub1 = dir.filePath(QStringLiteral("one.epub"));
    const QString epub2 = dir.filePath(QStringLiteral("two.epub"));
    QVERIFY(TestBooks::write(epub1, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epub2, TestBooks::epub3(QByteArray(128, 'a'))));

    int id1 = 0, id2 = 0, colId = 0;
    {
        Library rw(dataDir, cacheDir);
        QVERIFY2(rw.open(), qPrintable(rw.errorString()));
        QVERIFY(!rw.isReadOnly());
        QCOMPARE(rw.schemaVersion(), LibrarySchema::kLatestVersion);
        QCOMPARE(LibrarySavepoint::activeDepth(), 0);

        id1 = rw.addBookFile(epub1);
        id2 = rw.addBookFile(epub2);
        QVERIFY(id1 > 0 && id2 > 0);
        colId = rw.createCollection(QStringLiteral("Essays"));
        QVERIFY(colId > 0);
        QVERIFY(rw.addToCollection(id1, colId));
        rw.setFinished(id2, true);
        rw.markOpened(id1);
        rw.setBookSetting(id1, QStringLiteral("voiceName"), QStringLiteral("Kokoro"));

        // Verify nested LibrarySavepoint depth tracking and automatic rollback.
        {
            QSqlDatabase db = rw.database();
            LibrarySavepoint outer(db, "test_outer");
            QVERIFY(outer.isActive());
            QCOMPARE(LibrarySavepoint::activeDepth(), 1);
            {
                LibrarySavepoint inner(db, "test_inner");
                QVERIFY(inner.isActive());
                QCOMPARE(LibrarySavepoint::activeDepth(), 2);
                QSqlQuery ins(db);
                QVERIFY(ins.exec(QStringLiteral("INSERT INTO meta (key, value) VALUES ('rolled_back', '1')")));
            }  // inner auto-rolls back
            QCOMPARE(LibrarySavepoint::activeDepth(), 1);
            QVERIFY(outer.commit());
            QCOMPARE(LibrarySavepoint::activeDepth(), 0);
            QSqlQuery chk(db);
            QVERIFY(chk.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'rolled_back'")));
            QVERIFY(!chk.next());
        }

        // Verify ReadingTelemetry deterministic pace learning, streaks, and per-book stats.
        const QDate refDay(2026, 6, 15);
        QSqlDatabase db = rw.database();
        const auto r1 = ReadingTelemetry::recordReadingTime(db, id1, 300, 7500, 1300.0, refDay.addDays(-2));
        QVERIFY(r1.ok);
        QVERIFY(!r1.paceUpdated);  // < 600s paced so far
        const auto r2 = ReadingTelemetry::recordReadingTime(db, id1, 360, 10800, r1.newCharsPerMinute, refDay.addDays(-1));
        QVERIFY(r2.ok);
        QVERIFY(r2.paceUpdated);  // 660s total paced -> (18300 / 660) * 60 = 1663.6 cpm
        QVERIFY(r2.newCharsPerMinute > 1600.0 && r2.newCharsPerMinute < 1700.0);
        const auto r3 = ReadingTelemetry::recordReadingTime(db, id1, 120, 2400, r2.newCharsPerMinute, refDay);
        QVERIFY(r3.ok);
        QCOMPARE(LibrarySavepoint::activeDepth(), 0);

        const QVariantMap stats = ReadingTelemetry::readingStats(db, {{id1, rw.record(id1)}, {id2, rw.record(id2)}},
                                                                 r3.newCharsPerMinute, refDay);
        QCOMPARE(stats.value(QStringLiteral("streakDays")).toInt(), 3);
        QCOMPARE(stats.value(QStringLiteral("longestStreak")).toInt(), 3);
        QCOMPARE(stats.value(QStringLiteral("todaySeconds")).toLongLong(), qint64(120));
        QCOMPARE(stats.value(QStringLiteral("weekSeconds")).toLongLong(), qint64(780));
        QCOMPARE(stats.value(QStringLiteral("recentDays")).toList().size(), 14);

        const QVariantMap bStats = ReadingTelemetry::bookReadingStats(db, rw.record(id1), r3.newCharsPerMinute);
        QCOMPARE(bStats.value(QStringLiteral("daysRead")).toInt(), 3);
        QCOMPARE(bStats.value(QStringLiteral("firstDay")).toString(), QStringLiteral("2026-06-13"));
        QCOMPARE(bStats.value(QStringLiteral("lastDay")).toString(), QStringLiteral("2026-06-15"));
    }

    // Record exact DB digest before locking file + directory strictly read-only.
    auto sha256Of = [](const QString &path) -> QByteArray {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray digestBefore = sha256Of(dbPath);
    QVERIFY(!digestBefore.isEmpty());

    QVERIFY(QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                               QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                               QFileDevice::ReadOther | QFileDevice::ExeOther));
    auto restorePerms = qScopeGuard([&] {
        QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    });

    const QString untouchedCache = dir.filePath(QStringLiteral("never_created_cache"));
    {
        Library ro(dataDir, untouchedCache);
        QVERIFY2(ro.openReadOnly(dbPath), qPrintable(ro.errorString()));
        QVERIFY(ro.isReadOnly());
        QCOMPARE(ro.schemaVersion(), LibrarySchema::kLatestVersion);
        QCOMPARE(ro.bookCount(), 2);
        QCOMPARE(ro.record(id1).title, QStringLiteral("The Lantern Keeper"));
        QCOMPARE(ro.record(id2).title, QStringLiteral("Salt Roads"));
        QCOMPARE(ro.collections().size(), 1);
        QVERIFY(ro.isInCollection(id1, colId));
        QCOMPARE(ro.bookSetting(id1, QStringLiteral("voiceName")), QStringLiteral("Kokoro"));
        QCOMPARE(ro.chapterLengths(id1).size(), 2);
        QCOMPARE(ro.shelfCounts().value(QStringLiteral("all")).toInt(), 2);
        QCOMPARE(ro.bookReadingStats(id1).value(QStringLiteral("daysRead")).toInt(), 3);

        // Mutations on read-only Library fail cleanly without touching disk.
        QCOMPARE(ro.createCollection(QStringLiteral("Forbidden")), 0);
        QVERIFY(!ro.errorString().isEmpty());
        QCOMPARE(ro.addBookFile(epub1), 0);
        QVERIFY(!ro.deleteCollection(colId));
        QVERIFY(!ro.addToCollection(id2, colId));
        ro.setFinished(id1, true);
        QCOMPARE(ro.record(id1).finishedAt, qint64(0));
        ro.addReadingTime(id1, 60, 1000);
    }

    QVERIFY(!QFileInfo::exists(untouchedCache));
    QCOMPARE(sha256Of(dbPath), digestBefore);

    // Verify fail-closed on future schema version > kLatestVersion for both open() and openReadOnly().
    QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    {
        const QString conn = QStringLiteral("w5-future-schema");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 99")));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }
    {
        Library roFuture(dataDir, cacheDir);
        QVERIFY(!roFuture.openReadOnly(dbPath));
        QVERIFY2(roFuture.errorString().contains(QStringLiteral("99")), qPrintable(roFuture.errorString()));
        Library rwFuture(dataDir, cacheDir);
        QVERIFY(!rwFuture.open());
        QVERIFY2(rwFuture.errorString().contains(QStringLiteral("99")), qPrintable(rwFuture.errorString()));
    }
}

void ClothboundTests::w5CoreLibFastEpubImportScanPerformance() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Build a realistic ~200k-word, 40-chapter EPUB (~1.35 MB uncompressed XHTML).
    constexpr int kChapters = 40;
    QByteArray manifestItems;
    QByteArray spineRefs;
    QList<QPair<QString, QByteArray>> files;
    files.reserve(kChapters + 2);

    const QByteArray para =
        "<p>The lantern keeper walked slowly along the weathered basalt seawall watching the northern fog drift "
        "past the iron bell tower while distant harbor buoys chimed across the cold dark tide.</p>\n";
    QByteArray longBody;
    longBody.reserve(para.size() * 190);
    for (int p = 0; p < 190; ++p)  // ~5,300 words / ~33 KB per chapter -> ~212k words total
        longBody += para;
    const QByteArray longChapter = TestBooks::chapter("Chapter", longBody);
    const QByteArray shortChapter = TestBooks::chapter("Short", "<p>A brief epigraph.</p>");

    for (int i = 0; i < kChapters; ++i) {
        const QByteArray id = "c" + QByteArray::number(i);
        const QByteArray href = "text/ch" + QByteArray::number(i) + ".xhtml";
        manifestItems += "    <item id=\"" + id + "\" href=\"" + href + "\" media-type=\"application/xhtml+xml\"/>\n";
        spineRefs += "    <itemref idref=\"" + id + "\"/>\n";
        files.append({QStringLiteral("OEBPS/") + QString::fromLatin1(href), (i == 0) ? shortChapter : longChapter});
    }
    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:title>Large Novel</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en</dc:language>\n"
        "    <meta name=\"cover\" content=\"cover-img\"/>\n"
        "  </metadata>\n"
        "  <manifest>\n"
        "    <item id=\"cover-img\" href=\"cover.png\" media-type=\"image/png\"/>\n" +
        manifestItems +
        "  </manifest>\n"
        "  <spine>\n" +
        spineRefs +
        "  </spine>\n"
        "</package>";
    files.prepend({QStringLiteral("OEBPS/cover.png"), png(40, 60)});
    files.prepend({QStringLiteral("OEBPS/content.opf"), opf});

    const QString epubPath = dir.filePath(QStringLiteral("big_200k.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub(files)));

    // 1. EpubBook::estimatedChapterLengths() inflates literally ZERO chapter bytes.
    EpubBook book(epubPath);
    QVERIFY2(book.open(), qPrintable(book.errorString()));
    const quint64 inflatedBefore = book.inflatedBytes();
    const QList<int> estLengths = book.estimatedChapterLengths();
    QCOMPARE(book.inflatedBytes(), inflatedBefore);
    QCOMPARE(estLengths.size(), kChapters);
    QVERIFY(estLengths[0] > 0);
    QVERIFY(estLengths[1] > estLengths[0] * 10);

    // 2. Fast default BookFiles::read() avoids chapter inflation and runs much faster than exact measurement.
    QList<int> fastLengths, exactLengths;
    QElapsedTimer fastTimer;
    fastTimer.start();
    const BookInfo fastInfo = BookFiles::read(epubPath, 0, &fastLengths, false);
    const qint64 fastUs = fastTimer.nsecsElapsed() / 1000;

    QElapsedTimer exactTimer;
    exactTimer.start();
    const BookInfo exactInfo = BookFiles::read(epubPath, 0, &exactLengths, true);
    const qint64 exactUs = exactTimer.nsecsElapsed() / 1000;

    QVERIFY(fastInfo.ok() && exactInfo.ok());
    QCOMPARE(fastLengths.size(), kChapters);
    QCOMPARE(exactLengths.size(), kChapters);
    QCOMPARE(fastLengths, estLengths);
    QVERIFY2(fastUs * 5 < exactUs,
             qPrintable(QStringLiteral("Expected fast scan (%1 us) to be >5x faster than exact scan (%2 us)")
                            .arg(fastUs)
                            .arg(exactUs)));

    // 3. Library::addBookFile() imports the 200k-word EPUB quickly with populated proportional chapter lengths.
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    QElapsedTimer importTimer;
    importTimer.start();
    const int bookId = library.addBookFile(epubPath);
    const qint64 importMs = importTimer.elapsed();
    QVERIFY(bookId > 0);
    QCOMPARE(library.chapterLengths(bookId), fastLengths);
    QVERIFY(library.record(bookId).length > 500000);
    QVERIFY2(importMs < 250, qPrintable(QStringLiteral("200k-word EPUB import took %1 ms").arg(importMs)));
}
// === end w5-core-lib tests ===

#include "clothbound_tests.moc"
