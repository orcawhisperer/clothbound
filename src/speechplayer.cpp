#include "speechplayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <algorithm>
#include <cmath>
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
        if (m_previous >= 0) {
            // The new frame should start where it best continues the last
            // one: compare its first half with what naturally followed.
            const qint64 natural = m_previous + hop;
            if (natural + hop <= available) {
                float bestScore = -1e30f;
                for (qint64 candidate = lo; candidate <= hi; ++candidate) {
                    float dot = 0.f, energy = 0.f;
                    for (int n = 0; n < hop; n += 2) {
                        const float a = float(source[natural + n]), b = float(source[candidate + n]);
                        dot += a * b;
                        energy += b * b;
                    }
                    const float score = energy > 0.f ? dot / std::sqrt(energy) : 0.f;
                    if (score > bestScore) {
                        bestScore = score;
                        best = candidate;
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
    m_muted = qEnvironmentVariableIsSet("DOGEAR_MUTE");
    m_timer.setInterval(15);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        fill();
        report();
    });
    connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, &SpeechPlayer::handleOutputsChanged);
}

SpeechPlayer::~SpeechPlayer() { releaseSink(); }

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
            if (m_playing && !m_recovered) {
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
        if (state == QAudio::IdleState && m_playing) {
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
    const bool wasPlaying = m_playing;
    releaseSink();
    m_marks.clear();
    m_writtenBytes = 0;
    if (wasPlaying) {
        ensureSink();
        if (m_sink && m_out)
            fill();
    }
}

SpeechPlayer::Track *SpeechPlayer::track(int id) {
    for (Track &t : m_tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

void SpeechPlayer::addTrack(int id) {
    if (track(id))
        return;
    Track t;
    t.id = id;
    m_tracks.append(t);
    m_drained = false;
}

void SpeechPlayer::appendAudio(int id, const QByteArray &pcm) {
    if (Track *t = track(id))
        t->pcm += pcm;
}

void SpeechPlayer::finishTrack(int id) {
    if (Track *t = track(id))
        t->complete = true;
}

bool SpeechPlayer::hasTrack(int id) const {
    for (const Track &t : m_tracks)
        if (t.id == id)
            return true;
    return false;
}

qint64 SpeechPlayer::trackSamples(int id) const {
    for (const Track &t : m_tracks)
        if (t.id == id)
            return t.pcm.size() / 2;
    return 0;
}

bool SpeechPlayer::trackComplete(int id) const {
    for (const Track &t : m_tracks)
        if (t.id == id)
            return t.complete;
    return false;
}

void SpeechPlayer::clear() {
    m_tracks.clear();
    m_index = 0;
    m_marks.clear();
    m_writtenBytes = 0;
    m_pendingOut.clear();
    m_stretcher.reset();
    m_lastTrack = -1;
    m_lastSample = -1;
    m_drained = true;
    if (m_sink) {
        if (!m_playing) {
            releaseSink();
        } else {
            m_sink->reset();
            m_out = m_sink->start();
        }
    }
    setStarved(false);
}

void SpeechPlayer::play() {
    if (m_headless) {
        m_playing = true;
        return;
    }
    ensureSink();
    if (!m_sink || !m_out) {
        emit sinkError(tr("No audio output device is available."));
        return;
    }
    m_playing = true;
    if (m_sink->state() == QAudio::SuspendedState)
        m_sink->resume();
    m_timer.start();
    fill();
}

void SpeechPlayer::pause() {
    m_playing = false;
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
    if (qFuzzyCompare(clamped, m_rate))
        return;
    // The source cursor carries over; the stretcher starts fresh there.
    if (m_index < m_tracks.size()) {
        Track &t = m_tracks[m_index];
        if (!qFuzzyCompare(m_rate, 1.0))
            t.read = m_stretcher.position();
        m_stretcher.reset(t.read);
    }
    m_rate = clamped;
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
    if (!m_playing || !m_out || !m_sink)
        return;
    if (!flushPending())
        return;
    qint64 room = m_sink->bytesFree() / 2;
    QVector<qint16> out;
    while (room >= Stretcher::hop && m_index < m_tracks.size()) {
        Track &t = m_tracks[m_index];
        const auto *source = reinterpret_cast<const qint16 *>(t.pcm.constData());
        const qint64 available = t.pcm.size() / 2;
        out.clear();
        qint64 sourceStart = 0;
        bool more = true;
        const bool plain = qFuzzyCompare(m_rate, 1.0);
        if (plain) {
            sourceStart = t.read;
            const qint64 n = std::min<qint64>(room, available - t.read);
            if (n > 0) {
                out.resize(n);
                std::memcpy(out.data(), source + t.read, size_t(n) * 2);
                t.read += n;
            }
            more = !(t.complete && t.read >= available);
        } else {
            sourceStart = m_stretcher.position();
            more = m_stretcher.process(source, available, t.complete, m_rate, int(room), out);
            t.read = m_stretcher.position();
        }
        bool partialWrite = false;
        if (!out.isEmpty()) {
            m_marks.append({m_writtenBytes / 2, t.id, sourceStart, plain ? 1.0 : m_rate});
            const char *data = reinterpret_cast<const char *>(out.constData());
            const qint64 bytes = out.size() * 2;
            const qint64 wrote = m_out->write(data, bytes);
            if (wrote > 0)
                m_writtenBytes += wrote;
            if (wrote < bytes) {
                const qint64 offset = qMax<qint64>(0, wrote);
                m_pendingOut.append(data + offset, bytes - offset);
                partialWrite = true;
            }
            room -= qMax<qint64>(0, wrote) / 2;
        }
        if (!more) {
            // The track is heard out; its audio can go.
            t.pcm = QByteArray();
            ++m_index;
            m_stretcher.reset();
            if (partialWrite)
                break;
            continue;
        }
        if (partialWrite || out.isEmpty())
            break;
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
    // Nothing left to hear: either the queue is done, or the track being
    // played is still arriving.
    const bool empty = (buffered <= 0 || m_sink->state() == QAudio::IdleState) && m_pendingOut.isEmpty();
    if (empty && m_index >= m_tracks.size()) {
        setStarved(false);
        if (!m_drained) {
            m_drained = true;
            emit drained();
        }
    } else {
        setStarved(m_playing && empty);
    }
}
