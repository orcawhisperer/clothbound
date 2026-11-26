#pragma once

#include "tts/tts_protocol.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTimer>
#include <cstdint>

// Qt6::Core client managing the out-of-process `clothbound-tts` helper binary:
// - Lazy-spawns `clothbound-tts` on first request (0 MB idle RAM at app start).
// - Idle exit timer (default 120s) shuts the helper down cleanly when quiet so
//   ONNX Runtime releases 100% of its 150-400 MB RSS back to the OS.
// - Exponential-backoff auto-respawn on unexpected helper crash, transparently
//   reloading the last configured voice model.
class TtsProcessClient : public QObject {
    Q_OBJECT

public:
    struct ModelConfig {
        QString kind = QStringLiteral("piper");  // "kokoro", "piper", "vits", or "pocket"
        QString model;
        QString tokens;
        QString dataDir;
        QString voices;
        QString lexicon;
        QString dictDir;
        QString lang;
        QString dir;
        QString referenceWav;
        QString libPath;
        QString provider = QStringLiteral("cpu");
        int threads = 0;
    };

    static constexpr int kDefaultIdleTimeoutMs = 120000;
    static constexpr int kBaseRespawnBackoffMs = 250;
    static constexpr int kMaxRespawnBackoffMs = 8000;
    static constexpr int kMaxConsecutiveCrashes = 5;

    explicit TtsProcessClient(QObject *parent = nullptr);
    ~TtsProcessClient() override;

    void setExecutablePath(const QString &path);
    QString executablePath() const;
    void setLibraryDir(const QString &dir);
    QString libraryDir() const { return m_libraryDir; }

    void setIdleTimeoutMs(int ms);
    int idleTimeoutMs() const { return m_idleTimeoutMs; }

    bool isRunning() const;
    bool isModelLoaded() const { return m_modelLoaded; }
    int sampleRate() const { return m_sampleRate; }
    int numSpeakers() const { return m_numSpeakers; }
    qint64 helperPid() const;

    void ping();
    void loadModel(const ModelConfig &config);
    uint64_t synthesize(const QString &text,
                        int speakerId = 0,
                        float speed = 1.0f,
                        ClothboundTts::PcmSampleFormat format = ClothboundTts::PcmSampleFormat::Float32);
    void cancel(uint64_t requestId);
    void shutdown();

signals:
    void pongReceived(int protocolVersion, bool modelLoaded, qint64 pid);
    void modelReady(int sampleRate, int numSpeakers, const QString &kind);
    void pcmChunkReceived(uint64_t requestId,
                          int sampleRate,
                          int sampleCount,
                          int sampleFormat,
                          const QByteArray &pcmBytes);
    void synthesisDone(uint64_t requestId, bool cancelled, uint64_t totalSamples);
    void requestError(uint64_t requestId, const QString &code, const QString &message);
    void helperExited(int exitCode, bool crashed);
    void helperRespawnScheduled(int delayMs, int attempt);

private:
    QString resolveExecutablePath() const;
    bool ensureStarted();
    bool sendFrame(ClothboundTts::FrameType type, const QByteArray &payload = {});
    void sendLoadModelFrame(const ModelConfig &config);
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onIdleTimeout();
    void onRespawnTimeout();
    void handleFrame(const ClothboundTts::Frame &frame);
    void armIdleTimerIfQuiet();
    void failAllActiveRequests(const QString &code, const QString &message);

    QString m_executablePath;
    QString m_libraryDir;
    int m_idleTimeoutMs = kDefaultIdleTimeoutMs;

    QPointer<QProcess> m_process;
    QByteArray m_readBuffer;
    qsizetype m_readOffset = 0;

    QTimer m_idleTimer;
    QTimer m_respawnTimer;

    bool m_intentionalStop = false;
    bool m_destroying = false;
    bool m_modelLoaded = false;
    bool m_hasModelConfig = false;
    ModelConfig m_lastModelConfig;
    int m_sampleRate = 0;
    int m_numSpeakers = 0;
    int m_consecutiveCrashes = 0;
    uint64_t m_nextRequestId = 1;
    QSet<uint64_t> m_activeRequests;
};
