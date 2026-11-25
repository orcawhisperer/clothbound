#include "ai/localmodels.h"

#include "ai/sseparser.h"
#include "formats/audiopcm.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>

namespace {

QString role(const Ai::Message &message) { return message.fromUser ? QStringLiteral("user") : QStringLiteral("assistant"); }

bool supportsOllamaThinking(const QString &model) {
    const QString lower = model.toLower();
    static const QRegularExpression thinking(
        QStringLiteral("qwen3|deepseek-r1|qwq|gpt-oss|cogito|magistral|granite3\\.2"));
    return thinking.match(lower).hasMatch();
}

bool looksLikeUnsupportedImageError(const QString &message) {
    if (message.isEmpty())
        return false;
    static const QRegularExpression rx(
        QStringLiteral("image|vision|multimodal|mmproj|does not support|not support|unsupported|cannot unmarshal"),
        QRegularExpression::CaseInsensitiveOption);
    return rx.match(message).hasMatch();
}

Ai::Error errorFor(QNetworkReply *reply, const QByteArray &body, bool hasImage = false) {
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
    } else if (hasImage && (status == 400 || status == 422 || status == 415 || looksLikeUnsupportedImageError(message))) {
        error.kind = Ai::Error::Invalid;
        message = LocalModels::visionUnsupportedMessage();
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

    static constexpr qsizetype kMaxTextBodyBytes = 16 * 1024 * 1024;
    static constexpr qsizetype kMaxSpeechBodyBytes = 64 * 1024 * 1024;

    ServerReply(QNetworkReply *reply, Mode mode, LocalModels::Kind kind, const QString &model, const QString &feature,
                bool json, QObject *parent, bool hasImage = false, bool requestStream = false)
        : Ai::Reply(parent),
          m_reply(reply),
          m_mode(mode),
          m_kind(kind),
          m_json(json),
          m_hasImage(hasImage),
          m_requestStream(requestStream) {
        m_model = model;
        m_feature = feature;
        reply->setParent(this);
        connect(reply, &QNetworkReply::readyRead, this, [this] { read(); });
        connect(reply, &QNetworkReply::finished, this, [this] { done(); });
    }

    void abort() override {
        m_aborted = true;
        if (m_reply && m_reply->isRunning())
            m_reply->abort();
        finish({Ai::Error::Cancelled, {}});
    }

private:
    bool ok() const {
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        return status >= 200 && status < 300;
    }

    qsizetype maxBodyBytes() const { return m_mode == Speech ? kMaxSpeechBodyBytes : kMaxTextBodyBytes; }

    bool appendBodyBounded(const QByteArray &chunk) {
        const qsizetype unread = m_body.size() - m_ndjsonPos;
        if (unread + chunk.size() > maxBodyBytes()) {
            if (m_reply && m_reply->isRunning())
                m_reply->abort();
            finish({Ai::Error::Server, QStringLiteral("The model server response exceeded the size limit.")});
            return false;
        }
        m_body += chunk;
        return true;
    }

    void drainNdjsonLines(bool flushTail) {
        while (m_ndjsonPos < m_body.size()) {
            const qsizetype end = m_body.indexOf('\n', m_ndjsonPos);
            if (end < 0) {
                if (!flushTail)
                    break;
                const QByteArray line = m_body.mid(m_ndjsonPos).trimmed();
                m_ndjsonPos = m_body.size();
                if (!line.isEmpty())
                    chatEvent(line);
                break;
            }
            const QByteArray line = m_body.mid(m_ndjsonPos, end - m_ndjsonPos).trimmed();
            m_ndjsonPos = end + 1;
            if (!line.isEmpty())
                chatEvent(line);
        }
        if (m_ndjsonPos >= m_body.size()) {
            m_body.clear();
            m_ndjsonPos = 0;
        } else if (m_ndjsonPos >= 65536) {
            m_body.remove(0, m_ndjsonPos);
            m_ndjsonPos = 0;
        }
    }

    void read() {
        if (m_aborted || isFinished() || !m_reply)
            return;
        const QByteArray chunk = m_reply->readAll();
        if (chunk.isEmpty())
            return;
        if (m_mode != Chat || !ok()) {
            (void)appendBodyBounded(chunk);
            return;
        }
        const QByteArray type = m_reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (type.contains("event-stream")) {
            m_streamingSse = true;
            for (const SseParser::Event &event : m_sse.feed(chunk))
                chatEvent(event.data);
            if (m_sse.overflowed()) {
                m_reply->abort();
                finish({Ai::Error::Server, QStringLiteral("The model server stream exceeded the frame size limit.")});
            }
        } else if (type.contains("ndjson") || (m_kind == LocalModels::Ollama && !m_hasImage && m_requestStream)) {
            // Ollama native /api/chat streams a JSON object per line.
            m_streamingNdjson = true;
            if (!appendBodyBounded(chunk))
                return;
            drainNdjsonLines(false);
        } else {
            (void)appendBodyBounded(chunk);
        }
    }

    // A piece of a chat: an SSE "data:" or one of Ollama's lines, or the
    // whole answer when it didn't stream.
    void chatEvent(const QByteArray &data) {
        if (data.trimmed() == "[DONE]") {
            m_streamDone = true;
            return;
        }
        const QJsonObject json = QJsonDocument::fromJson(data).object();
        if (json.isEmpty())
            return;
        QString delta;
        if (json.contains(QStringLiteral("message"))) {  // Ollama
            delta = json.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
            if (json.value(QStringLiteral("done")).toBool()) {
                m_streamDone = true;
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
        if (m_raw.size() + delta.size() > kMaxTextBodyBytes) {
            if (m_reply && m_reply->isRunning())
                m_reply->abort();
            finish({Ai::Error::Server, QStringLiteral("The model server response exceeded the size limit.")});
            return;
        }
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
        if (m_aborted || !m_reply) {
            finish({Ai::Error::Cancelled, {}});
            return;
        }
        const QByteArray tail = m_reply->readAll();
        if (!ok() || m_reply->error() != QNetworkReply::NoError) {
            finish(errorFor(m_reply, m_body + tail.left(4096), m_hasImage));
            return;
        }
        switch (m_mode) {
        case Chat: {
            const QByteArray type = m_reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
            const bool isSse = m_streamingSse || type.contains("event-stream");
            const bool isNdjson =
                !isSse && (m_streamingNdjson || type.contains("ndjson") ||
                           (m_kind == LocalModels::Ollama && !m_hasImage && m_requestStream));
            if (isSse) {
                if (!tail.isEmpty()) {
                    for (const SseParser::Event &event : m_sse.feed(tail))
                        chatEvent(event.data);
                }
                if (isFinished())
                    return;
                if (!m_sse.overflowed()) {
                    for (const SseParser::Event &event : m_sse.finish())
                        chatEvent(event.data);
                }
                if (isFinished())
                    return;
                if (m_sse.overflowed()) {
                    finish({Ai::Error::Server, QStringLiteral("The model server stream exceeded the frame size limit.")});
                    return;
                }
                if (!m_streamDone) {
                    finish({Ai::Error::Offline, QStringLiteral("The connection ended before the answer finished.")});
                    return;
                }
            } else if (isNdjson) {
                if (!tail.isEmpty() && !appendBodyBounded(tail))
                    return;
                drainNdjsonLines(true);
                if (isFinished())
                    return;
                if (!m_streamDone) {
                    finish({Ai::Error::Offline, QStringLiteral("The connection ended before the answer finished.")});
                    return;
                }
            } else {
                if (!tail.isEmpty() && !appendBodyBounded(tail))
                    return;
                if (!m_body.trimmed().isEmpty()) {
                    if (QJsonDocument::fromJson(m_body).isObject()) {
                        chatEvent(m_body);
                    } else {
                        for (const QByteArray &line : m_body.split('\n'))
                            chatEvent(line.trimmed().startsWith("data:") ? line.trimmed().mid(5) : line);
                    }
                }
                if (isFinished())
                    return;
            }
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
            if (!tail.isEmpty() && !appendBodyBounded(tail))
                return;
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
            if (!tail.isEmpty() && !appendBodyBounded(tail))
                return;
            int rate = 24000;
            QByteArray pcm = AudioPcm::pcmFromWav(m_body, &rate, true);
            // The player's rate: servers speak at 22.05, 24 or 44.1 kHz.
            if (rate != 24000 && rate > 0 && !pcm.isEmpty())
                pcm = AudioPcm::resamplePcm16(pcm, rate, 24000);
            m_sampleRate = 24000;
            addAudio(pcm);
            finish(pcm.isEmpty() ? Ai::Error{Ai::Error::Invalid, QStringLiteral("No audio came back.")} : Ai::Error{});
            return;
        }
        case Models: {
            if (!tail.isEmpty() && !appendBodyBounded(tail))
                return;
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
    bool m_hasImage = false;
    bool m_requestStream = false;
    bool m_aborted = false;
    bool m_streamingSse = false;
    bool m_streamingNdjson = false;
    bool m_streamDone = false;
    QByteArray m_body;
    qsizetype m_ndjsonPos = 0;
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

QUrl LocalModels::normalizeServerUrl(const QUrl &url) {
    QUrl out = url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::StripTrailingSlash);
    QString path = out.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    static const QStringList suffixes = {
        QStringLiteral("/v1/chat/completions"),
        QStringLiteral("/v1/completions"),
        QStringLiteral("/v1/embeddings"),
        QStringLiteral("/v1/audio/speech"),
        QStringLiteral("/v1/audio/voices"),
        QStringLiteral("/v1/models"),
        QStringLiteral("/v1"),
        QStringLiteral("/api/v0/models"),
        QStringLiteral("/api/tags"),
        QStringLiteral("/api/chat"),
        QStringLiteral("/api/embed"),
        QStringLiteral("/api/show"),
        QStringLiteral("/props"),
    };
    for (const QString &suffix : suffixes) {
        if (path.endsWith(suffix, Qt::CaseInsensitive)) {
            path.chop(suffix.size());
            break;
        }
    }
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    out.setPath(path);
    return out;
}

int LocalModels::parseContextWindow(Kind kind, const QJsonObject &json) {
    if (json.isEmpty())
        return 0;
    if (kind == Ollama) {
        int modelCtx = 0;
        const QJsonObject info = json.value(QStringLiteral("model_info")).toObject();
        for (auto it = info.constBegin(); it != info.constEnd(); ++it) {
            if (it.key().endsWith(QLatin1String(".context_length")) || it.key() == QLatin1String("context_length")) {
                const int v = it.value().toInt();
                if (v > modelCtx)
                    modelCtx = v;
            }
        }
        int paramCtx = 0;
        const QString params = json.value(QStringLiteral("parameters")).toString();
        if (!params.isEmpty()) {
            static const QRegularExpression numCtxRx(QStringLiteral("(?:^|\\s)num_ctx\\s+(\\d+)"));
            const auto match = numCtxRx.match(params);
            if (match.hasMatch())
                paramCtx = match.captured(1).toInt();
        }
        if (modelCtx > 0)
            return modelCtx;
        if (paramCtx > 0)
            return paramCtx;
        return json.value(QStringLiteral("context_length")).toInt();
    }
    if (kind == LlamaCpp) {
        const int genCtx = json.value(QStringLiteral("default_generation_settings"))
                               .toObject()
                               .value(QStringLiteral("n_ctx"))
                               .toInt();
        if (genCtx > 0)
            return genCtx;
        const int topCtx = json.value(QStringLiteral("n_ctx")).toInt();
        if (topCtx > 0)
            return topCtx;
    }
    return 0;
}

void LocalModels::configure(const Server &server, const QString &textModel, const QString &embeddingModel,
                            const QString &speechModel) {
    ++m_contextGeneration;
    if (m_contextReply) {
        m_contextReply->abort();
        m_contextReply.clear();
    }
    if (!server.isValid()) {
        m_server = {};
        m_textModel.clear();
        m_embeddingModel.clear();
        m_speechModel.clear();
        m_contextWindow = 0;
        return;
    }
    m_server = server;
    m_server.url = normalizeServerUrl(server.url);
    m_textModel = textModel;
    m_embeddingModel = embeddingModel;
    m_speechModel = speechModel;
    m_contextWindow = server.contextWindow > 0 ? server.contextWindow : 0;
}

void LocalModels::setContextWindow(int tokens) {
    tokens = qMax(0, tokens);
    if (tokens == m_contextWindow)
        return;
    m_contextWindow = tokens;
    m_server.contextWindow = tokens;
    emit contextWindowChanged();
}

void LocalModels::queryContextWindow() {
    ++m_contextGeneration;
    if (m_contextReply) {
        m_contextReply->abort();
        m_contextReply.clear();
    }
    if (!isConfigured() || m_textModel.isEmpty())
        return;
    QNetworkReply *reply = nullptr;
    const Kind kind = m_server.kind;
    if (kind == Ollama)
        reply = send("POST", QStringLiteral("/api/show"), QJsonObject{{QStringLiteral("model"), m_textModel}});
    else if (kind == LlamaCpp)
        reply = send("GET", QStringLiteral("/props"));
    else
        return;
    reply->setParent(this);
    m_contextReply = reply;
    const int generation = m_contextGeneration;
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind, generation] {
        reply->deleteLater();
        if (reply == m_contextReply)
            m_contextReply.clear();
        if (generation != m_contextGeneration)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
            return;
        const int ctx = parseContextWindow(kind, QJsonDocument::fromJson(reply->readAll()).object());
        if (ctx > 0)
            setContextWindow(ctx);
    });
}

int LocalModels::contextWindow(bool fast) const {
    if (m_server.kind == Ollama) {
        const int target = fast ? 8192 : 32768;
        return m_contextWindow > 0 ? qMin(target, m_contextWindow) : target;
    }
    return m_contextWindow;
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

QString LocalModels::visionUnsupportedMessage() {
    return tr("This local model doesn't support images. Switch to a vision model (such as Gemma 3 or Qwen2.5-VL in Ollama) "
              "or use Gemini.");
}

QJsonObject LocalModels::chatBody(Kind kind, const Ai::TextRequest &request, const QString &model, int contextLimit) {
    QJsonArray messages;
    QString system = request.system;
    const bool schema = !request.schema.isEmpty();
    const bool hasImage = !request.imagePng.isEmpty();
    if (schema && kind == Mlx)
        system += QStringLiteral("\n\nAnswer with JSON only, matching this JSON schema:\n") +
                  QString::fromUtf8(QJsonDocument(request.schema).toJson(QJsonDocument::Compact));
    if (!system.trimmed().isEmpty())
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), system.trimmed()}});

    qsizetype lastUserIdx = -1;
    if (hasImage) {
        for (qsizetype i = 0; i < request.messages.size(); ++i)
            if (request.messages[i].fromUser)
                lastUserIdx = i;
    }
    const auto imageUrlPart = [&request] {
        const QString mime = request.imageMimeType.trimmed().isEmpty()
                                 ? QStringLiteral("image/png")
                                 : request.imageMimeType.trimmed();
        const QString dataUri =
            QStringLiteral("data:%1;base64,%2").arg(mime, QString::fromLatin1(request.imagePng.toBase64()));
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                           {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), dataUri}}}};
    };

    for (qsizetype i = 0; i < request.messages.size(); ++i) {
        const Ai::Message &message = request.messages[i];
        if (hasImage && i == lastUserIdx) {
            QJsonArray content;
            if (!message.text.isEmpty())
                content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                           {QStringLiteral("text"), message.text}});
            content.append(imageUrlPart());
            messages.append(QJsonObject{{QStringLiteral("role"), role(message)}, {QStringLiteral("content"), content}});
        } else {
            messages.append(QJsonObject{{QStringLiteral("role"), role(message)}, {QStringLiteral("content"), message.text}});
        }
    }
    if (hasImage && lastUserIdx < 0) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), QJsonArray{imageUrlPart()}}});
    }

    QJsonObject body{{QStringLiteral("model"), model}, {QStringLiteral("messages"), messages}, {QStringLiteral("stream"), request.stream}};
    if (kind == Ollama && !hasImage) {
        // Ollama's own API: its OpenAI one can't set the context, which is
        // 4K by default, too short for a chapter. Clamp to the model's reported
        // context window when known so Ollama never silently truncates or OOMs.
        if (supportsOllamaThinking(model))
            body.insert(QStringLiteral("think"), false);
        body.insert(QStringLiteral("keep_alive"), QStringLiteral("15m"));
        int numCtx = request.fast ? 8192 : 32768;
        if (contextLimit > 0)
            numCtx = qMin(numCtx, contextLimit);
        body.insert(QStringLiteral("options"), QJsonObject{{QStringLiteral("num_ctx"), numCtx}});
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
    const bool hasImage = !request.imagePng.isEmpty();
    // Use OpenAI-compatible /v1/chat/completions for multimodal image_url payloads
    // (supported by Ollama vision models, LM Studio, llama.cpp, vLLM, and MLX).
    const QString path =
        (m_server.kind == Ollama && !hasImage) ? QStringLiteral("/api/chat") : QStringLiteral("/v1/chat/completions");
    return new ServerReply(send("POST", path, chatBody(m_server.kind, request, m_textModel, m_contextWindow)),
                           ServerReply::Chat, m_server.kind, m_textModel, request.feature, !request.schema.isEmpty(),
                           this, hasImage, request.stream);
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
    // Deduplicate loopback aliases (localhost vs 127.0.0.1 on the same port/path)
    // while preferring an explicit extra URL so it matches a saved localUrl.
    auto endpointKey = [](const QUrl &u) {
        QString host = u.host().toLower();
        if (host == QLatin1String("localhost") || host == QLatin1String("localhost."))
            host = QStringLiteral("127.0.0.1");
        const int port = u.port(u.scheme().toLower() == QLatin1String("https") ? 443 : 80);
        return QStringLiteral("%1://%2:%3%4").arg(u.scheme().toLower(), host).arg(port).arg(u.path());
    };
    QList<QUrl> places;
    QSet<QString> seen;
    for (const QUrl &url : extra) {
        // Strip trailing API endpoint suffixes (/v1, /api/tags, ...) while preserving reverse-proxy base paths.
        const QUrl origin = LocalModels::normalizeServerUrl(url);
        if (!LocalModels::isHttpUrl(origin))
            continue;
        const QString key = endpointKey(origin);
        if (!seen.contains(key)) {
            seen.insert(key);
            places.append(origin);
        }
    }
    for (const QUrl &origin : usualPlaces()) {
        const QString key = endpointKey(origin);
        if (!seen.contains(key)) {
            seen.insert(key);
            places.append(origin);
        }
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
    const auto found = [this, origin, generation](LocalModels::Kind kind, const QStringList &models, int contextWindow = 0) {
        LocalModels::Server server;
        server.kind = kind;
        server.url = origin;
        server.contextWindow = contextWindow;
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
                const int llamaCtx = llama ? LocalModels::parseContextWindow(LocalModels::LlamaCpp, props.object()) : 0;
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
                    else if (firstId.contains(QLatin1String("mlx"), Qt::CaseInsensitive) ||
                             owner.contains(QLatin1String("mlx"), Qt::CaseInsensitive))
                        kind = LocalModels::Mlx;
#ifdef Q_OS_MACOS
                    else if (origin.port() == 8080 && owner.isEmpty())
                        kind = LocalModels::Mlx;
#endif
                    found(kind, ids(models), llamaCtx);
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
