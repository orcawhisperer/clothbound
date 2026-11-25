#pragma once

#include <QString>
#include <QStringList>

// Stateless Markdown, HTML, and LaTeX-to-spoken-text cleaning pipeline used by
// Narration for reading Companion responses and structured text aloud. Pure
// C++/Qt (no QObject dependency).
namespace SpeechCleaner {

// Converts LaTeX expressions ($...$, $$...$$, \(...\), \[...\], and known bare
// commands like \frac, \alpha, \nabla, \sqrt) into spoken English phrases while
// preserving inline code spans and currency spans such as "$5 to $10".
QString cleanLatexForSpeech(const QString &text);

// Full Markdown/HTML/LaTeX-to-speech cleaning pipeline: strips code fences,
// converts LaTeX to spoken phrases, removes tags/links/citations/formatting
// markers, decodes common entities, and normalizes spacing for TTS.
QString cleanSpeechText(const QString &markdownOrText);

// Subdivides an overlong sentence at clause punctuation or whitespace so each
// chunk stays within `maxChars` for low-latency TTS synthesis.
QStringList splitOverlongSentence(const QString &sentence, qsizetype maxChars = 450);

}  // namespace SpeechCleaner
