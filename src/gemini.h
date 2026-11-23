#pragma once

#include "ai.h"

#include <QNetworkAccessManager>
#include <QStringList>

class QNetworkReply;

// Google Gemini over its REST API, with the reader's own key: text through
// the Interactions API, speech through the TTS models, and Voice Design.
// The only place in Dogear that knows Gemini's wire format.
class GeminiProvider : public Ai::Provider, public Ai::TextModel, public Ai::SpeechSynthesizer, public Ai::VoiceDesigner,
                       public Ai::Embedder, public Ai::ImageGenerator {
    Q_OBJECT

public:
    explicit GeminiProvider(QObject *parent = nullptr,
                            const QString &endpoint = QStringLiteral("https://generativelanguage.googleapis.com/v1beta"));

    QString id() const override { return QStringLiteral("gemini"); }
    QString name() const override { return QStringLiteral("Google Gemini"); }
    QString privacyNote() const override;
    bool isLocal() const override { return false; }
    bool needsKey() const override { return true; }
    void setKey(const QString &key) override { m_key = key.trimmed(); }
    bool hasKey() const override { return !m_key.isEmpty(); }
    Ai::Reply *check() override;

    Ai::TextModel *text() override { return this; }
    Ai::SpeechSynthesizer *speech() override { return this; }
    Ai::VoiceDesigner *voiceDesigner() override { return this; }
    Ai::Embedder *embedder() override { return this; }
    Ai::ImageGenerator *images() override { return this; }

    Ai::Reply *generateImage(const Ai::ImageRequest &request) override;
    QString imageModelName() const override { return m_imageModel; }
    double imageCost() const override;
    static QJsonObject imageBody(const Ai::ImageRequest &request);

    Ai::Reply *embed(const QStringList &texts, bool query) override;
    QString embeddingModelName() const override { return m_embeddingModel; }

    Ai::Reply *generate(const Ai::TextRequest &request) override;
    QString textModelName(bool fast) const override { return fast ? m_fastModel : m_textModel; }

    Ai::Reply *synthesize(const Ai::SpeechRequest &request) override;
    Ai::Reply *listVoices(const QString &language) override;
    QString speechModelName(bool economy) const override { return economy ? m_economySpeechModel : m_speechModel; }
    bool supportsStyle() const override { return true; }
    bool supportsStreaming() const override { return true; }

    Ai::Reply *design(const Ai::VoiceDesign &voice) override;
    Ai::Reply *forget(const QString &voiceId) override;

    // Prefers the newest models the key can use, from check()'s list.
    void chooseModels(const QStringList &available);

    // The wire format, public so tests can hold it to the recorded API.
    static QJsonObject textBody(const Ai::TextRequest &request, const QString &model);
    static QJsonObject speechBody(const Ai::SpeechRequest &request, const QString &model);
    static QJsonObject embedBody(const QStringList &texts, bool query, const QString &model);
    static QJsonObject voiceDesignBody(const Ai::VoiceDesign &voice, const QString &model);
    static Ai::Usage parseUsage(const QJsonObject &usage);
    static Ai::Error parseError(int status, const QByteArray &body, int networkError);
    static QList<Ai::Voice> parseVoices(const QJsonArray &voices);
    static Ai::Voice parseVoice(const QJsonObject &voice);
    // The samples of a WAV file (or the bytes as they are, if not WAV).
    static QByteArray pcmFromWav(const QByteArray &wav, int *sampleRate = nullptr);

private:
    QNetworkReply *send(const QByteArray &method, const QString &path, const QJsonObject &body = {});

    QNetworkAccessManager m_network;
    QString m_endpoint;
    QString m_key;
    QString m_textModel = QStringLiteral("gemini-4-hydrogen");
    QString m_fastModel = QStringLiteral("gemini-3.5-flash-lite");
    QString m_speechModel = QStringLiteral("gemini-3.8-flash-tts");
    QString m_economySpeechModel = QStringLiteral("gemini-3.8-flash-lite-tts");
    QString m_embeddingModel = QStringLiteral("gemini-embedding-2");
    QString m_imageModel = QStringLiteral("gemini-3.1-flash-image");
};
