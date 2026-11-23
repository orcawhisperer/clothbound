#include "localmodels.h"

#include "gemini.h"
#include "sseparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>

namespace {

QString role(const Ai::Message &message) { return message.fromUser ? QStringLiteral("user") : QStringLiteral("assistant"); }

Ai::Error errorFor(QNetworkReply *reply, const QByteArray &body) {
    Ai::Error error;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(body).object();
    QString message = json.value(QStringLiteral("error")).isObject()
                          ? json.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()
                          : json.value(QStringLiteral("error")).toString();
    if (message.isEmpty())
        message = QString::fromUtf8(body.left(300)).trimmed();
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
            }
        } else {
            const QJsonObject choice = json.value(QStringLiteral("choices")).toArray().first().toObject();
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
        // A reasoning model's thinking isn't part of the answer: what
        // streams out begins after it.
        const QString shown = LocalModels::cleanText(m_raw, false);
        if (!m_raw.trimmed().startsWith(QLatin1String("<think>")) || m_raw.contains(QLatin1String("</think>"))) {
            if (shown.size() > m_shown) {
                const QString more = shown.mid(m_shown);
                m_shown = shown.size();
                addText(more);
            }
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
                // Whatever didn't stream: a whole answer, or a last line.
                for (const QByteArray &line : m_body.split('\n'))
                    chatEvent(line.trimmed().startsWith("data:") ? line.trimmed().mid(5) : line);
            }
            for (const SseParser::Event &event : m_sse.finish())
                chatEvent(event.data);
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
            const QByteArray pcm = GeminiProvider::pcmFromWav(m_body, &rate);
            m_sampleRate = rate;
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

    QPointer<QNetworkReply> m_reply;
    Mode m_mode;
    LocalModels::Kind m_kind;
    bool m_json;
    QByteArray m_body;
    SseParser m_sse;
    QString m_raw;
    qsizetype m_shown = 0;
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

void LocalModels::configure(const Server &server, const QString &textModel, const QString &embeddingModel,
                            const QString &speechModel) {
    m_server = server;
    m_textModel = server.isValid() ? textModel : QString();
    m_embeddingModel = server.isValid() ? embeddingModel : QString();
    m_speechModel = server.isValid() ? speechModel : QString();
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
    return tr("Made on this computer by %1: nothing leaves it.").arg(name());
}

QNetworkReply *LocalModels::send(const QByteArray &method, const QString &path, const QJsonObject &body) {
    QUrl url = m_server.url;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
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
    if (json) {
        // Some models fence their JSON, or say a word before it.
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
    const QJsonObject body{{QStringLiteral("model"), m_embeddingModel}, {QStringLiteral("input"), input}};
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

void ServerFinder::find(const QList<QUrl> &extra) {
    m_found.clear();
    QList<QUrl> places = usualPlaces();
    for (const QUrl &url : extra) {
        QUrl origin = url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::StripTrailingSlash);
        if (origin.isValid() && !places.contains(origin))
            places.append(origin);
    }
    m_pending = int(places.size());
    for (const QUrl &place : places)
        probe(place);
}

void ServerFinder::probe(const QUrl &origin) {
    const auto get = [this, origin](const QString &path, std::function<void(int, const QJsonDocument &)> then) {
        QUrl url = origin;
        url.setPath(path);
        QNetworkRequest request(url);
        request.setTransferTimeout(1500);
        QNetworkReply *reply = m_network.get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, then] {
            reply->deleteLater();
            then(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), QJsonDocument::fromJson(reply->readAll()));
        });
    };
    const auto found = [this, origin](LocalModels::Kind kind, const QStringList &models) {
        LocalModels::Server server;
        server.kind = kind;
        server.url = origin;
        sortModels(server, models);
        m_found.append(server);
        done();
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
            done();
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
                done();
                return;
            }
            get(QStringLiteral("/props"), [=, this](int status, const QJsonDocument &props) {
                const bool llama = status == 200 && (props.object().contains(QStringLiteral("default_generation_settings")) ||
                                                     props.object().contains(QStringLiteral("model_path")));
                get(QStringLiteral("/v1/models"), [=, this](int status, const QJsonDocument &models) {
                    if (status != 200 || !models.object().contains(QStringLiteral("data"))) {
                        done();
                        return;
                    }
                    const QJsonObject first = models.object().value(QStringLiteral("data")).toArray().first().toObject();
                    const QString owner = first.value(QStringLiteral("owned_by")).toString();
                    LocalModels::Kind kind = LocalModels::Generic;
                    if (llama || owner == QLatin1String("llamacpp"))
                        kind = LocalModels::LlamaCpp;
                    else if (owner == QLatin1String("vllm"))
                        kind = LocalModels::Vllm;
                    else if (origin.port() == 8080 && owner.isEmpty())
                        kind = LocalModels::Mlx;
                    found(kind, ids(models));
                });
            });
        });
    });
}

void ServerFinder::done() {
    if (--m_pending == 0) {
        // The usual order of preference: Ollama, then the others as found.
        std::stable_sort(m_found.begin(), m_found.end(), [](const LocalModels::Server &a, const LocalModels::Server &b) {
            return int(a.kind) < int(b.kind);
        });
        emit finished();
    }
}
