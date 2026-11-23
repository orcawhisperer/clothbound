#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

// One sentence as narrated: where it is in the chapter's text (the reader's
// offsets, so its highlight lands exactly) and the words actually said.
struct SpeechSentence {
    qint64 start = 0;
    qint64 end = 0;
    QString text;
};

// What the reading surface reports for each paragraph-like block.
struct SpeechBlock {
    qint64 start = 0;
    qint64 end = 0;
    QString text;
    QList<QPair<qint64, qint64>> skips;  // note markers, seen but not said
    QString kind;                         // "text", "heading", or "break"
};

// One request's worth of narration: a paragraph or a few short ones, or a
// long paragraph's part, or a pause at a scene break.
struct SpeechClip {
    int chapter = 0;
    int block = 0;  // the first block it reads, for the director's notes
    QList<SpeechSentence> sentences;
    QString text;
    QString style;
    int pauseMs = 0;

    qint64 start() const { return sentences.isEmpty() ? 0 : sentences.first().start; }
    qint64 end() const { return sentences.isEmpty() ? 0 : sentences.last().end; }
};

namespace Speech {
QList<SpeechBlock> parseBlocks(const QString &json);
// A block's sentences, at model offsets, without the skipped spans.
QList<SpeechSentence> sentences(const SpeechBlock &block);
// A heading as it should be said: a chapter's Roman numeral as its number
// ("CHAPTER V" says "Chapter 5"), capitals that would sound like shouting
// in ordinary case, and a full stop so the title that follows gets a beat.
QString spokenHeading(const QString &text);
// A chapter's clips, always cut the same way from its start, so audio made
// once (heard, or downloaded) is found again whenever the chapter is read.
// A chapter's number and title are read together; short paragraphs share a
// clip; long ones are split between sentences. Blocks in `breaks` (where
// the mood of the reading changes) always begin a clip, so each clip is
// read in one mood.
QList<SpeechClip> clips(int chapter, const QList<SpeechBlock> &blocks, const QSet<int> &breaks = {},
                        int longest = 1500, int merge = 600);
// The clips to read from `from` on: the chapter's own clips, with the one
// `from` falls in begun at its sentence.
QList<SpeechClip> clipsFrom(int chapter, const QList<SpeechBlock> &blocks, qint64 from, const QSet<int> &breaks = {});
// The moods a director can ask for, from a fixed list so the same scene is
// always read the same way (and found again in the cache).
QStringList moods();
// What the narrator is told for a mood: a few plain words, fuller when the
// performance is dramatic. "" for the ordinary reading.
QString styleFor(const QString &mood, bool dramatic);
// The mood in effect at a block, given where it changes.
QString moodAt(const QMap<int, QString> &changes, int block);
// Where a sentence starts and ends in a clip, as fractions of its length
// (by characters, with a little weight for the pause after punctuation).
QList<QPair<double, double>> timeline(const SpeechClip &clip);
QByteArray wav(const QByteArray &pcm, int sampleRate);
// Samples at another rate (cubic interpolation, which is clean enough for
// speech between 16 and 24 kHz).
QVector<float> resample(const QVector<float> &samples, int from, int to);
// Text as it's sent to a voice: without the soft hyphens and zero-width
// spaces that mend words broken across a PDF's lines (they keep offsets
// true on the page, but aren't spoken).
QString speakable(const QString &text);
// Samples between -1 and 1 as 16-bit PCM.
QByteArray pcm16(const QVector<float> &samples);
}
