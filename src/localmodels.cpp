#include "localmodels.h"

#include "gemini.h"
#include "speech.h"
#include "sseparser.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>

namespace {

QString role(const Ai::Message &message) { return message.fromUser ? QStringLiteral("user") : QStringLiteral("assistant"); }

bool supportsOllamaThinking(const QString &model) {
    const QString lower = model.toLower();
    static const QRegularExpression thinking(
        QStringLiteral("qwen3|deepseek-r1|qwq|gpt-oss|cogito|magistral|granite3\\.2"));
    return thinking.match(lower).hasMatch();
}

Ai::Error errorFor(QNetworkReply *reply, const QByteArray &body) {
    Ai::Error error;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(body).object();
    QString message = json.value(QStringLiteral("error")).isObject()
                          ? json.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()
                          : json.value(QStringLiteral("error")).toString();
    if (message.isEmpty())
        message = QString::fromUtf8(body.left(300)).trimmed();
    if (message.isEmpty() && reply->error() != QNetworkReply::NoError)
        message = reply->errorString();
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        error.kind = Ai::Error::Cancelled;
    } else if (status == 0) {
        // Nothing answered: the server isn't running (or was stopped).
        error.kind = Ai::Error::Offline;
        message = reply->errorString();
    } else if (status == 404) {
        error.kind = Ai::Error::NotFound;
    } else if (status == 401 || status == 403) {
        error.kind = Ai::Error::BadKey;
    } else if (status == 400 || status == 422) {
        error.kind = Ai::Error::Invalid;
    } else {
        error.kind = Ai::Error::Server;
    }
    error.message = message;
    return error;
}

// One request to a local server: a chat (streamed or not), embeddings,
// speech, or the model list.
class ServerReply : public Ai::Reply {
public:
    enum Mode { Chat, Embed, Speech, Models };

    ServerReply(QNetworkReply *reply, Mode mode, LocalModels::Kind kind, const QString &model, const QString &feature,
                bool json, QObject *parent)
        : Ai::Reply(parent), m_reply(reply), m_mode(mode), m_kind(kind), m_json(json) {
        m_model = model;
        m_feature = feature;
        reply->setParent(this);
        connect(reply, &QNetworkReply::readyRead, this, [this] { read(); });
        connect(reply, &QNetworkReply::finished, this, [this] { done(); });
    }

    void abort() override {
        if (m_reply && m_reply->isRunning())
            m_reply->abort();
        finish({Ai::Error::Cancelled, {}});
    }

private:
    bool ok() const {
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        return status >= 200 && status < 300;
    }

    void read() {
        if (m_mode != Chat || !ok()) {
            m_body += m_reply->readAll();
            return;
        }
        const QByteArray chunk = m_reply->readAll();
        const QByteArray type = m_reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (type.contains("event-stream")) {
            for (const SseParser::Event &event : m_sse.feed(chunk))
                chatEvent(event.data);
        } else if (type.contains("ndjson") || m_kind == LocalModels::Ollama) {
            // Ollama streams a JSON object per line.
            m_body += chunk;
            qsizetype end;
            while ((end = m_body.indexOf('\n')) >= 0) {
                const QByteArray line = m_body.left(end).trimmed();
                m_body.remove(0, end + 1);
                if (!line.isEmpty())
                    chatEvent(line);
            }
        } else {
            m_body += chunk;
        }
    }

    // A piece of a chat: an SSE "data:" or one of Ollama's lines, or the
    // whole answer when it didn't stream.
    void chatEvent(const QByteArray &data) {
        if (data.trimmed() == "[DONE]")
            return;
        const QJsonObject json = QJsonDocument::fromJson(data).object();
        if (json.isEmpty())
            return;
        QString delta;
        if (json.contains(QStringLiteral("message"))) {  // Ollama
            delta = json.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
            if (json.value(QStringLiteral("done")).toBool()) {
                m_usage.inputTokens = json.value(QStringLiteral("prompt_eval_count")).toInteger();
                m_usage.outputTokens = json.value(QStringLiteral("eval_count")).toInteger();
                m_usage.promptEvalNs = json.value(QStringLiteral("prompt_eval_duration")).toInteger();
                m_usage.evalNs = json.value(QStringLiteral("eval_duration")).toInteger();
            }
        } else {
            const QJsonArray choices = json.value(QStringLiteral("choices")).toArray();
            const QJsonObject choice = choices.at(0).toObject();
            const QJsonObject part = choice.contains(QStringLiteral("delta")) ? choice.value(QStringLiteral("delta")).toObject()
                                                                               : choice.value(QStringLiteral("message")).toObject();
            delta = part.value(QStringLiteral("content")).toString();
            const QJsonObject usage = json.value(QStringLiteral("usage")).toObject();
            if (!usage.isEmpty()) {
                m_usage.inputTokens = usage.value(QStringLiteral("prompt_tokens")).toInteger();
                m_usage.outputTokens = usage.value(QStringLiteral("completion_tokens")).toInteger();
            }
        }
        if (delta.isEmpty())
            return;
        m_raw += delta;
        emitFiltered(delta);
    }

    // Filters a leading <think>...</think> block incrementally in O(1) per chunk,
    // even when the opening or closing tags are split across streamed tokens.
    void emitFiltered(const QString &delta) {
        static const QString openTag = QStringLiteral("<think>");
        static const QString closeTag = QStringLiteral("</think>");
        if (m_thinkState == ThinkDone) {
            if (m_trimLeading) {
                qsizetype i = 0;
                while (i < delta.size() && delta[i].isSpace())
                    ++i;
                if (i < delta.size()) {
                    m_trimLeading = false;
                    addText(delta.mid(i));
                }
            } else {
                addText(delta);
            }
            return;
        }
        m_thinkBuf += delta;
        if (m_thinkState == ThinkPending) {
            qsizetype lead = 0;
            while (lead < m_thinkBuf.size() && m_thinkBuf[lead].isSpace())
                ++lead;
            if (lead >= m_thinkBuf.size())
                return;
            const QStringView rest = QStringView(m_thinkBuf).mid(lead);
            if (rest.startsWith(openTag)) {
                m_thinkState = ThinkInside;
                m_thinkBuf = m_thinkBuf.mid(lead + openTag.size());
            } else if (QStringView(openTag).startsWith(rest)) {
                return;  // could still be a chunk-split "<think>"
            } else {
                m_thinkState = ThinkDone;
                const QString out = m_thinkBuf.mid(lead);
                m_thinkBuf.clear();
                if (!out.isEmpty())
                    addText(out);
                return;
            }
        }
        if (m_thinkState == ThinkInside) {
            const qsizetype end = m_thinkBuf.indexOf(closeTag);
            if (end < 0) {
                // Keep just enough tail to match a "</think>" split across chunks.
                if (m_thinkBuf.size() > closeTag.size() - 1)
                    m_thinkBuf = m_thinkBuf.right(closeTag.size() - 1);
                return;
            }
            const QString after = m_thinkBuf.mid(end + closeTag.size());
            m_thinkBuf.clear();
            m_thinkState = ThinkDone;
            m_trimLeading = true;
            if (!after.isEmpty())
                emitFiltered(after);
        }
    }

    void done() {
        if (isFinished())
            return;
        if (!ok() || m_reply->error() != QNetworkReply::NoError) {
            finish(errorFor(m_reply, m_body + m_reply->readAll()));
            return;
        }
        m_body += m_reply->readAll();
        switch (m_mode) {
        case Chat: {
            if (!m_body.trimmed().isEmpty()) {
                // A single whole JSON response (including pretty-printed multi-line JSON),
                // or line-delimited SSE/NDJSON tail lines.
                if (QJsonDocument::fromJson(m_body).isObject()) {
                    chatEvent(m_body);
                } else {
                    for (const QByteArray &line : m_body.split('\n'))
                        chatEvent(line.trimmed().startsWith("data:") ? line.trimmed().mid(5) : line);
                }
            }
            for (const SseParser::Event &event : m_sse.finish())
                chatEvent(event.data);
            if (m_thinkState == ThinkPending && !m_thinkBuf.trimmed().isEmpty()) {
                m_thinkState = ThinkDone;
                addText(m_thinkBuf.trimmed());
                m_thinkBuf.clear();
            }
            m_text = LocalModels::cleanText(m_raw, m_json);
            if (m_json) {
                const QJsonDocument document = QJsonDocument::fromJson(m_text.toUtf8());
                if (!document.isObject()) {
                    finish({Ai::Error::Invalid, QStringLiteral("The model's answer wasn't the JSON asked for.")});
                    return;
                }
                m_result = document.object();
            }
            finish();
            return;
        }
        case Embed: {
            const QJsonObject json = QJsonDocument::fromJson(m_body).object();
            const auto vector = [](const QJsonArray &values) {
                QList<float> out;
                out.reserve(values.size());
                for (const QJsonValue &value : values)
                    out.append(float(value.toDouble()));
                return out;
            };
            if (json.contains(QStringLiteral("embeddings"))) {  // Ollama
                for (const QJsonValue &values : json.value(QStringLiteral("embeddings")).toArray())
                    m_vectors.append(vector(values.toArray()));
                m_usage.inputTokens = json.value(QStringLiteral("prompt_eval_count")).toInteger();
            } else {
                QList<QJsonObject> items;
                for (const QJsonValue &item : json.value(QStringLiteral("data")).toArray())
                    items.append(item.toObject());
                std::sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) {
                    return a.value(QStringLiteral("index")).toInt() < b.value(QStringLiteral("index")).toInt();
                });
                for (const QJsonObject &item : items)
                    m_vectors.append(vector(item.value(QStringLiteral("embedding")).toArray()));
                m_usage.inputTokens = json.value(QStringLiteral("usage")).toObject().value(QStringLiteral("prompt_tokens")).toInteger();
            }
            finish(m_vectors.isEmpty() ? Ai::Error{Ai::Error::Invalid, QStringLiteral("No embeddings came back.")} : Ai::Error{});
            return;
        }
        case Speech: {
            int rate = 24000;
            QByteArray pcm = GeminiProvider::pcmFromWav(m_body, &rate);
            // The player's rate: servers speak at 22.05, 24 or 44.1 kHz.
            if (rate != 24000 && rate > 0 && !pcm.isEmpty()) {
                QVector<float> samples(pcm.size() / 2);
                const auto *data = reinterpret_cast<const qint16 *>(pcm.constData());
                for (qsizetype i = 0; i < samples.size(); ++i)
                    samples[i] = data[i] / 32768.0f;
                pcm = Speech::pcm16(Speech::resample(samples, rate, 24000));
            }
            m_sampleRate = 24000;
            addAudio(pcm);
            finish(pcm.isEmpty() ? Ai::Error{Ai::Error::Invalid, QStringLiteral("No audio came back.")} : Ai::Error{});
            return;
        }
        case Models: {
            const QJsonObject json = QJsonDocument::fromJson(m_body).object();
            QJsonArray names;
            for (const QJsonValue &model : json.value(QStringLiteral("models")).toArray())
                names.append(model.toObject().value(QStringLiteral("name")).toString());
            for (const QJsonValue &model : json.value(QStringLiteral("data")).toArray())
                names.append(model.toObject().value(QStringLiteral("id")).toString());
            for (const QJsonValue &voice : json.value(QStringLiteral("voices")).toArray()) {
                Ai::Voice v;
                v.id = voice.isString() ? voice.toString() : voice.toObject().value(QStringLiteral("id")).toString();
                v.name = voice.isString() ? voice.toString() : voice.toObject().value(QStringLiteral("name")).toString(v.id);
                if (!v.id.isEmpty())
                    m_voices.append(v);
            }
            m_result.insert(QStringLiteral("models"), names);
            finish();
            return;
        }
        }
    }

    enum ThinkState { ThinkPending, ThinkInside, ThinkDone };
    QPointer<QNetworkReply> m_reply;
    Mode m_mode;
    LocalModels::Kind m_kind;
    bool m_json;
    QByteArray m_body;
    SseParser m_sse;
    QString m_raw;
    QString m_thinkBuf;
    ThinkState m_thinkState = ThinkPending;
    bool m_trimLeading = false;
};

// A reply already decided, for what needs no request.
class ReadyReply : public Ai::Reply {
public:
    ReadyReply(const QList<Ai::Voice> &voices, QObject *parent) : Ai::Reply(parent) {
        m_voices = voices;
        QMetaObject::invokeMethod(this, [this] { finish(); }, Qt::QueuedConnection);
    }
    void abort() override { finish({Ai::Error::Cancelled, {}}); }
};

}  // namespace

LocalModels::LocalModels(QObject *parent) : Ai::Provider(parent) {}

bool LocalModels::isHttpUrl(const QUrl &url) {
    if (!url.isValid() || url.isEmpty() || url.host().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

bool LocalModels::isLoopbackUrl(const QUrl &url) {
    if (!isHttpUrl(url))
        return false;
    const QString host = url.host().toLower();
    if (host == QLatin1String("localhost") || host == QLatin1String("localhost."))
        return true;
    const QHostAddress addr(host);
    return !addr.isNull() && addr.isLoopback();
}

void LocalModels::configure(const Server &server, const QString &textModel, const QString &embeddingModel,
                            const QString &speechModel) {
    if (!server.isValid()) {
        m_server = {};
        m_textModel.clear();
        m_embeddingModel.clear();
        m_speechModel.clear();
        return;
    }
    m_server = server;
    m_textModel = textModel;
    m_embeddingModel = embeddingModel;
    m_speechModel = speechModel;
}

QString LocalModels::kindName(Kind kind) {
    switch (kind) {
    case Ollama:
        return QStringLiteral("Ollama");
    case LlamaCpp:
        return QStringLiteral("llama.cpp");
    case LmStudio:
        return QStringLiteral("LM Studio");
    case Mlx:
        return QStringLiteral("MLX");
    case Vllm:
        return QStringLiteral("vLLM");
    case Generic:
        break;
    }
    return tr("Local server");
}

QString LocalModels::kindId(Kind kind) {
    static const char *ids[] = {"ollama", "llamacpp", "lmstudio", "mlx", "vllm", "generic"};
    return QString::fromLatin1(ids[kind]);
}

LocalModels::Kind LocalModels::kindFromId(const QString &id) {
    for (int kind = Ollama; kind <= Generic; ++kind)
        if (kindId(Kind(kind)) == id)
            return Kind(kind);
    return Generic;
}

QString LocalModels::privacyNote() const {
    if (isLocal())
        return tr("Made on this computer by %1: nothing leaves it.").arg(name());
    return tr("Sent to %1 at %2.").arg(name(), m_server.url.host());
}

QNetworkReply *LocalModels::send(const QByteArray &method, const QString &path, const QJsonObject &body) {
    QUrl url = m_server.url;
    QString base = url.path();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.endsWith(QLatin1String("/v1")) && path.startsWith(QLatin1String("/v1/")))
        base.chop(3);
    url.setPath(base + path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!m_key.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_key.toUtf8());
    // A model loading for the first time can take a while to answer.
    request.setTransferTimeout(10 * 60 * 1000);
    if (method == "GET")
        return m_network.get(request);
    return m_network.sendCustomRequest(request, method, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

Ai::Reply *LocalModels::check() {
    const QString path = m_server.kind == Ollama ? QStringLiteral("/api/tags") : QStringLiteral("/v1/models");
    return new ServerReply(send("GET", path), ServerReply::Models, m_server.kind, {}, QStringLiteral("check"), false, this);
}

QJsonObject LocalModels::chatBody(Kind kind, const Ai::TextRequest &request, const QString &model) {
    QJsonArray messages;
    QString system = request.system;
    const bool schema = !request.schema.isEmpty();
    if (schema && kind == Mlx)
        system += QStringLiteral("\n\nAnswer with JSON only, matching this JSON schema:\n") +
                  QString::fromUtf8(QJsonDocument(request.schema).toJson(QJsonDocument::Compact));
    if (!system.trimmed().isEmpty())
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), system.trimmed()}});
    for (const Ai::Message &message : request.messages)
        messages.append(QJsonObject{{QStringLiteral("role"), role(message)}, {QStringLiteral("content"), message.text}});

    QJsonObject body{{QStringLiteral("model"), model}, {QStringLiteral("messages"), messages}, {QStringLiteral("stream"), request.stream}};
    if (kind == Ollama) {
        // Ollama's own API: its OpenAI one can't set the context, which is
        // 4K by default, too short for a chapter.
        if (supportsOllamaThinking(model))
            body.insert(QStringLiteral("think"), false);
        body.insert(QStringLiteral("keep_alive"), QStringLiteral("15m"));
        body.insert(QStringLiteral("options"), QJsonObject{{QStringLiteral("num_ctx"), request.fast ? 8192 : 32768}});
        if (schema)
            body.insert(QStringLiteral("format"), request.schema);
        return body;
    }
    if (request.stream)
        body.insert(QStringLiteral("stream_options"), QJsonObject{{QStringLiteral("include_usage"), true}});
    if (kind == LlamaCpp || kind == Vllm)
        // Answers, not thinking aloud first, for reasoning models (Qwen3).
        body.insert(QStringLiteral("chat_template_kwargs"), QJsonObject{{QStringLiteral("enable_thinking"), false}});
    if (schema && kind != Mlx)
        body.insert(QStringLiteral("response_format"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                {QStringLiteral("json_schema"), QJsonObject{{QStringLiteral("name"), QStringLiteral("answer")},
                                                                            {QStringLiteral("strict"), true},
                                                                            {QStringLiteral("schema"), request.schema}}}});
    return body;
}

QString LocalModels::cleanText(const QString &text, bool json) {
    QString out = text;
    static const QRegularExpression thinking(QStringLiteral("<think>.*?(</think>|$)"),
                                             QRegularExpression::DotMatchesEverythingOption);
    out.remove(thinking);
    out = out.trimmed();
    if (json && !QJsonDocument::fromJson(out.toUtf8()).isObject()) {
        // Prefer an explicit fenced JSON block before scanning for outer braces.
        static const QRegularExpression fence(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)\\s*```"),
                                              QRegularExpression::CaseInsensitiveOption);
        const auto match = fence.match(out);
        if (match.hasMatch() && QJsonDocument::fromJson(match.captured(1).trimmed().toUtf8()).isObject())
            return match.captured(1).trimmed();
        // Fallback: some models say a word before or after the JSON object.
        const qsizetype open = out.indexOf(QLatin1Char('{'));
        const qsizetype close = out.lastIndexOf(QLatin1Char('}'));
        if (open >= 0 && close > open)
            out = out.mid(open, close - open + 1);
    }
    return out;
}

QString LocalModels::queryText(const QString &model, const QString &text) {
    const QString name = model.toLower();
    if (name.contains(QLatin1String("qwen3-embedding")) || name.contains(QLatin1String("qwen3_embedding")))
        return QStringLiteral("Instruct: Given a question about a book, find the passages that answer it\nQuery: ") + text;
    if (name.contains(QLatin1String("embeddinggemma")))
        return QStringLiteral("task: search result | query: ") + text;
    if (name.contains(QLatin1String("nomic")))
        return QStringLiteral("search_query: ") + text;
    return text;
}

Ai::Reply *LocalModels::generate(const Ai::TextRequest &request) {
    const QString path = m_server.kind == Ollama ? QStringLiteral("/api/chat") : QStringLiteral("/v1/chat/completions");
    return new ServerReply(send("POST", path, chatBody(m_server.kind, request, m_textModel)), ServerReply::Chat,
                           m_server.kind, m_textModel, request.feature, !request.schema.isEmpty(), this);
}

Ai::Reply *LocalModels::embed(const QStringList &texts, bool query) {
    QJsonArray input;
    for (const QString &text : texts)
        input.append(query ? queryText(m_embeddingModel, text) : text);
    const bool ollama = m_server.kind == Ollama;
    QJsonObject body{{QStringLiteral("model"), m_embeddingModel}, {QStringLiteral("input"), input}};
    if (ollama)
        body.insert(QStringLiteral("keep_alive"), QStringLiteral("2m"));
    return new ServerReply(send("POST", ollama ? QStringLiteral("/api/embed") : QStringLiteral("/v1/embeddings"), body),
                           ServerReply::Embed, m_server.kind, m_embeddingModel, QStringLiteral("embed"), false, this);
}

Ai::Reply *LocalModels::synthesize(const Ai::SpeechRequest &request) {
    QJsonObject body{{QStringLiteral("model"), m_speechModel},
                     {QStringLiteral("input"), request.text},
                     {QStringLiteral("voice"), request.voice.section(QLatin1Char(':'), 1)},
                     {QStringLiteral("response_format"), QStringLiteral("wav")}};
    if (m_speechInstructions && !request.style.isEmpty())
        body.insert(QStringLiteral("instructions"), request.style);
    return new ServerReply(send("POST", QStringLiteral("/v1/audio/speech"), body), ServerReply::Speech, m_server.kind,
                           m_speechModel, request.feature, false, this);
}

Ai::Reply *LocalModels::listVoices(const QString &language) {
    Q_UNUSED(language);
    if (!m_speechVoices.isEmpty())
        return new ReadyReply(m_speechVoices, this);
    return new ServerReply(send("GET", QStringLiteral("/v1/audio/voices")), ServerReply::Models, m_server.kind,
                           m_speechModel, QStringLiteral("voices"), false, this);
}

// --- Finding servers ---------------------------------------------------------

QList<QUrl> ServerFinder::usualPlaces() {
    QList<QUrl> places;
    // Ollama, LM Studio, llama-server and MLX, vLLM, Kokoro-FastAPI.
    for (int port : {11434, 1234, 8080, 8000, 8880})
        places.append(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)));
    return places;
}

void ServerFinder::sortModels(LocalModels::Server &server, const QStringList &models) {
    static const QRegularExpression embedding(QStringLiteral("embed|bge|e5-|minilm|gte-|nomic"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression speech(QStringLiteral("tts|kokoro|orpheus|piper|chatterbox|dia-"),
                                           QRegularExpression::CaseInsensitiveOption);
    for (const QString &model : models) {
        if (model.isEmpty())
            continue;
        if (embedding.match(model).hasMatch())
            server.embeddingModels.append(model);
        else if (speech.match(model).hasMatch())
            server.speechModels.append(model);
        else
            server.textModels.append(model);
    }
}

void ServerFinder::cancel() {
    ++m_generation;
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_replies))
        if (reply && reply->isRunning())
            reply->abort();
    m_replies.clear();
    m_pending = 0;
}

void ServerFinder::find(const QList<QUrl> &extra) {
    cancel();
    m_found.clear();
    QList<QUrl> places = usualPlaces();
    for (const QUrl &url : extra) {
        QUrl origin = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::StripTrailingSlash);
        // Strip API endpoint suffixes (/v1) while preserving any reverse-proxy prefix.
        if (origin.path() == QLatin1String("/v1") || origin.path().startsWith(QLatin1String("/v1/")))
            origin.setPath(QString());
        if (LocalModels::isHttpUrl(origin) && !places.contains(origin))
            places.append(origin);
    }
    const int generation = m_generation;
    m_pending = int(places.size());
    if (m_pending == 0) {
        emit finished();
        return;
    }
    for (const QUrl &place : places)
        probe(place, generation);
}

void ServerFinder::probe(const QUrl &origin, int generation) {
    const auto get = [this, origin, generation](const QString &path, std::function<void(int, const QJsonDocument &)> then) {
        QUrl url = origin;
        QString base = url.path();
        while (base.endsWith(QLatin1Char('/')))
            base.chop(1);
        url.setPath(base + path);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(1500);
        QNetworkReply *reply = m_network.get(request);
        m_replies.append(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, then, generation] {
            reply->deleteLater();
            if (generation != m_generation)
                return;
            then(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), QJsonDocument::fromJson(reply->readAll()));
        });
    };
    const auto found = [this, origin, generation](LocalModels::Kind kind, const QStringList &models) {
        LocalModels::Server server;
        server.kind = kind;
        server.url = origin;
        sortModels(server, models);
        m_found.append(server);
        done(generation);
    };
    const auto ids = [](const QJsonDocument &json) {
        QStringList out;
        for (const QJsonValue &model : json.object().value(QStringLiteral("data")).toArray())
            out.append(model.toObject().value(QStringLiteral("id")).toString());
        return out;
    };
    // Ollama first (its /api/tags), then LM Studio (/api/v0/models), then
    // llama-server (/props), then anything with /v1/models.
    get(QStringLiteral("/api/tags"), [=, this](int status, const QJsonDocument &tags) {
        if (status == 200 && tags.object().contains(QStringLiteral("models"))) {
            QStringList names;
            for (const QJsonValue &model : tags.object().value(QStringLiteral("models")).toArray())
                names.append(model.toObject().value(QStringLiteral("name")).toString());
            found(LocalModels::Ollama, names);
            return;
        }
        if (status == 0) {  // nothing listens here
            done(generation);
            return;
        }
        get(QStringLiteral("/api/v0/models"), [=, this](int status, const QJsonDocument &lmstudio) {
            if (status == 200 && lmstudio.object().contains(QStringLiteral("data"))) {
                LocalModels::Server server;
                server.kind = LocalModels::LmStudio;
                server.url = origin;
                for (const QJsonValue &value : lmstudio.object().value(QStringLiteral("data")).toArray()) {
                    const QJsonObject model = value.toObject();
                    const QString id = model.value(QStringLiteral("id")).toString();
                    const QString type = model.value(QStringLiteral("type")).toString();
                    if (type == QLatin1String("embeddings"))
                        server.embeddingModels.append(id);
                    else if (type == QLatin1String("llm") || type == QLatin1String("vlm"))
                        server.textModels.append(id);
                }
                m_found.append(server);
                done(generation);
                return;
            }
            get(QStringLiteral("/props"), [=, this](int status, const QJsonDocument &props) {
                const bool llama = status == 200 && (props.object().contains(QStringLiteral("default_generation_settings")) ||
                                                     props.object().contains(QStringLiteral("model_path")));
                get(QStringLiteral("/v1/models"), [=, this](int status, const QJsonDocument &models) {
                    if (status != 200 || !models.object().contains(QStringLiteral("data"))) {
                        done(generation);
                        return;
                    }
                    const QJsonArray data = models.object().value(QStringLiteral("data")).toArray();
                    const QJsonObject first = data.at(0).toObject();
                    const QString owner = first.value(QStringLiteral("owned_by")).toString();
                    const QString firstId = first.value(QStringLiteral("id")).toString();
                    LocalModels::Kind kind = LocalModels::Generic;
                    if (llama || owner == QLatin1String("llamacpp"))
                        kind = LocalModels::LlamaCpp;
                    else if (owner == QLatin1String("vllm"))
                        kind = LocalModels::Vllm;
                    else if (firstId.contains(QLatin1String("mlx"), Qt::CaseInsensitive))
                        kind = LocalModels::Mlx;
#ifdef Q_OS_MACOS
                    else if (origin.port() == 8080 && owner.isEmpty())
                        kind = LocalModels::Mlx;
#endif
                    found(kind, ids(models));
                });
            });
        });
    });
}

void ServerFinder::done(int generation) {
    if (generation != m_generation)
        return;
    if (--m_pending == 0) {
        m_replies.clear();
        // The usual order of preference: Ollama, then the others as found.
        std::stable_sort(m_found.begin(), m_found.end(), [](const LocalModels::Server &a, const LocalModels::Server &b) {
            return int(a.kind) < int(b.kind);
        });
        emit finished();
    }
}
