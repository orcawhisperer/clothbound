#include "audio/speechplayer.h"

#include "audio/speech.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QMetaObject>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace {
bool audioProfileEnabled() {
    static const bool on = qEnvironmentVariableIntValue("CLOTHBOUND_PROFILE") > 0;
    return on;
}

const QVector<float> &window() {
    // A periodic Hann window: two of them half a frame apart sum to one, so
    // overlap-adding keeps the level steady.
    static const QVector<float> hann = [] {
        QVector<float> w(Stretcher::frame);
        for (int n = 0; n < Stretcher::frame; ++n)
            w[n] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * n / Stretcher::frame));
        return w;
    }();
    return hann;
}

qint16 clip16(float value) { return qint16(std::clamp(std::lround(value), -32768L, 32767L)); }

QAudioFormat makeFormat(int rate, int channels, QAudioFormat::SampleFormat sampleFormat) {
    QAudioFormat fmt;
    fmt.setSampleRate(rate);
    fmt.setChannelCount(channels);
    fmt.setSampleFormat(sampleFormat);
    return fmt;
}

QAudioFormat negotiateFormat(const QAudioDevice &device, int requestedRate) {
    const QAudioFormat ideal = makeFormat(requestedRate, 1, QAudioFormat::Int16);
    if (device.isFormatSupported(ideal))
        return ideal;
    const QList<QAudioFormat> candidates{
        makeFormat(requestedRate, 2, QAudioFormat::Int16),
        makeFormat(48000, 1, QAudioFormat::Int16),
        makeFormat(48000, 2, QAudioFormat::Int16),
        makeFormat(44100, 1, QAudioFormat::Int16),
        makeFormat(44100, 2, QAudioFormat::Int16),
        makeFormat(requestedRate, 1, QAudioFormat::Float),
        makeFormat(requestedRate, 2, QAudioFormat::Float),
        makeFormat(48000, 2, QAudioFormat::Float),
        makeFormat(44100, 2, QAudioFormat::Float),
        device.preferredFormat(),
    };
    for (const QAudioFormat &cand : candidates) {
        if (cand.isValid() && cand.sampleRate() > 0 && cand.channelCount() > 0 &&
            (cand.sampleFormat() == QAudioFormat::Int16 || cand.sampleFormat() == QAudioFormat::Int32 ||
             cand.sampleFormat() == QAudioFormat::Float || cand.sampleFormat() == QAudioFormat::UInt8) &&
            device.isFormatSupported(cand)) {
            qWarning("SpeechPlayer: audio device '%s' does not support %d Hz mono Int16; falling back to %d Hz %d ch fmt=%d",
                     qPrintable(device.description()), requestedRate, cand.sampleRate(), cand.channelCount(),
                     int(cand.sampleFormat()));
            return cand;
        }
    }
    qWarning("SpeechPlayer: audio device '%s' has no supported PCM format for %d Hz mono Int16",
             qPrintable(device.description()), requestedRate);
    return {};
}

QByteArray convertForSink(const QByteArray &monoInt16, int srcRate, const QAudioFormat &dstFmt) {
    if (monoInt16.isEmpty())
        return {};
    const int dstRate = dstFmt.sampleRate();
    const int dstCh = dstFmt.channelCount();
    const auto dstSampleFmt = dstFmt.sampleFormat();
    if (dstRate == srcRate && dstCh == 1 && dstSampleFmt == QAudioFormat::Int16)
        return monoInt16;

    const qsizetype srcCount = monoInt16.size() / 2;
    const auto *src = reinterpret_cast<const qint16 *>(monoInt16.constData());
    if (dstRate == srcRate && dstSampleFmt == QAudioFormat::Int16) {
        QByteArray out(srcCount * dstCh * 2, Qt::Uninitialized);
        auto *dst = reinterpret_cast<qint16 *>(out.data());
        for (qsizetype i = 0; i < srcCount; ++i) {
            const qint16 s = src[i];
            for (int c = 0; c < dstCh; ++c)
                dst[i * dstCh + c] = s;
        }
        return out;
    }

    QVector<float> samples(srcCount);
    for (qsizetype i = 0; i < srcCount; ++i)
        samples[i] = float(src[i]) / 32768.0f;
    if (dstRate != srcRate)
        samples = Speech::resample(samples, srcRate, dstRate);

    const qsizetype frames = samples.size();
    switch (dstSampleFmt) {
    case QAudioFormat::Int16: {
        QByteArray out(frames * dstCh * 2, Qt::Uninitialized);
        auto *dst = reinterpret_cast<qint16 *>(out.data());
        for (qsizetype i = 0; i < frames; ++i) {
            const qint16 s = clip16(samples[i] * 32767.0f);
            for (int c = 0; c < dstCh; ++c)
                dst[i * dstCh + c] = s;
        }
        return out;
    }
    case QAudioFormat::Int32: {
        QByteArray out(frames * dstCh * 4, Qt::Uninitialized);
        auto *dst = reinterpret_cast<qint32 *>(out.data());
        for (qsizetype i = 0; i < frames; ++i) {
            const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
            const qint32 s = qint32(std::llround(double(clamped) * 2147483647.0));
            for (int c = 0; c < dstCh; ++c)
                dst[i * dstCh + c] = s;
        }
        return out;
    }
    case QAudioFormat::Float: {
        QByteArray out(frames * dstCh * 4, Qt::Uninitialized);
        auto *dst = reinterpret_cast<float *>(out.data());
        for (qsizetype i = 0; i < frames; ++i) {
            const float s = std::clamp(samples[i], -1.0f, 1.0f);
            for (int c = 0; c < dstCh; ++c)
                dst[i * dstCh + c] = s;
        }
        return out;
    }
    case QAudioFormat::UInt8: {
        QByteArray out(frames * dstCh, Qt::Uninitialized);
        auto *dst = reinterpret_cast<quint8 *>(out.data());
        for (qsizetype i = 0; i < frames; ++i) {
            const quint8 s = quint8(std::clamp(std::lround((samples[i] + 1.0f) * 127.5f), 0L, 255L));
            for (int c = 0; c < dstCh; ++c)
                dst[i * dstCh + c] = s;
        }
        return out;
    }
    default:
        return {};
    }
}
}  // namespace

void SpeechPlayer::copySampleRange(const std::vector<PcmBlock> &blocks, qint64 startSample, qint64 count, qint16 *dst) {
    if (count <= 0)
        return;
    if (blocks.empty()) {
        std::memset(dst, 0, size_t(count) * sizeof(qint16));
        return;
    }
    auto it = std::upper_bound(blocks.cbegin(), blocks.cend(), startSample,
                               [](qint64 s, const PcmBlock &b) { return s < b.offset; });
    size_t idx = (it == blocks.cbegin()) ? 0 : size_t(it - blocks.cbegin()) - 1;
    qint64 copied = 0;
    qint64 cursor = startSample;
    while (copied < count && idx < blocks.size()) {
        const auto &block = blocks[idx];
        const qint64 chunkStart = block.offset;
        const qint64 chunkSamples = block.samples ? qint64(block.samples->size()) : 0;
        if (chunkStart + chunkSamples > cursor) {
            const qint64 localStart = qMax<qint64>(0, cursor - chunkStart);
            const qint64 availInChunk = chunkSamples - localStart;
            const qint64 take = std::min(count - copied, availInChunk);
            if (take > 0) {
                std::memcpy(dst + copied, block.samples->data() + localStart, size_t(take) * sizeof(qint16));
                copied += take;
                cursor += take;
            }
        }
        ++idx;
    }
    if (copied < count)
        std::memset(dst + copied, 0, size_t(count - copied) * sizeof(qint16));
}

// Dedicated high-priority audio output driver living on SpeechPlayer::m_audioThread.
// Owns QAudioSink, QMediaDevices, and the 15 ms feed/report timer so GUI-thread
// transitions, Wayland swaps, or document loads never starve audio playback.
class AudioOutputPump : public QObject {
    Q_OBJECT

public:
    AudioOutputPump(SpeechPlayer *owner, int sampleRate, double volume, bool muted)
        : m_owner(owner), m_sampleRate(sampleRate), m_volume(volume), m_muted(muted), m_timer(this) {
        m_timer.setInterval(15);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            fill();
            report();
        });
    }

public slots:
    void startPlayback(quint64 clearGen) {
        Q_ASSERT(QThread::currentThread() == thread());
        if (m_clearGen != clearGen) {
            m_clearGen = clearGen;
            m_consumedIndex = 0;
            m_marks.clear();
            m_writtenBytes = 0;
            m_pendingOut.clear();
            m_lastTrack = -1;
            m_lastSample = -1;
            m_drained = false;
            m_starved = false;
        }
        ensureSink();
        if (!m_sink || !m_out) {
            emit sinkErrorReported(SpeechPlayer::tr("No audio output device is available."));
            return;
        }
        if (m_sink->state() == QAudio::SuspendedState)
            m_sink->resume();
        m_timer.start();
        fill();
    }

    void pausePlayback() {
        Q_ASSERT(QThread::currentThread() == thread());
        m_timer.stop();
        if (m_sink)
            m_sink->suspend();
        reportStarved(false);
    }

    void resetPlayback(quint64 clearGen, bool playingNow) {
        Q_ASSERT(QThread::currentThread() == thread());
        m_clearGen = clearGen;
        m_consumedIndex = 0;
        m_marks.clear();
        m_writtenBytes = 0;
        m_pendingOut.clear();
        m_lastTrack = -1;
        m_lastSample = -1;
        m_drained = true;
        m_starved = false;
        if (m_sink) {
            if (!playingNow) {
                m_timer.stop();
                releaseSink();
            } else {
                // Invalidate m_out BEFORE reset()/start(): Qt 6's PipeWire backend
                // destroys the old QIODevice during reset() and synchronously emits
                // stateChanged(IdleState) before start() returns. Block re-entrant
                // signal callbacks until the new QIODevice is assigned, then fill.
                m_out = nullptr;
                m_resetting = true;
                {
                    const QSignalBlocker blocker(m_sink);
                    m_sink->reset();
                    m_out = m_sink->start();
                }
                m_resetting = false;
                if (m_sink && m_out && m_out->isOpen())
                    fill();
            }
        } else if (!playingNow) {
            m_timer.stop();
        }
    }

    void setVolume(double volume) {
        Q_ASSERT(QThread::currentThread() == thread());
        Q_ASSERT(volume >= 0.0 && volume <= 1.0);
        m_volume = volume;
        if (m_sink && !m_muted)
            m_sink->setVolume(m_volume);
    }

    void setMuted(bool muted) {
        Q_ASSERT(QThread::currentThread() == thread());
        m_muted = muted;
        if (m_sink)
            m_sink->setVolume(m_muted ? 0.0 : m_volume);
    }

    void onWorkerReady() {
        Q_ASSERT(QThread::currentThread() == thread());
        m_owner->m_notifyPending.store(false, std::memory_order_release);
        if (m_owner->isPlaying())
            fill();
    }

    void shutdown() {
        Q_ASSERT(QThread::currentThread() == thread());
        m_timer.stop();
        releaseSink();
        m_devices.reset();
    }

signals:
    void positionReported(quint64 clearGen, int track, qint64 sample);
    void starvedReported(quint64 clearGen, bool starved);
    void drainedReported(quint64 clearGen);
    void sinkErrorReported(const QString &message);

private:
    void ensureSink() {
        if (m_sink)
            return;
        if (!m_devices) {
            m_devices = std::make_unique<QMediaDevices>();
            connect(m_devices.get(), &QMediaDevices::audioOutputsChanged, this, &AudioOutputPump::handleOutputsChanged);
        }
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            return;
        const QAudioFormat format = negotiateFormat(device, m_sampleRate);
        if (!format.isValid())
            return;
        m_outputDevice = device;
        m_sinkFormat = format;
        m_bytesPerFrame = qMax(1, format.bytesPerFrame());
        m_passthroughFormat = (format.sampleRate() == m_sampleRate && format.channelCount() == 1 &&
                               format.sampleFormat() == QAudioFormat::Int16);
        m_sink = new QAudioSink(device, format, this);
        // Half a second queued in the sink on a dedicated HighPriority audio
        // thread: rides out system load spikes without PipeWire/PulseAudio
        // underruns while suspend()/reset() keep pause/skip instant and
        // report() subtracts buffered samples for exact sentence highlight sync.
        const qint64 reqBytes = qMax<qint64>(m_bytesPerFrame, format.bytesForDuration(500000));
        m_sink->setBufferSize(reqBytes);
        m_sink->setVolume(m_muted ? 0.0 : m_volume);
        connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
            if (!m_sink || m_resetting)
                return;
            if (audioProfileEnabled() && state == QAudio::IdleState && m_owner->isPlaying()) {
                fprintf(stderr, "PROFILE audio_idle buf_bytes=%lld free_bytes=%lld\n",
                        static_cast<long long>(m_sink->bufferSize()), static_cast<long long>(m_sink->bytesFree()));
                fflush(stderr);
            }
            if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
                // Never delete m_sink synchronously from inside its own stateChanged signal.
                if (!m_recoveryPending) {
                    m_recoveryPending = true;
                    QMetaObject::invokeMethod(this, &AudioOutputPump::recoverFromSinkError, Qt::QueuedConnection);
                }
                return;
            }
            if (state == QAudio::ActiveState)
                m_recovered = false;
            if (state == QAudio::IdleState && m_owner->isPlaying()) {
                fill();
                report();
            }
        });
        m_out = nullptr;
        m_resetting = true;
        {
            const QSignalBlocker blocker(m_sink);
            m_out = m_sink->start();
        }
        m_resetting = false;
        if (audioProfileEnabled()) {
            fprintf(stderr, "PROFILE audio_sink device=\"%s\" req_bytes=%lld actual_bytes=%lld rate=%d\n",
                    qPrintable(device.description()), static_cast<long long>(reqBytes),
                    static_cast<long long>(m_sink->bufferSize()), m_sampleRate);
            fflush(stderr);
        }
    }

    void recoverFromSinkError() {
        Q_ASSERT(QThread::currentThread() == thread());
        m_recoveryPending = false;
        if (!m_sink)
            return;
        if (m_owner->isPlaying() && !m_recovered) {
            m_recovered = true;
            releaseSink();
            m_marks.clear();
            m_writtenBytes = 0;
            m_pendingOut.clear();
            ensureSink();
            if (m_sink && m_out && m_out->isOpen()) {
                fill();
                return;
            }
        }
        pausePlayback();
        emit sinkErrorReported(SpeechPlayer::tr("Audio output stopped working."));
    }

    void releaseSink() {
        m_pendingOut.clear();
        m_out = nullptr;
        m_recoveryPending = false;
        if (!m_sink)
            return;
        QAudioSink *sink = m_sink;
        m_sink = nullptr;
        sink->disconnect(this);
        sink->stop();
        delete sink;
    }

    void handleOutputsChanged() {
        if (!m_sink)
            return;
        const QAudioDevice current = QMediaDevices::defaultAudioOutput();
        if (current == m_outputDevice && m_sink->error() == QAudio::NoError)
            return;
        const bool wasPlaying = m_owner->isPlaying();
        releaseSink();
        m_marks.clear();
        m_writtenBytes = 0;
        if (wasPlaying) {
            ensureSink();
            if (m_sink && m_out && m_out->isOpen())
                fill();
        }
    }

    bool flushPending() {
        while (!m_pendingOut.isEmpty() && m_out && m_out->isOpen()) {
            const qint64 n = m_out->write(m_pendingOut.constData(), m_pendingOut.size());
            if (n <= 0)
                return false;
            m_pendingOut.remove(0, n);
            m_writtenBytes += n;
        }
        return m_pendingOut.isEmpty();
    }

    void reportStarved(bool starved) {
        if (starved == m_starved)
            return;
        m_starved = starved;
        if (audioProfileEnabled() && starved && m_sink) {
            fprintf(stderr, "PROFILE audio_starved buf_bytes=%lld free_bytes=%lld state=%d\n",
                    static_cast<long long>(m_sink->bufferSize()), static_cast<long long>(m_sink->bytesFree()),
                    int(m_sink->state()));
            fflush(stderr);
        }
        emit starvedReported(m_clearGen, starved);
    }

    qint64 sinkBytesToSourceSamples(qint64 bytes) const {
        if (m_passthroughFormat)
            return bytes / 2;
        const qint64 frames = bytes / qMax(1, m_bytesPerFrame);
        return qint64(double(frames) * double(m_sampleRate) / double(qMax(1, m_sinkFormat.sampleRate())));
    }

    void fill() {
        Q_ASSERT(QThread::currentThread() == thread());
        Q_ASSERT(m_sampleRate > 0 && m_bytesPerFrame > 0 && m_writtenBytes >= 0 && m_consumedIndex >= 0);
        if (!m_out || !m_out->isOpen() || !m_sink || m_resetting)
            return;
        {
            std::unique_lock<std::mutex> lock(m_owner->m_mutex);
            if (!m_owner->m_playing || m_owner->m_headless || m_owner->m_clearGeneration != m_clearGen)
                return;
        }
        if (!flushPending())
            return;
        qint64 room = sinkBytesToSourceSamples(m_sink->bytesFree());
        bool poppedAny = false;
        for (;;) {
            SpeechPlayer::ReadyChunk chunk;
            {
                std::unique_lock<std::mutex> lock(m_owner->m_mutex);
                if (!m_owner->m_playing || m_owner->m_headless || m_owner->m_clearGeneration != m_clearGen ||
                    m_owner->m_ready.isEmpty())
                    break;
                if (room <= 0 && !m_owner->m_ready.first().bytes.isEmpty())
                    break;
                chunk = m_owner->m_ready.takeFirst();
                Q_ASSERT((chunk.bytes.size() & 1) == 0);
                m_owner->m_readySamples = qMax<qint64>(0, m_owner->m_readySamples - chunk.bytes.size() / 2);
                Q_ASSERT(m_owner->m_readySamples >= 0);
            }
            poppedAny = true;
            m_drained = false;
            bool partialWrite = false;
            if (!chunk.bytes.isEmpty()) {
                m_marks.append({sinkBytesToSourceSamples(m_writtenBytes), chunk.trackId, chunk.sourceStart, chunk.rate});
                const QByteArray outBytes =
                    m_passthroughFormat ? chunk.bytes : convertForSink(chunk.bytes, m_sampleRate, m_sinkFormat);
                const qint64 bytes = outBytes.size();
                if (!m_out || !m_out->isOpen()) {
                    m_pendingOut.append(outBytes);
                    partialWrite = true;
                    break;
                }
                const qint64 wrote = bytes > 0 ? m_out->write(outBytes.constData(), bytes) : 0;
                if (wrote > 0)
                    m_writtenBytes += wrote;
                if (wrote < bytes) {
                    const qint64 offset = qMax<qint64>(0, wrote);
                    m_pendingOut.append(outBytes.constData() + offset, bytes - offset);
                    partialWrite = true;
                }
                room -= sinkBytesToSourceSamples(qMax<qint64>(0, wrote));
            }
            if (chunk.trackDone) {
                std::unique_lock<std::mutex> lock(m_owner->m_mutex);
                if (m_owner->m_clearGeneration == m_clearGen) {
                    if (SpeechPlayer::Track *t = m_owner->trackLocked(chunk.trackId, lock)) {
                        t->blocks.clear();
                        t->totalSamples = 0;
                        t->hasTrailingByte = false;
                    }
                    ++m_consumedIndex;
                }
            }
            if (partialWrite)
                break;
        }
        if (poppedAny) {
            std::unique_lock<std::mutex> lock(m_owner->m_mutex);
            m_owner->m_cond.notify_one();
        }
        // Marks already behind what's audible are kept only as far as needed.
        while (m_marks.size() > 64)
            m_marks.removeFirst();
    }

    void report() {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!m_sink)
            return;
        bool allConsumed = false;
        bool playingNow = false;
        {
            std::unique_lock<std::mutex> lock(m_owner->m_mutex);
            if (m_owner->m_clearGeneration != m_clearGen)
                return;
            allConsumed = m_owner->m_ready.isEmpty() && m_owner->m_workerFinishedAll &&
                          m_consumedIndex >= m_owner->m_tracks.size();
            playingNow = m_owner->m_playing;
        }
        const qint64 buffered = sinkBytesToSourceSamples(qMax<qint64>(0, m_sink->bufferSize() - m_sink->bytesFree()));
        const qint64 audible = qMax<qint64>(0, sinkBytesToSourceSamples(m_writtenBytes) - buffered);
        for (int i = int(m_marks.size()) - 1; i >= 0; --i)
            if (m_marks[i].output <= audible) {
                const SpeechPlayer::Mark &mark = m_marks[i];
                const qint64 sample = mark.source + qint64((audible - mark.output) * mark.rate);
                if (mark.track != m_lastTrack || sample != m_lastSample) {
                    m_lastTrack = mark.track;
                    m_lastSample = sample;
                    emit positionReported(m_clearGen, mark.track, sample);
                }
                break;
            }
        const bool empty = (buffered <= 0 || m_sink->state() == QAudio::IdleState) && m_pendingOut.isEmpty();
        if (empty && allConsumed) {
            reportStarved(false);
            if (!m_drained) {
                m_drained = true;
                emit drainedReported(m_clearGen);
            }
        } else {
            reportStarved(playingNow && empty);
        }
    }

    SpeechPlayer *m_owner;
    int m_sampleRate;
    double m_volume;
    bool m_muted;
    QTimer m_timer;
    std::unique_ptr<QMediaDevices> m_devices;
    QAudioDevice m_outputDevice;
    QAudioFormat m_sinkFormat;
    int m_bytesPerFrame = 2;
    bool m_passthroughFormat = true;
    QPointer<QAudioSink> m_sink;
    QPointer<QIODevice> m_out;
    QByteArray m_pendingOut;
    QList<SpeechPlayer::Mark> m_marks;
    qint64 m_writtenBytes = 0;
    int m_consumedIndex = 0;
    int m_lastTrack = -1;
    qint64 m_lastSample = -1;
    quint64 m_clearGen = 0;
    bool m_recovered = false;
    bool m_resetting = false;
    bool m_recoveryPending = false;
    bool m_drained = true;
    bool m_starved = false;
};

void Stretcher::reset(qint64 at) {
    m_position = double(at);
    m_previous = -1;
    m_tail = QVector<float>(hop, 0.f);
    m_finalTail.clear();
    m_finalPos = 0;
    m_flushing = false;
    m_done = false;
}

void Stretcher::reseed(qint64 at, const qint16 *windowPtr, qint64 windowStart, qint64 windowLen, qint64 available) {
    if (!windowPtr || at < hop || at + hop > available || at < windowStart || at + hop > windowStart + windowLen) {
        reset(at);
        return;
    }
    const QVector<float> &w = window();
    m_position = double(at);
    m_previous = at - hop;
    m_tail.resize(hop);
    const qint64 rel = at - windowStart;
    for (int n = 0; n < hop; ++n)
        m_tail[n] = w[hop + n] * float(windowPtr[rel + n]);
    m_finalTail.clear();
    m_finalPos = 0;
    m_flushing = false;
    m_done = false;
}

bool Stretcher::process(const qint16 *source, qint64 available, bool complete, double rate, int room,
                        QVector<qint16> &out) {
    return processWindow(source, 0, available, available, complete, rate, room, out);
}

bool Stretcher::processWindow(const qint16 *windowPtr, qint64 windowStart, qint64 windowLen, qint64 available,
                              bool complete, double rate, int room, QVector<qint16> &out) {
    if (m_done)
        return false;
    if (m_tail.size() != hop)
        m_tail = QVector<float>(hop, 0.f);

    auto sampleAt = [&](qint64 idx) -> qint16 {
        const qint64 rel = idx - windowStart;
        return (windowPtr && rel >= 0 && rel < windowLen) ? windowPtr[rel] : qint16(0);
    };

    auto drainFinal = [&]() -> bool {
        const int take = std::min(room, int(m_finalTail.size()) - m_finalPos);
        if (take > 0) {
            const qsizetype before = out.size();
            out.resize(before + take);
            std::memcpy(out.data() + before, m_finalTail.constData() + m_finalPos, size_t(take) * sizeof(qint16));
            m_finalPos += take;
            room -= take;
        }
        if (m_finalPos >= m_finalTail.size()) {
            m_done = true;
            return false;
        }
        return true;
    };

    if (m_flushing)
        return drainFinal();

    const QVector<float> &w = window();
    while (room >= hop) {
        const qint64 target = qint64(m_position + 0.5);
        if (!complete && target + seek + frame > available)
            return true;
        if (complete && target + frame > available) {
            // The source is spent: blend any remaining samples cleanly instead
            // of dropping the last ~53 ms of speech.
            m_finalTail.clear();
            m_finalPos = 0;
            m_flushing = true;
            if (m_previous < 0) {
                const qint64 start = std::clamp<qint64>(target, 0, available);
                const qint64 rem = available - start;
                m_finalTail.resize(rem);
                for (qint64 k = 0; k < rem; ++k)
                    m_finalTail[k] = sampleAt(start + k);
            } else {
                const qint64 rem = std::max<qint64>(0, available - target);
                m_finalTail.reserve(int(std::max<qint64>(hop, rem)));
                for (int n = 0; n < hop; ++n) {
                    const float incoming = n < rem ? w[n] * float(sampleAt(target + n)) : 0.f;
                    m_finalTail.append(clip16(m_tail[n] + incoming));
                }
                if (rem > hop) {
                    const qint64 extra = rem - hop;
                    for (qint64 k = 0; k < extra; ++k) {
                        const float fade = 1.0f - float(k) / float(extra);
                        m_finalTail.append(clip16(fade * float(sampleAt(target + hop + k))));
                    }
                }
            }
            return drainFinal();
        }
        const qint64 lo = std::max<qint64>(0, target - seek);
        const qint64 hi = std::min<qint64>(available - frame, target + seek);
        qint64 best = std::clamp(target, lo, hi);
        if (m_previous >= 0 && hi >= lo) {
            // The new frame should start where it best continues the last
            // one: compare its first half with what naturally followed, using
            // an exact sliding-window squared-energy prefix sum (O(1) energy
            // per candidate, zero floating-point drift) and coarse-to-fine search.
            const qint64 natural = m_previous + hop;
            if (natural + hop <= available) {
                const qint64 spanLen = hi - lo + hop;
                std::int64_t sqPrefix[2 * seek + hop + 2];
                sqPrefix[0] = 0;
                sqPrefix[1] = 0;
                for (qint64 i = 0; i < spanLen; ++i) {
                    const std::int64_t v = sampleAt(lo + i);
                    sqPrefix[i + 2] = sqPrefix[i] + v * v;
                }
                float ref[hop / 2];
                for (int k = 0; k < hop / 2; ++k)
                    ref[k] = float(sampleAt(natural + 2 * k));

                auto scoreAt = [&](qint64 candidate) -> float {
                    const qint64 c = candidate - lo;
                    const std::int64_t energy = sqPrefix[c + hop] - sqPrefix[c];
                    if (energy <= 0)
                        return 0.f;
                    float dot = 0.f;
                    const qint64 rel = candidate - windowStart;
                    if (windowPtr && rel >= 0 && rel + hop <= windowLen) {
                        const qint16 *candPtr = windowPtr + rel;
                        for (int k = 0; k < hop / 2; ++k)
                            dot += ref[k] * float(candPtr[2 * k]);
                    } else {
                        for (int k = 0; k < hop / 2; ++k)
                            dot += ref[k] * float(sampleAt(candidate + 2 * k));
                    }
                    return dot / std::sqrt(float(energy));
                };

                float bestScore = -1e30f;
                qint64 bestCoarse = best;
                for (qint64 candidate = lo; candidate <= hi; candidate += 2) {
                    const float score = scoreAt(candidate);
                    if (score > bestScore) {
                        bestScore = score;
                        bestCoarse = candidate;
                    }
                }
                best = bestCoarse;
                for (qint64 candidate : {bestCoarse - 1, bestCoarse + 1}) {
                    if (candidate >= lo && candidate <= hi) {
                        const float score = scoreAt(candidate);
                        if (score > bestScore) {
                            bestScore = score;
                            best = candidate;
                        }
                    }
                }
            }
        }
        for (int n = 0; n < hop; ++n)
            out.append(clip16(m_tail[n] + w[n] * float(sampleAt(best + n))));
        for (int n = 0; n < hop; ++n)
            m_tail[n] = w[hop + n] * float(sampleAt(best + hop + n));
        m_previous = best;
        m_position += hop * rate;
        room -= hop;
    }
    return true;
}

SpeechPlayer::SpeechPlayer(int sampleRate, QObject *parent) : QObject(parent), m_sampleRate(sampleRate) {
    m_muted = qEnvironmentVariableIsSet("CLOTHBOUND_MUTE") || qEnvironmentVariableIsSet("DOGEAR_MUTE");
}

SpeechPlayer::~SpeechPlayer() {
    stopWorker();
    stopAudioThread();
}

void SpeechPlayer::ensureAudioThread() {
    Q_ASSERT(QThread::currentThread() == thread());
    double initialVolume = 1.0;
    bool initialMuted = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_pump)
            return;
        initialVolume = m_volume;
        initialMuted = m_muted;
    }
    auto *pump = new AudioOutputPump(this, m_sampleRate, initialVolume, initialMuted);
    pump->moveToThread(&m_audioThread);
    connect(
        pump, &AudioOutputPump::positionReported, this,
        [this](quint64 clearGen, int track, qint64 sample) {
            quint64 currentGen = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                currentGen = m_clearGeneration;
            }
            if (clearGen == currentGen)
                emit position(track, sample);
        },
        Qt::QueuedConnection);
    connect(
        pump, &AudioOutputPump::starvedReported, this,
        [this](quint64 clearGen, bool starved) {
            quint64 currentGen = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                currentGen = m_clearGeneration;
            }
            if (clearGen == currentGen)
                setStarved(starved);
        },
        Qt::QueuedConnection);
    connect(
        pump, &AudioOutputPump::drainedReported, this,
        [this](quint64 clearGen) {
            bool shouldEmit = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (clearGen == m_clearGeneration && !m_drained) {
                    m_drained = true;
                    shouldEmit = true;
                }
            }
            if (shouldEmit)
                emit drained();
        },
        Qt::QueuedConnection);
    connect(
        pump, &AudioOutputPump::sinkErrorReported, this,
        [this](const QString &message) {
            pause();
            emit sinkError(message);
        },
        Qt::QueuedConnection);
    m_audioThread.setObjectName(QStringLiteral("ClothboundAudio"));
    m_audioThread.start(QThread::HighPriority);
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        Q_ASSERT(!m_pump);
        m_pump = pump;
    }
}

void SpeechPlayer::stopAudioThread() {
    Q_ASSERT(QThread::currentThread() == thread());
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        pump = std::exchange(m_pump, nullptr);
    }
    if (!pump)
        return;
    if (m_audioThread.isRunning()) {
        QMetaObject::invokeMethod(pump, &AudioOutputPump::shutdown, Qt::BlockingQueuedConnection);
        m_audioThread.quit();
        m_audioThread.wait();
    }
    delete pump;
}

void SpeechPlayer::startWorker(const std::unique_lock<std::mutex> &lock) {
    Q_ASSERT(lock.mutex() == &m_mutex && lock.owns_lock());
    Q_UNUSED(lock);
    if (m_worker.joinable())
        return;
    m_stopWorker = false;
    m_worker = std::thread(&SpeechPlayer::workerLoop, this);
}

void SpeechPlayer::stopWorker() {
    std::thread worker;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stopWorker = true;
        ++m_generation;
        m_cond.notify_all();
        worker = std::move(m_worker);
    }
    if (worker.joinable())
        worker.join();
}

void SpeechPlayer::workerLoop() {
    Stretcher stretcher;
    quint64 localGen = 0;
    quint64 localRateGen = 0;
    bool stretcherInited = false;
    int stretcherTrackIndex = -1;
    bool needReseed = false;
    const qint64 targetReadySamples = qMax<qint64>(Stretcher::frame * 4, m_sampleRate);
    const int chunkRoom = Stretcher::hop * 8;
    QVector<qint16> windowBuf;

    for (;;) {
        std::vector<PcmBlock> blocksSnap;
        qint64 available = 0;
        bool complete = false;
        double rate = 1.0;
        int trackId = 0;
        qint64 readPos = 0;
        quint64 gen = 0;
        quint64 rateGen = 0;
        bool doReseed = false;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;) {
                if (m_stopWorker)
                    return;
                Q_ASSERT(m_workerIndex >= 0 && m_workerIndex <= m_tracks.size());
                Q_ASSERT(m_workerRead >= 0);
                Q_ASSERT(m_readySamples >= 0);
                Q_ASSERT(!m_ready.isEmpty() || m_readySamples == 0);
                Q_ASSERT(m_rate >= 0.25 && m_rate <= 4.0);
                if (localGen != m_generation) {
                    localGen = m_generation;
                    localRateGen = m_rateGeneration;
                    stretcherInited = false;
                    stretcherTrackIndex = -1;
                    needReseed = false;
                } else if (localRateGen != m_rateGeneration) {
                    localRateGen = m_rateGeneration;
                    if (stretcherInited && stretcherTrackIndex == m_workerIndex &&
                        stretcher.position() == m_workerRead) {
                        // Continuing seamlessly on the same track position: preserve live WSOLA tail.
                        needReseed = false;
                    } else {
                        // Rewound or switching from 1.0x into WSOLA: reseed unity-gain Hann tail cleanly.
                        needReseed = true;
                        stretcherInited = false;
                    }
                }
                const bool canWork = m_playing && !m_headless && m_readySamples < targetReadySamples &&
                                     m_workerIndex < m_tracks.size();
                if (canWork) {
                    const Track &t = m_tracks[m_workerIndex];
                    const qint64 avail = t.totalSamples;
                    const bool plain = qFuzzyCompare(m_rate, 1.0);
                    if (plain) {
                        stretcherInited = false;
                        stretcherTrackIndex = -1;
                        needReseed = false;
                    } else if (!stretcherInited && !needReseed) {
                        stretcher.reset(m_workerRead);
                        stretcherInited = true;
                        stretcherTrackIndex = m_workerIndex;
                    }
                    bool hasData = false;
                    if (plain) {
                        hasData = (avail > m_workerRead) || t.complete;
                    } else {
                        const qint64 targetUpper = (needReseed ? m_workerRead : stretcher.position()) + 1;
                        hasData = t.complete || (targetUpper + Stretcher::seek + Stretcher::frame <= avail);
                    }
                    if (hasData) {
                        blocksSnap = t.blocks;
                        available = avail;
                        complete = t.complete;
                        rate = m_rate;
                        trackId = t.id;
                        readPos = m_workerRead;
                        gen = m_generation;
                        rateGen = m_rateGeneration;
                        doReseed = needReseed && !plain;
                        if (doReseed) {
                            stretcherTrackIndex = m_workerIndex;
                            stretcherInited = true;
                            needReseed = false;
                        }
                        break;
                    }
                }
                m_cond.wait(lock);
            }
        }

        // Run WSOLA / pass-through completely off the GUI thread without holding m_mutex.
        Q_ASSERT(available >= 0 && readPos >= 0);
        const bool plain = qFuzzyCompare(rate, 1.0);
        QVector<qint16> out;
        qint64 sourceStart = 0;
        qint64 newReadPos = readPos;
        bool more = true;

        if (plain) {
            sourceStart = readPos;
            const qint64 n = std::min<qint64>(chunkRoom, qMax<qint64>(0, available - readPos));
            if (n > 0) {
                out.resize(n);
                copySampleRange(blocksSnap, readPos, n, out.data());
                newReadPos = readPos + n;
            }
            more = !(complete && newReadPos >= available);
        } else {
            const qint64 curPos = doReseed ? readPos : stretcher.position();
            const qint64 prevPos = doReseed ? ((readPos >= Stretcher::hop) ? (readPos - Stretcher::hop) : -1)
                                            : stretcher.previous();
            const qint64 hops = chunkRoom / Stretcher::hop;
            const qint64 maxTarget = qint64(double(curPos) + double(hops) * Stretcher::hop * rate + 2.0);
            qint64 winStart = qMax<qint64>(0, curPos - Stretcher::seek);
            if (prevPos >= 0)
                winStart = qMin(winStart, prevPos);
            if (doReseed)
                winStart = qMin(winStart, qMax<qint64>(0, readPos - Stretcher::hop));
            qint64 winEnd =
                qMin(available, qMax(maxTarget + Stretcher::seek + Stretcher::frame,
                                     prevPos >= 0 ? prevPos + Stretcher::frame : 0));
            if (complete && curPos + Stretcher::frame > available)
                winEnd = available;
            if (winEnd < winStart)
                winEnd = winStart;
            const qint64 winLen = winEnd - winStart;
            windowBuf.resize(qsizetype(winLen));
            if (winLen > 0)
                copySampleRange(blocksSnap, winStart, winLen, windowBuf.data());
            if (doReseed)
                stretcher.reseed(readPos, windowBuf.constData(), winStart, winLen, available);
            sourceStart = stretcher.position();
            more = stretcher.processWindow(windowBuf.constData(), winStart, winLen, available, complete, rate,
                                           chunkRoom, out);
            newReadPos = stretcher.position();
        }

        QByteArray bytes;
        if (!out.isEmpty())
            bytes = QByteArray(reinterpret_cast<const char *>(out.constData()), int(out.size() * 2));
        Q_ASSERT((bytes.size() & 1) == 0);

        AudioOutputPump *pumpToNotify = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_stopWorker)
                return;
            if (gen != m_generation || rateGen != m_rateGeneration)
                continue;
            Q_ASSERT(m_workerIndex >= 0 && m_workerIndex < m_tracks.size());
            if (!bytes.isEmpty() || !more) {
                m_ready.append({std::move(bytes), trackId, sourceStart, plain ? 1.0 : rate, !more});
                m_readySamples += out.size();
                Q_ASSERT(m_readySamples >= 0);
                pumpToNotify = m_pump;
            }
            if (!more) {
                ++m_workerIndex;
                m_workerRead = (m_workerIndex < m_tracks.size()) ? m_tracks[m_workerIndex].startSample : 0;
                stretcherInited = false;
                stretcherTrackIndex = -1;
                needReseed = false;
                if (m_workerIndex >= m_tracks.size())
                    m_workerFinishedAll = true;
            } else {
                m_workerRead = newReadPos;
                if (out.isEmpty())
                    m_cond.wait_for(lock, std::chrono::milliseconds(15));
            }
        }

        if (pumpToNotify && !m_notifyPending.exchange(true, std::memory_order_acq_rel))
            QMetaObject::invokeMethod(pumpToNotify, &AudioOutputPump::onWorkerReady, Qt::QueuedConnection);
    }
}

SpeechPlayer::Track *SpeechPlayer::trackLocked(int id, const std::unique_lock<std::mutex> &lock) {
    Q_ASSERT(lock.mutex() == &m_mutex && lock.owns_lock());
    Q_UNUSED(lock);
    for (Track &t : m_tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

int SpeechPlayer::trackIndexLocked(int id, const std::unique_lock<std::mutex> &lock) const {
    Q_ASSERT(lock.mutex() == &m_mutex && lock.owns_lock());
    Q_UNUSED(lock);
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].id == id)
            return i;
    return -1;
}

void SpeechPlayer::addTrack(int id, qint64 startSample) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (trackLocked(id, lock))
        return;
    Track t;
    t.id = id;
    t.startSample = qMax<qint64>(0, startSample);
    Q_ASSERT(t.startSample >= 0);
    const bool nextForWorker = (m_workerIndex == m_tracks.size());
    m_tracks.append(t);
    if (nextForWorker)
        m_workerRead = t.startSample;
    m_workerFinishedAll = false;
    m_drained = false;
    Q_ASSERT(m_workerIndex >= 0 && m_workerIndex <= m_tracks.size());
    m_cond.notify_one();
}

void SpeechPlayer::appendAudio(int id, const QByteArray &pcm) {
    if (pcm.isEmpty())
        return;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (Track *t = trackLocked(id, lock)) {
        const qsizetype totalBytes = (t->hasTrailingByte ? 1 : 0) + pcm.size();
        const qsizetype evenBytes = totalBytes & ~qsizetype(1);
        const bool nextHasTrailing = (totalBytes & 1) != 0;
        const char nextTrailing = nextHasTrailing ? pcm.back() : 0;
        if (evenBytes > 0) {
            auto block = std::make_shared<std::vector<qint16>>(size_t(evenBytes / 2));
            auto *dst = reinterpret_cast<char *>(block->data());
            qsizetype written = 0;
            if (t->hasTrailingByte) {
                dst[0] = t->trailingByte;
                written = 1;
            }
            std::memcpy(dst + written, pcm.constData(), size_t(evenBytes - written));
            const qint64 blockSamples = qint64(block->size());
            t->blocks.push_back({t->totalSamples, std::move(block)});
            t->totalSamples += blockSamples;
            Q_ASSERT(t->totalSamples >= 0);
            m_cond.notify_one();
        }
        t->hasTrailingByte = nextHasTrailing;
        t->trailingByte = nextTrailing;
    }
}

void SpeechPlayer::finishTrack(int id) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (Track *t = trackLocked(id, lock)) {
        t->complete = true;
        m_cond.notify_one();
    }
}

bool SpeechPlayer::hasTrack(int id) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return trackIndexLocked(id, lock) >= 0;
}

qint64 SpeechPlayer::trackSamples(int id) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    const int idx = trackIndexLocked(id, lock);
    return idx >= 0 ? m_tracks[idx].totalSamples : 0;
}

bool SpeechPlayer::trackComplete(int id) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    const int idx = trackIndexLocked(id, lock);
    return idx >= 0 ? m_tracks[idx].complete : false;
}

void SpeechPlayer::clear() {
    bool playingNow = false;
    quint64 clearGen = 0;
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_tracks.clear();
        m_ready.clear();
        m_readySamples = 0;
        m_workerIndex = 0;
        m_workerRead = 0;
        m_workerFinishedAll = true;
        m_drained = true;
        ++m_generation;
        clearGen = ++m_clearGeneration;
        playingNow = m_playing;
        pump = m_pump;
        Q_ASSERT(m_tracks.isEmpty() && m_ready.isEmpty() && m_readySamples == 0);
        Q_ASSERT(m_workerIndex == 0 && m_workerRead == 0 && m_workerFinishedAll);
        m_cond.notify_one();
    }
    setStarved(false);
    if (pump) {
        QMetaObject::invokeMethod(
            pump, [pump, clearGen, playingNow] { pump->resetPlayback(clearGen, playingNow); },
            Qt::QueuedConnection);
    }
}

bool SpeechPlayer::isPlaying() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_playing;
}

void SpeechPlayer::setHeadless(bool headless) {
    bool needAudioThread = false;
    bool startNow = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool wasHeadless = m_headless;
        m_headless = headless;
        if (wasHeadless && !headless && m_playing) {
            needAudioThread = (m_pump == nullptr);
            startNow = true;
        }
        m_cond.notify_one();
    }
    if (!startNow)
        return;
    if (needAudioThread && QThread::currentThread() == thread())
        ensureAudioThread();
    quint64 clearGen = 0;
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_headless || !m_playing)
            return;
        startWorker(lock);
        clearGen = m_clearGeneration;
        pump = m_pump;
        m_cond.notify_one();
    }
    if (pump) {
        QMetaObject::invokeMethod(
            pump, [pump, clearGen] { pump->startPlayback(clearGen); }, Qt::QueuedConnection);
    }
}

bool SpeechPlayer::isHeadless() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_headless;
}

void SpeechPlayer::play() {
    bool needAudioThread = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_headless) {
            m_playing = true;
            return;
        }
        needAudioThread = (m_pump == nullptr);
    }
    if (needAudioThread && QThread::currentThread() == thread())
        ensureAudioThread();
    quint64 clearGen = 0;
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_headless) {
            m_playing = true;
            return;
        }
        m_playing = true;
        startWorker(lock);
        clearGen = m_clearGeneration;
        pump = m_pump;
        m_cond.notify_one();
    }
    if (pump) {
        QMetaObject::invokeMethod(
            pump, [pump, clearGen] { pump->startPlayback(clearGen); }, Qt::QueuedConnection);
    }
}

void SpeechPlayer::pause() {
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_playing = false;
        pump = m_pump;
        m_cond.notify_one();
    }
    if (pump)
        QMetaObject::invokeMethod(pump, &AudioOutputPump::pausePlayback, Qt::QueuedConnection);
    setStarved(false);
}

void SpeechPlayer::setMuted(bool muted) {
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_muted = muted;
        pump = m_pump;
    }
    if (pump)
        QMetaObject::invokeMethod(pump, [pump, muted] { pump->setMuted(muted); }, Qt::QueuedConnection);
}

double SpeechPlayer::volume() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_volume;
}

void SpeechPlayer::setVolume(double volume) {
    const double clamped = std::clamp(volume, 0.0, 1.0);
    AudioOutputPump *pump = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (qFuzzyCompare(clamped, m_volume))
            return;
        m_volume = clamped;
        pump = m_pump;
    }
    if (pump)
        QMetaObject::invokeMethod(pump, [pump, clamped] { pump->setVolume(clamped); }, Qt::QueuedConnection);
    if (QThread::currentThread() == thread())
        emit volumeChanged();
    else
        QMetaObject::invokeMethod(this, &SpeechPlayer::volumeChanged, Qt::QueuedConnection);
}

bool SpeechPlayer::starved() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_starved;
}

bool SpeechPlayer::drainedOut() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_drained;
}

void SpeechPlayer::setRate(double rate) {
    const double clamped = std::clamp(rate, 0.25, 4.0);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (qFuzzyCompare(clamped, m_rate))
        return;
    // Discard buffered output produced at the old rate and rewind the worker
    // cursor to the exact first unplayed source sample so nothing is skipped.
    if (!m_ready.isEmpty()) {
        const ReadyChunk &first = m_ready.first();
        Q_ASSERT(first.sourceStart >= 0);
        const int idx = trackIndexLocked(first.trackId, lock);
        if (idx >= 0) {
            m_workerIndex = idx;
            m_workerRead = first.sourceStart;
        }
        m_ready.clear();
        m_readySamples = 0;
    }
    m_rate = clamped;
    ++m_rateGeneration;
    if (m_workerIndex < m_tracks.size())
        m_workerFinishedAll = false;
    Q_ASSERT(m_rate >= 0.25 && m_rate <= 4.0);
    Q_ASSERT(m_workerIndex >= 0 && m_workerIndex <= m_tracks.size());
    Q_ASSERT(m_workerRead >= 0 && m_readySamples == 0);
    m_cond.notify_one();
}

void SpeechPlayer::setStarved(bool starved) {
    bool changed = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (starved != m_starved) {
            m_starved = starved;
            changed = true;
        }
    }
    if (!changed)
        return;
    if (QThread::currentThread() == thread()) {
        emit starvedChanged(starved);
    } else {
        QMetaObject::invokeMethod(
            this,
            [this] {
                bool current = false;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    current = m_starved;
                }
                emit starvedChanged(current);
            },
            Qt::QueuedConnection);
    }
}

#include "speechplayer.moc"
