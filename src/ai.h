#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

// Dogear's intelligence features (narration, the companion, illustration)
// talk to these interfaces only. A provider (Gemini today; an
// OpenAI-compatible server or a local model tomorrow) implements the
// capabilities it has, and the features adapt to what's there. Nothing
// outside a provider's adapter knows its wire format.
namespace Ai {

// What a request used, as the provider reported it.
struct Usage {
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    qint64 cachedTokens = 0;
    qint64 thoughtTokens = 0;
    qint64 audioTokens = 0;
};

struct Error {
    enum Kind { None, NoKey, BadKey, Limited, Budget, Offline, Refused, Server, Unsupported, Cancelled, Invalid, NotFound };
    Kind kind = None;
    QString message;
    int retryAfterSeconds = 0;

    explicit operator bool() const { return kind != None; }
};

struct Voice {
    QString id;
    QString name;
    QString description;
    QString language;
    QString accent;
    QString gender;
    QString pitch;
    QString persona;
    // A designed voice: the description it was made from, and when the
    // provider will let it go unless it's used.
    bool designed = false;
    QString prompt;
    QDateTime expires;
};

struct Message {
    bool fromUser = true;
    QString text;
};

struct TextRequest {
    QString feature;  // for the usage ledger: "ask", "explain", "director"…
    QString system;
    QList<Message> messages;
    // When set, the reply is JSON matching this schema.
    QJsonObject schema;
    // Quick, cheap work (a director's note, a definition) or careful work
    // (answering about a whole book).
    bool fast = false;
    QString effort = QStringLiteral("low");
    bool stream = false;
};

struct SpeechRequest {
    QString feature = QStringLiteral("listen");
    QString text;      // read verbatim
    QString style;     // how to read it, or empty for the voice's own way
    QString voice;
    bool stream = true;
    // The cheaper, faster voice model.
    bool economy = false;
    // Not needed now (an offline download): cheaper, and may take longer.
    bool background = false;
};

struct VoiceDesign {
    QString name;
    QString description;
    QString gender;
    QString language;
};

// One request in flight. Text and audio arrive as signals while it runs;
// finished() comes last, exactly once, with error() set if it failed.
class Reply : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    QString text() const { return m_text; }
    QByteArray audio() const { return m_audio; }  // 16-bit mono PCM
    int sampleRate() const { return m_sampleRate; }
    QJsonObject result() const { return m_result; }
    QList<Voice> voices() const { return m_voices; }
    Usage usage() const { return m_usage; }
    Error error() const { return m_error; }
    QString model() const { return m_model; }
    QString feature() const { return m_feature; }
    bool isFinished() const { return m_finished; }

    virtual void abort() = 0;

signals:
    void textReceived(const QString &delta);
    void audioReceived(const QByteArray &pcm);
    void finished();

protected:
    void addText(const QString &delta) {
        m_text += delta;
        emit textReceived(delta);
    }
    void addAudio(const QByteArray &pcm) {
        m_audio += pcm;
        emit audioReceived(pcm);
    }
    void finish(const Error &error = {}) {
        if (m_finished)
            return;
        m_finished = true;
        m_error = error;
        emit finished();
    }

    QString m_text;
    QByteArray m_audio;
    int m_sampleRate = 24000;
    QJsonObject m_result;
    QList<Voice> m_voices;
    Usage m_usage;
    Error m_error;
    QString m_model;
    QString m_feature;
    bool m_finished = false;
};

class TextModel {
public:
    virtual ~TextModel() = default;
    virtual Reply *generate(const TextRequest &request) = 0;
    virtual QString textModelName(bool fast) const = 0;
    virtual bool supportsSchema() const { return true; }
};

class SpeechSynthesizer {
public:
    virtual ~SpeechSynthesizer() = default;
    virtual Reply *synthesize(const SpeechRequest &request) = 0;
    // Voices for a language ("en-GB"), or every voice for "".
    virtual Reply *listVoices(const QString &language) = 0;
    virtual QString speechModelName(bool economy) const = 0;
    virtual bool supportsStyle() const { return false; }
    virtual bool supportsStreaming() const { return false; }
};

class VoiceDesigner {
public:
    virtual ~VoiceDesigner() = default;
    // Creates a voice from a description; result() holds it, audio() a sample.
    virtual Reply *design(const VoiceDesign &voice) = 0;
    virtual Reply *forget(const QString &voiceId) = 0;
};

// A provider: who it is, where text goes, and the capabilities it offers
// (null for the ones it doesn't).
class Provider : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    // One sentence on where the text goes, shown wherever a feature sends it.
    virtual QString privacyNote() const = 0;
    virtual bool isLocal() const = 0;
    virtual bool needsKey() const = 0;
    virtual void setKey(const QString &key) = 0;
    virtual bool hasKey() const = 0;
    // Confirms the key and reachability; result() lists the models.
    virtual Reply *check() = 0;

    virtual TextModel *text() { return nullptr; }
    virtual SpeechSynthesizer *speech() { return nullptr; }
    virtual VoiceDesigner *voiceDesigner() { return nullptr; }
};

}
