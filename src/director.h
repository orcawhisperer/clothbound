#pragma once

#include "ai.h"
#include "speech.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <optional>

class AiServices;
class AudioCache;
class Reader;

// The director's moods for a chapter: where the reading turns hushed,
// tense, or tender, asked of the quick text model once and then kept with
// the narration cache, so a chapter is always read the same way (and its
// audio, heard or downloaded, is found again).
class Director : public QObject {
    Q_OBJECT

public:
    Director(AiServices *ai, AudioCache *cache, Reader *reader, QObject *parent = nullptr);
    ~Director() override;

    static QString prompt();
    // Where the mood changes (a paragraph's index), from the director's answer.
    static QMap<int, QString> parse(const QString &json, int blocks);
    static QSet<int> breaks(const QMap<int, QString> &moods);

    // Whether this chapter can be directed at all (a text model, something to read).
    bool canDirect(const QList<SpeechBlock> &blocks) const;
    // The moods kept from an earlier reading, if any.
    std::optional<QMap<int, QString>> kept(int chapter, const QList<SpeechBlock> &blocks, const QString &performance) const;
    // Asks for them. `done` runs in `context` with the moods, or none if the
    // director failed (which means the ordinary reading, never a wait).
    void ask(int chapter, const QList<SpeechBlock> &blocks, const QString &performance, QObject *context,
             std::function<void(const QMap<int, QString> &moods)> done);
    // Aborts any in-flight direction requests.
    void cancel();

private:
    Ai::TextRequest request(int chapter, const QList<SpeechBlock> &blocks, const QString &performance) const;
    QString key(const Ai::TextRequest &request) const;

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    QList<QPointer<Ai::Reply>> m_replies;
};
