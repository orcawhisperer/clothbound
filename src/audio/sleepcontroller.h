#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

class SpeechPlayer;

// Sleep timer for narration: monotonic QElapsedTimer-backed countdown,
// end-of-chapter stop mode, smooth 10-second quadratic volume fade-out,
// and clean pre-fade volume capture/restoration on pause, resume, cancel,
// or explicit external volume changes.
class SleepController : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 fadeDurationMs = 10000;

    struct Deadline {
        QElapsedTimer clock;
        qint64 durationMs = 0;

        bool isValid() const { return clock.isValid(); }
        void clear() {
            clock.invalidate();
            durationMs = 0;
        }
        void startMs(qint64 ms) {
            durationMs = ms;
            clock.start();
        }
        qint64 remainingMs() const { return clock.isValid() ? (durationMs - clock.elapsed()) : 0; }
        Deadline &operator=(const QDateTime &dt) {
            if (!dt.isValid()) {
                clear();
            } else {
                durationMs = QDateTime::currentDateTime().msecsTo(dt);
                clock.start();
            }
            return *this;
        }
    };

    struct Callbacks {
        std::function<bool()> isActive;
        std::function<bool()> isPlaying;
        std::function<void()> onBeforeFinish;
        std::function<void()> onPauseForSleep;
    };

    explicit SleepController(SpeechPlayer *player, QObject *parent = nullptr);
    ~SleepController() override;

    void setCallbacks(Callbacks callbacks);

    int sleepMinutes() const;
    int sleepSecondsLeft() const;
    bool isFading() const { return m_fading; }
    bool atChapterEnd() const { return m_atChapterEnd; }
    bool isPausedRestore() const { return m_pausedRestore; }
    bool isActive() const { return m_deadline.isValid() || m_atChapterEnd || m_fading; }
    double preFadeVolume() const { return m_preFadeVolume; }

    // Arms (minutes > 0) or clears (minutes <= 0) the countdown timer.
    // Returns true if end-of-chapter sleep mode was active prior to this call.
    bool sleepIn(int minutes);
    void sleepAtEndOfChapter();
    void consumeChapterEnd();

    // Sets a remaining monotonic deadline directly (in ms) for deterministic testing.
    void setDeadlineMs(qint64 remainingMs);

    void onTick();
    void finish();
    void restoreVolume();

    // Hooks invoked by Narration lifecycle transitions; return whether sleepChanged()
    // should be emitted by the caller once its own state transition completes.
    bool onExternalVolumeChange();
    bool onPause();
    bool onResume();
    void onJumpToPlay();
    bool reset();

    // Direct references preserved for Narration's existing friend-class test hooks.
    Deadline &deadlineRef() { return m_deadline; }
    bool &fadingRef() { return m_fading; }
    double &preFadeVolumeRef() { return m_preFadeVolume; }

signals:
    void changed();
    void sleepFinished();

private:
    bool isPlaybackActive() const;
    bool isPlaybackPlaying() const;

    QPointer<SpeechPlayer> m_player;
    Callbacks m_callbacks;
    QTimer m_timer;
    QTimer m_tick;
    Deadline m_deadline;
    int m_lastSleepSec = 0;
    bool m_fading = false;
    bool m_pausedRestore = false;
    double m_preFadeVolume = -1.0;
    bool m_atChapterEnd = false;
};
