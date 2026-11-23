#include "ai/gemini.h"
#include "ai/aiusage.h"
#include <QDate>

#include "ai/sseparser.h"

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
    enum Kind { Interaction, Voices, VoiceDesign, Forget, Models, Embed, Image };

    GeminiReply(QNetworkReply *reply, Kind kind, const QString &model, const QString &feature, QObject *parent,
                qint64 estimatedInputTokens = 0)
        : Ai::Reply(parent), m_reply(reply), m_kind(kind), m_estimatedInputTokens(estimatedInputTokens) {
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
        finish({Ai::Error::Cancelled, QStringLiteral("Cancelled."), 0});
    }

    // Voice listing can take several pages; the provider chains them here.
    std::function<void(const QString &token)> nextPage;

private:
    int status() const { return m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0; }
    bool streaming() const {
        return m_reply && m_reply->header(QNetworkRequest::ContentTypeHeader).toString().contains(QStringLiteral("event-stream"));
    }

    void onReadyRead() {
        if (m_aborted || isFinished() || !m_reply)
            return;
        // Errors arrive as a JSON body, read whole at the end.
        if (status() >= 400 || !streaming())
            return;
        for (const auto &event : m_sse.feed(m_reply->readAll()))
            handle(event);
    }

    void handle(const SseParser::Event &event) {
        if (event.data.trimmed() == "[DONE]") {
            m_streamDone = true;
            return;
        }
        const QJsonObject data = QJsonDocument::fromJson(event.data).object();
        const QByteArray name = !event.name.isEmpty() ? event.name : data.value(QStringLiteral("event_type")).toString().toUtf8();
        if (name == "step.delta") {
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
        } else if (name == "interaction.completed") {
            m_streamDone = true;
            m_usage = GeminiProvider::parseUsage(data.value("interaction").toObject().value("usage").toObject());
        } else if (name == "error" || name == "interaction.failed") {
            const QJsonObject err = data.value("error").isObject()
                                        ? data.value("error").toObject()
                                        : data.value("interaction").toObject().value("error").toObject();
            const int errCode = err.value("code").toInt();
            const QString errStatus = err.value("status").toString().toUpper();
            const QString errMsg = err.value("message").toString();
            if (errCode >= 400) {
                m_streamError = GeminiProvider::parseError(errCode, event.data, 0);
            } else if (errStatus == QLatin1String("RESOURCE_EXHAUSTED")) {
                m_streamError = {Ai::Error::Limited, errMsg, 0};
            } else if (errStatus == QLatin1String("UNAUTHENTICATED") || errMsg.contains(QStringLiteral("API key"), Qt::CaseInsensitive)) {
                m_streamError = {Ai::Error::BadKey, errMsg, 0};
            } else if (errStatus == QLatin1String("PERMISSION_DENIED")) {
                m_streamError = {Ai::Error::Refused, errMsg, 0};
            } else if (errStatus == QLatin1String("NOT_FOUND")) {
                m_streamError = {Ai::Error::NotFound, errMsg, 0};
            } else if (errStatus == QLatin1String("INVALID_ARGUMENT")) {
                m_streamError = {Ai::Error::Invalid, errMsg, 0};
            } else {
                m_streamError = {Ai::Error::Server, errMsg, 0};
            }
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
        if (isFinished())
            return;
        if (m_aborted || !m_reply) {
            finish({Ai::Error::Cancelled, QStringLiteral("Cancelled."), 0});
            return;
        }
        const int code = status();
        const bool isStream = streaming();
        if (isStream && code > 0 && code < 400) {
            for (const auto &event : m_sse.feed(m_reply->readAll()))
                handle(event);
            if (m_reply->error() == QNetworkReply::NoError) {
                for (const auto &event : m_sse.finish())
                    handle(event);
            }
            if (m_streamError) {
                finish(m_streamError);
                return;
            }
            if (m_streamDone) {
                finish();
                return;
            }
            if (m_reply->error() != QNetworkReply::NoError) {
                finish(GeminiProvider::parseError(0, {}, int(m_reply->error())));
                return;
            }
            finish({Ai::Error::Offline, QStringLiteral("The connection ended before the answer finished."), 0});
            return;
        }
        const QByteArray body = m_reply->readAll();
        if (code >= 400 || m_reply->error() != QNetworkReply::NoError) {
            Ai::Error err = GeminiProvider::parseError(code >= 400 ? code : 0, body, int(m_reply->error()));
            if (code == 429) {
                bool ok = false;
                const int retrySecs = m_reply->rawHeader("Retry-After").trimmed().toInt(&ok);
                if (ok && retrySecs > 0)
                    err.retryAfterSeconds = retrySecs;
            }
            finish(err);
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (m_kind != Forget && (parseError.error != QJsonParseError::NoError || !doc.isObject())) {
            finish({Ai::Error::Server, QStringLiteral("Gemini returned an incomplete response."), 0});
            return;
        }
        const QJsonObject json = doc.object();
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
            const qint64 reported = json.value("usageMetadata").toObject().value("promptTokenCount").toInteger();
            m_usage.inputTokens = reported > 0 ? reported : m_estimatedInputTokens;
            break;
        }
        case Image: {
            for (const auto &candidate : json.value("candidates").toArray())
                for (const auto &part : candidate.toObject().value("content").toObject().value("parts").toArray()) {
                    const QJsonObject inlineData = part.toObject().value("inlineData").toObject();
                    if (!inlineData.isEmpty() && m_image.isEmpty()) {
                        m_image = QByteArray::fromBase64(inlineData.value("data").toString().toLatin1());
                        m_imageMimeType = inlineData.value("mimeType").toString();
                    }
                    if (part.toObject().contains("text"))
                        m_text += part.toObject().value("text").toString();
                }
            const QJsonObject usage = json.value("usageMetadata").toObject();
            m_usage.inputTokens = usage.value("promptTokenCount").toInteger();
            m_usage.outputTokens = usage.value("candidatesTokenCount").toInteger();
            m_usage.thoughtTokens =
                usage.value("thoughtsTokenCount").toInteger(usage.value("thoughtTokenCount").toInteger());
            if (m_image.isEmpty()) {
                finish({Ai::Error::Refused, m_text.isEmpty() ? QStringLiteral("No picture came back.") : m_text});
                return;
            }
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
    bool m_streamDone = false;
    qint64 m_estimatedInputTokens = 0;
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
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
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
    // Version-aware comparison so "gemini-10" sorts after "gemini-3.1".
    auto versionLess = [](const QString &a, const QString &b) {
        qsizetype ia = 0, ib = 0;
        while (ia < a.size() && ib < b.size()) {
            if (a[ia].isDigit() && b[ib].isDigit()) {
                qsizetype ja = ia, jb = ib;
                while (ja < a.size() && a[ja].isDigit())
                    ++ja;
                while (jb < b.size() && b[jb].isDigit())
                    ++jb;
                const qint64 va = QStringView(a).mid(ia, ja - ia).toLongLong();
                const qint64 vb = QStringView(b).mid(ib, jb - ib).toLongLong();
                if (va != vb)
                    return va < vb;
                ia = ja;
                ib = jb;
            } else {
                if (a[ia] != b[ib])
                    return a[ia] < b[ib];
                ++ia;
                ++ib;
            }
        }
        return a.size() < b.size();
    };
    // The preferred model when the key has it; otherwise the newest of its kind
    // (preferring stable releases before preview builds).
    auto pick = [&available, &versionLess](const QString &preferred, auto accept, bool allowPreviewFallback = false) {
        if (available.contains(preferred))
            return preferred;
        QStringList stable, previews;
        for (const QString &name : available) {
            if (!accept(name))
                continue;
            if (name.contains(QLatin1String("preview")) || name.contains(QLatin1String("-exp")))
                previews.append(name);
            else
                stable.append(name);
        }
        if (!stable.isEmpty()) {
            std::sort(stable.begin(), stable.end(), versionLess);
            return stable.last();
        }
        if (allowPreviewFallback && !previews.isEmpty()) {
            std::sort(previews.begin(), previews.end(), versionLess);
            return previews.last();
        }
        return preferred;
    };
    auto plain = [](const QString &name) {
        return !name.contains("tts") && !name.contains("image") && !name.contains("nano-banana") &&
               !name.contains("embedding") && !name.contains("live") && name.startsWith("gemini");
    };
    m_textModel = pick(m_textModel, [&](const QString &n) { return plain(n) && !n.contains("lite"); });
    m_fastModel = pick(m_fastModel, [&](const QString &n) { return plain(n) && n.contains("flash-lite"); });
    m_speechModel = pick(m_speechModel, [](const QString &n) { return n.contains("tts") && !n.contains("lite"); });
    m_economySpeechModel = pick(m_economySpeechModel, [](const QString &n) { return n.contains("lite-tts"); });
    m_embeddingModel = pick(
        m_embeddingModel, [](const QString &n) { return n.contains("embedding"); }, true);
    m_imageModel = pick(
        m_imageModel,
        [](const QString &n) { return n.startsWith("gemini") && (n.contains("image") || n.contains("nano-banana")); },
        true);
}

QJsonObject GeminiProvider::textBody(const Ai::TextRequest &request, const QString &model) {
    QJsonArray input;
    const bool hasImage = !request.imagePng.isEmpty();
    qsizetype lastUserIdx = -1;
    if (hasImage) {
        for (qsizetype i = 0; i < request.messages.size(); ++i)
            if (request.messages[i].fromUser)
                lastUserIdx = i;
    }
    const auto imagePart = [&request] {
        const QString mime = request.imageMimeType.trimmed().isEmpty()
                                 ? QStringLiteral("image/png")
                                 : request.imageMimeType.trimmed();
        return QJsonObject{
            {"type", "image"},
            {"data", QString::fromLatin1(request.imagePng.toBase64())},
            {"mime_type", mime},
        };
    };
    for (qsizetype i = 0; i < request.messages.size(); ++i) {
        const Ai::Message &message = request.messages[i];
        QJsonArray content;
        if (!hasImage || i != lastUserIdx || !message.text.isEmpty())
            content.append(QJsonObject{{"type", "text"}, {"text", message.text}});
        if (hasImage && i == lastUserIdx)
            content.append(imagePart());
        input.append(QJsonObject{
            {"type", message.fromUser ? "user_input" : "model_output"},
            {"content", content},
        });
    }
    if (hasImage && lastUserIdx < 0) {
        input.append(QJsonObject{
            {"type", "user_input"},
            {"content", QJsonArray{imagePart()}},
        });
    }
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
    qint64 at = 12;
    const qint64 total = wav.size();
    while (at + 8 <= total) {
        const QByteArray id = wav.mid(qsizetype(at), 4);
        const qint64 size = qint64(le32(wav.constData() + at + 4));
        const qint64 available = total - (at + 8);
        if (id == "fmt " && sampleRate && at + 16 <= total) {
            const int rate = int(le32(wav.constData() + at + 12));
            if (rate > 0)
                *sampleRate = rate;
        }
        if (id == "data")
            return wav.mid(qsizetype(at + 8), qsizetype(qMin(size, available)));
        const qint64 step = 8 + size + (size & 1);
        if (step <= 0 || at > total - step)
            break;
        at += step;
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
    qint64 chars = 0;
    for (const QString &text : texts)
        chars += text.size();
    const qint64 estimatedTokens = texts.isEmpty() ? 0 : qMax<qint64>(1, chars / 4);
    return new GeminiReply(send("POST", QStringLiteral("models/%1:batchEmbedContents").arg(m_embeddingModel),
                                embedBody(texts, query, m_embeddingModel)),
                           GeminiReply::Embed, m_embeddingModel, QStringLiteral("embed"), this, estimatedTokens);
}

QJsonObject GeminiProvider::imageBody(const Ai::ImageRequest &request) {
    return QJsonObject{
        {"contents", QJsonArray{QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", request.prompt}}}}}}},
        {"generationConfig", QJsonObject{{"responseModalities", QJsonArray{"IMAGE"}},
                                         {"imageConfig", QJsonObject{{"aspectRatio", request.aspectRatio}, {"imageSize", "1K"}}}}}};
}

Ai::Reply *GeminiProvider::generateImage(const Ai::ImageRequest &request) {
    return new GeminiReply(send("POST", QStringLiteral("models/%1:generateContent").arg(m_imageModel), imageBody(request)),
                           GeminiReply::Image, m_imageModel, request.feature, this);
}

double GeminiProvider::imageCost() const {
    // A 1K picture is about 1,120 output tokens, and the prompt a few hundred.
    Ai::Usage usage;
    usage.inputTokens = 400;
    usage.outputTokens = 1120;
    return AiUsageLedger::cost(m_imageModel, usage, QDate::currentDate(), false);
}
