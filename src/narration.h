#pragma once

#include "ai.h"
#include "speech.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
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
    Q_PROPERTY(int sleepMinutes READ sleepMinutes NOTIFY sleepChanged)
    Q_PROPERTY(bool sleepAtChapterEnd READ sleepAtChapterEnd NOTIFY sleepChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // A voice slower than listening (built-in, on a slow computer) gets a
    // head start first, so it then plays through without pausing.
    Q_PROPERTY(bool preparing READ preparing NOTIFY stateChanged)
    Q_PROPERTY(int preparedPercent READ preparedPercent NOTIFY positionChanged)

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
    double rate() const { return m_rate; }
    void setRate(double rate);
    QString performance() const { return m_performance; }
    void setPerformance(const QString &performance);
    bool economy() const { return m_economy; }
    void setEconomy(bool economy);
    int chapter() const;
    qint64 sentenceStart() const { return m_sentenceStart; }
    qint64 sentenceEnd() const { return m_sentenceEnd; }
    int minutesLeftInChapter() const;
    int sleepMinutes() const;
    bool sleepAtChapterEnd() const { return m_sleepAtChapterEnd; }
    QString problem() const { return m_problem; }
    bool preparing() const { return m_holdSamples > 0; }
    int preparedPercent() const;

    // Starts reading the open book at a chapter and offset.
    Q_INVOKABLE void start(int chapter, qint64 offset);
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void nextParagraph();
    Q_INVOKABLE void previousParagraph();
    // The voice for this book from now on (and the default for new books).
    Q_INVOKABLE void setVoice(const QString &id, const QString &name);
    Q_INVOKABLE void sleepIn(int minutes);
    Q_INVOKABLE void sleepAtEndOfChapter();

    // A chapter's blocks, as the reading surface reports them.
    void addBlocks(int chapter, const QString &json);

signals:
    void stateChanged();
    void voiceChanged();
    void rateChanged();
    void settingsChanged();
    void positionChanged();
    void sleepChanged();
    void problemChanged();
    // The engine needs a chapter's blocks from the reading surface.
    void blocksWanted(int chapter);
    void speaking(int chapter, qint64 start, qint64 end);
    void needsKey();
    // The chosen built-in voice has to be downloaded first.
    void voiceMissing();

private:
    struct Clip : SpeechClip {
        // Waiting: not started. Working: its audio is arriving (from the
        // network, or being decoded from the cache). Complete: all of it is
        // here. Failed: it couldn't be made.
        enum State { Waiting, Working, Complete, Failed };
        State state = Waiting;
        bool immediate = false;
        bool queued = false;
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
    void heard(int track, qint64 sample);
    void jumpTo(int index);
    void showSentence(double fraction);
    void showSentenceAt(int sentence);
    qint64 expectedSamples(const Clip &clip) const;
    void checkHeadStart();
    void setBuffering(bool buffering);
    void setProblem(const QString &problem);
    bool keptDirections(int chapter);
    QSet<int> breaksFor(int chapter) const;
    void requestDirections(int chapter);
    void applyMoods(int chapter, const QMap<int, QString> &moods);
    bool waitingForDirections(const Clip &clip) const;
    QString styleFor(const Clip &clip) const;
    bool directed() const;
    Ai::SpeechSynthesizer *speaker() const;
    int nextChapterAfter(int chapter) const;
    void loadVoice();

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    Library *m_library;

    Director *m_director;
    SpeechPlayer *m_player;
    QList<Clip> m_clips;
    int m_current = -1;
    int m_queued = -1;
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

    QString m_voice;
    QString m_voiceName;
    double m_rate = 1.0;
    QString m_performance = QStringLiteral("natural");
    bool m_economy = false;
    QString m_problem;

    QTimer m_directionsTimer;
    QTimer m_sleepTimer;
    QDateTime m_sleepAt;
    bool m_sleepAtChapterEnd = false;
};
