#include "tts/tts_process_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstring>

TtsProcessClient::TtsProcessClient(QObject *parent) : QObject(parent) {
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(m_idleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, &TtsProcessClient::onIdleTimeout);

    m_respawnTimer.setSingleShot(true);
    connect(&m_respawnTimer, &QTimer::timeout, this, &TtsProcessClient::onRespawnTimeout);
}

TtsProcessClient::~TtsProcessClient() {
    m_destroying = true;
    shutdown();
}

void TtsProcessClient::setExecutablePath(const QString &path) { m_executablePath = path.trimmed(); }

QString TtsProcessClient::executablePath() const { return resolveExecutablePath(); }

void TtsProcessClient::setLibraryDir(const QString &dir) { m_libraryDir = dir.trimmed(); }

void TtsProcessClient::setIdleTimeoutMs(int ms) {
    m_idleTimeoutMs = qMax(0, ms);
    m_idleTimer.setInterval(m_idleTimeoutMs);
    if (m_idleTimeoutMs == 0)
        m_idleTimer.stop();
}

bool TtsProcessClient::isRunning() const {
    return m_process && m_process->state() != QProcess::NotRunning;
}

qint64 TtsProcessClient::helperPid() const {
    return isRunning() ? m_process->processId() : 0;
}

QString TtsProcessClient::resolveExecutablePath() const {
    if (!m_executablePath.isEmpty())
        return m_executablePath;
    const QString envBin = qEnvironmentVariable("CLOTHBOUND_TTS_BIN").trimmed();
    if (!envBin.isEmpty())
        return envBin;
    if (QCoreApplication::instance()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates{
            appDir + QStringLiteral("/clothbound-tts"),
            QDir::cleanPath(appDir + QStringLiteral("/../Helpers/clothbound-tts")),
            QDir::cleanPath(appDir + QStringLiteral("/../libexec/clothbound-tts")),
        };
        for (const QString &c : candidates) {
            if (QFileInfo::exists(c))
                return c;
        }
    }
    return QStringLiteral("clothbound-tts");
}

bool TtsProcessClient::ensureStarted() {
    if (isRunning())
        return true;

    m_respawnTimer.stop();
    m_intentionalStop = false;
    m_readBuffer.clear();
    m_readOffset = 0;
    m_modelLoaded = false;

    const QString bin = resolveExecutablePath();
    auto *proc = new QProcess(this);
    m_process = proc;
    proc->setProgram(bin);
    QStringList args;
    if (!m_libraryDir.isEmpty()) {
        args << QStringLiteral("--lib-dir") << m_libraryDir;
    }
    proc->setArguments(args);

    connect(proc, &QProcess::readyReadStandardOutput, this, &TtsProcessClient::onReadyReadStdout);
    connect(proc, &QProcess::readyReadStandardError, this, &TtsProcessClient::onReadyReadStderr);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &TtsProcessClient::onProcessFinished);
    connect(proc, &QProcess::errorOccurred, this, &TtsProcessClient::onProcessError);

    proc->start(QIODevice::ReadWrite);
    if (!proc->waitForStarted(3000)) {
        const QString err = proc->errorString();
        proc->disconnect(this);
        m_process = nullptr;
        delete proc;
        if (!m_destroying)
            emit requestError(0, QStringLiteral("spawn_failed"),
                              QStringLiteral("Failed to launch clothbound-tts (%1): %2").arg(bin, err));
        return false;
    }
    return true;
}

bool TtsProcessClient::sendFrame(ClothboundTts::FrameType type, const QByteArray &payload) {
    if (!isRunning())
        return false;
    const auto wire = ClothboundTts::encodeFrame(
        type, reinterpret_cast<const uint8_t *>(payload.constData()), static_cast<size_t>(payload.size()));
    if (wire.empty())
        return false;
    const qint64 written = m_process->write(reinterpret_cast<const char *>(wire.data()), static_cast<qint64>(wire.size()));
    return written == static_cast<qint64>(wire.size());
}

void TtsProcessClient::sendLoadModelFrame(const ModelConfig &config) {
    QJsonObject obj{
        {QStringLiteral("kind"), config.kind},
        {QStringLiteral("model"), config.model},
        {QStringLiteral("tokens"), config.tokens},
        {QStringLiteral("data_dir"), config.dataDir},
        {QStringLiteral("voices"), config.voices},
        {QStringLiteral("lexicon"), config.lexicon},
        {QStringLiteral("dict_dir"), config.dictDir},
        {QStringLiteral("lang"), config.lang},
        {QStringLiteral("dir"), config.dir},
        {QStringLiteral("reference_wav"), config.referenceWav},
        {QStringLiteral("lib_path"), config.libPath},
        {QStringLiteral("provider"), config.provider},
        {QStringLiteral("threads"), config.threads},
    };
    sendFrame(ClothboundTts::FrameType::LoadModel, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void TtsProcessClient::ping() {
    m_idleTimer.stop();
    if (!ensureStarted())
        return;
    sendFrame(ClothboundTts::FrameType::Ping);
}

void TtsProcessClient::loadModel(const ModelConfig &config) {
    m_idleTimer.stop();
    m_lastModelConfig = config;
    m_hasModelConfig = true;
    m_modelLoaded = false;
    if (!ensureStarted())
        return;
    sendLoadModelFrame(config);
}

uint64_t TtsProcessClient::synthesize(const QString &text,
                                      int speakerId,
                                      float speed,
                                      ClothboundTts::PcmSampleFormat format) {
    m_idleTimer.stop();
    const uint64_t reqId = m_nextRequestId++;
    const bool wasRunning = isRunning();
    if (!ensureStarted()) {
        if (!m_destroying)
            emit requestError(reqId, QStringLiteral("spawn_failed"),
                              QStringLiteral("TTS helper process is not running."));
        return reqId;
    }
    if (!wasRunning && m_hasModelConfig)
        sendLoadModelFrame(m_lastModelConfig);

    m_activeRequests.insert(reqId);
    QJsonObject obj{
        {QStringLiteral("request_id"), static_cast<qint64>(reqId)},
        {QStringLiteral("text"), text},
        {QStringLiteral("speaker_id"), speakerId},
        {QStringLiteral("speed"), static_cast<double>(speed)},
        {QStringLiteral("pcm_format"),
         format == ClothboundTts::PcmSampleFormat::Int16 ? QStringLiteral("i16") : QStringLiteral("f32")},
    };
    if (!sendFrame(ClothboundTts::FrameType::Synthesize, QJsonDocument(obj).toJson(QJsonDocument::Compact))) {
        m_activeRequests.remove(reqId);
        if (!m_destroying)
            emit requestError(reqId, QStringLiteral("write_failed"),
                              QStringLiteral("Failed writing SYNTHESIZE request to helper."));
        armIdleTimerIfQuiet();
    }
    return reqId;
}

void TtsProcessClient::cancel(uint64_t requestId) {
    if (requestId == 0 || !isRunning())
        return;
    QJsonObject obj{{QStringLiteral("request_id"), static_cast<qint64>(requestId)}};
    sendFrame(ClothboundTts::FrameType::Cancel, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void TtsProcessClient::shutdown() {
    m_idleTimer.stop();
    m_respawnTimer.stop();
    m_intentionalStop = true;
    m_modelLoaded = false;
    m_activeRequests.clear();

    if (QProcess *proc = m_process.data()) {
        m_process = nullptr;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            const auto wire = ClothboundTts::encodeFrame(ClothboundTts::FrameType::Shutdown);
            if (!wire.empty())
                proc->write(reinterpret_cast<const char *>(wire.data()), static_cast<qint64>(wire.size()));
            proc->closeWriteChannel();
            if (!proc->waitForFinished(1000)) {
                proc->terminate();
                if (!proc->waitForFinished(500)) {
                    proc->kill();
                    proc->waitForFinished(500);
                }
            }
        }
        delete proc;
    }
    m_readBuffer.clear();
    m_readOffset = 0;
}

void TtsProcessClient::onReadyReadStderr() {
    if (!m_process)
        return;
    // Drain stderr continuously so the helper never stalls on a full pipe buffer.
    (void)m_process->readAllStandardError();
}

void TtsProcessClient::onReadyReadStdout() {
    if (!m_process)
        return;
    m_readBuffer.append(m_process->readAllStandardOutput());

    while (true) {
        const auto available = static_cast<size_t>(m_readBuffer.size() - m_readOffset);
        if (available == 0)
            break;
        const auto *ptr = reinterpret_cast<const uint8_t *>(m_readBuffer.constData() + m_readOffset);
        ClothboundTts::Frame frame;
        size_t consumed = 0;
        const ClothboundTts::DecodeStatus st = ClothboundTts::tryDecodeFrame(ptr, available, frame, consumed);
        if (st == ClothboundTts::DecodeStatus::NeedMoreData)
            break;
        if (st == ClothboundTts::DecodeStatus::BadMagic || st == ClothboundTts::DecodeStatus::FrameTooLarge) {
            m_readBuffer.clear();
            m_readOffset = 0;
            if (!m_destroying)
                emit requestError(0, QStringLiteral("protocol_error"),
                                  st == ClothboundTts::DecodeStatus::BadMagic
                                      ? QStringLiteral("Corrupt frame magic from clothbound-tts.")
                                      : QStringLiteral("Oversized frame from clothbound-tts."));
            if (m_process && m_process->state() != QProcess::NotRunning)
                m_process->kill();
            return;
        }
        m_readOffset += static_cast<qsizetype>(consumed);
        handleFrame(frame);
    }

    if (m_readOffset >= m_readBuffer.size()) {
        m_readBuffer.clear();
        m_readOffset = 0;
    } else if (m_readOffset > 65536) {
        m_readBuffer.remove(0, m_readOffset);
        m_readOffset = 0;
    }
}

void TtsProcessClient::handleFrame(const ClothboundTts::Frame &frame) {
    if (m_destroying)
        return;

    auto parseJsonPayload = [&frame]() -> QJsonObject {
        if (frame.payload.empty())
            return {};
        const QByteArray raw(reinterpret_cast<const char *>(frame.payload.data()),
                             static_cast<qsizetype>(frame.payload.size()));
        return QJsonDocument::fromJson(raw).object();
    };

    switch (frame.type) {
    case ClothboundTts::FrameType::Pong: {
        m_consecutiveCrashes = 0;
        const QJsonObject obj = parseJsonPayload();
        m_modelLoaded = obj.value(QStringLiteral("model_loaded")).toBool(m_modelLoaded);
        emit pongReceived(obj.value(QStringLiteral("protocol")).toInt(int(ClothboundTts::kProtocolVersion)),
                          m_modelLoaded,
                          static_cast<qint64>(obj.value(QStringLiteral("pid")).toDouble(0)));
        armIdleTimerIfQuiet();
        break;
    }
    case ClothboundTts::FrameType::Ready: {
        m_consecutiveCrashes = 0;
        const QJsonObject obj = parseJsonPayload();
        m_modelLoaded = true;
        m_sampleRate = obj.value(QStringLiteral("sample_rate")).toInt(0);
        m_numSpeakers = obj.value(QStringLiteral("num_speakers")).toInt(1);
        emit modelReady(m_sampleRate, m_numSpeakers, obj.value(QStringLiteral("kind")).toString());
        armIdleTimerIfQuiet();
        break;
    }
    case ClothboundTts::FrameType::PcmChunk: {
        ClothboundTts::PcmChunkHeader hdr;
        const uint8_t *pcmPtr = nullptr;
        size_t pcmLen = 0;
        if (!ClothboundTts::decodePcmChunkPayload(frame.payload.data(), frame.payload.size(), hdr, pcmPtr, pcmLen)) {
            emit requestError(0, QStringLiteral("protocol_error"), QStringLiteral("Malformed PCM_CHUNK frame."));
            return;
        }
        const QByteArray pcm(reinterpret_cast<const char *>(pcmPtr), static_cast<qsizetype>(pcmLen));
        emit pcmChunkReceived(hdr.request_id, int(hdr.sample_rate), int(hdr.sample_count), int(hdr.sample_format), pcm);
        break;
    }
    case ClothboundTts::FrameType::Done: {
        const QJsonObject obj = parseJsonPayload();
        const uint64_t reqId = static_cast<uint64_t>(qMax<qint64>(0, static_cast<qint64>(obj.value(QStringLiteral("request_id")).toDouble(0))));
        m_activeRequests.remove(reqId);
        emit synthesisDone(
            reqId, obj.value(QStringLiteral("cancelled")).toBool(false),
            static_cast<uint64_t>(qMax<qint64>(0, static_cast<qint64>(obj.value(QStringLiteral("total_samples")).toDouble(0)))));
        armIdleTimerIfQuiet();
        break;
    }
    case ClothboundTts::FrameType::Error: {
        const QJsonObject obj = parseJsonPayload();
        const uint64_t reqId = static_cast<uint64_t>(qMax<qint64>(0, static_cast<qint64>(obj.value(QStringLiteral("request_id")).toDouble(0))));
        if (reqId > 0)
            m_activeRequests.remove(reqId);
        emit requestError(reqId, obj.value(QStringLiteral("code")).toString(QStringLiteral("error")),
                          obj.value(QStringLiteral("message")).toString());
        armIdleTimerIfQuiet();
        break;
    }
    default:
        break;
    }
}

void TtsProcessClient::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    m_idleTimer.stop();
    m_modelLoaded = false;

    const bool intentional = m_intentionalStop;
    const bool crashed = !intentional && (exitStatus == QProcess::CrashExit || exitCode != 0);

    if (QProcess *proc = m_process.data()) {
        m_process = nullptr;
        proc->disconnect(this);
        proc->deleteLater();
    }
    m_readBuffer.clear();
    m_readOffset = 0;

    if (m_destroying)
        return;

    if (!m_activeRequests.isEmpty()) {
        failAllActiveRequests(crashed ? QStringLiteral("helper_crashed") : QStringLiteral("helper_exited"),
                              QStringLiteral("clothbound-tts exited while synthesis was active."));
    }

    emit helperExited(exitCode, crashed);

    if (crashed) {
        ++m_consecutiveCrashes;
        if (m_consecutiveCrashes <= kMaxConsecutiveCrashes) {
            const int shift = qMin(m_consecutiveCrashes - 1, 6);
            const int delayMs = qMin(kMaxRespawnBackoffMs, kBaseRespawnBackoffMs << shift);
            m_respawnTimer.start(delayMs);
            emit helperRespawnScheduled(delayMs, m_consecutiveCrashes);
        } else {
            emit requestError(0, QStringLiteral("respawn_exhausted"),
                              QStringLiteral("clothbound-tts crashed repeatedly; auto-respawn paused."));
        }
    }
}

void TtsProcessClient::onProcessError(QProcess::ProcessError error) {
    if (m_destroying || m_intentionalStop)
        return;
    if (error == QProcess::FailedToStart) {
        emit requestError(0, QStringLiteral("spawn_failed"),
                          m_process ? m_process->errorString() : QStringLiteral("Failed to start clothbound-tts."));
    }
}

void TtsProcessClient::onIdleTimeout() {
    if (m_destroying || !m_activeRequests.isEmpty() || !isRunning())
        return;
    // Gracefully stop the helper so ONNX Runtime releases all RAM back to the OS.
    m_intentionalStop = true;
    sendFrame(ClothboundTts::FrameType::Shutdown);
    m_process->closeWriteChannel();
}

void TtsProcessClient::onRespawnTimeout() {
    if (m_destroying || isRunning())
        return;
    if (ensureStarted() && m_hasModelConfig)
        sendLoadModelFrame(m_lastModelConfig);
}

void TtsProcessClient::armIdleTimerIfQuiet() {
    if (m_idleTimeoutMs > 0 && isRunning() && m_activeRequests.isEmpty())
        m_idleTimer.start(m_idleTimeoutMs);
}

void TtsProcessClient::failAllActiveRequests(const QString &code, const QString &message) {
    const QList<uint64_t> ids = m_activeRequests.values();
    m_activeRequests.clear();
    for (uint64_t id : ids)
        emit requestError(id, code, message);
}
