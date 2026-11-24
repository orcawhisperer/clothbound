#pragma once

#include "ai/ai.h"
#include "audio/speech.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace Ai {
class Reply;
}
class AiServices;
class AudioCache;
class Library;
class Reader;
class Cast;
class Director;
class SpeechPlayer;

// Listening: the open book read aloud from where the reader is. Each
// chapter's blocks (from the reading surface) become clips; a director's
// pass marks the passages that want a particular delivery; clips are made
// a few ahead (from the cache when they've been heard before) and played
// back to back, the sentence being spoken reported as it goes.
class Narration : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY stateChanged)
    Q_PROPERTY(bool following READ following WRITE setFollowing NOTIFY stateChanged)
    Q_PROPERTY(QString voice READ voice NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceName READ voiceName NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceAccent READ voiceAccent NOTIFY voiceChanged)
    Q_PROPERTY(double rate READ rate WRITE setRate NOTIFY rateChanged)
    // How the book is performed: "plain" (an even reading, no direction),
    // "natural" (the mood follows the scene, never acted; the default), or
    // "dramatic" (a fuller performance). Kept for each book.
    Q_PROPERTY(QString performance READ performance WRITE setPerformance NOTIFY settingsChanged)
    Q_PROPERTY(bool economy READ economy WRITE setEconomy NOTIFY settingsChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY positionChanged)
    Q_PROPERTY(qint64 sentenceStart READ sentenceStart NOTIFY positionChanged)
    Q_PROPERTY(qint64 sentenceEnd READ sentenceEnd NOTIFY positionChanged)
    Q_PROPERTY(int minutesLeftInChapter READ minutesLeftInChapter NOTIFY positionChanged)
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY timeChanged)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY timeChanged)
    Q_PROPERTY(int sleepMinutes READ sleepMinutes NOTIFY sleepChanged)
    Q_PROPERTY(int sleepSecondsLeft READ sleepSecondsLeft NOTIFY sleepChanged)
    Q_PROPERTY(bool sleepFading READ sleepFading NOTIFY sleepChanged)
    Q_PROPERTY(bool sleepAtChapterEnd READ sleepAtChapterEnd NOTIFY sleepChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // Seamless cloud-to-local fallback offer / notice.
    Q_PROPERTY(QString fallbackVoice READ fallbackVoice NOTIFY fallbackChanged)
    Q_PROPERTY(QString fallbackVoiceName READ fallbackVoiceName NOTIFY fallbackChanged)
    Q_PROPERTY(bool offerOfflineSetup READ offerOfflineSetup NOTIFY fallbackChanged)
    Q_PROPERTY(QString fallbackNotice READ fallbackNotice NOTIFY fallbackChanged)
    Q_PROPERTY(bool hasPendingStart READ hasPendingStart NOTIFY stateChanged)
    // A voice slower than listening (built-in, on a slow computer) gets a
    // head start first, so it then plays through without pausing.
    Q_PROPERTY(bool preparing READ preparing NOTIFY stateChanged)
    Q_PROPERTY(int preparedPercent READ preparedPercent NOTIFY positionChanged)
    // One-shot speech playback for Companion responses (reuses the active/default voice).
    Q_PROPERTY(bool speakingResponse READ speakingResponse NOTIFY responseSpeechChanged)
    Q_PROPERTY(bool responseBuffering READ responseBuffering NOTIFY responseSpeechChanged)
    Q_PROPERTY(QString speakingResponseId READ speakingResponseId NOTIFY responseSpeechChanged)

public:
    Narration(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, QObject *parent = nullptr);
    ~Narration() override;

    bool active() const { return m_active; }
    bool playing() const { return m_active && m_wanted; }
    bool buffering() const { return m_buffering; }
    bool following() const { return m_following; }
    void setFollowing(bool following);
    QString voice() const { return m_voice; }
    QString voiceName() const { return m_voiceName; }
    QString voiceAccent() const;
    Library *library() const { return m_library; }
    double rate() const { return m_rate; }
    void setRate(double rate);
    double volume() const;
    void setVolume(double volume);
    QString performance() const { return m_performance; }
    void setPerformance(const QString &performance);
    bool economy() const { return m_economy; }
    void setEconomy(bool economy);
    int chapter() const;
    qint64 sentenceStart() const { return m_sentenceStart; }
    qint64 sentenceEnd() const { return m_sentenceEnd; }
    int minutesLeftInChapter() const;
    qint64 positionUs() const;
    qint64 durationUs() const;
    int sleepMinutes() const;
    int sleepSecondsLeft() const;
    bool sleepFading() const { return m_sleepFading; }
    bool sleepAtChapterEnd() const { return m_sleepAtChapterEnd; }
    QString problem() const { return m_problem; }
    QString fallbackVoice() const { return m_fallbackVoice; }
    QString fallbackVoiceName() const { return m_fallbackVoiceName; }
    bool offerOfflineSetup() const { return m_offerOfflineSetup; }
    QString fallbackNotice() const { return m_fallbackNotice; }
    bool hasPendingStart() const { return m_pendingStart; }
    bool preparing() const { return m_holdSamples > 0; }
    int preparedPercent() const;
    bool speakingResponse() const { return m_responseActive; }
    bool responseBuffering() const { return m_responseBuffering; }
    QString speakingResponseId() const { return m_responseId; }

    // Starts reading the open book at a chapter and offset.
    Q_INVOKABLE void start(int chapter, qint64 offset);
    Q_INVOKABLE void startPending();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void speakResponse(const QString &markdownOrText, const QString &itemId);
    Q_INVOKABLE void stopResponse();
    static QString cleanSpeechText(const QString &markdownOrText);
    Q_INVOKABLE void nextParagraph();
    Q_INVOKABLE void previousParagraph();
    Q_INVOKABLE void nextSentence();
    Q_INVOKABLE void prevSentence();
    Q_INVOKABLE void skipSeconds(int delta);
    // Relative and absolute seek in microseconds (used by skipSeconds, scrubber, and MPRIS2).
    void seekByUs(qint64 deltaUs);
    Q_INVOKABLE void seekToUs(qint64 targetUs);
    // The voice for this book from now on (and the default for new books).
    Q_INVOKABLE void setVoice(const QString &id, const QString &name);
    Q_INVOKABLE void useFallbackVoice();
    Q_INVOKABLE void sleepIn(int minutes);
    Q_INVOKABLE void sleepAtEndOfChapter();

    // A chapter's blocks, as the reading surface reports them.
    void addBlocks(int chapter, const QString &json);
    // A full cast for the book, when on.
    void setCast(Cast *cast);

signals:
    void stateChanged();
    void voiceChanged();
    void rateChanged();
    void volumeChanged();
    void settingsChanged();
    void positionChanged();
    void timeChanged();
    void seeked(qint64 positionUs);
    void sleepChanged();
    void problemChanged();
    void fallbackChanged();
    // The engine needs a chapter's blocks from the reading surface.
    void blocksWanted(int chapter);
    void speaking(int chapter, qint64 start, qint64 end);
    void needsKey();
    // The chosen built-in voice has to be downloaded first.
    void voiceMissing();
    void responseSpeechChanged();
    void responseSpeechProblem(const QString &message);

private:
    friend class ClothboundTests;
    struct ResponseChunk {
        enum State { Waiting, Working, Complete, Failed };
        int trackId = 0;
        QString text;
        QStringList sentences;
        QString voice;
        QString key;
        State state = Waiting;
        bool queued = false;
        QByteArray pcm;
        QPointer<Ai::Reply> reply;
    };
    struct Clip : SpeechClip {
        // Waiting: not started. Working: its audio is arriving (from the
        // network, or being decoded from the cache). Complete: all of it is
        // here. Failed: it couldn't be made.
        enum State { Waiting, Working, Complete, Failed };
        int id = 0;  // stable monotonic track ID for SpeechPlayer
        State state = Waiting;
        bool immediate = false;
        bool queued = false;
        int tries = 0;
        QString key;
        QByteArray pcm;
        qint64 samples = 0;
        QList<QPair<double, double>> spans;  // its sentences' share of its length
        QList<qint64> boundaries;            // where each sentence ends, when the voice said
        QPointer<Ai::Reply> reply;
    };

    void pump();
    void fetch(int index);
    void decode(int index, const QString &file);
    void queue();
    void stallIfFailed();
    void heard(int trackId, qint64 sample);
    void jumpTo(int index, qint64 sampleOffset = 0, bool preservePause = false, int forceSentence = -1);
    void seekBySamples(qint64 deltaSamples);
    qint64 sampleOfSentence(const Clip &clip, int sentence) const;
    void showSentenceForSample(int index, qint64 sample, int forceSentence = -1);
    void showSentence(double fraction);
    void showSentenceAt(int sentence);
    int indexOfClip(int trackId) const;
    qint64 expectedSamples(const Clip &clip) const;
    void checkHeadStart();
    void setBuffering(bool buffering);
    void setProblem(const QString &problem);
    void onSleepTick();
    void finishSleep();
    void restoreSleepVolume();
    QPair<QString, QString> bestLocalVoice() const;
    bool localPacksSupported() const;
    void updateFallbackOffer();
    void clearFallbackOffer();
    void setFallbackNotice(const QString &notice);
    bool fallbackToLocalMidSession(int fromIndex, const QString &reason);
    bool keptDirections(int chapter);
    QSet<int> breaksFor(int chapter) const;
    QMap<int, QString> blockVoicesFor(int chapter, const QList<SpeechBlock> &blocks) const;
    // A clip of the chapter's, with its reader.
    Clip clipFrom(const SpeechClip &spoken);
    QString voiceOf(const Clip &clip) const;
    // The chapter's clips not yet begun, cut again (after new direction or casting).
    void recut(int chapter);
    void requestCast(int chapter);
    void requestDirections(int chapter);
    void applyMoods(int chapter, const QMap<int, QString> &moods);
    bool waitingForDirections(const Clip &clip) const;
    qint64 directionWaitLeft(const Clip &clip) const;
    QString styleFor(const Clip &clip) const;
    bool directed() const;
    Ai::SpeechSynthesizer *speaker() const;
    int nextChapterAfter(int chapter) const;
    void loadVoice();
    QString resolveResponseVoice() const;
    int indexOfResponseChunk(int trackId) const;
    void pumpResponse();
    void fetchResponse(int index);
    void decodeResponse(int index, const QString &file);
    void queueResponse();
    void failResponse(const QString &message);
    void setResponseBuffering(bool buffering);

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    Library *m_library;

    Director *m_director;
    QPointer<Cast> m_cast;
    SpeechPlayer *m_player;
    QList<Clip> m_clips;
    int m_nextClipId = 1;
    quint64 m_session = 0;
    int m_current = -1;
    int m_queued = -1;
    qint64 m_currentSample = 0;
    qint64 m_lastTimeEmitMs = 0;
    qint64 m_lastReportedSec = -1;
    double m_samplesPerChar = 24000.0 / 15.0;
    qint64 m_holdSamples = 0;
    int m_sentence = -1;
    qint64 m_sentenceStart = -1;
    qint64 m_sentenceEnd = -1;

    bool m_active = false;
    bool m_wanted = false;
    bool m_buffering = false;
    bool m_following = true;
    int m_bookId = 0;
    int m_startChapter = 0;
    qint64 m_startOffset = 0;
    int m_lastChapter = -1;
    QSet<int> m_requested;
    QSet<int> m_loadedChapters;
    QHash<int, QList<SpeechBlock>> m_blocks;
    QHash<int, QMap<int, QString>> m_moods;
    QHash<int, qint64> m_directionsAsked;
    QHash<int, qint64> m_castAsked;
    QSet<int> m_directorBatchesAsked;
    QSet<int> m_castBatchesAsked;

    QString m_voice;
    QString m_voiceName;
    double m_rate = 1.0;
    QString m_performance = QStringLiteral("natural");
    bool m_economy = false;
    QString m_problem;

    QString m_fallbackVoice;
    QString m_fallbackVoiceName;
    bool m_offerOfflineSetup = false;
    QString m_fallbackNotice;
    bool m_pendingStart = false;
    int m_pendingChapter = 0;
    qint64 m_pendingOffset = 0;

    QTimer m_directionsTimer;
    QTimer m_retryTimer;
    QTimer m_sleepTimer;
    QTimer m_sleepTick;
    QDateTime m_sleepAt;
    int m_lastSleepSec = 0;
    bool m_sleepFading = false;
    bool m_sleepPausedRestore = false;
    double m_preFadeVolume = -1.0;
    bool m_sleepAtChapterEnd = false;

    QList<ResponseChunk> m_responseChunks;
    quint64 m_responseSession = 0;
    int m_responseCurrent = 0;
    int m_responseQueued = -1;
    bool m_responseActive = false;
    bool m_responseBuffering = false;
    bool m_responseHeadless = false;
    QString m_responseId;
    QString m_responseVoice;
    bool m_bookPlayerDirty = false;
};
