#pragma once

#include "ai/ai.h"
#include "audio/speech.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantList>
#include <functional>
#include <memory>
#include <optional>

class AiServices;
class AudioCache;
class Library;
class Reader;
class VoiceCatalog;
class VoicePacks;

// A full cast: each character's lines in a voice of their own, the
// narrator reading the rest. A quick text model says who speaks each
// paragraph of dialogue (once a chapter, kept with the narration cache, so
// a chapter always sounds the same); characters are given distinct voices
// from the narrator's own family (Gemini's, or the built-in Kokoro voices
// of the book's language), matched by gender, and the reader can change any.
class Cast : public QObject {
    Q_OBJECT
    // On for the open book.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    // Whether the narrator's voices have a family to cast from.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // Characters heard so far: {speaker, gender, voice, voiceName, lines}.
    Q_PROPERTY(QVariantList members READ members NOTIFY changed)

public:
    struct Line {
        QString speaker;
        QString gender;  // "female", "male", or ""
    };

    Cast(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, VoiceCatalog *catalog,
         VoicePacks *packs, QObject *parent = nullptr);
    ~Cast() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool available() const;
    QVariantList members() const;

    void setNarrator(const QString &voice);

    static QString prompt();
    // Who speaks which paragraph, from the model's answer.
    static QMap<int, Line> parse(const QString &json, int blocks);
    // The paragraphs where the voice changes, for cutting clips.
    QSet<int> breaks(int chapter, const QList<SpeechBlock> &blocks) const;
    // Character voice overrides per block (excluding narrator blocks), used for
    // inline dialogue-span splitting when a paragraph mixes speech & narration.
    QMap<int, QString> blockVoices(int chapter, const QList<SpeechBlock> &blocks) const;
    // The voice of a paragraph: its speaker's, or the narrator's.
    QString voiceAt(int chapter, int block) const;

    std::optional<QMap<int, Line>> kept(int chapter, const QList<SpeechBlock> &blocks) const;
    std::optional<QHash<int, QMap<int, Line>>> keptPdfBatch(int page) const;
    void ask(int chapter, const QList<SpeechBlock> &blocks, QObject *context, std::function<void(const QMap<int, Line> &)> done);
    void askPdfBatch(int page, QObject *context, std::function<void(const QHash<int, QMap<int, Line>> &byPage)> done);
    // Aborts in-flight casting requests for `scope`, or all when `scope` is null.
    void cancel(const QObject *scope = nullptr);
    // What's known of a chapter's speakers (asked, or kept).
    void learn(int chapter, const QMap<int, Line> &lines);
    bool knows(int chapter) const { return m_lines.contains(chapter); }

    // Voices a character could have: {id, name, gender}.
    Q_INVOKABLE QVariantList choices() const;
    Q_INVOKABLE void setVoice(const QString &speaker, const QString &voice);

signals:
    void changed();
    // A character's voice was changed by hand: what's not yet heard is remade.
    void recast();

private:
    struct Choice {
        QString id;
        QString name;
        QString gender;
        bool home = true;  // in the narrator's accent
    };
    QString family() const;
    static bool voiceMatchesFamily(const QString &voice, const QString &family);
    static QString formatParagraphs(const QList<SpeechBlock> &blocks);
    QList<Choice> pool() const;
    QString voiceFor(const QString &speaker) const;
    bool assignVoice(const QString &speaker, const QString &gender, const QList<Choice> &choices,
                     QMap<QString, QString> &familyMap, QSet<QString> &used);
    void assignMissing();
    Ai::TextRequest request(int chapter, const QList<SpeechBlock> &blocks) const;
    QString key(const QList<SpeechBlock> &blocks) const;
    void load();
    void save() const;

    struct PendingRequest {
        const QObject *scope = nullptr;
        QPointer<Ai::Reply> reply;
        QPointer<QObject> guard;
        std::function<void(const QMap<int, Line> &)> done;
        bool settled = false;
    };

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    Library *m_library;
    VoiceCatalog *m_catalog;
    VoicePacks *m_packs;
    bool m_enabled = false;
    bool m_destroying = false;
    QString m_narrator;
    QList<std::shared_ptr<PendingRequest>> m_pending;
    // chapter → paragraph → who
    QHash<int, QMap<int, Line>> m_lines;
    QSet<int> m_countedChapters;
    // voice family ("kokoro" / "gemini") → speaker → voice, kept separately so
    // switching to offline Kokoro never leaks a paid cloud voice.
    QHash<QString, QMap<QString, QString>> m_familyVoices;
    QMap<QString, QString> m_genders;
    QMap<QString, int> m_counts;
};
