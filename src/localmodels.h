#pragma once

#include "ai.h"

#include <QNetworkAccessManager>
#include <QStringList>
#include <QUrl>

class QNetworkReply;

// A model server the reader runs on their own computer (Ollama, llama.cpp's
// llama-server, LM Studio, MLX, vLLM, or any OpenAI-compatible one), for
// text, embeddings and, where it offers /v1/audio/speech, voices. Nothing
// is bundled: Dogear talks to what's installed. The only place in Dogear
// that knows these servers' wire formats.
class LocalModels : public Ai::Provider, public Ai::TextModel, public Ai::Embedder, public Ai::SpeechSynthesizer {
    Q_OBJECT

public:
    enum Kind { Ollama, LlamaCpp, LmStudio, Mlx, Vllm, Generic };
    Q_ENUM(Kind)

    struct Server {
        Kind kind = Generic;
        QUrl url;  // the server's origin, as http://127.0.0.1:11434
        QStringList textModels;
        QStringList embeddingModels;
        QStringList speechModels;
        bool isValid() const { return url.isValid() && !url.isEmpty(); }
    };

    explicit LocalModels(QObject *parent = nullptr);

    // Which server, and which of its models do what ("" for none).
    void configure(const Server &server, const QString &textModel, const QString &embeddingModel = {},
                   const QString &speechModel = {});
    Server server() const { return m_server; }
    bool isConfigured() const { return m_server.isValid(); }
    static QString kindName(Kind kind);
    static QString kindId(Kind kind);
    static Kind kindFromId(const QString &id);

    QString id() const override { return QStringLiteral("local"); }
    QString name() const override { return kindName(m_server.kind); }
    QString privacyNote() const override;
    bool isLocal() const override { return true; }
    bool needsKey() const override { return false; }
    void setKey(const QString &key) override { m_key = key.trimmed(); }
    bool hasKey() const override { return isConfigured(); }
    Ai::Reply *check() override;

    Ai::TextModel *text() override { return m_textModel.isEmpty() ? nullptr : this; }
    Ai::Embedder *embedder() override { return m_embeddingModel.isEmpty() ? nullptr : this; }
    Ai::SpeechSynthesizer *speech() override { return m_speechModel.isEmpty() ? nullptr : this; }

    Ai::Reply *generate(const Ai::TextRequest &request) override;
    QString textModelName(bool) const override { return m_textModel; }
    // MLX's server has no structured output: its JSON is checked instead.
    bool supportsSchema() const override { return m_server.kind != Mlx; }

    Ai::Reply *embed(const QStringList &texts, bool query) override;
    QString embeddingModelName() const override { return m_embeddingModel; }

    // Studio voices: /v1/audio/speech, with instructions where the server
    // takes them (Qwen3-TTS does), so styles work.
    Ai::Reply *synthesize(const Ai::SpeechRequest &request) override;
    Ai::Reply *listVoices(const QString &language) override;
    QString speechModelName(bool) const override { return m_speechModel; }
    bool supportsStyle() const override { return m_speechInstructions; }
    void setSpeechVoices(const QList<Ai::Voice> &voices, bool instructions) {
        m_speechVoices = voices;
        m_speechInstructions = instructions;
    }

    // The wire formats, public so tests can hold them to the servers' docs.
    static QJsonObject chatBody(Kind kind, const Ai::TextRequest &request, const QString &model);
    // A reply's text, without a reasoning model's thinking or a fence
    // around its JSON.
    static QString cleanText(const QString &text, bool json);
    // Embedding models that want their queries marked (Qwen3-Embedding,
    // EmbeddingGemma, nomic) get the mark they were trained with.
    static QString queryText(const QString &model, const QString &text);
    // Speech models that read instructions on how to speak (Qwen3-TTS's
    // instruct models, gpt-4o-mini-tts-style servers), so styles work.
    static bool takesInstructions(const QString &model) {
        const QString name = model.toLower();
        return name.contains(QLatin1String("qwen3-tts")) || name.contains(QLatin1String("qwen3_tts")) ||
               name.contains(QLatin1String("instruct")) || name.contains(QLatin1String("gpt-4o"));
    }

private:
    QNetworkReply *send(const QByteArray &method, const QString &path, const QJsonObject &body = {});

    QNetworkAccessManager m_network;
    Server m_server;
    QString m_key;
    QString m_textModel;
    QString m_embeddingModel;
    QString m_speechModel;
    QList<Ai::Voice> m_speechVoices;
    bool m_speechInstructions = false;
};

// Finds the model servers running on this computer, by asking each usual
// port what it is: the answer's shape tells the servers apart, since some
// share a port (llama-server and MLX use 8080, LM Studio 1234).
class ServerFinder : public QObject {
    Q_OBJECT

public:
    explicit ServerFinder(QObject *parent = nullptr) : QObject(parent) {}

    // Looks at the usual ports, and `extra` (a URL the reader gave).
    void find(const QList<QUrl> &extra = {});
    QList<LocalModels::Server> servers() const { return m_found; }

    static QList<QUrl> usualPlaces();
    // Which models are for text, embeddings or speech, by their names.
    static void sortModels(LocalModels::Server &server, const QStringList &models);

signals:
    void finished();

private:
    void probe(const QUrl &origin);
    void done();

    QNetworkAccessManager m_network;
    QList<LocalModels::Server> m_found;
    int m_pending = 0;
};
