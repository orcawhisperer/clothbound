#include "clothbound_tests.h"

#include "ai/aiservices.h"
#include "ai/gemini.h"
#include "ai/keystore.h"
#include "ai/localmodels.h"
#include "audio/audiobook.h"
#include "audio/audiocache.h"
#include "audio/cast.h"
#include "audio/director.h"
#include "audio/download.h"
#include "audio/localspeech.h"
#include "audio/mpris.h"
#include "audio/narration.h"
#include "audio/responsespeech.h"
#include "audio/sleepcontroller.h"
#include "audio/speech.h"
#include "audio/speechcleaner.h"
#include "audio/speechplayer.h"
#include "audio/voicecatalog.h"
#include "audio/voicepacks.h"
#include "fakeserver.h"
#include "formats/audiopcm.h"
#include "formats/bookscheme.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "library/library.h"
#include "reader/reader.h"
#include "testbooks.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <QtTest>
#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <thread>

namespace {

Ai::Voice makeVoice(const QString &id, const QString &language, const QString &persona,
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

class SilentEngine : public TtsEngine {
public:
    QString prepare(const VoicePacks::Model &) override { return {}; }
    int sampleRate() const override { return 24000; }
    QVector<float> speak(const QString &, int, const std::atomic_bool &) override { return {}; }
};

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

}  // namespace

void ClothboundTests::speechFindsSentences() {
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

void ClothboundTests::speechLeavesOutNoteMarkers() {
    SpeechBlock b = block(0, "He arrived in Paris.12 It rained.");
    b.skips = {{20, 22}};
    const QList<SpeechSentence> said = Speech::sentences(b);
    QCOMPARE(said.size(), 2);
    QCOMPARE(said[0].text, QString("He arrived in Paris."));
    // The marker is inside the highlight, though it isn't said.
    QCOMPARE(said[0].end, 22);
    QCOMPARE(said[1].text, QString("It rained."));
}

void ClothboundTests::speechSaysHeadingsWell() {
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

void ClothboundTests::speechMakesClips() {
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

void ClothboundTests::speechBeginsAClipWhereTheMoodChanges() {
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

void ClothboundTests::speechSplitsLongParagraphs() {
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

void ClothboundTests::speechTimelineCoversTheClip() {
    SpeechClip clip;
    clip.sentences = {{0, 10, "Short one."}, {11, 80, "A much longer sentence, with a pause or two; and more after it."}};
    const auto spans = Speech::timeline(clip);
    QCOMPARE(spans.size(), 2);
    QCOMPARE(spans[0].first, 0.0);
    QCOMPARE(spans[1].second, 1.0);
    QCOMPARE(spans[0].second, spans[1].first);
    QVERIFY(spans[0].second < 0.3);
}

void ClothboundTests::stretcherKeepsPitchAtSpeed() {
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

void ClothboundTests::stretcherWaitsForAudioStillArriving() {
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

void ClothboundTests::audioCacheKeepsClips() {
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

void ClothboundTests::voicePacksOfferTheRightVoices() {
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

void ClothboundTests::voicePacksInstallFromTheNetwork() {
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

void ClothboundTests::resamplingKeepsPitchAndLength() {
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

void ClothboundTests::builtinVoicesSpeakASentenceAtATime() {
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

void ClothboundTests::studioVoicesSpeak() {
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

void ClothboundTests::castLinesAreRead() {
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

void ClothboundTests::audiobookMetadata() {
    const QString meta = Audiobook::metadata("Love; and Mr Lewisham", "H. G. Wells",
                                             {{"I. Introduces Mr Lewisham", 0, 61000}, {"A=B #2", 61000, 90500}});
    QVERIFY(meta.startsWith(";FFMETADATA1\n"));
    QVERIFY2(meta.contains("title=Love\\; and Mr Lewisham\n"), qPrintable(meta));
    QVERIFY(meta.contains("artist=H. G. Wells\n"));
    QVERIFY(meta.contains("[CHAPTER]\nTIMEBASE=1/1000\nSTART=61000\nEND=90500\ntitle=A\\=B \\#2\n"));
    QCOMPARE(Audiobook::concatList({"/a/it's.opus"}), QString("ffconcat version 1.0\nfile '/a/it'\\''s.opus'\n"));
}

void ClothboundTests::voicesAreSpokenByTheirOwnProvider() {
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

void ClothboundTests::voicesSuggestNarratorsForTheBook() {
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

void ClothboundTests::voicesAreKeptForADay() {
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

void ClothboundTests::voicesCastADesignedNarrator() {
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

void ClothboundTests::narrationNullProviderRecutAndChapterEndSleepCancel() {
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

void ClothboundTests::castIsolatesVoiceFamiliesAcrossNarratorSwitches() {
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

void ClothboundTests::audioCacheConcurrentWritesAndCorruptDecodeEviction() {
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

void ClothboundTests::stretcherFlushesShortClipsAndTailsAtNonUnitRate() {
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

void ClothboundTests::localReplyRecordsBoundaryBeforeAudioReceived() {
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

void ClothboundTests::audiobookResolvesCoverArtAndSanitizesFilename() {
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

void ClothboundTests::speechHeadingWhitelistAndDialogueSpanSplitting() {
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

void ClothboundTests::speechPlayerOffThreadWsolaAndMidPlaybackRateRewind() {
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
        std::unique_lock<std::mutex> lock(player.m_mutex);
        player.m_headless = false;
        player.m_playing = true;
        player.startWorker(lock);
        player.m_cond.notify_one();
    }

    // Wait for the off-thread worker to populate at least 2 ready chunks at 1.0x.
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        std::unique_lock<std::mutex> lock(player.m_mutex);
        return player.m_ready.size() >= 2;
    }()), 5000);

    // Simulate sink consuming chunk 0; record exact sourceStart of the first unplayed chunk.
    qint64 expectedRewind = -1;
    {
        std::unique_lock<std::mutex> lock(player.m_mutex);
        const SpeechPlayer::ReadyChunk played = player.m_ready.takeFirst();
        player.m_readySamples = qMax<qint64>(0, player.m_readySamples - played.bytes.size() / 2);
        QVERIFY(!player.m_ready.isEmpty());
        expectedRewind = player.m_ready.first().sourceStart;
    }
    QVERIFY(expectedRewind > 0 && expectedRewind < totalSamples);

    // Mid-playback setRate(1.5) must discard stale ready chunks and rewind worker to expectedRewind.
    player.setRate(1.5);
    {
        std::unique_lock<std::mutex> lock(player.m_mutex);
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
        std::unique_lock<std::mutex> lock(player.m_mutex);
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
        player.m_cond.notify_one();
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

void ClothboundTests::narrationSkipSecondsSleepFadeRestoreAndPendingStart() {
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

void ClothboundTests::directorPdfBatchRangeAndSplitMoodsByPage() {
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

void ClothboundTests::audiobookSynthesizesPauseSilenceAndPartialChapterSuffix() {
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

void ClothboundTests::wave2NarrationScrubberVoiceAccentAndAudioBadgesPersistence() {
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

void ClothboundTests::wave2VoiceNarrationSleepFadeAndEmptyLeadGuard() {
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

void ClothboundTests::mprisMetadataAndForcedAsserts() {
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

void ClothboundTests::fixAudioScopedCastCancelDoesNotDeadlockSecondCaller() {
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

void ClothboundTests::fixAudioNarrationHoldSamplesReleasesOnShortClipsAndSleepTrim() {
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

void ClothboundTests::fixAudioNarrationQueueBlockedDuringResponseAndPauseSleepClear() {
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

void ClothboundTests::fixAudioCacheClearStoreRaceAndWavValidation() {
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

void ClothboundTests::fixAudioAudiobookSnapshotsMetadataAcrossBookSwitch() {
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

void ClothboundTests::fixAudioVoicePacksCachedIsInstalledAndSpeechPlayerChunks() {
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

void ClothboundTests::aPlusArchSpeechCleanerUnitAndFuzz() {
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

void ClothboundTests::aPlusArchSpeechResampleAntiAliasingAndMicroFade() {
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

void ClothboundTests::w5AudioSleepControllerMonotonicFadeAndChapterEnd() {
    // 1. Standalone SleepController: monotonic QElapsedTimer countdown, smooth quadratic fade,
    //    external volume override, sleep expiry + pre-fade volume restore, and chapter-end mode.
    SpeechPlayer player(24000);
    player.setHeadless(true);
    player.setVolume(0.8);
    player.play();

    SleepController sleep(&player);
    QSignalSpy changedSpy(&sleep, &SleepController::changed);
    QSignalSpy finishedSpy(&sleep, &SleepController::sleepFinished);

    QCOMPARE(sleep.sleepMinutes(), 0);
    QCOMPARE(sleep.sleepSecondsLeft(), 0);
    QVERIFY(!sleep.isActive());
    QVERIFY(!sleep.isFading());
    QVERIFY(!sleep.atChapterEnd());

    // Arm a 2-minute countdown and verify monotonic elapsed progress.
    QCOMPARE(sleep.sleepIn(2), false);
    QVERIFY(sleep.isActive());
    QCOMPARE(sleep.sleepMinutes(), 2);
    QVERIFY(sleep.sleepSecondsLeft() >= 119 && sleep.sleepSecondsLeft() <= 120);
    QVERIFY(changedSpy.count() >= 1);

    sleep.setDeadlineMs(15000);
    const qint64 remBefore = sleep.deadlineRef().remainingMs();
    QTest::qWait(30);
    const qint64 remAfter = sleep.deadlineRef().remainingMs();
    QVERIFY2(remAfter < remBefore, qPrintable(QStringLiteral("before=%1 after=%2").arg(remBefore).arg(remAfter)));
    QVERIFY(!sleep.isFading());

    // Smooth quadratic fade curve over the final 10 seconds: vol = preFade * t^2.
    sleep.setDeadlineMs(8000);
    sleep.onTick();
    QVERIFY(sleep.isFading());
    QVERIFY(qFuzzyCompare(sleep.preFadeVolume(), 0.8));
    const double vol8s = player.volume();
    QVERIFY2(vol8s > 0.45 && vol8s < 0.56, qPrintable(QString::number(vol8s)));

    sleep.setDeadlineMs(5000);
    sleep.onTick();
    const double vol5s = player.volume();
    QVERIFY2(vol5s > 0.14 && vol5s < 0.25 && vol5s < vol8s, qPrintable(QString::number(vol5s)));

    sleep.setDeadlineMs(2000);
    sleep.onTick();
    const double vol2s = player.volume();
    QVERIFY2(vol2s > 0.01 && vol2s < 0.06 && vol2s < vol5s, qPrintable(QString::number(vol2s)));

    // External volume change cancels fade and discards pending restore.
    QCOMPARE(sleep.onExternalVolumeChange(), true);
    player.setVolume(0.65);
    QVERIFY(!sleep.isFading());
    QVERIFY(sleep.preFadeVolume() < 0.0);
    QVERIFY(qFuzzyCompare(player.volume(), 0.65));

    // Reaching deadline triggers finish(): pauses playback at 0.0 and arms volume restore.
    sleep.setDeadlineMs(0);
    sleep.onTick();
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!player.isPlaying());
    QVERIFY(qFuzzyCompare(player.volume(), 0.0));
    QVERIFY(!sleep.isFading());
    QVERIFY(sleep.isPausedRestore());

    // Resuming after sleep-pause restores exact pre-fade volume (0.65).
    QCOMPARE(sleep.onResume(), false);
    QVERIFY(qFuzzyCompare(player.volume(), 0.65));
    QVERIFY(!sleep.isPausedRestore());

    // End-of-chapter mode arming, clearing via sleepIn(0), and consumeChapterEnd().
    sleep.sleepAtEndOfChapter();
    QVERIFY(sleep.atChapterEnd());
    QVERIFY(sleep.isActive());
    QCOMPARE(sleep.sleepMinutes(), 0);
    QCOMPARE(sleep.sleepSecondsLeft(), 0);
    QCOMPARE(sleep.sleepIn(0), true);
    QVERIFY(!sleep.atChapterEnd());
    QVERIFY(!sleep.isActive());

    sleep.sleepAtEndOfChapter();
    QVERIFY(sleep.atChapterEnd());
    sleep.consumeChapterEnd();
    QVERIFY(!sleep.atChapterEnd());
    QVERIFY(!sleep.isActive());

    // 2. Deduplicated Speech::atomicReplace() & Speech::classicGeminiTrait().
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString srcPath = dir.filePath(QStringLiteral("clip.part"));
    const QString dstPath = dir.filePath(QStringLiteral("clip.opus"));
    {
        QFile f(srcPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("fresh-payload");
    }
    {
        QFile f(dstPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("stale-payload");
    }
    QVERIFY(Speech::atomicReplace(srcPath, dstPath));
    QVERIFY(!QFile::exists(srcPath));
    {
        QFile f(dstPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("fresh-payload"));
    }
    QCOMPARE(Speech::classicGeminiTrait(QStringLiteral("sulafat")), QStringLiteral("Warm"));
    QCOMPARE(Speech::classicGeminiTrait(QStringLiteral("Kore")), QStringLiteral("Firm"));
}

void ClothboundTests::w5AudioResponseSpeechAndFocusArbitration() {
    // 1. Direct AudioFocus exclusivity & re-entrancy tests.
    QObject ownerA;
    QObject ownerB;
    int yieldA = 0;
    int yieldB = 0;
    auto &focus = AudioFocus::instance();
    focus.claim(&ownerA, AudioFocus::Kind::Book, [&] {
        ++yieldA;
        focus.release(&ownerA, AudioFocus::Kind::Book);
    });
    QCOMPARE(focus.currentOwner(), &ownerA);
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Book);

    // Different owner claiming Preview yields Book synchronously.
    focus.claim(&ownerB, AudioFocus::Kind::Preview, [&] { ++yieldB; });
    QCOMPARE(yieldA, 1);
    QCOMPARE(yieldB, 0);
    QCOMPARE(focus.currentOwner(), &ownerB);
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Preview);

    // Re-claiming the same (owner, kind) does not fire yield; stale release is ignored.
    focus.claim(&ownerB, AudioFocus::Kind::Preview, [&] { ++yieldB; });
    QCOMPARE(yieldB, 0);
    focus.release(&ownerA, AudioFocus::Kind::Book);
    QCOMPARE(focus.currentOwner(), &ownerB);
    focus.release(&ownerB, AudioFocus::Kind::Preview);
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::None);

    // 2. End-to-end Book vs Response vs VoiceCatalog Preview mutual exclusion.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("focus-arb.epub"));
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
    VoiceCatalog voices(&services, &cache, &reader, library.database().connectionName(),
                        dir.filePath(QStringLiteral("voices.json")));
    voices.m_player->setHeadless(true);

    const QString voiceId = QStringLiteral("builtin:kokoro:bm_george");
    narration.setVoice(voiceId, QStringLiteral("George"));

    // 2a. Voice preview active -> starting Book narration stops preview cleanly.
    voices.preview(voiceId, QStringLiteral("Preview sample text before book starts."));
    QCOMPARE(voices.previewing(), voiceId);
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Preview);

    narration.start(0, 0);
    narration.addBlocks(0, R"([{"start":0,"end":36,"text":"Book sentence taking exclusive focus.","kind":"text"}])");
    QVERIFY(voices.previewing().isEmpty());
    QVERIFY(narration.playing());
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Book);
    QTRY_VERIFY_WITH_TIMEOUT(!narration.m_clips.isEmpty() && narration.m_clips[0].state == Narration::Clip::Complete,
                             5000);

    // 2b. Book narration active -> starting VoiceCatalog preview pauses Book cleanly.
    voices.preview(voiceId, QStringLiteral("Preview interrupts playing book narration."));
    QVERIFY(!narration.playing());
    QCOMPARE(voices.previewing(), voiceId);
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Preview);

    // 2c. Voice preview active -> starting Companion response speech stops preview cleanly.
    narration.speakResponse(QStringLiteral("Companion response stops voice preview."), QStringLiteral("w5-resp-1"));
    QVERIFY(voices.previewing().isEmpty());
    QVERIFY(narration.speakingResponse());
    QVERIFY(!narration.playing());
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Response);
    QTRY_VERIFY_WITH_TIMEOUT(!narration.m_responseChunks.isEmpty() &&
                                 narration.m_responseChunks.first().state == Narration::ResponseChunk::Complete,
                             5000);

    // 2d. Companion response active -> VoiceCatalog preview stops response cleanly.
    voices.preview(voiceId, QStringLiteral("Preview interrupts companion response."));
    QVERIFY(!narration.speakingResponse());
    QCOMPARE(voices.previewing(), voiceId);
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Preview);

    // 2e. Resuming Book narration stops active preview cleanly.
    narration.resume();
    QVERIFY(voices.previewing().isEmpty());
    QVERIFY(narration.playing());
    QVERIFY(!narration.speakingResponse());
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Book);

    narration.stop();
    voices.stopPreview();
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::None);

    // 3. Direct standalone ResponseSpeech lifecycle + drain completion.
    SpeechPlayer standalonePlayer(24000);
    standalonePlayer.setHeadless(true);
    ResponseSpeech standalone(&services, &cache, &standalonePlayer);
    standalone.speak(QStringLiteral("First standalone sentence. Second standalone sentence."),
                     QStringLiteral("standalone-1"));
    QVERIFY(standalone.active());
    QCOMPARE(standalone.itemId(), QStringLiteral("standalone-1"));
    QVERIFY(!standalone.chunks().isEmpty());
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::Response);
    QTRY_VERIFY_WITH_TIMEOUT(standalone.chunks().first().state == ResponseSpeech::Chunk::Complete, 5000);
    QVERIFY(standalonePlayer.hasTrack(standalone.chunks().first().trackId));
    QVERIFY(standalone.handleDrained());
    QVERIFY(!standalone.active());
    QCOMPARE(focus.currentKind(), AudioFocus::Kind::None);
}

void ClothboundTests::w5AudioSpeechPlayerConcurrencyStress() {
    constexpr int sr = 24000;
    constexpr int sineSamples = sr / 2;  // 0.5 s of 330 Hz sine wave
    QByteArray sinePcm(sineSamples * 2, '\0');
    auto *pcmData = reinterpret_cast<qint16 *>(sinePcm.data());
    for (int n = 0; n < sineSamples; ++n)
        pcmData[n] = qint16(6000 * std::sin(2.0 * M_PI * 330.0 * n / sr));

    SpeechPlayer player(sr);
    player.setMuted(true);

    // Start the real off-thread WSOLA std::thread worker without requiring a hardware QAudioSink.
    {
        std::unique_lock<std::mutex> lock(player.m_mutex);
        player.m_headless = false;
        player.m_playing = true;
        player.startWorker(lock);
        player.m_cond.notify_one();
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<int> tracksAdded{0};
    std::atomic<int> rateChanges{0};
    std::atomic<int> clearsDone{0};
    std::atomic<int> chunksConsumed{0};
    std::atomic<bool> invariantFailed{false};

    // Thread 1: producer appending both aligned and unaligned (odd-byte) PCM chunks across tracks.
    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        int nextId = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            const int id = nextId++;
            player.addTrack(id, (id % 3 == 0) ? 128 : 0);
            tracksAdded.fetch_add(1, std::memory_order_relaxed);
            qsizetype off = 0;
            int stepIdx = 0;
            while (off < sinePcm.size() && !stop.load(std::memory_order_relaxed)) {
                const qsizetype sliceLen = ((stepIdx++ & 1) == 0) ? 1024 : 777;  // mix even & odd byte boundaries
                const qsizetype take = qMin(sliceLen, sinePcm.size() - off);
                player.appendAudio(id, sinePcm.mid(off, take));
                off += take;
            }
            player.finishTrack(id);
            std::this_thread::yield();
        }
    });

    // Thread 2: rapid rate/volume/mute mutations exercising WSOLA <-> 1.0x pass-through transitions.
    std::thread rateMutator([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        constexpr double rates[] = {0.75, 1.0, 1.25, 1.5, 2.0, 1.0, 0.85, 1.75};
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            player.setRate(rates[i % 8]);
            player.setVolume(0.2 + 0.1 * (i % 8));
            player.setMuted((i & 1) == 0);
            (void)player.volume();
            (void)player.starved();
            (void)player.drainedOut();
            (void)player.isPlaying();
            ++i;
            rateChanges.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 3: rapid pause / play / clear lifecycle storms.
    std::thread lifecycleMutator([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        int iter = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if ((iter % 5) == 0)
                player.pause();
            else if ((iter % 5) == 1)
                player.play();
            else if ((iter % 5) == 4) {
                player.clear();
                clearsDone.fetch_add(1, std::memory_order_relaxed);
            }
            ++iter;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Thread 4: simulated AudioOutputPump consumer verifying ready-queue invariants.
    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        while (!stop.load(std::memory_order_relaxed)) {
            bool woke = false;
            {
                std::unique_lock<std::mutex> lock(player.m_mutex);
                if (player.m_readySamples < 0 || (player.m_ready.isEmpty() && player.m_readySamples != 0))
                    invariantFailed.store(true, std::memory_order_relaxed);
                while (!player.m_ready.isEmpty()) {
                    const SpeechPlayer::ReadyChunk chunk = player.m_ready.takeFirst();
                    if ((chunk.bytes.size() & 1) != 0 || chunk.sourceStart < 0 || chunk.rate < 0.25 ||
                        chunk.rate > 4.0) {
                        invariantFailed.store(true, std::memory_order_relaxed);
                    }
                    player.m_readySamples = qMax<qint64>(0, player.m_readySamples - chunk.bytes.size() / 2);
                    chunksConsumed.fetch_add(1, std::memory_order_relaxed);
                    woke = true;
                }
                if (woke)
                    player.m_cond.notify_one();
            }
            std::this_thread::yield();
        }
    });

    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);

    producer.join();
    rateMutator.join();
    lifecycleMutator.join();
    consumer.join();

    QVERIFY(!invariantFailed.load());
    QVERIFY2(tracksAdded.load() > 0, "Producer must have added tracks");
    QVERIFY2(rateChanges.load() > 0, "Rate mutator must have executed");
    QVERIFY2(clearsDone.load() > 0, "Lifecycle mutator must have cleared");
    QVERIFY2(chunksConsumed.load() > 0, "Consumer must have drained WSOLA output chunks");

    // Verify clean deterministic recovery on the same SpeechPlayer after the stress storm.
    player.clear();
    player.setRate(1.25);
    player.addTrack(99999);
    player.appendAudio(99999, sinePcm);
    player.finishTrack(99999);
    {
        std::unique_lock<std::mutex> lock(player.m_mutex);
        player.m_playing = true;
        player.m_cond.notify_one();
    }

    qint64 recoveredSamples = 0;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        std::unique_lock<std::mutex> lock(player.m_mutex);
        while (!player.m_ready.isEmpty()) {
            const SpeechPlayer::ReadyChunk c = player.m_ready.takeFirst();
            player.m_readySamples = qMax<qint64>(0, player.m_readySamples - c.bytes.size() / 2);
            recoveredSamples += c.bytes.size() / 2;
        }
        player.m_cond.notify_one();
        return player.m_workerFinishedAll && player.m_ready.isEmpty() && player.m_readySamples == 0;
    }()), 5000);

    player.stopWorker();
    QCoreApplication::processEvents();
    QVERIFY2(recoveredSamples > sineSamples / 2, qPrintable(QString::number(recoveredSamples)));
}
