#include "speechplayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
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
    m_done = false;
}

bool Stretcher::process(const qint16 *source, qint64 available, bool complete, double rate, int room,
                        QVector<qint16> &out) {
    if (m_done)
        return false;
    if (m_tail.size() != hop)
        m_tail = QVector<float>(hop, 0.f);
    const QVector<float> &w = window();
    while (room >= hop) {
        const qint64 target = qint64(m_position + 0.5);
        if (target + seek + frame > available) {
            if (!complete)
                return true;
            // The source is spent: let the last half-frame fade out.
            for (int n = 0; n < hop; ++n)
                out.append(clip16(m_tail[n]));
            m_done = true;
            return false;
        }
        qint64 best = target;
        if (m_previous >= 0) {
            // The new frame should start where it best continues the last
            // one: compare its first half with what naturally followed.
            const qint64 natural = m_previous + hop;
            double bestScore = -1e300;
            for (qint64 candidate = std::max<qint64>(0, target - seek); candidate <= target + seek; ++candidate) {
                double dot = 0, energy = 0;
                for (int n = 0; n < hop; n += 2) {
                    const double a = source[natural + n], b = source[candidate + n];
                    dot += a * b;
                    energy += b * b;
                }
                const double score = energy > 0 ? dot / std::sqrt(energy) : 0;
                if (score > bestScore) {
                    bestScore = score;
                    best = candidate;
                }
            }
        }
        for (int n = 0; n < hop; ++n)
            out.append(clip16(m_tail[n] + w[n] * source[best + n]));
        for (int n = 0; n < hop; ++n)
            m_tail[n] = w[hop + n] * source[best + hop + n];
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
}

SpeechPlayer::~SpeechPlayer() {
    if (m_sink)
        m_sink->stop();
}

void SpeechPlayer::ensureSink() {
    if (m_sink)
        return;
    QAudioFormat format;
    format.setSampleRate(m_sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    m_sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
    // A quarter second queued: enough to ride out a busy moment, little
    // enough that pausing and the highlight stay close to the voice.
    m_sink->setBufferSize(m_sampleRate * 2 / 4);
    m_sink->setVolume(m_muted ? 0.0 : 1.0);
    m_out = m_sink->start();
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
    m_written = 0;
    m_stretcher.reset();
    m_lastTrack = -1;
    m_lastSample = -1;
    m_drained = true;
    if (m_sink) {
        m_sink->reset();
        m_out = m_sink->start();
        if (!m_playing)
            m_sink->suspend();
    }
    setStarved(false);
}

void SpeechPlayer::play() {
    ensureSink();
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
        m_sink->setVolume(muted ? 0.0 : 1.0);
}

void SpeechPlayer::setRate(double rate) {
    if (qFuzzyCompare(rate, m_rate))
        return;
    // The source cursor carries over; the stretcher starts fresh there.
    if (m_index < m_tracks.size()) {
        Track &t = m_tracks[m_index];
        if (!qFuzzyCompare(m_rate, 1.0))
            t.read = m_stretcher.position();
        m_stretcher.reset(t.read);
    }
    m_rate = rate;
}

void SpeechPlayer::fill() {
    if (!m_playing || !m_out)
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
        if (!out.isEmpty()) {
            m_marks.append({m_written, t.id, sourceStart, plain ? 1.0 : m_rate});
            m_out->write(reinterpret_cast<const char *>(out.constData()), out.size() * 2);
            m_written += out.size();
            room -= out.size();
        }
        if (!more) {
            // The track is heard out; its audio can go.
            t.pcm = QByteArray();
            ++m_index;
            m_stretcher.reset();
            continue;
        }
        if (out.isEmpty())
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
    const qint64 buffered = (m_sink->bufferSize() - m_sink->bytesFree()) / 2;
    const qint64 audible = m_written - buffered;
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
    const bool empty = buffered <= 0;
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
