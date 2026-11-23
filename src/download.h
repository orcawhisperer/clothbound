#pragma once

#include "ai.h"
#include "speech.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

class AiServices;
class Cast;
class AudioCache;
class Director;
class Narration;
class Reader;

// Narration made ahead, to listen offline: every clip of a run of chapters,
// directed and cut exactly as listening would (so listening finds it), made
// at the provider's slower, cheaper tier, and kept in the cache. What's made
// is kept, so a paused or interrupted download picks up where it stopped.
class Download : public QObject {
    Q_OBJECT
    // Begun and not finished or cancelled (it may be paused).
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool finished READ finished NOTIFY changed)
    Q_PROPERTY(int bookId READ bookId NOTIFY changed)
    Q_PROPERTY(int firstChapter READ firstChapter NOTIFY changed)
    Q_PROPERTY(int lastChapter READ lastChapter NOTIFY changed)
    Q_PROPERTY(double minutesReady READ minutesReady NOTIFY progressChanged)
    Q_PROPERTY(double minutesTotal READ minutesTotal NOTIFY progressChanged)
    Q_PROPERTY(double fraction READ fraction NOTIFY progressChanged)
    // An estimate once a few clips have shown the pace; -1 before.
    Q_PROPERTY(int secondsLeft READ secondsLeft NOTIFY progressChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)

public:
    Download(AiServices *ai, AudioCache *cache, Reader *reader, Narration *narration, QObject *parent = nullptr);
    ~Download() override;

    bool active() const { return m_active; }
    bool running() const { return m_active && !m_paused; }
    bool finished() const { return m_finished; }
    int bookId() const { return m_bookId; }
    int firstChapter() const { return m_chapters.isEmpty() ? -1 : m_chapters.first(); }
    int lastChapter() const { return m_chapters.isEmpty() ? -1 : m_chapters.last(); }
    double minutesReady() const { return m_minutesReady; }
    double minutesTotal() const { return m_minutesTotal; }
    double fraction() const { return m_minutesTotal > 0 ? qMin(1.0, m_minutesReady / m_minutesTotal) : 0; }
    int secondsLeft() const;
    QString problem() const { return m_problem; }

    // What downloading these chapters of the open book would take:
    // {chapters, minutes, cost}.
    Q_INVOKABLE QVariantMap estimate(int first, int last) const;
    Q_INVOKABLE void start(int first, int last);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void cancel();

    // A chapter's blocks, as the reading surface reports them.
    void addBlocks(int chapter, const QString &json);
    // A full cast, so downloads are read by the voices listening uses.
    void setCast(Cast *cast) { m_cast = cast; }

signals:
    void changed();
    void progressChanged();
    // The download needs a chapter's blocks from the reading surface.
    void blocksWanted(int chapter);
    // Every clip of the run is kept.
    void ready();

private:
    struct Job {
        QString key;
        QString text;
        QStringList sentences;
        QString style;
        QString voice;
        double minutes = 0;
        int tries = 0;
    };

    QList<int> chaptersBetween(int first, int last) const;
    double estimatedMinutes(int chapter) const;
    void planAhead();
    void plan(int chapter, const QList<SpeechBlock> &blocks, const QMap<int, QString> &moods);
    void direct(int chapter, const QList<SpeechBlock> &blocks);
    void pump();
    void made(const Job &job, Ai::Reply *reply);
    void failed(Job job, const Ai::Error &error);
    void stopWith(const QString &problem);
    void checkFinished();
    void setProblem(const QString &problem);

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    Narration *m_narration;
    Director *m_director;
    Cast *m_cast = nullptr;

    bool m_active = false;
    bool m_paused = false;
    bool m_finished = false;
    int m_bookId = 0;
    QString m_voice;
    QString m_performance;
    bool m_economy = false;
    bool m_flex = true;
    // Built-in voices: made on this computer, one at a time, and free.
    bool m_local = false;
    QList<int> m_chapters;
    int m_nextChapter = 0;
    QSet<int> m_waiting;  // chapters whose blocks or direction are on their way
    QList<Job> m_queue;
    QList<QPointer<Ai::Reply>> m_replies;
    double m_minutesReady = 0;
    double m_minutesTotal = 0;
    double m_audioSeconds = 0;  // made by this download, for its pace
    QElapsedTimer m_clock;
    qint64 m_activeMs = 0;
    QTimer m_retry;
    QString m_problem;
};
