#include "gemini.h"

#include "sseparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrlQuery>

namespace {
quint32 le32(const char *p) {
    return quint32(quint8(p[0])) | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])) << 16 | quint32(quint8(p[3])) << 24;
}

// One Gemini request, turning the response (streamed or whole) into the
// Ai::Reply signals and results.
class GeminiReply : public Ai::Reply {
public:
    enum Kind { Interaction, Voices, VoiceDesign, Forget, Models, Embed };

    GeminiReply(QNetworkReply *reply, Kind kind, const QString &model, const QString &feature, QObject *parent)
        : Ai::Reply(parent), m_reply(reply), m_kind(kind) {
        m_model = model;
        m_feature = feature;
        reply->setParent(this);
        connect(reply, &QNetworkReply::readyRead, this, &GeminiReply::onReadyRead);
        connect(reply, &QNetworkReply::finished, this, &GeminiReply::onFinished);
    }

    void abort() override {
        m_aborted = true;
        if (m_reply && m_reply->isRunning())
            m_reply->abort();
    }

    // Voice listing can take several pages; the provider chains them here.
    std::function<void(const QString &token)> nextPage;

private:
    int status() const { return m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }
    bool streaming() const {
        return m_reply->header(QNetworkRequest::ContentTypeHeader).toString().contains(QStringLiteral("event-stream"));
    }

    void onReadyRead() {
        // Errors arrive as a JSON body, read whole at the end.
        if (status() >= 400 || !streaming())
            return;
        for (const auto &event : m_sse.feed(m_reply->readAll()))
            handle(event);
    }

    void handle(const SseParser::Event &event) {
        if (event.data == "[DONE]")
            return;
        const QJsonObject data = QJsonDocument::fromJson(event.data).object();
        if (event.name == "step.delta") {
            const QJsonObject delta = data.value("delta").toObject();
            const QString type = delta.value("type").toString();
            if (type == "text") {
                addText(delta.value("text").toString());
            } else if (type == "audio") {
                if (delta.contains("sample_rate"))
                    m_sampleRate = delta.value("sample_rate").toInt(m_sampleRate);
                QByteArray bytes = QByteArray::fromBase64(delta.value("data").toString().toLatin1());
                if (delta.value("mime_type").toString() == "audio/wav")
                    bytes = GeminiProvider::pcmFromWav(bytes, &m_sampleRate);
                addAudio(bytes);
            }
        } else if (event.name == "interaction.completed") {
            m_usage = GeminiProvider::parseUsage(data.value("interaction").toObject().value("usage").toObject());
        } else if (event.name == "error") {
            m_streamError.kind = Ai::Error::Server;
            m_streamError.message = data.value("error").toObject().value("message").toString();
        }
    }

    void readInteraction(const QJsonObject &interaction) {
        m_usage = GeminiProvider::parseUsage(interaction.value("usage").toObject());
        for (const auto &stepValue : interaction.value("steps").toArray()) {
            const QJsonObject step = stepValue.toObject();
            if (step.value("type").toString() != "model_output")
                continue;
            for (const auto &contentValue : step.value("content").toArray()) {
                const QJsonObject content = contentValue.toObject();
                const QString type = content.value("type").toString();
                if (type == "text") {
                    addText(content.value("text").toString());
                } else if (type == "audio") {
                    const QByteArray bytes = QByteArray::fromBase64(content.value("data").toString().toLatin1());
                    addAudio(content.value("mime_type").toString() == "audio/wav"
                                 ? GeminiProvider::pcmFromWav(bytes, &m_sampleRate) : bytes);
                } else if (type == "image") {
                    m_result.insert("image", content);
                }
            }
        }
    }

    void onFinished() {
        if (m_aborted) {
            finish({Ai::Error::Cancelled, QStringLiteral("Cancelled."), 0});
            return;
        }
        const int code = status();
        const QByteArray body = streaming() && code < 400 ? QByteArray() : m_reply->readAll();
        if (code >= 400 || (code == 0 && m_reply->error() != QNetworkReply::NoError)) {
            finish(GeminiProvider::parseError(code, body, int(m_reply->error())));
            return;
        }
        if (streaming()) {
            for (const auto &event : m_sse.feed(m_reply->readAll()))
                handle(event);
            for (const auto &event : m_sse.finish())
                handle(event);
            finish(m_streamError);
            return;
        }
        const QJsonObject json = QJsonDocument::fromJson(body).object();
        switch (m_kind) {
        case Interaction:
            readInteraction(json);
            break;
        case Voices: {
            m_voices += GeminiProvider::parseVoices(json.value("voices").toArray());
            const QString token = json.value("next_page_token").toString();
            if (!token.isEmpty() && nextPage && ++m_pages < 5) {
                nextPage(token);
                return;
            }
            break;
        }
        case VoiceDesign: {
            m_result = json;
            m_voices = {GeminiProvider::parseVoice(json)};
            m_usage = GeminiProvider::parseUsage(json.value("usage").toObject());
            const QJsonObject sample = json.value("sample_audio").toObject();
            const QByteArray bytes = QByteArray::fromBase64(sample.value("data").toString().toLatin1());
            if (!bytes.isEmpty())
                addAudio(GeminiProvider::pcmFromWav(bytes, &m_sampleRate));
            break;
        }
        case Models: {
            QJsonArray names;
            for (const auto &model : json.value("models").toArray())
                names.append(model.toObject().value("name").toString().remove(QStringLiteral("models/")));
            m_result.insert("models", names);
            break;
        }
        case Embed: {
            for (const auto &embedding : json.value("embeddings").toArray()) {
                QList<float> vector;
                for (const auto &value : embedding.toObject().value("values").toArray())
                    vector.append(float(value.toDouble()));
                m_vectors.append(vector);
            }
            m_usage.inputTokens = json.value("usageMetadata").toObject().value("promptTokenCount").toInteger();
            break;
        }
        case Forget:
            break;
        }
        finish();
    }

public:
    // Swaps in the next page's network reply, keeping what was gathered.
    void follow(QNetworkReply *reply) {
        m_reply->deleteLater();
        m_reply = reply;
        reply->setParent(this);
        connect(reply, &QNetworkReply::readyRead, this, &GeminiReply::onReadyRead);
        connect(reply, &QNetworkReply::finished, this, &GeminiReply::onFinished);
    }

private:
    QPointer<QNetworkReply> m_reply;
    Kind m_kind;
    SseParser m_sse;
    Ai::Error m_streamError;
    bool m_aborted = false;
    int m_pages = 0;
};
}

GeminiProvider::GeminiProvider(QObject *parent, const QString &endpoint) : Ai::Provider(parent), m_endpoint(endpoint) {}

QString GeminiProvider::privacyNote() const {
    return tr("Sends the text it needs to Google Gemini with your key. Google may use free-tier requests to improve its "
              "products; paid keys aren't used that way.");
}

QNetworkReply *GeminiProvider::send(const QByteArray &method, const QString &path, const QJsonObject &body) {
    QNetworkRequest request(QUrl(m_endpoint + '/' + path));
    request.setRawHeader("x-goog-api-key", m_key.toUtf8());
    request.setRawHeader("Api-Revision", "2026-05-20");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Long enough for a designed voice (about 25 s) or a long clip; the
    // timer restarts with every byte, so streams never time out mid-way.
    request.setTransferTimeout(90000);
    if (method == "GET")
        return m_network.get(request);
    if (method == "DELETE")
        return m_network.deleteResource(request);
    return m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

Ai::Reply *GeminiProvider::check() {
    auto *reply = new GeminiReply(send("GET", QStringLiteral("models?pageSize=200")), GeminiReply::Models, {}, {}, this);
    connect(reply, &Ai::Reply::finished, this, [this, reply] {
        if (!reply->error()) {
            QStringList names;
            for (const auto &name : reply->result().value("models").toArray())
                names.append(name.toString());
            chooseModels(names);
        }
    });
    return reply;
}

void GeminiProvider::chooseModels(const QStringList &available) {
    if (available.isEmpty())
        return;
    // The preferred model when the key has it; otherwise the newest of its kind.
    auto pick = [&available](const QString &preferred, auto accept) {
        if (available.contains(preferred))
            return preferred;
        QStringList candidates;
        for (const QString &name : available)
            if (accept(name))
                candidates.append(name);
        std::sort(candidates.begin(), candidates.end());
        return candidates.isEmpty() ? preferred : candidates.last();
    };
    auto plain = [](const QString &name) {
        return !name.contains("tts") && !name.contains("image") && !name.contains("embedding") &&
               !name.contains("live") && !name.contains("preview") && name.startsWith("gemini");
    };
    m_textModel = pick(m_textModel, [&](const QString &n) { return plain(n) && !n.contains("lite"); });
    m_fastModel = pick(m_fastModel, [&](const QString &n) { return plain(n) && n.contains("flash-lite"); });
    m_speechModel = pick(m_speechModel, [](const QString &n) { return n.contains("tts") && !n.contains("lite") && !n.contains("preview"); });
    m_economySpeechModel = pick(m_economySpeechModel, [](const QString &n) { return n.contains("lite-tts"); });
}

QJsonObject GeminiProvider::textBody(const Ai::TextRequest &request, const QString &model) {
    QJsonArray input;
    for (const Ai::Message &message : request.messages)
        input.append(QJsonObject{
            {"type", message.fromUser ? "user_input" : "model_output"},
            {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", message.text}}}},
        });
    QJsonObject body{{"model", model}, {"input", input}, {"store", false}};
    if (!request.system.isEmpty())
        body.insert("system_instruction", request.system);
    body.insert("generation_config", QJsonObject{{"thinking_level", request.effort}});
    if (!request.schema.isEmpty())
        body.insert("response_format",
                    QJsonObject{{"type", "text"}, {"mime_type", "application/json"}, {"schema", request.schema}});
    if (request.stream)
        body.insert("stream", true);
    return body;
}

QJsonObject GeminiProvider::speechBody(const Ai::SpeechRequest &request, const QString &model) {
    QJsonObject text{{"type", "text"}, {"text", request.text}};
    if (!request.style.isEmpty())
        text.insert("annotations", QJsonArray{QJsonObject{{"type", "speech_metadata"}, {"style", request.style}}});
    QJsonObject body{
        {"model", model},
        {"input", QJsonArray{QJsonObject{{"type", "user_input"}, {"content", QJsonArray{text}}}}},
        {"response_format", QJsonObject{{"type", "audio"}}},
        {"generation_config", QJsonObject{{"speech_config", QJsonArray{QJsonObject{{"voice", request.voice}}}}}},
        {"store", false},
    };
    if (request.stream)
        body.insert("stream", true);
    if (request.background)
        body.insert("service_tier", "flex");
    return body;
}

QJsonObject GeminiProvider::voiceDesignBody(const Ai::VoiceDesign &voice, const QString &model) {
    QJsonObject design{
        {"model", model},
        {"type", "prompted"},
        {"display_name", voice.name},
        {"prompted", QJsonObject{{"input", voice.description}}},
    };
    if (!voice.gender.isEmpty())
        design.insert("gender", voice.gender);
    if (!voice.language.isEmpty())
        design.insert("language_code", voice.language);
    // Designed voices are always stored; the API refuses them otherwise.
    return QJsonObject{{"store", true}, {"voice", design}};
}

Ai::Reply *GeminiProvider::generate(const Ai::TextRequest &request) {
    const QString model = textModelName(request.fast);
    return new GeminiReply(send("POST", QStringLiteral("interactions"), textBody(request, model)), GeminiReply::Interaction,
                           model, request.feature, this);
}

Ai::Reply *GeminiProvider::synthesize(const Ai::SpeechRequest &request) {
    const QString model = speechModelName(request.economy);
    return new GeminiReply(send("POST", QStringLiteral("interactions"), speechBody(request, model)),
                           GeminiReply::Interaction, model, request.feature, this);
}

Ai::Reply *GeminiProvider::listVoices(const QString &language) {
    auto path = [language](const QString &token) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("page_size"), QStringLiteral("1000"));
        // A region ("en-GB") filters on the server; a bare language is
        // matched against every region here.
        if (language.contains('-'))
            query.addQueryItem(QStringLiteral("language_code"), language);
        if (!token.isEmpty())
            query.addQueryItem(QStringLiteral("page_token"), token);
        return QStringLiteral("voices?") + query.toString(QUrl::FullyEncoded);
    };
    auto *reply = new GeminiReply(send("GET", path({})), GeminiReply::Voices, m_speechModel, QStringLiteral("voices"), this);
    reply->nextPage = [this, reply, path](const QString &token) { reply->follow(send("GET", path(token))); };
    return reply;
}

Ai::Reply *GeminiProvider::design(const Ai::VoiceDesign &voice) {
    return new GeminiReply(send("POST", QStringLiteral("voices"), voiceDesignBody(voice, m_speechModel)),
                           GeminiReply::VoiceDesign, m_speechModel, QStringLiteral("voice design"), this);
}

Ai::Reply *GeminiProvider::forget(const QString &voiceId) {
    return new GeminiReply(send("DELETE", QStringLiteral("voices/") + voiceId), GeminiReply::Forget, {}, {}, this);
}

Ai::Usage GeminiProvider::parseUsage(const QJsonObject &usage) {
    Ai::Usage result;
    result.inputTokens = usage.value("total_input_tokens").toInteger();
    result.outputTokens = usage.value("total_output_tokens").toInteger();
    result.cachedTokens = usage.value("total_cached_tokens").toInteger();
    result.thoughtTokens = usage.value("total_thought_tokens").toInteger();
    for (const auto &entry : usage.value("output_tokens_by_modality").toArray())
        if (entry.toObject().value("modality").toString() == "audio")
            result.audioTokens += entry.toObject().value("tokens").toInteger();
    return result;
}

Ai::Error GeminiProvider::parseError(int status, const QByteArray &body, int networkError) {
    const QJsonObject error = QJsonDocument::fromJson(body).object().value("error").toObject();
    const QString message = error.value("message").toString();
    Ai::Error result;
    result.message = message;
    if (status == 0) {
        result.kind = networkError == QNetworkReply::OperationCanceledError ? Ai::Error::Cancelled : Ai::Error::Offline;
        if (result.message.isEmpty())
            result.message = QStringLiteral("Can't reach Gemini.");
    } else if (body.contains("API_KEY_INVALID") || status == 401 ||
               (status == 400 && message.contains(QStringLiteral("API key"), Qt::CaseInsensitive))) {
        result.kind = Ai::Error::BadKey;
    } else if (status == 429) {
        result.kind = Ai::Error::Limited;
    } else if (status == 403) {
        result.kind = Ai::Error::Refused;
    } else if (status == 404) {
        result.kind = Ai::Error::NotFound;
    } else if (status >= 500) {
        result.kind = Ai::Error::Server;
    } else {
        result.kind = Ai::Error::Invalid;
    }
    if (result.message.isEmpty())
        result.message = QStringLiteral("Gemini answered %1.").arg(status);
    return result;
}

Ai::Voice GeminiProvider::parseVoice(const QJsonObject &json) {
    Ai::Voice voice;
    voice.id = json.value("id").toString();
    voice.name = json.value("display_name").toString();
    voice.description = json.value("description").toString();
    voice.language = json.value("language_code").toString();
    voice.accent = json.value("accent").toString();
    voice.gender = json.value("gender").toString();
    voice.pitch = json.value("pitch").toString();
    voice.persona = json.value("persona").toString();
    voice.designed = json.value("type").toString() == "prompted" || json.value("type").toString() == "replicated";
    voice.prompt = json.value("prompted").toObject().value("input").toString();
    voice.expires = QDateTime::fromString(json.value("expire_time").toString(), Qt::ISODateWithMs);
    if (!voice.expires.isValid())
        voice.expires = QDateTime::fromString(json.value("expire_time").toString(), Qt::ISODate);
    return voice;
}

QList<Ai::Voice> GeminiProvider::parseVoices(const QJsonArray &voices) {
    QList<Ai::Voice> result;
    for (const auto &value : voices)
        result.append(parseVoice(value.toObject()));
    return result;
}

QByteArray GeminiProvider::pcmFromWav(const QByteArray &wav, int *sampleRate) {
    if (wav.size() < 12 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE")
        return wav;
    qsizetype at = 12;
    while (at + 8 <= wav.size()) {
        const QByteArray id = wav.mid(at, 4);
        const quint32 size = le32(wav.constData() + at + 4);
        if (id == "fmt " && sampleRate && at + 16 <= wav.size())
            *sampleRate = int(le32(wav.constData() + at + 12));
        if (id == "data")
            return wav.mid(at + 8, qMin<qsizetype>(size, wav.size() - at - 8));
        at += 8 + size + (size & 1);
    }
    return {};
}

QJsonObject GeminiProvider::embedBody(const QStringList &texts, bool query, const QString &model) {
    QJsonArray requests;
    for (const QString &text : texts)
        requests.append(QJsonObject{{"model", "models/" + model},
                                    {"content", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", text}}}}}},
                                    {"taskType", query ? "RETRIEVAL_QUERY" : "RETRIEVAL_DOCUMENT"},
                                    {"outputDimensionality", 768}});
    return QJsonObject{{"requests", requests}};
}

Ai::Reply *GeminiProvider::embed(const QStringList &texts, bool query) {
    return new GeminiReply(send("POST", QStringLiteral("models/%1:batchEmbedContents").arg(m_embeddingModel),
                                embedBody(texts, query, m_embeddingModel)),
                           GeminiReply::Embed, m_embeddingModel, QStringLiteral("embed"), this);
}
