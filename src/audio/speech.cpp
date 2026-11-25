#include "audio/speech.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
bool closing(QChar c) {
    static const QString marks = QStringLiteral("\"'’”»)]」』》）】");
    return marks.contains(c);
}

bool opening(QChar c) {
    static const QString marks = QStringLiteral("\"'‘“«([¡¿—–„‚「『《（【");
    return marks.contains(c);
}

bool cjkTerminator(QChar c) {
    return c == QChar(0x3002) || c == QChar(0xFF01) || c == QChar(0xFF1F);
}

bool sentenceTerminator(QChar c) {
    return c == '.' || c == '!' || c == '?' || c == QChar(0x2026) || c == QChar(0x0964) || c == QChar(0x0965) ||
           c == QChar(0x061F) || cjkTerminator(c);
}

bool caselessLetter(QChar c) { return c.isLetter() && !c.isLower() && !c.isUpper(); }

// Words that end in a period without ending a sentence.
bool abbreviation(const QString &word) {
    static const QSet<QString> words{"mr", "mrs", "ms", "dr", "st", "prof", "rev", "gen", "col", "capt", "lt",
                                     "sgt", "hon", "jr", "sr", "vs", "etc", "e.g", "i.e", "no", "vol", "ch",
                                     "pp", "fig", "mt", "ft", "esq", "messrs", "mme", "mlle", "viz", "cf"};
    return words.contains(word.toLower());
}

QString spoken(const SpeechBlock &block, qint64 from, qint64 to) {
    QString text;
    for (qint64 i = from; i < to; ++i) {
        const qint64 at = block.start + i;
        bool skip = false;
        for (const auto &[start, end] : block.skips)
            if (at >= start && at < end) {
                skip = true;
                break;
            }
        if (!skip)
            text += block.text[i];
    }
    return text.simplified();
}

bool isStructuralHeadingWord(const QString &word) {
    static const QSet<QString> structural{
        // English structural chapter/section/division labels (excluding names/pronouns/appendix)
        QStringLiteral("chapter"), QStringLiteral("ch"), QStringLiteral("chap"), QStringLiteral("part"),
        QStringLiteral("book"), QStringLiteral("section"), QStringLiteral("sec"), QStringLiteral("act"),
        QStringLiteral("scene"), QStringLiteral("volume"), QStringLiteral("vol"), QStringLiteral("canto"),
        QStringLiteral("stave"), QStringLiteral("letter"), QStringLiteral("epistle"), QStringLiteral("tale"),
        QStringLiteral("lesson"), QStringLiteral("unit"), QStringLiteral("module"), QStringLiteral("stage"),
        QStringLiteral("phase"), QStringLiteral("division"), QStringLiteral("episode"), QStringLiteral("entry"),
        QStringLiteral("day"), QStringLiteral("night"), QStringLiteral("number"), QStringLiteral("no"),
        QStringLiteral("plate"), QStringLiteral("figure"), QStringLiteral("fig"), QStringLiteral("table"),
        QStringLiteral("article"), QStringLiteral("psalm"), QStringLiteral("sonnet"), QStringLiteral("ode"),
        QStringLiteral("fable"), QStringLiteral("sermon"), QStringLiteral("lecture"), QStringLiteral("essay"),
        QStringLiteral("meditation"), QStringLiteral("proposition"), QStringLiteral("theorem"), QStringLiteral("question"),
        QStringLiteral("interlude"), QStringLiteral("prologue"), QStringLiteral("epilogue"),
        // Common multilingual chapter/part/book/act/scene labels
        QStringLiteral("chapitre"), QStringLiteral("partie"), QStringLiteral("livre"), QStringLiteral("tome"),
        QStringLiteral("acte"), QStringLiteral("scène"), QStringLiteral("chant"), QStringLiteral("lettre"),
        QStringLiteral("kapitel"), QStringLiteral("teil"), QStringLiteral("buch"), QStringLiteral("band"),
        QStringLiteral("abschnitt"), QStringLiteral("akt"), QStringLiteral("szene"), QStringLiteral("gesang"),
        QStringLiteral("brief"), QStringLiteral("capítulo"), QStringLiteral("capitulo"), QStringLiteral("parte"),
        QStringLiteral("libro"), QStringLiteral("tomo"), QStringLiteral("sección"), QStringLiteral("seccion"),
        QStringLiteral("acto"), QStringLiteral("escena"), QStringLiteral("carta"), QStringLiteral("capitolo"),
        QStringLiteral("sezione"), QStringLiteral("atto"), QStringLiteral("scena"), QStringLiteral("lettera"),
        QStringLiteral("hoofdstuk"), QStringLiteral("deel"), QStringLiteral("boek"), QStringLiteral("rozdział"),
        QStringLiteral("część"), QStringLiteral("księga"), QStringLiteral("kapittel"), QStringLiteral("del"),
        QStringLiteral("bok"), QStringLiteral("luku"), QStringLiteral("osa"), QStringLiteral("kirja"),
        QStringLiteral("глава"), QStringLiteral("часть"), QStringLiteral("книга"), QStringLiteral("том"),
        QStringLiteral("действие"), QStringLiteral("сцена"), QStringLiteral("письмо"), QStringLiteral("розділ"),
        QStringLiteral("частина"), QStringLiteral("fejezet"), QStringLiteral("rész"), QStringLiteral("könyv"),
        QStringLiteral("kapitola"), QStringLiteral("část"), QStringLiteral("kniha"), QStringLiteral("bölüm"),
        QStringLiteral("kısım"), QStringLiteral("kitap"),
    };
    return structural.contains(word.toLower());
}

bool hasLetterOrDigit(const QString &s) {
    for (const QChar c : s)
        if (c.isLetterOrNumber())
            return true;
    return false;
}

bool isQuoteCloseFor(QChar open, QChar c) {
    if (open == QChar(0x201C))  // “
        return c == QChar(0x201D);  // ”
    if (open == QLatin1Char('"'))
        return c == QLatin1Char('"');
    if (open == QChar(0x00AB))  // «
        return c == QChar(0x00BB);  // »
    if (open == QChar(0x300C))  // 「
        return c == QChar(0x300D);  // 」
    if (open == QChar(0x300E))  // 『
        return c == QChar(0x300F);  // 』
    if (open == QChar(0x201E))  // „
        return c == QChar(0x201C) || c == QChar(0x201D);  // “ or ”
    if (open == QChar(0x2018))  // ‘
        return c == QChar(0x2019);  // ’
    return false;
}

bool isQuoteOpen(const QString &text, qsizetype i) {
    const QChar c = text[i];
    if (c == QChar(0x201C) || c == QChar(0x00AB) || c == QChar(0x300C) || c == QChar(0x300E) || c == QChar(0x201E))
        return true;
    if (c == QLatin1Char('"'))
        return true;
    // British single quote: only open when not immediately preceded by a letter/digit
    // (avoids English contractions like don’t or possessives).
    if (c == QChar(0x2018))
        return i == 0 || !text[i - 1].isLetterOrNumber();
    return false;
}

bool isValidClose(const QString &text, qsizetype i, QChar open) {
    if (!isQuoteCloseFor(open, text[i]))
        return false;
    // For single quotes, don't close inside a contraction like "we’re" or "don’t".
    if (open == QChar(0x2018) && i + 1 < text.size() && text[i + 1].isLetterOrNumber())
        return false;
    return true;
}

QList<SpeechSentence> splitOverlongSentence(const SpeechSentence &rawSentence, int longest) {
    QList<SpeechSentence> pieces;
    if (rawSentence.text.size() <= longest) {
        pieces.append(rawSentence);
        return pieces;
    }
    // Subdivide overlong sentences (e.g. unpunctuated text or run-on
    // clauses) at clause punctuation or whitespace so TTS limits hold.
    const QString &sText = rawSentence.text;
    const qsizetype sLen = sText.size();
    const qint64 span = rawSentence.end - rawSentence.start;
    qsizetype pos = 0;
    while (pos < sLen) {
        qsizetype take = qMin<qsizetype>(longest, sLen - pos);
        if (pos + take < sLen) {
            qsizetype cut = -1;
            for (qsizetype j = pos + take; j > pos + longest / 2; --j) {
                const QChar ch = sText[j - 1];
                if (ch == ';' || ch == ',' || ch == QChar(0x2014) || ch == QChar(0xFF0C) ||
                    ch == QChar(0x3001) || ch == QChar(0x061B)) {
                    cut = j;
                    break;
                }
            }
            if (cut < 0)
                for (qsizetype j = pos + take; j > pos + longest / 2; --j)
                    if (sText[j - 1].isSpace()) {
                        cut = j;
                        break;
                    }
            if (cut > pos)
                take = cut - pos;
        }
        const QString chunk = sText.mid(pos, take).trimmed();
        if (!chunk.isEmpty()) {
            const qint64 pieceStart = rawSentence.start + (sLen > 0 ? span * pos / sLen : 0);
            const qint64 pieceEnd = rawSentence.start + (sLen > 0 ? span * (pos + take) / sLen : span);
            pieces.append({pieceStart, qMax(pieceStart, pieceEnd), chunk});
        }
        pos += take;
        while (pos < sLen && sText[pos].isSpace())
            ++pos;
    }
    return pieces;
}
}

namespace Speech {
QList<SpeechBlock> parseBlocks(const QString &json) {
    QList<SpeechBlock> blocks;
    for (const auto &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QJsonObject object = value.toObject();
        SpeechBlock block;
        block.start = object.value("start").toInteger();
        block.end = object.value("end").toInteger();
        block.text = object.value("text").toString();
        block.kind = object.value("kind").toString(QStringLiteral("text"));
        for (const auto &skip : object.value("skips").toArray())
            block.skips.append({skip.toArray().at(0).toInteger(), skip.toArray().at(1).toInteger()});
        blocks.append(block);
    }
    return blocks;
}

QList<SpeechSentence> sentences(const SpeechBlock &block) {
    QList<SpeechSentence> result;
    const QString &text = block.text;
    const qsizetype length = text.size();
    auto add = [&](qsizetype from, qsizetype to) {
        while (from < to && text[from].isSpace())
            ++from;
        while (to > from && text[to - 1].isSpace())
            --to;
        if (from >= to)
            return;
        const QString said = spoken(block, from, to);
        if (said.isEmpty())
            return;
        result.append({block.start + from, block.start + to, said});
    };
    // A note marker after the full stop ("Paris.¹²") is passed over like a
    // closing quote.
    auto skipped = [&block](qsizetype i) {
        for (const auto &[start, end] : block.skips)
            if (block.start + i >= start && block.start + i < end)
                return true;
        return false;
    };
    qsizetype start = 0;
    for (qsizetype i = 0; i < length; ++i) {
        const QChar c = text[i];
        if (!sentenceTerminator(c))
            continue;
        const bool cjk = cjkTerminator(c);
        qsizetype after = i + 1;
        while (after < length && (closing(text[after]) || sentenceTerminator(text[after]) || skipped(after)))
            ++after;
        if (after >= length)
            break;
        if (cjk) {
            qsizetype next = after;
            while (next < length && text[next].isSpace())
                ++next;
            add(start, after);
            start = next;
            i = next - 1;
            continue;
        }
        if (!text[after].isSpace())
            continue;
        if (c == '.') {
            qsizetype wordStart = i;
            while (wordStart > 0 && (text[wordStart - 1].isLetter() || text[wordStart - 1] == '.'))
                --wordStart;
            const QString word = text.mid(wordStart, i - wordStart);
            // "H. G. Wells": a lone capital is an initial; and "Mr." is a title.
            if ((word.size() == 1 && word[0].isUpper()) || abbreviation(word))
                continue;
        }
        qsizetype next = after;
        while (next < length && text[next].isSpace())
            ++next;
        if (next < length &&
            !(text[next].isUpper() || text[next].isDigit() || opening(text[next]) || caselessLetter(text[next])))
            continue;
        add(start, after);
        start = next;
        i = next - 1;
    }
    add(start, length);
    return result;
}

QList<DialogueSpan> splitDialogueSpans(const SpeechBlock &block) {
    struct RawSpan {
        qsizetype start = 0;
        qsizetype end = 0;
        bool dialogue = false;
    };
    const QString &text = block.text;
    const qsizetype len = text.size();
    QList<RawSpan> raw;

    // 1. Scan balanced quote spans (also supporting unclosed multi-paragraph speech).
    qsizetype pos = 0;
    while (pos < len) {
        qsizetype qStart = -1;
        for (qsizetype i = pos; i < len; ++i) {
            if (isQuoteOpen(text, i)) {
                qStart = i;
                break;
            }
        }
        if (qStart < 0) {
            if (pos < len)
                raw.append({pos, len, false});
            break;
        }
        if (qStart > pos)
            raw.append({pos, qStart, false});
        const QChar open = text[qStart];
        qsizetype qEnd = -1;
        for (qsizetype i = qStart + 1; i < len; ++i) {
            if (isValidClose(text, i, open)) {
                qEnd = i + 1;
                break;
            }
        }
        if (qEnd < 0) {
            // Unclosed opening quote (classic multi-paragraph speech): the rest
            // of the paragraph is dialogue.
            raw.append({qStart, len, true});
            break;
        }
        raw.append({qStart, qEnd, true});
        pos = qEnd;
    }

    // 2. If no quote pairs were found, check continental em-dash dialogue at
    // paragraph start: "— Watch out! — Harry shouted."
    bool foundQuote = false;
    for (const RawSpan &s : raw)
        if (s.dialogue) {
            foundQuote = true;
            break;
        }
    if (!foundQuote) {
        qsizetype i = 0;
        while (i < len && text[i].isSpace())
            ++i;
        if (i < len && (text[i] == QChar(0x2014) || text[i] == QChar(0x2013) || text[i] == QChar(0x2015))) {
            raw.clear();
            bool inDialogue = true;
            qsizetype segStart = i;
            for (qsizetype j = i + 1; j < len; ++j) {
                const QChar ch = text[j];
                if ((ch == QChar(0x2014) || ch == QChar(0x2013) || ch == QChar(0x2015)) && j > segStart + 1 &&
                    (text[j - 1].isSpace() || (j + 1 < len && text[j + 1].isSpace()))) {
                    raw.append({segStart, j, inDialogue});
                    inDialogue = !inDialogue;
                    segStart = j + 1;
                }
            }
            if (segStart < len)
                raw.append({segStart, len, inDialogue});
        }
    }

    // 3. Merge adjacent spans of the same role, or dialogue spans separated
    // only by whitespace/punctuation (no spoken words between them).
    QList<RawSpan> merged;
    for (const RawSpan &span : std::as_const(raw)) {
        const QString slice = text.mid(span.start, span.end - span.start);
        if (!hasLetterOrDigit(slice)) {
            if (!merged.isEmpty())
                merged.last().end = span.end;
            continue;
        }
        if (!merged.isEmpty() && merged.last().dialogue == span.dialogue)
            merged.last().end = span.end;
        else
            merged.append(span);
    }

    // 4. Turn each span into SpeechSentences with exact original offsets and skips.
    QList<DialogueSpan> out;
    for (const RawSpan &span : std::as_const(merged)) {
        SpeechBlock sub;
        sub.start = block.start + span.start;
        sub.end = block.start + span.end;
        sub.text = text.mid(span.start, span.end - span.start);
        sub.skips = block.skips;
        sub.kind = block.kind;
        const QList<SpeechSentence> subSentences = sentences(sub);
        if (!subSentences.isEmpty())
            out.append({span.dialogue, subSentences});
    }
    return out;
}

QString spokenHeading(const QString &heading) {
    QString text = heading.simplified();
    // A numeral on its own ("V", "III"), after a structural heading word
    // ("CHAPTER V", "Part iv", "Book II"), or before the title ("IV. The
    // Golden Age"). Ordinary words and names ("When I", "Henry V", "Appendix C")
    // are left untouched; numbers past 200 are more likely years/names.
    static const QRegularExpression numbered(
        QStringLiteral("^((?:\\p{L}+\\s+)?)([IVXLC]+|[ivxlc]+)(?=\\s*(?:$|[.:\\x{2013}\\x{2014}-]))"));
    static const QRegularExpression roman(QStringLiteral("^(C{0,2})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$"));
    const QRegularExpressionMatch match = numbered.match(text);
    if (match.hasMatch()) {
        const QString prefix = match.captured(1).trimmed();
        const QString numeral = match.captured(2).toUpper();
        const bool allowedPrefix = prefix.isEmpty() || isStructuralHeadingWord(prefix);
        if (allowedPrefix && roman.match(numeral).hasMatch() &&
            (numeral.size() > 1 || QStringLiteral("IVX").contains(numeral))) {
            static const QList<QPair<QString, int>> values{{"C", 100}, {"XC", 90}, {"L", 50}, {"XL", 40}, {"X", 10},
                                                         {"IX", 9},   {"V", 5},   {"IV", 4}, {"I", 1}};
            int value = 0;
            qsizetype at = 0;
            for (const auto &[symbol, worth] : values)
                while (numeral.mid(at, symbol.size()) == symbol) {
                    value += worth;
                    at += symbol.size();
                }
            if (value > 0 && value <= 200)
                text.replace(match.capturedStart(2), match.capturedLength(2), QString::number(value));
        }
    }
    // All capitals: said in ordinary case, each word capitalized.
    bool letters = false, lower = false;
    for (const QChar c : std::as_const(text)) {
        letters = letters || c.isLetter();
        lower = lower || c.isLower();
    }
    if (letters && !lower) {
        QString cased = text.toLower();
        bool start = true;
        for (QChar &c : cased) {
            if (c.isLetterOrNumber()) {
                if (start && c.isLetter())
                    c = c.toUpper();
                start = false;
            } else if (c.isSpace() || c == '-' || c == QChar(0x2013) || c == QChar(0x2014)) {
                start = true;
            }
        }
        text = cased;
    }
    qsizetype last = text.size() - 1;
    while (last >= 0 && closing(text[last]))
        --last;
    if (last >= 0 && !QStringLiteral(".!?:;\u2026").contains(text[last]))
        text.insert(last + 1, '.');
    return text;
}

QStringList moods() {
    return {"neutral", "hushed", "tense", "urgent", "somber", "tender", "light", "letter", "verse"};
}

QString styleFor(const QString &mood, bool dramatic) {
    // Short and constant, as the speech model's guidance asks: long or
    // changing directions are what make a voice drift.
    static const QHash<QString, QPair<QString, QString>> styles{
        {"hushed", {"Hushed and low.", "Hushed, almost a whisper."}},
        {"tense", {"Quietly tense.", "Tense, with mounting dread."}},
        {"urgent", {"With some urgency.", "Urgent and breathless."}},
        {"somber", {"Somber.", "Grave and sorrowful."}},
        {"tender", {"Tender.", "Warm and tender."}},
        {"light", {"Lightly amused.", "Playful and amused."}},
        {"letter", {"As if reading a letter aloud.", "As if reading a letter aloud."}},
        {"verse", {"As verse, with its rhythm.", "As verse, with its rhythm."}},
    };
    const auto style = styles.value(mood);
    return dramatic ? style.second : style.first;
}

QString moodAt(const QMap<int, QString> &changes, int block) {
    auto at = changes.upperBound(block);
    return at == changes.begin() ? QString() : std::prev(at).value();
}

QList<SpeechClip> clips(int chapter, const QList<SpeechBlock> &blocks, const QSet<int> &breaks, int longest,
                        int merge, const QMap<int, QString> &blockVoices, const QString &narratorVoice) {
    auto empty = [chapter](int block) {
        SpeechClip clip;
        clip.chapter = chapter;
        clip.block = block;
        return clip;
    };
    QList<SpeechClip> result;
    SpeechClip current = empty(-1);
    qsizetype currentLength = 0;
    bool currentHeading = false;
    auto flush = [&] {
        if (!current.sentences.isEmpty()) {
            QStringList parts;
            for (const auto &sentence : std::as_const(current.sentences))
                parts.append(sentence.text);
            current.text = parts.join(' ');
            result.append(current);
        }
        current = empty(-1);
        currentLength = 0;
        currentHeading = false;
    };
    auto appendSentences = [&](int b, const QList<SpeechSentence> &list, const QString &voice) {
        for (const auto &rawSentence : list) {
            const QList<SpeechSentence> pieces = splitOverlongSentence(rawSentence, longest);
            for (const auto &sentence : pieces) {
                // (Sentences are joined with a space.)
                if (!current.sentences.isEmpty() && currentLength + 1 + sentence.text.size() > longest)
                    flush();
                if (current.sentences.isEmpty()) {
                    current.block = b;
                    current.voice = voice;
                }
                currentLength += sentence.text.size() + (current.sentences.isEmpty() ? 0 : 1);
                current.sentences.append(sentence);
            }
        }
    };
    for (int b = 0; b < blocks.size(); ++b) {
        const SpeechBlock &block = blocks[b];
        if (block.kind == QStringLiteral("break")) {
            flush();
            SpeechClip pause = empty(b);
            pause.pauseMs = 1300;
            result.append(pause);
            continue;
        }
        // Consecutive headings (a chapter's number, then its title) are one
        // clip of their own.
        if (block.kind == QStringLiteral("heading")) {
            const QList<SpeechSentence> said = sentences(block);
            if (said.isEmpty())
                continue;
            if (!currentHeading)
                flush();
            if (current.sentences.isEmpty()) {
                current.block = b;
                current.voice = narratorVoice;
            }
            currentHeading = true;
            for (SpeechSentence sentence : std::as_const(said)) {
                sentence.text = spokenHeading(sentence.text);
                current.sentences.append(sentence);
                currentLength += sentence.text.size();
            }
            continue;
        }

        // Inline dialogue-span splitting when full cast assigns a character
        // voice to a paragraph that mixes quoted speech and narration tags.
        const QString charVoice = blockVoices.value(b);
        if (!charVoice.isEmpty() && charVoice != narratorVoice) {
            const QList<DialogueSpan> spans = splitDialogueSpans(block);
            bool hasDialogue = false, hasNarration = false;
            for (const DialogueSpan &span : spans) {
                hasDialogue = hasDialogue || span.dialogue;
                hasNarration = hasNarration || !span.dialogue;
            }
            if (hasDialogue && hasNarration) {
                flush();
                for (const DialogueSpan &span : spans) {
                    const QString spanVoice = span.dialogue ? charVoice : narratorVoice;
                    if (!current.sentences.isEmpty() && current.voice != spanVoice)
                        flush();
                    appendSentences(b, span.sentences, spanVoice);
                    flush();
                }
                continue;
            }
        }

        const QList<SpeechSentence> said = sentences(block);
        qsizetype blockLength = 0;
        for (const auto &sentence : said)
            blockLength += sentence.text.size();
        if (said.isEmpty())
            continue;
        const QString blockVoice = !charVoice.isEmpty() ? charVoice : narratorVoice;
        // A short paragraph (a line of dialogue) shares the clip before it
        // when both are short and share a voice; any other paragraph starts a
        // clip of its own.
        const bool sameVoice = current.voice == blockVoice;
        const bool joins = !current.sentences.isEmpty() && !currentHeading && sameVoice && !breaks.contains(b) &&
                           currentLength + blockLength <= merge && blockLength < merge / 2;
        if (!joins)
            flush();
        appendSentences(b, said, blockVoice);
    }
    flush();
    return result;
}

QList<SpeechClip> clipsFrom(int chapter, const QList<SpeechBlock> &blocks, qint64 from, const QSet<int> &breaks,
                            const QMap<int, QString> &blockVoices, const QString &narratorVoice) {
    const QList<SpeechClip> all = clips(chapter, blocks, breaks, 1500, 600, blockVoices, narratorVoice);
    // Where a clip ends: its last sentence, or for a pause its break.
    auto ends = [&blocks](const SpeechClip &clip) {
        return clip.sentences.isEmpty() ? blocks.value(clip.block).end : clip.end();
    };
    qsizetype first = 0;
    while (first < all.size() && ends(all[first]) <= from)
        ++first;
    QList<SpeechClip> result = all.mid(first);
    if (!result.isEmpty() && !result.first().sentences.isEmpty() && result.first().start() < from) {
        // A lead-in: the rest of the clip `from` falls in, from its sentence.
        SpeechClip &lead = result.first();
        lead.sentences.removeIf([from](const SpeechSentence &sentence) { return sentence.end <= from; });
        QStringList parts;
        for (const auto &sentence : std::as_const(lead.sentences))
            parts.append(sentence.text);
        lead.text = parts.join(' ');
        // Its first block, for the director, is the one it now begins in.
        if (!lead.sentences.isEmpty()) {
            for (int b = lead.block; b < blocks.size(); ++b)
                if (blocks[b].end > lead.sentences.first().start) {
                    lead.block = b;
                    break;
                }
        }
    }
    return result;
}

QList<QPair<double, double>> timeline(const SpeechClip &clip) {
    QList<double> weights;
    double total = 0;
    static const QRegularExpression pauses(QStringLiteral("[,;:—]"));
    for (const auto &sentence : clip.sentences) {
        // A sentence takes about as long as its letters, plus a beat for
        // each pause its punctuation asks for.
        const double weight = sentence.text.size() + 6.0 * sentence.text.count(pauses) + 10.0;
        weights.append(weight);
        total += weight;
    }
    QList<QPair<double, double>> spans;
    double at = 0;
    for (const double weight : std::as_const(weights)) {
        spans.append({at / total, (at + weight) / total});
        at += weight;
    }
    return spans;
}

QByteArray wav(const QByteArray &pcm, int sampleRate) {
    QByteArray out;
    out.reserve(44 + pcm.size());
    auto put32 = [&out](quint32 v) {
        const quint32 le = qToLittleEndian(v);
        out.append(reinterpret_cast<const char *>(&le), 4);
    };
    auto put16 = [&out](quint16 v) {
        const quint16 le = qToLittleEndian(v);
        out.append(reinterpret_cast<const char *>(&le), 2);
    };
    out.append("RIFF");
    put32(quint32(36 + pcm.size()));
    out.append("WAVEfmt ");
    put32(16);
    put16(1);
    put16(1);
    put32(quint32(sampleRate));
    put32(quint32(sampleRate * 2));
    put16(2);
    put16(16);
    out.append("data");
    put32(quint32(pcm.size()));
    out.append(pcm);
    return out;
}

QVector<float> resample(const QVector<float> &samples, int from, int to) {
    if (from == to || from <= 0 || to <= 0 || samples.isEmpty())
        return samples;
    const qsizetype count = qsizetype(double(samples.size()) * to / from);
    if (count <= 0)
        return {};
    QVector<float> out(count);
    const double step = double(from) / double(to);
    auto at = [&samples](qsizetype i) { return samples[qBound<qsizetype>(0, i, samples.size() - 1)]; };

    if (from > to) {
        // Downsampling: Blackman-windowed sinc low-pass anti-aliasing filter
        // (cutoff = 0.45 * to / from cycles per input sample, 32 taps at 2:1)
        // evaluated with exact double-precision angle-addition recurrences and
        // unity DC gain normalization.
        constexpr double kPi = 3.14159265358979323846;
        const int halfWidth = std::clamp(int(std::ceil(8.0 * step)), 8, 32);
        const double winRadius = double(halfWidth);
        const double cutoff = 0.45 * double(to) / double(from);
        const double omega = 2.0 * kPi * cutoff;
        const double cosOmega = std::cos(omega);
        const double sinOmega = std::sin(omega);
        const double winStep = kPi / winRadius;
        const double cosWinStep = std::cos(winStep);
        const double sinWinStep = std::sin(winStep);

        for (qsizetype n = 0; n < count; ++n) {
            const double position = n * step;
            const qsizetype center = qsizetype(std::floor(position));
            const qsizetype kStart = center - halfWidth + 1;
            const qsizetype kEnd = center + halfWidth;

            const double d0 = double(kStart) - position;
            double alpha = omega * d0;
            double sinA = std::sin(alpha);
            double cosA = std::cos(alpha);
            const double beta0 = winStep * d0;
            double sinB = std::sin(beta0);
            double cosB = std::cos(beta0);

            double sum = 0.0;
            double weightSum = 0.0;
            for (qsizetype k = kStart; k <= kEnd; ++k) {
                const double sinc = (std::abs(alpha) < 1e-7) ? 1.0 : (sinA / alpha);
                const double cos2B = 2.0 * cosB * cosB - 1.0;
                const double win = 0.42 + 0.50 * cosB + 0.08 * cos2B;
                const double w = sinc * win;
                sum += w * double(at(k));
                weightSum += w;

                alpha += omega;
                const double nextSinA = sinA * cosOmega + cosA * sinOmega;
                const double nextCosA = cosA * cosOmega - sinA * sinOmega;
                sinA = nextSinA;
                cosA = nextCosA;

                const double nextSinB = sinB * cosWinStep + cosB * sinWinStep;
                const double nextCosB = cosB * cosWinStep - sinB * sinWinStep;
                sinB = nextSinB;
                cosB = nextCosB;
            }
            out[n] = (weightSum > 0.0) ? float(sum / weightSum) : 0.0f;
        }
        return out;
    }

    for (qsizetype n = 0; n < count; ++n) {
        const double position = n * step;
        const qsizetype i = qsizetype(position);
        const float t = float(position - double(i));
        // Catmull-Rom through the four samples around the point.
        const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        out[n] = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    }
    return out;
}

QByteArray pcm16(const QVector<float> &samples) {
    QByteArray out(samples.size() * 2, Qt::Uninitialized);
    auto *to = reinterpret_cast<qint16 *>(out.data());
    for (qsizetype n = 0; n < samples.size(); ++n)
        to[n] = qToLittleEndian(qint16(std::clamp(std::lround(samples[n] * 32767.0f), -32768L, 32767L)));
    return out;
}
}

QString Speech::speakable(const QString &text) {
    QString out = text;
    out.remove(QChar(0x00AD));
    out.remove(QChar(0x200B));
    out.remove(QChar(0xFFFE));
    return out;
}

bool Speech::atomicReplace(const QString &source, const QString &target) {
    const QByteArray src = QFile::encodeName(source);
    const QByteArray dst = QFile::encodeName(target);
    if (std::rename(src.constData(), dst.constData()) == 0)
        return true;
    QFile::remove(target);
    return QFile::rename(source, target);
}

QString Speech::classicGeminiTrait(const QString &voiceId) {
    static const QHash<QString, QString> traits{
        {QStringLiteral("sulafat"), QStringLiteral("Warm")},
        {QStringLiteral("charon"), QStringLiteral("Informative")},
        {QStringLiteral("kore"), QStringLiteral("Firm")},
        {QStringLiteral("puck"), QStringLiteral("Upbeat")},
        {QStringLiteral("aoede"), QStringLiteral("Breezy")},
        {QStringLiteral("fenrir"), QStringLiteral("Excitable")},
        {QStringLiteral("zephyr"), QStringLiteral("Bright")},
        {QStringLiteral("orus"), QStringLiteral("Firm")},
        {QStringLiteral("leda"), QStringLiteral("Youthful")},
        {QStringLiteral("autonoe"), QStringLiteral("Bright")},
        {QStringLiteral("enceladus"), QStringLiteral("Breathy")},
        {QStringLiteral("iapetus"), QStringLiteral("Clear")},
        {QStringLiteral("umbriel"), QStringLiteral("Easy-going")},
        {QStringLiteral("algieba"), QStringLiteral("Smooth")},
        {QStringLiteral("despina"), QStringLiteral("Smooth")},
        {QStringLiteral("erinome"), QStringLiteral("Clear")},
        {QStringLiteral("algenib"), QStringLiteral("Gravelly")},
        {QStringLiteral("rasalgethi"), QStringLiteral("Informative")},
        {QStringLiteral("laomedeia"), QStringLiteral("Upbeat")},
        {QStringLiteral("achernar"), QStringLiteral("Soft")},
        {QStringLiteral("alnilam"), QStringLiteral("Firm")},
        {QStringLiteral("schedar"), QStringLiteral("Even")},
        {QStringLiteral("gacrux"), QStringLiteral("Mature")},
        {QStringLiteral("pulcherrima"), QStringLiteral("Forward")},
        {QStringLiteral("achird"), QStringLiteral("Friendly")},
        {QStringLiteral("zubenelgenubi"), QStringLiteral("Casual")},
        {QStringLiteral("vindemiatrix"), QStringLiteral("Gentle")},
        {QStringLiteral("sadachbia"), QStringLiteral("Lively")},
        {QStringLiteral("sadaltager"), QStringLiteral("Knowledgeable")},
        {QStringLiteral("callirrhoe"), QStringLiteral("Easy-going")},
    };
    return traits.value(voiceId.toLower());
}

qint64 Speech::usToSamples(qint64 us, int sampleRate) {
    // Split into whole seconds + sub-second remainder after clamping so
    // extreme D-Bus Seek/SetPosition values can never overflow int64.
    constexpr qint64 maxUs = 100LL * 365 * 24 * 3600 * 1000000LL;
    const qint64 clamped = std::clamp(us, -maxUs, maxUs);
    const qint64 sec = clamped / 1000000LL;
    const qint64 rem = clamped % 1000000LL;
    return sec * sampleRate + (rem * sampleRate) / 1000000LL;
}

int Speech::sentenceForSample(const SpeechClip &clip, const QList<QPair<double, double>> &spans,
                              const QList<qint64> &boundaries, qint64 expectedSamples, qint64 sample) {
    if (clip.sentences.isEmpty())
        return -1;
    if (!boundaries.isEmpty() && boundaries.size() <= clip.sentences.size()) {
        int at = 0;
        while (at < boundaries.size() && sample >= boundaries[at])
            ++at;
        return qMin(at, int(clip.sentences.size()) - 1);
    }
    const double fraction = expectedSamples > 0 ? double(sample) / double(expectedSamples) : 0.0;
    int at = int(spans.size()) - 1;
    for (int i = 0; i < spans.size(); ++i)
        if (fraction < spans[i].second) {
            at = i;
            break;
        }
    return at;
}

qint64 Speech::sampleOfSentence(const SpeechClip &clip, const QList<QPair<double, double>> &spans,
                                const QList<qint64> &boundaries, qint64 expectedSamples, int sentence) {
    if (sentence <= 0 || clip.sentences.isEmpty())
        return 0;
    if (!boundaries.isEmpty() && sentence - 1 < boundaries.size())
        return qMax<qint64>(0, boundaries[sentence - 1]);
    if (sentence < spans.size() && expectedSamples > 0)
        return qBound<qint64>(0, qint64(std::ceil(spans[sentence].first * double(expectedSamples))),
                              expectedSamples - 1);
    return 0;
}

Speech::SeekTarget Speech::seekBySamples(const QList<TimelineEntry> &entries, int currentIndex, qint64 currentSample,
                                         qint64 deltaSamples, int sampleRate) {
    if (entries.isEmpty() || currentIndex < 0 || currentIndex >= entries.size())
        return {};
    int idx = currentIndex;
    qint64 pos = currentSample + deltaSamples;
    bool wantNext = false;

    if (deltaSamples > 0) {
        while (idx < entries.size()) {
            const qint64 len = qMax<qint64>(1, entries[idx].samples);
            if (pos < len)
                break;
            if (idx + 1 >= entries.size()) {
                wantNext = true;
                pos = qMax<qint64>(0, len - sampleRate);
                break;
            }
            pos -= len;
            ++idx;
        }
        while (idx + 1 < entries.size() && entries[idx].pauseMs > 0) {
            ++idx;
            pos = 0;
        }
    } else {
        while (pos < 0 && idx > 0) {
            --idx;
            pos += qMax<qint64>(1, entries[idx].samples);
        }
        if (pos < 0)
            pos = 0;
        if (entries[idx].pauseMs > 0) {
            if (idx > 0) {
                --idx;
                pos = qMax<qint64>(0, entries[idx].samples - sampleRate);
            } else if (idx + 1 < entries.size()) {
                ++idx;
                pos = 0;
            }
        }
    }
    return {idx, pos, wantNext};
}

Speech::SeekTarget Speech::seekToChapterUs(const QList<TimelineEntry> &entries, int currentIndex, qint64 targetUs,
                                           int sampleRate) {
    if (entries.isEmpty() || currentIndex < 0 || currentIndex >= entries.size())
        return {};
    const int at = entries[currentIndex].chapter;
    int firstInChapter = -1;
    int lastInChapter = -1;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].chapter == at) {
            if (firstInChapter < 0)
                firstInChapter = i;
            lastInChapter = i;
        }
    }
    if (firstInChapter < 0)
        return {};
    int idx = firstInChapter;
    qint64 pos = qMax<qint64>(0, usToSamples(targetUs, sampleRate));
    while (idx < lastInChapter) {
        const qint64 len = qMax<qint64>(1, entries[idx].samples);
        if (pos < len)
            break;
        pos -= len;
        ++idx;
    }
    const qint64 lastLen = qMax<qint64>(1, entries[idx].samples);
    pos = qBound<qint64>(0, pos, qMax<qint64>(0, lastLen - 1));
    if (entries[idx].pauseMs > 0 && idx + 1 <= lastInChapter) {
        ++idx;
        pos = 0;
    }
    return {idx, pos, false};
}

QPair<qint64, qint64> Speech::chapterTimingUs(const QList<TimelineEntry> &entries, int currentIndex,
                                              qint64 currentSample, int sampleRate) {
    if (entries.isEmpty() || currentIndex < 0 || currentIndex >= entries.size() || sampleRate <= 0)
        return {0, 0};
    const int at = entries[currentIndex].chapter;
    qint64 posSamples = 0;
    qint64 durSamples = 0;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].chapter != at)
            continue;
        const qint64 len = entries[i].samples;
        durSamples += len;
        if (i < currentIndex)
            posSamples += len;
        else if (i == currentIndex)
            posSamples += qMin(currentSample, len);
    }
    return {posSamples * 1000000LL / sampleRate, durSamples * 1000000LL / sampleRate};
}
