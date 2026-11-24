#pragma once

#include "ai/ai.h"
#include "audio/speech.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
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
    // Groups consecutive PDF pages by outline section (capped at ~8 pages per
    // batch) so 300-page PDFs don't trigger 300 separate LLM calls and keep
    // mood continuity across page turns.
    static QPair<int, int> pdfBatchRange(const Reader *reader, int page);
    static QHash<int, QMap<int, QString>> splitMoodsByPage(const QMap<int, QString> &combined,
                                                           const QHash<int, QPair<int, int>> &ranges,
                                                           int firstPage, int lastPage);
    // The moods kept from an earlier reading, if any.
    std::optional<QMap<int, QString>> kept(int chapter, const QList<SpeechBlock> &blocks, const QString &performance) const;
    // Kept moods for the PDF batch containing `page`, mapped page -> moods.
    std::optional<QHash<int, QMap<int, QString>>> keptPdfBatch(int page, const QString &performance) const;
    // Asks for them. `done` runs in `context` with the moods, or none if the
    // director failed (which means the ordinary reading, never a wait).
    void ask(int chapter, const QList<SpeechBlock> &blocks, const QString &performance, QObject *context,
             std::function<void(const QMap<int, QString> &moods)> done);
    // Directs all pages in the PDF batch containing `page` in a single request,
    // preserving mood continuity across page boundaries.
    void askPdfBatch(int page, const QString &performance, QObject *context,
                     std::function<void(const QHash<int, QMap<int, QString>> &byPage)> done);
    // Aborts any in-flight direction requests.
    void cancel();

private:
    struct PdfBatch {
        int firstPage = 0;
        int lastPage = 0;
        QList<SpeechBlock> blocks;
        QHash<int, QPair<int, int>> ranges;
    };
    PdfBatch buildPdfBatch(int page) const;
    Ai::TextRequest request(int chapter, const QList<SpeechBlock> &blocks, const QString &performance) const;
    QString key(const Ai::TextRequest &request) const;

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    QList<QPointer<Ai::Reply>> m_replies;
};
