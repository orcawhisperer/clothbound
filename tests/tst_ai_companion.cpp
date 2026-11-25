#include "clothbound_tests.h"

#include "ai/aiservices.h"
#include "ai/aiusage.h"
#include "ai/bookcontextreader.h"
#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/companionstore.h"
#include "ai/gemini.h"
#include "ai/illustrator.h"
#include "ai/keystore.h"
#include "ai/localdictionary.h"
#include "ai/localmodels.h"
#include "ai/localsetup.h"
#include "ai/mathformatter.h"
#include "ai/passageindex.h"
#include "ai/sseparser.h"
#include "app/theme.h"
#include "audio/audiocache.h"
#include "audio/localspeech.h"
#include "audio/narration.h"
#include "audio/speechplayer.h"
#include "audio/voicecatalog.h"
#include "audio/voicepacks.h"
#include "fakeserver.h"
#include "formats/bookscheme.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "library/library.h"
#include "library/marks.h"
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
#include <QPromise>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <QtTest>
#include <cmath>
#include <limits>
#include <random>

namespace {

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

}  // namespace

void ClothboundTests::localChatBodies() {
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

void ClothboundTests::localAnswersAreCleaned() {
    QCOMPARE(LocalModels::cleanText("<think>hmm, moods</think>\n\nCalm.", false), QString("Calm."));
    QCOMPARE(LocalModels::cleanText("Here:\n```json\n{\"a\": 1}\n```", true), QString("{\"a\": 1}"));
    QCOMPARE(LocalModels::cleanText("<think>still thinking", false), QString());
    QVERIFY(LocalModels::queryText("qwen3-embedding:0.6b", "who is Weena").endsWith("Query: who is Weena"));
    QCOMPARE(LocalModels::queryText("bge-m3", "who"), QString("who"));
}

void ClothboundTests::localModelsStream() {
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

void ClothboundTests::serversAreFound() {
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

void ClothboundTests::featuresAreRouted() {
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

void ClothboundTests::companionReadsOnlySoFar() {
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

void ClothboundTests::passagesAreFound() {
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

void ClothboundTests::passagesAreFoundByMeaning() {
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

void ClothboundTests::geminiEmbeddingRequests() {
    const QJsonObject body = GeminiProvider::embedBody({"a", "b"}, true, "gemini-embedding-2");
    const QJsonArray requests = body["requests"].toArray();
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests[0].toObject()["model"].toString(), QString("models/gemini-embedding-2"));
    QCOMPARE(requests[0].toObject()["taskType"].toString(), QString("RETRIEVAL_QUERY"));
    QCOMPARE(requests[1].toObject()["content"].toObject()["parts"].toArray()[0].toObject()["text"].toString(), QString("b"));
}

void ClothboundTests::keyArrivesWhenTheKeyringAnswers() {
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

void ClothboundTests::sseParserHandlesChunkedStreams() {
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

void ClothboundTests::geminiBuildsTheDocumentedRequests() {
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

void ClothboundTests::geminiReadsRecordedResponses() {
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

void ClothboundTests::geminiStreamsTextAndSpeech() {
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

void ClothboundTests::ledgerPricesByDateAndTier() {
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

void ClothboundTests::servicesKeepOnlyWorkingKeysAndCountCosts() {
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

void ClothboundTests::companionStripsMarkdownImagesAndHtmlImg() {
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

void ClothboundTests::aiServicesLocalOverrideNeverFallsBackToCloud() {
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

void ClothboundTests::aiServicesBudgetZeroStrictVsUnlimitedAndAskRollback() {
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

void ClothboundTests::geminiPcmFromWavGuards32BitWraparound() {
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

void ClothboundTests::localModelsStripsChunkSplitThinkTagsAndRejectsNonHttpUrls() {
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

void ClothboundTests::passageIndexSplitsMinifiedSingleLineEpub() {
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

void ClothboundTests::companionPersistsThreadsWhosWhoCascadesAndGuardsSpoilers() {
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

void ClothboundTests::hybridSearchRrfSentenceSpanAndSpoilerFree() {
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

void ClothboundTests::localModelsNormalizesProxyUrlsAndParsesContextWindow() {
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

void ClothboundTests::wave4MultimodalPayloadsAndVisionErrors() {
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

void ClothboundTests::wave4SmartScopeSpoilerGuardAndShelfCitations() {
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

void ClothboundTests::wave4OfflineLatexMathAndSsrfPreservation() {
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

void ClothboundTests::wave4VisualSnipEntryAndPrivacyBudgetInvariants() {
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

void ClothboundTests::wave5CompanionResponseSpeechAndResize() {
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

void ClothboundTests::fixAiMcpCompanionSanitizerAndSpoilerGuards() {
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

void ClothboundTests::fixAiMcpBudgetReservationPricingAndPassageIndex() {
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

void ClothboundTests::fixAiMcpSseLocalModelsAndWavValidation() {
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

void ClothboundTests::aPlusArchMathFormatterUnitAndFuzz() {
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

void ClothboundTests::aPlusArchSseParserFuzz() {
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

void ClothboundTests::w5AiMcpDagCompanionExtraction() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("study.epub"));
    QVERIFY(TestBooks::write(
        epubPath,
        TestBooks::epub({
            {"OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
             "<dc:title>Lecture Notes on Quantum Optics</dc:title><dc:creator>Ada Marsh</dc:creator>"
             "</metadata>"
             "<manifest>"
             "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
             "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
             "<item id=\"c3\" href=\"c3.xhtml\" media-type=\"application/xhtml+xml\"/>"
             "</manifest>"
             "<spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/><itemref idref=\"c3\"/></spine></package>"},
            {"OEBPS/c1.xhtml",
             TestBooks::chapter("Abstract", "<p>Theorem 1.1 establishes coherent photon interference in cavities.</p>")},
            {"OEBPS/c2.xhtml",
             TestBooks::chapter("Method", "<p>Dr. Clara Vance calibrated the interferometer using phase alpha.</p>")},
            {"OEBPS/c3.xhtml",
             TestBooks::chapter("References", "<p>Bibliography entry one. Bibliography entry two.</p>")},
        })));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(epubPath);
    QVERIFY(bookId > 0);
    const int colId = library.createCollection(QStringLiteral("Optics Shelf"));
    QVERIFY(colId > 0);
    QVERIFY(library.addToCollection(bookId, colId));

    // 1. CompanionStore: tables, reading mode, thread, spoiler-safe snapshots, shelf, pictures.
    CompanionStore store(library.database().connectionName());
    store.ensureTables();
    store.ensurePictureTable();
    store.saveReadingMode(bookId, QStringLiteral("study"));
    QCOMPARE(store.loadReadingMode(bookId), QStringLiteral("study"));

    QString shelfLabel;
    const auto shelfBooks = store.shelfBooksFor(bookId, &shelfLabel);
    QCOMPARE(shelfLabel, QStringLiteral("Optics Shelf"));
    QCOMPARE(shelfBooks.size(), 1);
    QCOMPARE(shelfBooks.first().id, bookId);

    CompanionStore::Citation cite{bookId, QStringLiteral("Lecture Notes"), 1, 10, QStringLiteral("Ch 2"),
                                  QStringLiteral("interferometer")};
    const QList<CompanionStore::Turn> turns{
        {true, QStringLiteral("Who calibrated it?"), {}, {}, false, QStringLiteral("data:image/png;base64,AA=="), 1, 40},
        {false, QStringLiteral("Dr. Clara Vance."), {cite}, QStringLiteral("Local"), true, {}, 1, 40},
    };
    QVERIFY(store.saveConversation(bookId, turns));
    const auto reloaded = store.loadConversation(bookId);
    QCOMPARE(reloaded.size(), 2);
    QCOMPARE(reloaded[0].snipDataUrl, QStringLiteral("data:image/png;base64,AA=="));
    QCOMPARE(reloaded[1].sources.size(), 1);
    QCOMPARE(reloaded[1].sources[0].label, QStringLiteral("Ch 2"));

    const QVariantList people{QVariantMap{{QStringLiteral("name"), QStringLiteral("Clara Vance")},
                                          {QStringLiteral("who"), QStringLiteral("Physicist")}}};
    QVERIFY(store.saveWhosWhoSnapshot(bookId, QStringLiteral("fiction"), 1, 50, 0, QStringLiteral("Local"), true, people));
    CompanionStore::WhosWhoSnapshot wwSnap;
    // Spoiler-safe: chapter 0 must NOT see chapter 1 snapshot!
    QVERIFY(!store.loadWhosWhoSnapshot(bookId, QStringLiteral("fiction"), false, 0, 100, 0, false, &wwSnap));
    QVERIFY(store.loadWhosWhoSnapshot(bookId, QStringLiteral("fiction"), false, 1, 60, 0, true, &wwSnap));
    QCOMPARE(wwSnap.people.size(), 1);
    QCOMPARE(wwSnap.chapter, 1);

    QVERIFY(store.saveRecapSnapshot(bookId, QStringLiteral("study"), -1, 0, 0, QStringLiteral("Local"), true,
                                    QStringLiteral("**Core Claims** — coherent interference.")));
    CompanionStore::RecapSnapshot recapSnap;
    QVERIFY(store.loadRecapSnapshot(bookId, QStringLiteral("study"), true, 0, 0, 0, false, &recapSnap));
    QVERIFY(recapSnap.text.contains(QStringLiteral("Core Claims")));

    CompanionStore::PictureRecord pic;
    pic.bookId = bookId;
    pic.chapter = 1;
    pic.offset = 12;
    pic.caption = QStringLiteral("Interferometer");
    pic.prompt = QStringLiteral("Engraving of brass optical bench");
    pic.file = dir.filePath(QStringLiteral("pic1.png"));
    pic.style = QStringLiteral("engraving");
    pic.passage = QStringLiteral("calibrated the interferometer");
    const int picId = store.insertPicture(pic);
    QVERIFY(picId > 0);
    QCOMPARE(store.loadPictures(bookId).size(), 1);
    CompanionStore::PictureRecord foundPic;
    QVERIFY(store.findPicture(picId, bookId, &foundPic));
    QCOMPARE(foundPic.caption, QStringLiteral("Interferometer"));
    QCOMPARE(store.deletePicture(picId), pic.file);
    QVERIFY(store.loadPictures(bookId).isEmpty());

    store.clearBookHistory(bookId);
    QVERIFY(store.loadConversation(bookId).isEmpty());

    // 2. BookContextReader: spoiler-safe textUpTo, reading-mode detection, study join, firstSeen, JSON extraction.
    const auto upToCh1 = BookContextReader::textUpTo(epubPath, QStringLiteral("epub"), 1, 20);
    QCOMPARE(upToCh1.size(), 2);
    QVERIFY(upToCh1[1].text.size() <= 20);
    const auto whole = BookContextReader::textOfBook(epubPath, QStringLiteral("epub"));
    QCOMPARE(whole.size(), 3);
    QCOMPARE(BookContextReader::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Lecture Notes on Quantum Optics"),
                                                  {QStringLiteral("Abstract"), QStringLiteral("References")},
                                                  upToCh1[0].text),
             QStringLiteral("study"));
    QCOMPARE(BookContextReader::detectReadingMode(QStringLiteral("epub"), QStringLiteral("The Quiet Harbor"),
                                                  {QStringLiteral("Chapter 1")}, QStringLiteral("Fog drifted over the pier.")),
             QStringLiteral("fiction"));

    QList<BookContextReader::Chapter> titled = whole;
    titled[0].title = QStringLiteral("Abstract");
    titled[1].title = QStringLiteral("Method");
    titled[2].title = QStringLiteral("References");
    const auto seen = BookContextReader::resolveFirstSeen(titled, QStringLiteral("Clara Vance"), {}, {}, QStringLiteral("epub"));
    QCOMPARE(seen.chapterIndex, 1);
    QCOMPARE(seen.label, QStringLiteral("Method"));
    QVERIFY(seen.offset >= 0);

    const QJsonObject extracted =
        BookContextReader::extractJsonObject(QStringLiteral("Sure:\n```json\n{\"ok\":true}\n```"));
    QVERIFY(extracted.value(QStringLiteral("ok")).toBool());

    // 3. LocalDictionary: word sanitization, JSON formatting, multi-word rejection.
    QCOMPARE(LocalDictionary::cleanWord(QStringLiteral("  “don’t,”  ")), QStringLiteral("don't"));
    const QStringList defs = LocalDictionary::parseSdcvJson(
        R"([{"word":"other","dict":"D1","definition":"fallback"},{"word":"quanta","dict":"Physics","definition":"discrete packets"}])",
        QStringLiteral("quanta"));
    QCOMPARE(defs.size(), 2);
    QVERIFY(defs[1].contains(QStringLiteral("discrete packets")));
    QVERIFY(defs[1].contains(QStringLiteral("— Physics")));
    LocalDictionary dict;
    QVERIFY(!dict.lookupAsync(QStringLiteral("two words"), [](const QStringList &) {}));
    QVERIFY(!dict.lookupAsync(QStringLiteral("   "), [](const QStringList &) {}));
}
