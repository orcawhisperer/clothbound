#pragma once

#include "ai/ai.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>

class AiServices;
class AudioCache;
class SpeechPlayer;

// Lightweight GUI-thread exclusive audio focus coordinator shared across Book
// narration, Companion response speech, and VoiceCatalog previews so two
// SpeechPlayers never talk over each other.
class AudioFocus {
public:
    enum class Kind { None, Book, Response, Preview };
    using YieldCallback = std::function<void()>;

    static AudioFocus &instance();

    // Grants exclusive focus to `(owner, kind)`. If another holder owns focus,
    // its registered yield callback runs synchronously before focus transfers.
    void claim(QObject *owner, Kind kind, YieldCallback onYield);
    // Releases focus if held by `owner` (matching `kind`, or any kind if None).
    void release(const QObject *owner, Kind kind = Kind::None);

    Kind currentKind() const { return m_kind; }
    QObject *currentOwner() const { return m_owner.data(); }

private:
    AudioFocus() = default;

    QPointer<QObject> m_owner;
    Kind m_kind = Kind::None;
    YieldCallback m_onYield;
    bool m_yielding = false;
};

// One-shot Companion response speech playback: cleans markdown, splits into
// low-latency sequential chunks, streams/caches audio on the shared SpeechPlayer,
// performs seamless cloud-to-local fallback, and arbitrates exclusive audio focus.
class ResponseSpeech : public QObject {
    Q_OBJECT

public:
    struct Chunk {
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

    struct Callbacks {
        std::function<int()> nextTrackId;
        std::function<QString()> preferredVoice;
        std::function<QString()> bookLanguage;
        std::function<bool()> economy;
        std::function<void()> onBeforeStart;
        std::function<void(const QString &)> onProblem;
    };

    ResponseSpeech(AiServices *ai, AudioCache *cache, SpeechPlayer *player, QObject *parent = nullptr);
    ~ResponseSpeech() override;

    void setCallbacks(Callbacks callbacks);

    bool active() const { return m_active; }
    bool buffering() const { return m_buffering; }
    QString itemId() const { return m_id; }
    const QString &voice() const { return m_voice; }
    const QList<Chunk> &chunks() const { return m_chunks; }

    void speak(const QString &markdownOrText, const QString &itemId);
    void stop();

    // Player event handlers while ResponseSpeech owns m_player. Return true if handled.
    bool handlePosition(int trackId, qint64 sample);
    bool handleDrained();
    bool handleStarved(bool starved);
    bool handleSinkError(const QString &message);

    static QPair<QString, QString> bestLocalVoice(AiServices *ai, const QString &language,
                                                  bool allowAnyLanguageFallback = false);
    static bool localPacksSupported(AiServices *ai);
    static QString voiceAccent(AiServices *ai, const QString &voice);
    static QString voiceUnavailableMessage(AiServices *ai, const QString &voice);

signals:
    void changed();
    void problemOccurred(const QString &message);

private:
    QString resolveVoice() const;
    int indexOfChunk(int trackId) const;
    void pump();
    void fetch(int index);
    void decode(int index, const QString &file);
    void queue();
    void fail(const QString &message);
    void setBuffering(bool buffering);
    bool economyMode() const;

    AiServices *m_ai;
    AudioCache *m_cache;
    QPointer<SpeechPlayer> m_player;
    Callbacks m_callbacks;

    QList<Chunk> m_chunks;
    quint64 m_session = 0;
    int m_localTrackId = 1;
    int m_current = 0;
    int m_queued = -1;
    bool m_active = false;
    bool m_buffering = false;
    bool m_headless = false;
    QString m_id;
    QString m_voice;
};
