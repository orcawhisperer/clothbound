#include "audio/speechplayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMetaObject>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
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
}

void Stretcher::reset(qint64 at) {
    m_position = double(at);
    m_previous = -1;
    m_tail = QVector<float>(hop, 0.f);
    m_finalTail.clear();
    m_finalPos = 0;
    m_flushing = false;
    m_done = false;
}

bool Stretcher::process(const qint16 *source, qint64 available, bool complete, double rate, int room,
                        QVector<qint16> &out) {
    if (m_done)
        return false;
    if (m_tail.size() != hop)
        m_tail = QVector<float>(hop, 0.f);

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
                if (rem > 0)
                    std::memcpy(m_finalTail.data(), source + start, size_t(rem) * sizeof(qint16));
            } else {
                const qint64 rem = std::max<qint64>(0, available - target);
                m_finalTail.reserve(int(std::max<qint64>(hop, rem)));
                for (int n = 0; n < hop; ++n) {
                    const float incoming = n < rem ? w[n] * float(source[target + n]) : 0.f;
                    m_finalTail.append(clip16(m_tail[n] + incoming));
                }
                if (rem > hop) {
                    const qint64 extra = rem - hop;
                    for (qint64 k = 0; k < extra; ++k) {
                        const float fade = 1.0f - float(k) / float(extra);
                        m_finalTail.append(clip16(fade * float(source[target + hop + k])));
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
                    const std::int64_t v = source[lo + i];
                    sqPrefix[i + 2] = sqPrefix[i] + v * v;
                }
                float ref[hop / 2];
                for (int k = 0; k < hop / 2; ++k)
                    ref[k] = float(source[natural + 2 * k]);

                auto scoreAt = [&](qint64 candidate) -> float {
                    const qint64 c = candidate - lo;
                    const std::int64_t energy = sqPrefix[c + hop] - sqPrefix[c];
                    if (energy <= 0)
                        return 0.f;
                    float dot = 0.f;
                    const qint16 *candPtr = source + candidate;
                    for (int k = 0; k < hop / 2; ++k)
                        dot += ref[k] * float(candPtr[2 * k]);
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
            out.append(clip16(m_tail[n] + w[n] * float(source[best + n])));
        for (int n = 0; n < hop; ++n)
            m_tail[n] = w[hop + n] * float(source[best + hop + n]);
        m_previous = best;
        m_position += hop * rate;
        room -= hop;
    }
    return true;
}

SpeechPlayer::SpeechPlayer(int sampleRate, QObject *parent) : QObject(parent), m_sampleRate(sampleRate) {
    m_muted = qEnvironmentVariableIsSet("CLOTHBOUND_MUTE") || qEnvironmentVariableIsSet("DOGEAR_MUTE");
    m_timer.setInterval(15);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        fill();
        report();
    });
    connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, &SpeechPlayer::handleOutputsChanged);
}

SpeechPlayer::~SpeechPlayer() {
    m_timer.stop();
    stopWorker();
    releaseSink();
}

void SpeechPlayer::startWorker() {
    if (m_worker.joinable())
        return;
    m_stopWorker = false;
    m_worker = std::thread(&SpeechPlayer::workerLoop, this);
}

void SpeechPlayer::stopWorker() {
    {
        QMutexLocker lock(&m_mutex);
        m_stopWorker = true;
        ++m_generation;
        m_cond.wakeAll();
    }
    if (m_worker.joinable())
        m_worker.join();
}

void SpeechPlayer::workerLoop() {
    Stretcher stretcher;
    quint64 localGen = 0;
    bool stretcherInited = false;
    const qint64 targetReadySamples = qMax<qint64>(Stretcher::frame * 4, m_sampleRate / 2);
    const int chunkRoom = Stretcher::hop * 8;

    for (;;) {
        QByteArray pcmCopy;
        qint64 available = 0;
        bool complete = false;
        double rate = 1.0;
        int trackId = 0;
        qint64 readPos = 0;
        quint64 gen = 0;

        {
            QMutexLocker lock(&m_mutex);
            for (;;) {
                if (m_stopWorker)
                    return;
                if (localGen != m_generation) {
                    localGen = m_generation;
                    stretcherInited = false;
                }
                const bool canWork = m_playing && !m_headless && m_readySamples < targetReadySamples &&
                                     m_workerIndex < m_tracks.size();
                if (canWork) {
                    const Track &t = m_tracks[m_workerIndex];
                    const qint64 avail = t.pcm.size() / 2;
                    const bool plain = qFuzzyCompare(m_rate, 1.0);
                    if (!stretcherInited) {
                        stretcher.reset(m_workerRead);
                        stretcherInited = true;
                    }
                    bool hasData = false;
                    if (plain) {
                        hasData = (avail > m_workerRead) || t.complete;
                    } else {
                        const qint64 targetUpper = stretcher.position() + 1;
                        hasData = t.complete || (targetUpper + Stretcher::seek + Stretcher::frame <= avail);
                    }
                    if (hasData) {
                        pcmCopy = t.pcm;
                        available = avail;
                        complete = t.complete;
                        rate = m_rate;
                        trackId = t.id;
                        readPos = m_workerRead;
                        gen = m_generation;
                        break;
                    }
                }
                m_cond.wait(&m_mutex);
            }
        }

        // Run WSOLA / pass-through completely off the GUI thread without holding m_mutex.
        const auto *source = reinterpret_cast<const qint16 *>(pcmCopy.constData());
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
                std::memcpy(out.data(), source + readPos, size_t(n) * sizeof(qint16));
                newReadPos = readPos + n;
            }
            more = !(complete && newReadPos >= available);
        } else {
            sourceStart = stretcher.position();
            more = stretcher.process(source, available, complete, rate, chunkRoom, out);
            newReadPos = stretcher.position();
        }

        QByteArray bytes;
        if (!out.isEmpty())
            bytes = QByteArray(reinterpret_cast<const char *>(out.constData()), int(out.size() * 2));

        bool shouldNotify = false;
        {
            QMutexLocker lock(&m_mutex);
            if (m_stopWorker)
                return;
            if (gen != m_generation)
                continue;
            if (!bytes.isEmpty() || !more) {
                m_ready.append({std::move(bytes), trackId, sourceStart, plain ? 1.0 : rate, !more});
                m_readySamples += out.size();
                shouldNotify = true;
            }
            if (!more) {
                ++m_workerIndex;
                m_workerRead = (m_workerIndex < m_tracks.size()) ? m_tracks[m_workerIndex].startSample : 0;
                stretcherInited = false;
                if (m_workerIndex >= m_tracks.size())
                    m_workerFinishedAll = true;
            } else {
                m_workerRead = newReadPos;
                if (out.isEmpty())
                    m_cond.wait(&m_mutex, 15);
            }
        }

        if (shouldNotify && !m_notifyPending.exchange(true))
            QMetaObject::invokeMethod(this, &SpeechPlayer::onWorkerReady, Qt::QueuedConnection);
    }
}

void SpeechPlayer::onWorkerReady() {
    m_notifyPending.store(false);
    if (isPlaying())
        fill();
}

void SpeechPlayer::ensureSink() {
    if (m_sink)
        return;
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
        return;
    QAudioFormat format;
    format.setSampleRate(m_sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    m_outputDevice = device;
    m_sink = new QAudioSink(device, format, this);
    // A quarter second queued: enough to ride out a busy moment, little
    // enough that pausing and the highlight stay close to the voice.
    m_sink->setBufferSize(m_sampleRate * 2 / 4);
    m_sink->setVolume(m_muted ? 0.0 : m_volume);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (!m_sink)
            return;
        if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
            if (isPlaying() && !m_recovered) {
                m_recovered = true;
                releaseSink();
                m_marks.clear();
                m_writtenBytes = 0;
                m_pendingOut.clear();
                ensureSink();
                if (m_sink && m_out) {
                    fill();
                    return;
                }
            }
            pause();
            emit sinkError(tr("Audio output stopped working."));
            return;
        }
        if (state == QAudio::ActiveState)
            m_recovered = false;
        if (state == QAudio::IdleState && isPlaying()) {
            fill();
            report();
        }
    });
    m_out = m_sink->start();
}

void SpeechPlayer::releaseSink() {
    m_pendingOut.clear();
    if (!m_sink)
        return;
    m_sink->disconnect(this);
    m_sink->stop();
    delete m_sink;
    m_sink = nullptr;
    m_out = nullptr;
}

void SpeechPlayer::handleOutputsChanged() {
    if (!m_sink)
        return;
    const QAudioDevice current = QMediaDevices::defaultAudioOutput();
    if (current == m_outputDevice && m_sink->error() == QAudio::NoError)
        return;
    const bool wasPlaying = isPlaying();
    releaseSink();
    m_marks.clear();
    m_writtenBytes = 0;
    if (wasPlaying) {
        ensureSink();
        if (m_sink && m_out)
            fill();
    }
}

SpeechPlayer::Track *SpeechPlayer::trackLocked(int id) {
    for (Track &t : m_tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

int SpeechPlayer::trackIndexLocked(int id) const {
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].id == id)
            return i;
    return -1;
}

void SpeechPlayer::addTrack(int id, qint64 startSample) {
    QMutexLocker lock(&m_mutex);
    if (trackLocked(id))
        return;
    Track t;
    t.id = id;
    t.startSample = qMax<qint64>(0, startSample);
    const bool nextForWorker = (m_workerIndex == m_tracks.size());
    m_tracks.append(t);
    if (nextForWorker)
        m_workerRead = t.startSample;
    m_workerFinishedAll = false;
    m_drained = false;
    m_cond.wakeOne();
}

void SpeechPlayer::appendAudio(int id, const QByteArray &pcm) {
    QMutexLocker lock(&m_mutex);
    if (Track *t = trackLocked(id)) {
        t->pcm += pcm;
        m_cond.wakeOne();
    }
}

void SpeechPlayer::finishTrack(int id) {
    QMutexLocker lock(&m_mutex);
    if (Track *t = trackLocked(id)) {
        t->complete = true;
        m_cond.wakeOne();
    }
}

bool SpeechPlayer::hasTrack(int id) const {
    QMutexLocker lock(&m_mutex);
    return trackIndexLocked(id) >= 0;
}

qint64 SpeechPlayer::trackSamples(int id) const {
    QMutexLocker lock(&m_mutex);
    const int idx = trackIndexLocked(id);
    return idx >= 0 ? m_tracks[idx].pcm.size() / 2 : 0;
}

bool SpeechPlayer::trackComplete(int id) const {
    QMutexLocker lock(&m_mutex);
    const int idx = trackIndexLocked(id);
    return idx >= 0 ? m_tracks[idx].complete : false;
}

void SpeechPlayer::clear() {
    bool playingNow = false;
    {
        QMutexLocker lock(&m_mutex);
        m_tracks.clear();
        m_ready.clear();
        m_readySamples = 0;
        m_workerIndex = 0;
        m_workerRead = 0;
        m_workerFinishedAll = true;
        ++m_generation;
        playingNow = m_playing;
        m_cond.wakeOne();
    }
    m_consumedIndex = 0;
    m_marks.clear();
    m_writtenBytes = 0;
    m_pendingOut.clear();
    m_lastTrack = -1;
    m_lastSample = -1;
    m_drained = true;
    if (m_sink) {
        if (!playingNow) {
            releaseSink();
        } else {
            m_sink->reset();
            m_out = m_sink->start();
        }
    }
    setStarved(false);
}

bool SpeechPlayer::isPlaying() const {
    QMutexLocker lock(&m_mutex);
    return m_playing;
}

void SpeechPlayer::setHeadless(bool headless) {
    QMutexLocker lock(&m_mutex);
    m_headless = headless;
    m_cond.wakeOne();
}

void SpeechPlayer::play() {
    {
        QMutexLocker lock(&m_mutex);
        if (m_headless) {
            m_playing = true;
            return;
        }
    }
    ensureSink();
    if (!m_sink || !m_out) {
        emit sinkError(tr("No audio output device is available."));
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_playing = true;
        startWorker();
        m_cond.wakeOne();
    }
    if (m_sink->state() == QAudio::SuspendedState)
        m_sink->resume();
    m_timer.start();
    fill();
}

void SpeechPlayer::pause() {
    {
        QMutexLocker lock(&m_mutex);
        m_playing = false;
        m_cond.wakeOne();
    }
    if (m_sink)
        m_sink->suspend();
    m_timer.stop();
    setStarved(false);
}

void SpeechPlayer::setMuted(bool muted) {
    m_muted = muted;
    if (m_sink)
        m_sink->setVolume(m_muted ? 0.0 : m_volume);
}

void SpeechPlayer::setVolume(double volume) {
    const double clamped = std::clamp(volume, 0.0, 1.0);
    if (qFuzzyCompare(clamped, m_volume))
        return;
    m_volume = clamped;
    if (m_sink && !m_muted)
        m_sink->setVolume(m_volume);
    emit volumeChanged();
}

void SpeechPlayer::setRate(double rate) {
    const double clamped = std::clamp(rate, 0.25, 4.0);
    QMutexLocker lock(&m_mutex);
    if (qFuzzyCompare(clamped, m_rate))
        return;
    // Discard buffered output produced at the old rate and rewind the worker
    // cursor to the exact first unplayed source sample so nothing is skipped.
    if (!m_ready.isEmpty()) {
        const ReadyChunk &first = m_ready.first();
        const int idx = trackIndexLocked(first.trackId);
        if (idx >= 0) {
            m_workerIndex = idx;
            m_workerRead = first.sourceStart;
        }
        m_ready.clear();
        m_readySamples = 0;
    }
    m_rate = clamped;
    ++m_generation;
    if (m_workerIndex < m_tracks.size())
        m_workerFinishedAll = false;
    m_cond.wakeOne();
}

bool SpeechPlayer::flushPending() {
    while (!m_pendingOut.isEmpty() && m_out) {
        const qint64 n = m_out->write(m_pendingOut.constData(), m_pendingOut.size());
        if (n <= 0)
            return false;
        m_pendingOut.remove(0, n);
        m_writtenBytes += n;
    }
    return m_pendingOut.isEmpty();
}

void SpeechPlayer::fill() {
    if (!m_out || !m_sink)
        return;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_playing || m_headless)
            return;
    }
    if (!flushPending())
        return;
    qint64 room = m_sink->bytesFree() / 2;
    bool poppedAny = false;
    for (;;) {
        ReadyChunk chunk;
        {
            QMutexLocker lock(&m_mutex);
            if (!m_playing || m_headless || m_ready.isEmpty())
                break;
            if (room <= 0 && !m_ready.first().bytes.isEmpty())
                break;
            chunk = m_ready.takeFirst();
            m_readySamples = qMax<qint64>(0, m_readySamples - chunk.bytes.size() / 2);
        }
        poppedAny = true;
        bool partialWrite = false;
        if (!chunk.bytes.isEmpty()) {
            m_marks.append({m_writtenBytes / 2, chunk.trackId, chunk.sourceStart, chunk.rate});
            const qint64 bytes = chunk.bytes.size();
            const qint64 wrote = m_out->write(chunk.bytes.constData(), bytes);
            if (wrote > 0)
                m_writtenBytes += wrote;
            if (wrote < bytes) {
                const qint64 offset = qMax<qint64>(0, wrote);
                m_pendingOut.append(chunk.bytes.constData() + offset, bytes - offset);
                partialWrite = true;
            }
            room -= qMax<qint64>(0, wrote) / 2;
        }
        if (chunk.trackDone) {
            QMutexLocker lock(&m_mutex);
            if (Track *t = trackLocked(chunk.trackId))
                t->pcm = QByteArray();
            ++m_consumedIndex;
        }
        if (partialWrite)
            break;
    }
    if (poppedAny) {
        QMutexLocker lock(&m_mutex);
        m_cond.wakeOne();
    }
    // Marks already behind what's audible are kept only as far as needed.
    while (m_marks.size() > 64)
        m_marks.removeFirst();
}

void SpeechPlayer::setStarved(bool starved) {
    if (starved == m_starved)
        return;
    m_starved = starved;
    emit starvedChanged(starved);
}

void SpeechPlayer::report() {
    if (!m_sink)
        return;
    const qint64 buffered = qMax<qint64>(0, (m_sink->bufferSize() - m_sink->bytesFree()) / 2);
    const qint64 audible = qMax<qint64>(0, m_writtenBytes / 2 - buffered);
    for (int i = int(m_marks.size()) - 1; i >= 0; --i)
        if (m_marks[i].output <= audible) {
            const Mark &mark = m_marks[i];
            const qint64 sample = mark.source + qint64((audible - mark.output) * mark.rate);
            if (mark.track != m_lastTrack || sample != m_lastSample) {
                m_lastTrack = mark.track;
                m_lastSample = sample;
                emit position(mark.track, sample);
            }
            break;
        }
    bool allConsumed = false;
    bool playingNow = false;
    {
        QMutexLocker lock(&m_mutex);
        allConsumed = m_ready.isEmpty() && m_workerFinishedAll && m_consumedIndex >= m_tracks.size();
        playingNow = m_playing;
    }
    // Nothing left to hear: either the queue is done, or the track being
    // played is still arriving.
    const bool empty = (buffered <= 0 || m_sink->state() == QAudio::IdleState) && m_pendingOut.isEmpty();
    if (empty && allConsumed) {
        setStarved(false);
        if (!m_drained) {
            m_drained = true;
            emit drained();
        }
    } else {
        setStarved(playingNow && empty);
    }
}
