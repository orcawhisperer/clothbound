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
    // Optional explicit voice override (e.g. from inline dialogue-span
    // splitting); empty means fall back to the block's cast/narrator voice.
    QString voice;

    qint64 start() const { return sentences.isEmpty() ? 0 : sentences.first().start; }
    qint64 end() const { return sentences.isEmpty() ? 0 : sentences.last().end; }
};

// A contiguous span inside a paragraph: either quoted speech (read by the
// speaking character) or surrounding narration / attribution (read by the
// narrator), with exact reader offsets.
struct DialogueSpan {
    bool dialogue = false;
    QList<SpeechSentence> sentences;
};

namespace Speech {
QList<SpeechBlock> parseBlocks(const QString &json);
// A block's sentences, at model offsets, without the skipped spans.
QList<SpeechSentence> sentences(const SpeechBlock &block);
// Splits a paragraph that mixes quoted speech and narration tags into
// alternating dialogue and narration spans with exact offsets.
QList<DialogueSpan> splitDialogueSpans(const SpeechBlock &block);
// A heading as it should be said: a chapter's Roman numeral as its number
// ("CHAPTER V" says "Chapter 5"), capitals that would sound like shouting
// in ordinary case, and a full stop so the title that follows gets a beat.
QString spokenHeading(const QString &text);
// A chapter's clips, always cut the same way from its start, so audio made
// once (heard, or downloaded) is found again whenever the chapter is read.
// A chapter's number and title are read together; short paragraphs share a
// clip; long ones are split between sentences. Blocks in `breaks` (where
// the mood of the reading changes) always begin a clip, so each clip is
// read in one mood. When `blockVoices` is supplied (full cast), paragraphs
// that mix quoted dialogue with narration tags are split into character and
// narrator clips.
QList<SpeechClip> clips(int chapter, const QList<SpeechBlock> &blocks, const QSet<int> &breaks = {},
                        int longest = 1500, int merge = 600,
                        const QMap<int, QString> &blockVoices = {}, const QString &narratorVoice = {});
// The clips to read from `from` on: the chapter's own clips, with the one
// `from` falls in begun at its sentence.
QList<SpeechClip> clipsFrom(int chapter, const QList<SpeechBlock> &blocks, qint64 from, const QSet<int> &breaks = {},
                            const QMap<int, QString> &blockVoices = {}, const QString &narratorVoice = {});
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
// Atomically replaces `target` with `source` (POSIX rename with remove+rename fallback).
bool atomicReplace(const QString &source, const QString &target);
// Personality trait label for classic Gemini prebuilt voices (shared by Narration & VoiceCatalog).
QString classicGeminiTrait(const QString &voiceId);

// Stateless clip timeline & seek calculations shared by Narration and tests.
struct TimelineEntry {
    int chapter = 0;
    int pauseMs = 0;
    qint64 samples = 0;
};
struct SeekTarget {
    int index = -1;
    qint64 sample = 0;
    bool wantNextChapter = false;
};
qint64 usToSamples(qint64 us, int sampleRate);
int sentenceForSample(const SpeechClip &clip, const QList<QPair<double, double>> &spans,
                      const QList<qint64> &boundaries, qint64 expectedSamples, qint64 sample);
qint64 sampleOfSentence(const SpeechClip &clip, const QList<QPair<double, double>> &spans,
                        const QList<qint64> &boundaries, qint64 expectedSamples, int sentence);
SeekTarget seekBySamples(const QList<TimelineEntry> &entries, int currentIndex, qint64 currentSample,
                         qint64 deltaSamples, int sampleRate);
SeekTarget seekToChapterUs(const QList<TimelineEntry> &entries, int currentIndex, qint64 targetUs, int sampleRate);
QPair<qint64, qint64> chapterTimingUs(const QList<TimelineEntry> &entries, int currentIndex, qint64 currentSample,
                                      int sampleRate);
}
