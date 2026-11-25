#pragma once

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVector>
#include <QWaitCondition>
#include <atomic>
#include <thread>

class AudioOutputPump;

// Speech at another pace without another pitch: WSOLA (waveform-similarity
// overlap-add). Each output frame is cut from the source near where the pace
// puts it, shifted to the spot that best continues the previous frame, and
// cross-faded in; at 1× the samples pass through untouched.
class Stretcher {
public:
    static constexpr int frame = 1024;     // about 43 ms at 24 kHz
    static constexpr int hop = frame / 2;  // output advances half a frame
    static constexpr int seek = 256;       // how far either way to look for a match

    void reset(qint64 at = 0);
    // Preserves a unity-gain overlap tail at `at` across a mid-track rate change
    // so changing speed does not introduce a zero-amplitude click.
    void reseed(qint64 at, const qint16 *window, qint64 windowStart, qint64 windowLen, qint64 available);
    // Writes up to `room` samples of output from `source` (`available`
    // samples so far, `complete` when no more will come). Returns false when
    // the source is used up and the output flushed.
    bool process(const qint16 *source, qint64 available, bool complete, double rate, int room, QVector<qint16> &out);
    // Windowed variant operating on `window[0..windowLen)` covering logical track
    // samples `[windowStart, windowStart + windowLen)`.
    bool processWindow(const qint16 *window, qint64 windowStart, qint64 windowLen, qint64 available, bool complete,
                       double rate, int room, QVector<qint16> &out);
    // Where in the source the next output sample comes from.
    qint64 position() const { return qint64(m_position); }
    qint64 previous() const { return m_previous; }

private:
    double m_position = 0;
    qint64 m_previous = -1;
    QVector<float> m_tail;
    QVector<qint16> m_finalTail;
    int m_finalPos = 0;
    bool m_flushing = false;
    bool m_done = false;
};

// Narration as one continuous stream: clips ("tracks") are queued in order,
// filled as their audio arrives from the network or the cache, and played
// back to back with no gap, at the chosen pace. Both WSOLA stretching and
// QAudioSink output feeding run off the GUI thread so visual transitions or
// heavy document loads can never break playback.
class SpeechPlayer : public QObject {
    Q_OBJECT

public:
    explicit SpeechPlayer(int sampleRate = 24000, QObject *parent = nullptr);
    ~SpeechPlayer() override;

    void addTrack(int id, qint64 startSample = 0);
    void appendAudio(int id, const QByteArray &pcm);
    void finishTrack(int id);
    bool hasTrack(int id) const;
    qint64 trackSamples(int id) const;
    bool trackComplete(int id) const;
    void clear();

    void play();
    void pause();
    bool isPlaying() const;
    void setRate(double rate);
    void setMuted(bool muted);
    void setHeadless(bool headless);
    bool isHeadless() const;
    double volume() const { return m_volume; }
    void setVolume(double volume);
    bool starved() const { return m_starved; }
    // Everything queued has been heard (or nothing was).
    bool drainedOut() const { return m_drained; }
    int sampleRate() const { return m_sampleRate; }

signals:
    // The audible track and the sample of it being heard.
    void position(int track, qint64 sample);
    void starvedChanged(bool starved);
    // Everything queued has been heard.
    void drained();
    void volumeChanged();
    void sinkError(const QString &message);

private:
    friend class ClothboundTests;
    friend class AudioOutputPump;
    struct Track {
        int id = 0;
        // Immutable even-aligned PCM chunks so appendAudio() never detaches/deep-copies
        // audio buffers while the WSOLA worker reads them outside m_mutex.
        QVector<QByteArray> chunks;
        QVector<qint64> chunkOffsets;
        qint64 totalSamples = 0;
        char trailingByte = 0;
        bool hasTrailingByte = false;
        bool complete = false;
        qint64 startSample = 0;
    };
    struct ReadyChunk {
        QByteArray bytes;
        int trackId = 0;
        qint64 sourceStart = 0;
        double rate = 1.0;
        bool trackDone = false;
    };
    struct Mark {
        qint64 output;  // output sample where this piece starts
        int track;
        qint64 source;  // source sample it starts from
        double rate;
    };

    void startWorker();
    void stopWorker();
    void workerLoop();
    void ensureAudioThread();
    void stopAudioThread();
    Track *trackLocked(int id);
    int trackIndexLocked(int id) const;
    void setStarved(bool starved);

    int m_sampleRate;

    // Dedicated high-priority thread owning QAudioSink, QMediaDevices, and the
    // 15 ms feed/report timer (started lazily on first non-headless play()).
    QThread m_audioThread;
    AudioOutputPump *m_pump = nullptr;

    // Shared between the GUI thread, WSOLA worker thread, and audio output thread.
    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    std::thread m_worker;
    QList<Track> m_tracks;
    QList<ReadyChunk> m_ready;
    qint64 m_readySamples = 0;
    int m_workerIndex = 0;
    qint64 m_workerRead = 0;
    bool m_workerFinishedAll = true;
    quint64 m_generation = 0;
    quint64 m_rateGeneration = 0;
    quint64 m_clearGeneration = 0;
    double m_rate = 1.0;
    bool m_playing = false;
    bool m_headless = false;
    bool m_stopWorker = false;
    std::atomic_bool m_notifyPending{false};

    // GUI thread state mirrors.
    double m_volume = 1.0;
    bool m_muted = false;
    bool m_starved = false;
    bool m_drained = true;
};
