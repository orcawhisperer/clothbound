#include "audio/sleepcontroller.h"

#include "audio/speechplayer.h"

#include <QThread>
#include <cmath>
#include <limits>

SleepController::SleepController(SpeechPlayer *player, QObject *parent) : QObject(parent), m_player(player) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &SleepController::finish);
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &SleepController::onTick);
}

SleepController::~SleepController() {
    m_timer.stop();
    m_tick.stop();
}

void SleepController::setCallbacks(Callbacks callbacks) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_callbacks = std::move(callbacks);
}

bool SleepController::isPlaybackActive() const {
    if (m_callbacks.isActive)
        return m_callbacks.isActive();
    return m_player && m_player->isPlaying();
}

bool SleepController::isPlaybackPlaying() const {
    if (m_callbacks.isPlaying)
        return m_callbacks.isPlaying();
    return m_player && m_player->isPlaying();
}

int SleepController::sleepMinutes() const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_deadline.isValid())
        return 0;
    return qMax(1, int(std::ceil(m_deadline.remainingMs() / 60000.0)));
}

int SleepController::sleepSecondsLeft() const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_deadline.isValid())
        return 0;
    return int(qMax<qint64>(0, (m_deadline.remainingMs() + 999) / 1000));
}

void SleepController::restoreVolume() {
    Q_ASSERT(QThread::currentThread() == thread());
    const double saved = m_preFadeVolume;
    m_preFadeVolume = -1.0;
    m_fading = false;
    m_pausedRestore = false;
    if (saved >= 0.0 && m_player)
        m_player->setVolume(saved);
}

bool SleepController::sleepIn(int minutes) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool wasChapterEnd = m_atChapterEnd;
    m_atChapterEnd = false;
    if (m_fading || m_pausedRestore)
        restoreVolume();
    if (minutes <= 0) {
        m_timer.stop();
        m_tick.stop();
        m_deadline.clear();
        m_lastSleepSec = 0;
    } else {
        const qint64 durationMs = qint64(minutes) * 60LL * 1000LL;
        m_deadline.startMs(durationMs);
        m_lastSleepSec = int(qMin<qint64>(qint64(minutes) * 60LL, std::numeric_limits<int>::max()));
        m_timer.start(int(qMin<qint64>(durationMs, std::numeric_limits<int>::max())));
        m_tick.setInterval(1000);
        m_tick.start();
    }
    emit changed();
    return wasChapterEnd;
}

void SleepController::sleepAtEndOfChapter() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_fading || m_pausedRestore)
        restoreVolume();
    m_timer.stop();
    m_tick.stop();
    m_deadline.clear();
    m_lastSleepSec = 0;
    m_atChapterEnd = true;
    emit changed();
}

void SleepController::consumeChapterEnd() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_atChapterEnd)
        return;
    m_atChapterEnd = false;
    emit changed();
}

void SleepController::setDeadlineMs(qint64 remainingMs) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_atChapterEnd = false;
    m_deadline.startMs(remainingMs);
    m_lastSleepSec = sleepSecondsLeft();
    if (remainingMs > 0) {
        m_timer.start(int(qMin<qint64>(remainingMs, std::numeric_limits<int>::max())));
        if (!m_tick.isActive())
            m_tick.start();
    } else {
        m_timer.stop();
    }
}

void SleepController::onTick() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_deadline.isValid()) {
        m_tick.stop();
        return;
    }
    const qint64 leftMs = m_deadline.remainingMs();
    if (leftMs <= 0) {
        finish();
        return;
    }
    const bool active = isPlaybackActive();
    const bool wanted = isPlaybackPlaying();
    if (leftMs <= fadeDurationMs && active && wanted) {
        if (!m_fading) {
            m_fading = true;
            if (m_preFadeVolume < 0.0 && m_player)
                m_preFadeVolume = m_player->volume();
            m_tick.setInterval(100);
        }
        const double t = qBound(0.0, double(leftMs) / double(fadeDurationMs), 1.0);
        if (m_player && m_preFadeVolume >= 0.0)
            m_player->setVolume(m_preFadeVolume * t * t);
    } else if (m_fading && !wanted) {
        restoreVolume();
        m_tick.setInterval(1000);
        emit changed();
    } else if (!wanted && m_tick.interval() < 1000) {
        m_tick.setInterval(1000);
    }
    const int secs = sleepSecondsLeft();
    if (secs != m_lastSleepSec || m_fading) {
        m_lastSleepSec = secs;
        emit changed();
    }
}

void SleepController::finish() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_callbacks.onBeforeFinish)
        m_callbacks.onBeforeFinish();
    m_timer.stop();
    m_tick.stop();
    m_deadline.clear();
    m_lastSleepSec = 0;
    if (!isPlaybackActive()) {
        if (m_fading || m_pausedRestore)
            restoreVolume();
        emit changed();
        return;
    }
    if (m_preFadeVolume < 0.0 && m_player)
        m_preFadeVolume = m_player->volume();
    if (m_player)
        m_player->setVolume(0.0);
    m_fading = false;
    m_pausedRestore = true;
    if (m_callbacks.onPauseForSleep)
        m_callbacks.onPauseForSleep();
    else if (m_player)
        m_player->pause();
    emit changed();
    emit sleepFinished();
}

bool SleepController::onExternalVolumeChange() {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool wasFading = m_fading;
    m_pausedRestore = false;
    m_fading = false;
    m_preFadeVolume = -1.0;
    return wasFading;
}

bool SleepController::onPause() {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool wasFading = m_fading;
    if (m_fading && !m_pausedRestore)
        restoreVolume();
    return wasFading;
}

bool SleepController::onResume() {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool wasFading = m_fading;
    if (m_pausedRestore || m_fading)
        restoreVolume();
    return wasFading;
}

void SleepController::onJumpToPlay() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_pausedRestore || m_fading)
        restoreVolume();
}

bool SleepController::reset() {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool hadSleep = isActive();
    m_timer.stop();
    m_tick.stop();
    m_deadline.clear();
    m_lastSleepSec = 0;
    m_atChapterEnd = false;
    if (m_fading || m_pausedRestore)
        restoreVolume();
    return hadSleep;
}
