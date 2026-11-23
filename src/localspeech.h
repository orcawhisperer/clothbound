#pragma once

#include "ai.h"
#include "voicepacks.h"

#include <QHash>
#include <QLibrary>
#include <QThread>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

namespace SherpaOnnx {
struct OfflineTts;
}

// What makes the samples of built-in speech: sherpa-onnx's engine, or in
// tests a stand-in. It's used from one thread only.
class TtsEngine {
public:
    virtual ~TtsEngine() = default;
    // Gets a voice ready (a model stays loaded while the voices that follow
    // share it). "" when ready, else what's wrong.
    virtual QString prepare(const VoicePacks::Model &model) = 0;
    virtual int sampleRate() const = 0;
    // One sentence, as samples between -1 and 1; empty when stopped or failed.
    virtual QVector<float> speak(const QString &text, int speaker, const std::atomic_bool &stop) = 0;
};

// The engine that comes with the built-in voices: sherpa-onnx's C library,
// loaded the first time it's needed.
class SherpaEngine : public TtsEngine {
public:
    explicit SherpaEngine(const QString &library);
    ~SherpaEngine() override;

    QString prepare(const VoicePacks::Model &model) override;
    int sampleRate() const override { return m_rate; }
    QVector<float> speak(const QString &text, int speaker, const std::atomic_bool &stop) override;
    static int threads();

private:
    // A Pocket voice's recording, as samples.
    QVector<float> m_reference;
    int m_referenceRate = 24000;
    QString m_referencePath;
    QString load();

    QLibrary m_library;
    bool m_loaded = false;
    void *m_create = nullptr;
    void *m_destroy = nullptr;
    void *m_generate = nullptr;
    void *m_free = nullptr;
    void *m_rateOf = nullptr;
    const SherpaOnnx::OfflineTts *m_tts = nullptr;
    QString m_key;
    int m_rate = 0;
};

class LocalWorker;

// Built-in voices as a provider: speech made on this computer, free and
// private, from the voices downloaded with VoicePacks. Requests are spoken
// one at a time on a worker thread, a sentence at a time, so the reply
// knows where each sentence ends in the audio.
class LocalSpeech : public Ai::Provider, public Ai::SpeechSynthesizer {
    Q_OBJECT

public:
    using Engines = std::function<std::unique_ptr<TtsEngine>(const QString &library)>;

    explicit LocalSpeech(VoicePacks *packs, QObject *parent = nullptr, Engines engines = {});
    ~LocalSpeech() override;

    QString id() const override { return QStringLiteral("builtin"); }
    QString name() const override { return tr("Built-in voices"); }
    QString privacyNote() const override { return tr("Made on this computer; nothing is sent anywhere."); }
    bool isLocal() const override { return true; }
    bool needsKey() const override { return false; }
    void setKey(const QString &) override {}
    bool hasKey() const override { return true; }
    Ai::Reply *check() override;
    Ai::SpeechSynthesizer *speech() override { return this; }

    Ai::Reply *synthesize(const Ai::SpeechRequest &request) override;
    Ai::Reply *listVoices(const QString &language) override;
    QString speechModelName(bool) const override;
    bool supportsStyle() const override { return false; }
    bool supportsStreaming() const override { return true; }
    bool canSpeak(const QString &voice) const override { return m_packs->model(voice).has_value(); }

    // Seconds of speech a voice makes in a second on this computer; 0 until
    // it has been heard here.
    Q_INVOKABLE double speedOf(const QString &voiceId) const override;
    // Records a measured pace (from the worker).
    void noteSpeed(const QString &modelKey, double speed);

signals:
    void speedChanged();

private:
    VoicePacks *m_packs;
    QThread m_thread;
    LocalWorker *m_worker;
    QHash<QString, double> m_speeds;
};
