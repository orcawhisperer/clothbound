#include "speech.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace {
bool closing(QChar c) {
    static const QString marks = QStringLiteral("\"'’”»)]");
    return marks.contains(c);
}

bool opening(QChar c) {
    static const QString marks = QStringLiteral("\"'‘“«([");
    return marks.contains(c);
}

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
        if (c != '.' && c != '!' && c != '?' && c != QChar(0x2026))
            continue;
        qsizetype after = i + 1;
        while (after < length && (closing(text[after]) || text[after] == '.' || text[after] == '!' ||
                                  text[after] == '?' || skipped(after)))
            ++after;
        if (after >= length)
            break;
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
        if (next < length && !(text[next].isUpper() || text[next].isDigit() || opening(text[next])))
            continue;
        add(start, after);
        start = next;
        i = next - 1;
    }
    add(start, length);
    return result;
}

QString spokenHeading(const QString &heading) {
    QString text = heading.simplified();
    // A numeral on its own ("V"), after one word ("CHAPTER V", "Part iv"),
    // or before the title ("IV. The Golden Age"). Single letters other than
    // I, V and X, and numbers past 200, are more likely names than numbers.
    static const QRegularExpression numbered(
        QStringLiteral("^((?:\\p{L}+\\s+)?)([IVXLC]+|[ivxlc]+)(?=\\s*(?:$|[.:\\x{2013}\\x{2014}-]))"));
    static const QRegularExpression roman(QStringLiteral("^(C{0,2})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$"));
    const QRegularExpressionMatch match = numbered.match(text);
    if (match.hasMatch()) {
        const QString numeral = match.captured(2).toUpper();
        if (roman.match(numeral).hasMatch() && (numeral.size() > 1 || QStringLiteral("IVX").contains(numeral))) {
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
                        int merge) {
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
    for (int b = 0; b < blocks.size(); ++b) {
        const SpeechBlock &block = blocks[b];
        if (block.kind == QStringLiteral("break")) {
            flush();
            SpeechClip pause = empty(b);
            pause.pauseMs = 1300;
            result.append(pause);
            continue;
        }
        const QList<SpeechSentence> said = sentences(block);
        qsizetype blockLength = 0;
        for (const auto &sentence : said)
            blockLength += sentence.text.size();
        if (said.isEmpty())
            continue;
        // Consecutive headings (a chapter's number, then its title) are one
        // clip of their own.
        if (block.kind == QStringLiteral("heading")) {
            if (!currentHeading)
                flush();
            if (current.sentences.isEmpty())
                current.block = b;
            currentHeading = true;
            for (SpeechSentence sentence : std::as_const(said)) {
                sentence.text = spokenHeading(sentence.text);
                current.sentences.append(sentence);
                currentLength += sentence.text.size();
            }
            continue;
        }
        // A short paragraph (a line of dialogue) shares the clip before it
        // when both are short; any other paragraph starts a clip of its own.
        const bool joins = !current.sentences.isEmpty() && !currentHeading && !breaks.contains(b) &&
                           currentLength + blockLength <= merge && blockLength < merge / 2;
        if (!joins)
            flush();
        for (const auto &sentence : std::as_const(said)) {
            // (Sentences are joined with a space.)
            if (!current.sentences.isEmpty() && currentLength + 1 + sentence.text.size() > longest)
                flush();
            if (current.sentences.isEmpty())
                current.block = b;
            currentLength += sentence.text.size() + (current.sentences.isEmpty() ? 0 : 1);
            current.sentences.append(sentence);
        }
    }
    flush();
    return result;
}

QList<SpeechClip> clipsFrom(int chapter, const QList<SpeechBlock> &blocks, qint64 from, const QSet<int> &breaks) {
    const QList<SpeechClip> all = clips(chapter, blocks, breaks);
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
        for (int b = lead.block; b < blocks.size(); ++b)
            if (blocks[b].end > lead.sentences.first().start) {
                lead.block = b;
                break;
            }
    }
    return result;
}

QList<QPair<double, double>> timeline(const SpeechClip &clip) {
    QList<double> weights;
    double total = 0;
    for (const auto &sentence : clip.sentences) {
        // A sentence takes about as long as its letters, plus a beat for
        // each pause its punctuation asks for.
        const double weight = sentence.text.size() + 6.0 * sentence.text.count(QRegularExpression(QStringLiteral("[,;:—]"))) + 10.0;
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
    QVector<float> out(count);
    const double step = double(from) / to;
    auto at = [&samples](qsizetype i) { return samples[qBound<qsizetype>(0, i, samples.size() - 1)]; };
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
