#include "ai/companion.h"

#include "ai/aiservices.h"
#include "ai/aiusage.h"
#include "ai/localmodels.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "reader/reader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextDocument>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>

namespace {

// Gemini reads up to this much of the book so far (about 300,000 tokens);
// a model on this computer, a chapter's worth (further bounded by actual num_ctx).
constexpr qint64 cloudLimit = 1200000;
constexpr qint64 localLimit = 24000;
constexpr qint64 localWhosWhoLimit = 16000;

QSettings companionSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

QStringList parseSdcvJson(const QByteArray &json, const QString &word) {
    QStringList out;
    for (const QJsonValue &entry : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = entry.toObject();
        const QString def = object.value(QStringLiteral("definition")).toString().trimmed();
        if (def.isEmpty())
            continue;
        if (object.value(QStringLiteral("word")).toString().compare(word, Qt::CaseInsensitive) == 0 || out.isEmpty()) {
            const QString dict = object.value(QStringLiteral("dict")).toString().trimmed();
            out.append(dict.isEmpty() ? def : def + QStringLiteral("\n— ") + dict);
        }
        if (out.size() >= 2)
            break;
    }
    return out;
}

QString cleanDictLookupWord(const QString &raw) {
    QString out = raw.trimmed();
    out.replace(QChar(0x2019), QLatin1Char('\''));
    out.replace(QChar(0x2018), QLatin1Char('\''));
    static const QRegularExpression lead(QStringLiteral("^[^\\p{L}\\p{N}]+"));
    static const QRegularExpression trail(QStringLiteral("[^\\p{L}\\p{N}]+$"));
    out.remove(lead);
    out.remove(trail);
    return out;
}

QJsonObject extractJsonObject(const QString &raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return {};
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        return doc.object();
    static const QRegularExpression fence(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)\\s*```"),
                                          QRegularExpression::CaseInsensitiveOption);
    const auto match = fence.match(trimmed);
    if (match.hasMatch()) {
        doc = QJsonDocument::fromJson(match.captured(1).trimmed().toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    const qsizetype open = trimmed.indexOf(QLatin1Char('{'));
    const qsizetype close = trimmed.lastIndexOf(QLatin1Char('}'));
    if (open >= 0 && close > open) {
        doc = QJsonDocument::fromJson(trimmed.mid(open, close - open + 1).toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    return {};
}

const char *noSpoilers =
    "The reader is partway through the book. You see only what they have read so far. Never reveal, hint at, or "
    "guess what happens later, even if you know the book; if they ask about something not yet in the text, say "
    "it hasn't come up yet.";

QString instructions(const QString &task, bool spoilerFree = true, bool allowHeadings = false) {
    QString out = task;
    if (spoilerFree)
        out += QStringLiteral("\n\n") + QString::fromLatin1(noSpoilers);
    out += allowHeadings
               ? QStringLiteral("\nWrite in %1. Be concise, well-structured, and clear.").arg(Companion::readerLanguage())
               : QStringLiteral("\nWrite in %1. Plain prose, no headings; short paragraphs.").arg(Companion::readerLanguage());
    return out;
}

QString chapterDisplayLabel(const QList<Companion::Chapter> &chapters, int chapterIndex, const QString &format) {
    for (const Companion::Chapter &chapter : chapters)
        if (chapter.index == chapterIndex && !chapter.title.isEmpty())
            return chapter.title;
    return format == QLatin1String("pdf")
               ? QCoreApplication::translate("Companion", "Page %1").arg(chapterIndex + 1)
               : QCoreApplication::translate("Companion", "Chapter %1").arg(chapterIndex + 1);
}

QString passageText(const PassageIndex::Hit &hit, const QList<Companion::Chapter> &chapters, const QString &format) {
    return QStringLiteral("[%1]\n%2")
        .arg(chapterDisplayLabel(chapters, hit.passage.chapter, format), hit.passage.text);
}

QString makeExcerpt(const QString &text, qsizetype maxChars = 120) {
    QString clean = text.simplified();
    if (clean.size() <= maxChars)
        return clean;
    qsizetype cut = clean.lastIndexOf(QLatin1Char(' '), maxChars);
    if (cut < maxChars / 2)
        cut = maxChars;
    return clean.left(cut).trimmed() + QChar(0x2026);
}

QString makeChipQuote(const QString &text) {
    QString clean = text.simplified();
    static const QRegularExpression leadTrim(QStringLiteral("^[^\\p{L}\\p{N}]+"));
    clean.remove(leadTrim);
    if (clean.isEmpty())
        return {};
    const QStringList words = clean.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString out;
    int taken = 0;
    for (const QString &word : words) {
        if (taken > 0 && (taken >= 5 || out.size() + 1 + word.size() > 28))
            break;
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += word;
        ++taken;
    }
    static const QRegularExpression trailTrim(QStringLiteral("[,;:\\-—\"'“”‘’()\\[\\]]+$"));
    out.remove(trailTrim);
    return out.trimmed();
}

QList<Companion::Citation> citationsFromHits(const QList<PassageIndex::Hit> &hits,
                                             const QList<Companion::Chapter> &chapters, const QString &format,
                                             int maxCount = 4) {
    QList<Companion::Citation> out;
    for (const PassageIndex::Hit &hit : hits) {
        bool duplicate = false;
        for (const Companion::Citation &existing : out) {
            if (existing.chapter == hit.passage.chapter && qAbs(existing.offset - hit.passage.start) < 400) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        Companion::Citation cite;
        cite.chapter = hit.passage.chapter;
        cite.offset = hit.passage.start;
        const QString place = chapterDisplayLabel(chapters, hit.passage.chapter, format);
        const QString quote = makeChipQuote(hit.passage.text);
        cite.label = quote.isEmpty() ? place : QStringLiteral("%1 · “%2”").arg(place, quote);
        cite.excerpt = makeExcerpt(hit.passage.text);
        out.append(cite);
        if (out.size() >= maxCount)
            break;
    }
    return out;
}

QVariantList citationsToVariant(const QList<Companion::Citation> &citations) {
    QVariantList out;
    out.reserve(citations.size());
    for (const Companion::Citation &cite : citations) {
        QVariantMap map{{QStringLiteral("chapter"), cite.chapter},
                        {QStringLiteral("offset"), cite.offset},
                        {QStringLiteral("label"), cite.label},
                        {QStringLiteral("excerpt"), cite.excerpt}};
        if (cite.bookId > 0)
            map.insert(QStringLiteral("bookId"), cite.bookId);
        if (!cite.bookTitle.isEmpty())
            map.insert(QStringLiteral("bookTitle"), cite.bookTitle);
        out.append(map);
    }
    return out;
}

QJsonArray citationsToJson(const QList<Companion::Citation> &citations) {
    QJsonArray out;
    for (const Companion::Citation &cite : citations) {
        QJsonObject obj{{QStringLiteral("chapter"), cite.chapter},
                        {QStringLiteral("offset"), cite.offset},
                        {QStringLiteral("label"), cite.label},
                        {QStringLiteral("excerpt"), cite.excerpt}};
        if (cite.bookId > 0)
            obj.insert(QStringLiteral("bookId"), cite.bookId);
        if (!cite.bookTitle.isEmpty())
            obj.insert(QStringLiteral("bookTitle"), cite.bookTitle);
        out.append(obj);
    }
    return out;
}

QList<Companion::Citation> citationsFromJson(const QByteArray &json) {
    QList<Companion::Citation> out;
    for (const QJsonValue &val : QJsonDocument::fromJson(json).array()) {
        const QJsonObject obj = val.toObject();
        Companion::Citation cite;
        cite.bookId = obj.value(QStringLiteral("bookId")).toInt();
        cite.bookTitle = obj.value(QStringLiteral("bookTitle")).toString();
        cite.chapter = obj.value(QStringLiteral("chapter")).toInt();
        cite.offset = obj.value(QStringLiteral("offset")).toInteger();
        cite.label = obj.value(QStringLiteral("label")).toString();
        cite.excerpt = obj.value(QStringLiteral("excerpt")).toString();
        if (!cite.label.isEmpty() || !cite.excerpt.isEmpty())
            out.append(cite);
    }
    return out;
}

int parseRomanNumeral(const QString &roman) {
    const QString s = roman.trimmed().toUpper();
    if (s.isEmpty() || s.size() > 10)
        return 0;
    auto val = [](QChar c) -> int {
        switch (c.unicode()) {
        case 'I': return 1;
        case 'V': return 5;
        case 'X': return 10;
        case 'L': return 50;
        case 'C': return 100;
        default: return 0;
        }
    };
    int total = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const int v = val(s[i]);
        if (v == 0)
            return 0;
        const int next = (i + 1 < s.size()) ? val(s[i + 1]) : 0;
        total += (v < next) ? -v : v;
    }
    return total;
}

struct FirstSeen {
    int chapterIndex = -1;
    qint64 offset = 0;
    QString label;
};

FirstSeen resolveFirstSeen(const QList<Companion::Chapter> &chapters, const QString &name, const QString &also,
                           const QString &llmChapter, const QString &format, bool studyGlossary = false) {
    FirstSeen result;
    result.label = llmChapter.trimmed();
    if (chapters.isEmpty())
        return result;

    // 1. Search the actual chapters for the earliest mention across name, aliases,
    // surname (Story mode) or symbol / acronym / expansion (Study Glossary mode).
    QStringList exactProbes;
    QStringList probes;
    auto addProbe = [&](const QString &candidate, bool preferExact = false) {
        QString trimmed = candidate.trimmed();
        if (trimmed.startsWith(QLatin1Char('$')) && trimmed.endsWith(QLatin1Char('$')) && trimmed.size() >= 3)
            trimmed = trimmed.mid(1, trimmed.size() - 2).trimmed();
        const bool isSingleSymbol = studyGlossary && trimmed.size() == 1 &&
                                    (trimmed[0].unicode() > 127 || trimmed[0].isUpper());
        if (trimmed.size() < 2 && !isSingleSymbol)
            return;
        if (preferExact || isSingleSymbol ||
            (studyGlossary && trimmed.size() <= 5 && trimmed == trimmed.toUpper() && trimmed != trimmed.toLower())) {
            if (!exactProbes.contains(trimmed))
                exactProbes.append(trimmed);
        }
        if (!probes.contains(trimmed, Qt::CaseInsensitive))
            probes.append(trimmed);
    };
    addProbe(name, studyGlossary && name.trimmed().size() <= 4);
    for (const QString &alias : also.split(QRegularExpression(QStringLiteral("[,;/()]")), Qt::SkipEmptyParts))
        addProbe(alias);

    if (!studyGlossary) {
        static const QSet<QString> honorifics = {
            QStringLiteral("mr"),      QStringLiteral("mrs"),      QStringLiteral("miss"),    QStringLiteral("ms"),
            QStringLiteral("dr"),      QStringLiteral("sir"),      QStringLiteral("lord"),    QStringLiteral("lady"),
            QStringLiteral("captain"), QStringLiteral("doctor"),   QStringLiteral("professor"), QStringLiteral("master"),
            QStringLiteral("king"),    QStringLiteral("queen"),    QStringLiteral("prince"),  QStringLiteral("princess"),
            QStringLiteral("father"),  QStringLiteral("mother"),   QStringLiteral("uncle"),   QStringLiteral("aunt"),
            QStringLiteral("the"),     QStringLiteral("old"),      QStringLiteral("young"),
        };
        const QStringList nameParts = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (nameParts.size() >= 2) {
            const QString &last = nameParts.last();
            if (last.size() >= 3 && !honorifics.contains(last.toLower()))
                addProbe(last);
            const QString &first = nameParts.first();
            if (first.size() >= 3 && !honorifics.contains(first.toLower()))
                addProbe(first);
        }
    }

    auto makeProbeRegex = [](const QString &probe, bool caseSensitive) {
        const QChar firstCh = probe.front();
        const QChar lastCh = probe.back();
        const QString leftBoundary =
            (firstCh.isLetterOrNumber() && firstCh.unicode() < 0x2000) ? QStringLiteral("(?<!\\p{L})") : QString();
        const QString rightBoundary =
            (lastCh.isLetterOrNumber() && lastCh.unicode() < 0x2000) ? QStringLiteral("(?!\\p{L})") : QString();
        return QRegularExpression(leftBoundary + QRegularExpression::escape(probe) + rightBoundary,
                                  caseSensitive ? QRegularExpression::NoPatternOption
                                                : QRegularExpression::CaseInsensitiveOption);
    };

    auto searchRegexes = [&](const QList<QRegularExpression> &rxList) -> bool {
        if (rxList.isEmpty())
            return false;
        for (const Companion::Chapter &ch : chapters) {
            qint64 bestOffset = -1;
            for (const QRegularExpression &rx : rxList) {
                const auto match = rx.match(ch.text);
                if (match.hasMatch()) {
                    const qint64 pos = match.capturedStart();
                    if (bestOffset < 0 || pos < bestOffset)
                        bestOffset = pos;
                }
            }
            if (bestOffset >= 0) {
                result.chapterIndex = ch.index;
                result.offset = bestOffset;
                if (!ch.title.isEmpty())
                    result.label = ch.title;
                else if (result.label.isEmpty())
                    result.label = chapterDisplayLabel(chapters, ch.index, format);
                return true;
            }
        }
        return false;
    };

    if (!exactProbes.isEmpty()) {
        QList<QRegularExpression> exactRx;
        exactRx.reserve(exactProbes.size());
        for (const QString &probe : exactProbes)
            exactRx.append(makeProbeRegex(probe, true));
        if (searchRegexes(exactRx))
            return result;
    }

    QList<QRegularExpression> regexes;
    regexes.reserve(probes.size());
    for (const QString &probe : probes)
        regexes.append(makeProbeRegex(probe, false));
    if (searchRegexes(regexes))
        return result;

    // 2. Fallback: match the LLM's "chapter" label against chapter titles or numbers.
    const QString needle = llmChapter.trimmed();
    if (!needle.isEmpty()) {
        for (const Companion::Chapter &ch : chapters) {
            if (!ch.title.isEmpty() && (ch.title.compare(needle, Qt::CaseInsensitive) == 0 ||
                                        ch.title.contains(needle, Qt::CaseInsensitive) ||
                                        needle.contains(ch.title, Qt::CaseInsensitive))) {
                result.chapterIndex = ch.index;
                result.offset = 0;
                result.label = ch.title;
                return result;
            }
        }
        static const QRegularExpression pageRx(QStringLiteral("\\b(?:page|p\\.?)\\s*(\\d+)\\b"),
                                               QRegularExpression::CaseInsensitiveOption);
        const auto pageMatch = pageRx.match(needle);
        if (pageMatch.hasMatch()) {
            const int target = pageMatch.captured(1).toInt() - 1;
            for (const Companion::Chapter &ch : chapters) {
                if (ch.index == target) {
                    result.chapterIndex = ch.index;
                    result.offset = 0;
                    return result;
                }
            }
        }
        static const QRegularExpression numRx(QStringLiteral("\\b(?:chapter|ch\\.?)\\s*(\\d+|[IVXLC]+)\\b"),
                                              QRegularExpression::CaseInsensitiveOption);
        const auto numMatch = numRx.match(needle);
        if (numMatch.hasMatch()) {
            bool ok = false;
            int oneBased = numMatch.captured(1).toInt(&ok);
            if (!ok)
                oneBased = parseRomanNumeral(numMatch.captured(1));
            if (oneBased >= 1 && oneBased <= chapters.size()) {
                result.chapterIndex = chapters[oneBased - 1].index;
                result.offset = 0;
                return result;
            }
        }
    }
    return result;
}

// --- Offline LaTeX / Math Formatter ------------------------------------------

QString escapeHtmlText(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        switch (c.unicode()) {
        case '&': out += QStringLiteral("&amp;"); break;
        case '<': out += QStringLiteral("&lt;"); break;
        case '>': out += QStringLiteral("&gt;"); break;
        case '"': out += QStringLiteral("&quot;"); break;
        default: out += c; break;
        }
    }
    return out;
}

QString mapBlackboard(const QString &s) {
    static const QHash<QChar, QString> table = {
        {'R', QStringLiteral("\u211D")}, {'N', QStringLiteral("\u2115")}, {'Z', QStringLiteral("\u2124")},
        {'Q', QStringLiteral("\u211A")}, {'C', QStringLiteral("\u2102")}, {'E', QStringLiteral("\U0001D53C")},
        {'P', QStringLiteral("\u2119")}, {'F', QStringLiteral("\U0001D53D")}, {'H', QStringLiteral("\u210D")},
        {'I', QStringLiteral("\U0001D540")}, {'K', QStringLiteral("\U0001D542")}, {'S', QStringLiteral("\U0001D54A")},
        {'T', QStringLiteral("\U0001D54B")}, {'1', QStringLiteral("\U0001D7D9")},
    };
    QString out;
    for (QChar c : s)
        out += table.value(c, escapeHtmlText(QString(c)));
    return QStringLiteral("<span style=\"font-style:normal;\">") + out + QStringLiteral("</span>");
}

QString mapCalligraphic(const QString &s) {
    static const QHash<QChar, QString> table = {
        {'A', QStringLiteral("\U0001D49C")}, {'B', QStringLiteral("\u212C")},     {'C', QStringLiteral("\U0001D49E")},
        {'D', QStringLiteral("\U0001D49F")}, {'E', QStringLiteral("\u2130")},     {'F', QStringLiteral("\u2131")},
        {'G', QStringLiteral("\U0001D4A2")}, {'H', QStringLiteral("\u210B")},     {'I', QStringLiteral("\u2110")},
        {'J', QStringLiteral("\U0001D4A5")}, {'K', QStringLiteral("\U0001D4A6")}, {'L', QStringLiteral("\u2112")},
        {'M', QStringLiteral("\u2133")},     {'N', QStringLiteral("\U0001D4A9")}, {'O', QStringLiteral("\U0001D4AA")},
        {'P', QStringLiteral("\U0001D4AB")}, {'Q', QStringLiteral("\U0001D4AC")}, {'R', QStringLiteral("\u211B")},
        {'S', QStringLiteral("\U0001D4AE")}, {'T', QStringLiteral("\U0001D4AF")}, {'U', QStringLiteral("\U0001D4B0")},
        {'V', QStringLiteral("\U0001D4B1")}, {'W', QStringLiteral("\U0001D4B2")}, {'X', QStringLiteral("\U0001D4B3")},
        {'Y', QStringLiteral("\U0001D4B4")}, {'Z', QStringLiteral("\U0001D4B5")},
    };
    QString out;
    for (QChar c : s)
        out += table.value(c, escapeHtmlText(QString(c)));
    return QStringLiteral("<span style=\"font-style:normal;\">") + out + QStringLiteral("</span>");
}

const QHash<QString, QString> &latexSymbols() {
    static const QHash<QString, QString> syms = {
        // Greek lowercase
        {QStringLiteral("alpha"), QStringLiteral("\u03B1")},
        {QStringLiteral("beta"), QStringLiteral("\u03B2")},
        {QStringLiteral("gamma"), QStringLiteral("\u03B3")},
        {QStringLiteral("delta"), QStringLiteral("\u03B4")},
        {QStringLiteral("epsilon"), QStringLiteral("\u03F5")},
        {QStringLiteral("varepsilon"), QStringLiteral("\u03B5")},
        {QStringLiteral("zeta"), QStringLiteral("\u03B6")},
        {QStringLiteral("eta"), QStringLiteral("\u03B7")},
        {QStringLiteral("theta"), QStringLiteral("\u03B8")},
        {QStringLiteral("vartheta"), QStringLiteral("\u03D1")},
        {QStringLiteral("iota"), QStringLiteral("\u03B9")},
        {QStringLiteral("kappa"), QStringLiteral("\u03BA")},
        {QStringLiteral("varkappa"), QStringLiteral("\u03F0")},
        {QStringLiteral("lambda"), QStringLiteral("\u03BB")},
        {QStringLiteral("mu"), QStringLiteral("\u03BC")},
        {QStringLiteral("nu"), QStringLiteral("\u03BD")},
        {QStringLiteral("xi"), QStringLiteral("\u03BE")},
        {QStringLiteral("omicron"), QStringLiteral("\u03BF")},
        {QStringLiteral("pi"), QStringLiteral("\u03C0")},
        {QStringLiteral("varpi"), QStringLiteral("\u03D6")},
        {QStringLiteral("rho"), QStringLiteral("\u03C1")},
        {QStringLiteral("varrho"), QStringLiteral("\u03F1")},
        {QStringLiteral("sigma"), QStringLiteral("\u03C3")},
        {QStringLiteral("varsigma"), QStringLiteral("\u03C2")},
        {QStringLiteral("tau"), QStringLiteral("\u03C4")},
        {QStringLiteral("upsilon"), QStringLiteral("\u03C5")},
        {QStringLiteral("phi"), QStringLiteral("\u03D5")},
        {QStringLiteral("varphi"), QStringLiteral("\u03C6")},
        {QStringLiteral("chi"), QStringLiteral("\u03C7")},
        {QStringLiteral("psi"), QStringLiteral("\u03C8")},
        {QStringLiteral("omega"), QStringLiteral("\u03C9")},
        // Greek uppercase
        {QStringLiteral("Alpha"), QStringLiteral("\u0391")},
        {QStringLiteral("Beta"), QStringLiteral("\u0392")},
        {QStringLiteral("Gamma"), QStringLiteral("\u0393")},
        {QStringLiteral("Delta"), QStringLiteral("\u0394")},
        {QStringLiteral("Epsilon"), QStringLiteral("\u0395")},
        {QStringLiteral("Zeta"), QStringLiteral("\u0396")},
        {QStringLiteral("Eta"), QStringLiteral("\u0397")},
        {QStringLiteral("Theta"), QStringLiteral("\u0398")},
        {QStringLiteral("Iota"), QStringLiteral("\u0399")},
        {QStringLiteral("Kappa"), QStringLiteral("\u039A")},
        {QStringLiteral("Lambda"), QStringLiteral("\u039B")},
        {QStringLiteral("Mu"), QStringLiteral("\u039C")},
        {QStringLiteral("Nu"), QStringLiteral("\u039D")},
        {QStringLiteral("Xi"), QStringLiteral("\u039E")},
        {QStringLiteral("Omicron"), QStringLiteral("\u039F")},
        {QStringLiteral("Pi"), QStringLiteral("\u03A0")},
        {QStringLiteral("Rho"), QStringLiteral("\u03A1")},
        {QStringLiteral("Sigma"), QStringLiteral("\u03A3")},
        {QStringLiteral("Tau"), QStringLiteral("\u03A4")},
        {QStringLiteral("Upsilon"), QStringLiteral("\u03A5")},
        {QStringLiteral("Phi"), QStringLiteral("\u03A6")},
        {QStringLiteral("Chi"), QStringLiteral("\u03A7")},
        {QStringLiteral("Psi"), QStringLiteral("\u03A8")},
        {QStringLiteral("Omega"), QStringLiteral("\u03A9")},
        // Big operators & calculus
        {QStringLiteral("sum"), QStringLiteral("\u2211")},
        {QStringLiteral("prod"), QStringLiteral("\u220F")},
        {QStringLiteral("coprod"), QStringLiteral("\u2210")},
        {QStringLiteral("int"), QStringLiteral("\u222B")},
        {QStringLiteral("iint"), QStringLiteral("\u222C")},
        {QStringLiteral("iiint"), QStringLiteral("\u222D")},
        {QStringLiteral("oint"), QStringLiteral("\u222E")},
        {QStringLiteral("nabla"), QStringLiteral("\u2207")},
        {QStringLiteral("partial"), QStringLiteral("\u2202")},
        {QStringLiteral("infty"), QStringLiteral("\u221E")},
        // Relations & logic
        {QStringLiteral("approx"), QStringLiteral("\u2248")},
        {QStringLiteral("sim"), QStringLiteral("\u223C")},
        {QStringLiteral("simeq"), QStringLiteral("\u2243")},
        {QStringLiteral("cong"), QStringLiteral("\u2245")},
        {QStringLiteral("equiv"), QStringLiteral("\u2261")},
        {QStringLiteral("propto"), QStringLiteral("\u221D")},
        {QStringLiteral("le"), QStringLiteral("\u2264")},
        {QStringLiteral("leq"), QStringLiteral("\u2264")},
        {QStringLiteral("ge"), QStringLiteral("\u2265")},
        {QStringLiteral("geq"), QStringLiteral("\u2265")},
        {QStringLiteral("ll"), QStringLiteral("\u226A")},
        {QStringLiteral("gg"), QStringLiteral("\u226B")},
        {QStringLiteral("ne"), QStringLiteral("\u2260")},
        {QStringLiteral("neq"), QStringLiteral("\u2260")},
        {QStringLiteral("in"), QStringLiteral("\u2208")},
        {QStringLiteral("notin"), QStringLiteral("\u2209")},
        {QStringLiteral("ni"), QStringLiteral("\u220B")},
        {QStringLiteral("subset"), QStringLiteral("\u2282")},
        {QStringLiteral("subseteq"), QStringLiteral("\u2286")},
        {QStringLiteral("supset"), QStringLiteral("\u2283")},
        {QStringLiteral("supseteq"), QStringLiteral("\u2287")},
        {QStringLiteral("cup"), QStringLiteral("\u222A")},
        {QStringLiteral("cap"), QStringLiteral("\u2229")},
        {QStringLiteral("setminus"), QStringLiteral("\u2216")},
        {QStringLiteral("emptyset"), QStringLiteral("\u2205")},
        {QStringLiteral("varnothing"), QStringLiteral("\u2205")},
        {QStringLiteral("forall"), QStringLiteral("\u2200")},
        {QStringLiteral("exists"), QStringLiteral("\u2203")},
        {QStringLiteral("nexists"), QStringLiteral("\u2204")},
        {QStringLiteral("neg"), QStringLiteral("\u00AC")},
        {QStringLiteral("lnot"), QStringLiteral("\u00AC")},
        {QStringLiteral("land"), QStringLiteral("\u2227")},
        {QStringLiteral("wedge"), QStringLiteral("\u2227")},
        {QStringLiteral("lor"), QStringLiteral("\u2228")},
        {QStringLiteral("vee"), QStringLiteral("\u2228")},
        // Arrows
        {QStringLiteral("to"), QStringLiteral("\u2192")},
        {QStringLiteral("rightarrow"), QStringLiteral("\u2192")},
        {QStringLiteral("longrightarrow"), QStringLiteral("\u27F6")},
        {QStringLiteral("leftarrow"), QStringLiteral("\u2190")},
        {QStringLiteral("gets"), QStringLiteral("\u2190")},
        {QStringLiteral("leftrightarrow"), QStringLiteral("\u2194")},
        {QStringLiteral("Rightarrow"), QStringLiteral("\u21D2")},
        {QStringLiteral("implies"), QStringLiteral("\u21D2")},
        {QStringLiteral("Leftarrow"), QStringLiteral("\u21D0")},
        {QStringLiteral("Leftrightarrow"), QStringLiteral("\u21D4")},
        {QStringLiteral("iff"), QStringLiteral("\u21D4")},
        {QStringLiteral("mapsto"), QStringLiteral("\u21A6")},
        {QStringLiteral("uparrow"), QStringLiteral("\u2191")},
        {QStringLiteral("downarrow"), QStringLiteral("\u2193")},
        // Binary & misc symbols
        {QStringLiteral("times"), QStringLiteral("\u00D7")},
        {QStringLiteral("cdot"), QStringLiteral("\u00B7")},
        {QStringLiteral("cdots"), QStringLiteral("\u22EF")},
        {QStringLiteral("ldots"), QStringLiteral("\u2026")},
        {QStringLiteral("dots"), QStringLiteral("\u2026")},
        {QStringLiteral("vdots"), QStringLiteral("\u22EE")},
        {QStringLiteral("ddots"), QStringLiteral("\u22F1")},
        {QStringLiteral("pm"), QStringLiteral("\u00B1")},
        {QStringLiteral("mp"), QStringLiteral("\u2213")},
        {QStringLiteral("div"), QStringLiteral("\u00F7")},
        {QStringLiteral("oplus"), QStringLiteral("\u2295")},
        {QStringLiteral("otimes"), QStringLiteral("\u2297")},
        {QStringLiteral("odot"), QStringLiteral("\u2299")},
        {QStringLiteral("circ"), QStringLiteral("\u2218")},
        {QStringLiteral("bullet"), QStringLiteral("\u2022")},
        {QStringLiteral("star"), QStringLiteral("\u22C6")},
        {QStringLiteral("dagger"), QStringLiteral("\u2020")},
        {QStringLiteral("perp"), QStringLiteral("\u22A5")},
        {QStringLiteral("parallel"), QStringLiteral("\u2225")},
        {QStringLiteral("angle"), QStringLiteral("\u2220")},
        {QStringLiteral("triangle"), QStringLiteral("\u25B3")},
        {QStringLiteral("ell"), QStringLiteral("\u2113")},
        {QStringLiteral("hbar"), QStringLiteral("\u210F")},
        {QStringLiteral("Re"), QStringLiteral("\u211C")},
        {QStringLiteral("Im"), QStringLiteral("\u2111")},
        {QStringLiteral("aleph"), QStringLiteral("\u2135")},
        {QStringLiteral("prime"), QStringLiteral("\u2032")},
        {QStringLiteral("langle"), QStringLiteral("\u27E8")},
        {QStringLiteral("rangle"), QStringLiteral("\u27E9")},
        {QStringLiteral("lceil"), QStringLiteral("\u2308")},
        {QStringLiteral("rceil"), QStringLiteral("\u2309")},
        {QStringLiteral("lfloor"), QStringLiteral("\u230A")},
        {QStringLiteral("rfloor"), QStringLiteral("\u230B")},
        {QStringLiteral("mid"), QStringLiteral("\u2223")},
        {QStringLiteral("top"), QStringLiteral("\u22A4")},
        {QStringLiteral("bot"), QStringLiteral("\u22A5")},
        {QStringLiteral("vdash"), QStringLiteral("\u22A2")},
        {QStringLiteral("models"), QStringLiteral("\u22A8")},
        {QStringLiteral("degree"), QStringLiteral("\u00B0")},
        {QStringLiteral("quad"), QStringLiteral("\u2003")},
        {QStringLiteral("qquad"), QStringLiteral("\u2003\u2003")},
    };
    return syms;
}

const QSet<QString> &uprightFunctions() {
    static const QSet<QString> fns = {
        QStringLiteral("sin"),    QStringLiteral("cos"),     QStringLiteral("tan"),     QStringLiteral("cot"),
        QStringLiteral("sec"),    QStringLiteral("csc"),     QStringLiteral("arcsin"),  QStringLiteral("arccos"),
        QStringLiteral("arctan"), QStringLiteral("sinh"),    QStringLiteral("cosh"),    QStringLiteral("tanh"),
        QStringLiteral("log"),    QStringLiteral("ln"),      QStringLiteral("exp"),     QStringLiteral("lim"),
        QStringLiteral("limsup"), QStringLiteral("liminf"),  QStringLiteral("max"),     QStringLiteral("min"),
        QStringLiteral("sup"),    QStringLiteral("inf"),     QStringLiteral("arg"),     QStringLiteral("argmax"),
        QStringLiteral("argmin"), QStringLiteral("det"),     QStringLiteral("dim"),     QStringLiteral("ker"),
        QStringLiteral("deg"),    QStringLiteral("gcd"),     QStringLiteral("mod"),     QStringLiteral("Pr"),
        QStringLiteral("Var"),    QStringLiteral("Cov"),     QStringLiteral("softmax"), QStringLiteral("relu"),
        QStringLiteral("sigmoid"),
    };
    return fns;
}

class LatexParser {
public:
    explicit LatexParser(const QString &src, bool display) : m_src(src), m_display(display) {}

    QString parseAll(int depth = 0) {
        if (depth > 24)
            return escapeHtmlText(m_src.mid(m_pos));
        QString out;
        while (m_pos < m_src.size()) {
            const QChar c = m_src[m_pos];
            if (c == QLatin1Char('}'))
                break;
            out += parseAtom(depth);
        }
        return out;
    }

private:
    void skipSpaces() {
        while (m_pos < m_src.size() && m_src[m_pos].isSpace())
            ++m_pos;
    }

    QString readRawGroup() {
        skipSpaces();
        if (m_pos >= m_src.size())
            return {};
        if (m_src[m_pos] != QLatin1Char('{'))
            return QString(m_src[m_pos++]);
        ++m_pos;  // consume '{'
        int depth = 1;
        const qsizetype start = m_pos;
        while (m_pos < m_src.size() && depth > 0) {
            if (m_src[m_pos] == QLatin1Char('\\') && m_pos + 1 < m_src.size()) {
                m_pos += 2;
                continue;
            }
            if (m_src[m_pos] == QLatin1Char('{'))
                ++depth;
            else if (m_src[m_pos] == QLatin1Char('}'))
                --depth;
            if (depth > 0)
                ++m_pos;
        }
        const QString inner = m_src.mid(start, m_pos - start);
        if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('}'))
            ++m_pos;
        return inner;
    }

    QString parseGroupOrAtom(int depth) {
        skipSpaces();
        if (m_pos >= m_src.size())
            return {};
        if (m_src[m_pos] == QLatin1Char('{')) {
            ++m_pos;
            const QString inner = parseAll(depth + 1);
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('}'))
                ++m_pos;
            return inner;
        }
        return parseBase(depth + 1);
    }

    QString parseAtom(int depth) {
        QString base = parseBase(depth);
        // Handle trailing ^ and _ in any order (e.g. x_i^2 or x^2_i).
        for (int pass = 0; pass < 2; ++pass) {
            const qsizetype saved = m_pos;
            skipSpaces();
            if (m_pos < m_src.size() && (m_src[m_pos] == QLatin1Char('^') || m_src[m_pos] == QLatin1Char('_'))) {
                const bool isSup = m_src[m_pos++] == QLatin1Char('^');
                const QString script = parseGroupOrAtom(depth + 1);
                if (!script.isEmpty())
                    base += isSup ? (QStringLiteral("<sup>") + script + QStringLiteral("</sup>"))
                                  : (QStringLiteral("<sub>") + script + QStringLiteral("</sub>"));
            } else {
                m_pos = saved;
                break;
            }
        }
        return base;
    }

    QString parseBase(int depth) {
        if (m_pos >= m_src.size() || depth > 24)
            return {};
        const QChar c = m_src[m_pos];
        if (c == QLatin1Char('{')) {
            ++m_pos;
            const QString inner = parseAll(depth + 1);
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('}'))
                ++m_pos;
            return inner;
        }
        if (c == QLatin1Char('\\'))
            return parseCommand(depth);
        if (c == QLatin1Char('^') || c == QLatin1Char('_'))
            return {};
        if (c == QLatin1Char('&')) {
            ++m_pos;
            return QStringLiteral("&nbsp;");
        }
        if (c == QLatin1Char('~')) {
            ++m_pos;
            return QStringLiteral("&nbsp;");
        }
        if (c == QLatin1Char('-')) {
            ++m_pos;
            return QStringLiteral("<span style=\"font-style:normal;\">\u2212</span>");
        }
        if (c.isDigit() || c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('[') ||
            c == QLatin1Char(']') || c == QLatin1Char('+') || c == QLatin1Char('=') || c == QLatin1Char(',') ||
            c == QLatin1Char(';') || c == QLatin1Char(':') || c == QLatin1Char('!') || c == QLatin1Char('|') ||
            c == QLatin1Char('/')) {
            ++m_pos;
            return QStringLiteral("<span style=\"font-style:normal;\">") + escapeHtmlText(QString(c)) +
                   QStringLiteral("</span>");
        }
        if (c.isSpace()) {
            while (m_pos < m_src.size() && m_src[m_pos].isSpace())
                ++m_pos;
            return QStringLiteral(" ");
        }
        ++m_pos;
        return escapeHtmlText(QString(c));
    }

    QString parseCommand(int depth) {
        ++m_pos;  // consume '\'
        if (m_pos >= m_src.size())
            return {};
        const QChar next = m_src[m_pos];
        // Punctuation / spacing commands
        if (!next.isLetter()) {
            ++m_pos;
            switch (next.unicode()) {
            case '\\':
                return m_display ? QStringLiteral("<br/>") : QStringLiteral(" ");
            case ',':
                return QStringLiteral("\u2009");
            case ';':
            case ':':
            case ' ':
                return QStringLiteral("\u2005");
            case '!':
                return {};
            case '|':
                return QStringLiteral("<span style=\"font-style:normal;\">\u2016</span>");
            case '{':
            case '}':
            case '%':
            case '_':
            case '&':
            case '$':
            case '#':
                return QStringLiteral("<span style=\"font-style:normal;\">") + escapeHtmlText(QString(next)) +
                       QStringLiteral("</span>");
            default:
                return escapeHtmlText(QString(next));
            }
        }
        const qsizetype start = m_pos;
        while (m_pos < m_src.size() && m_src[m_pos].isLetter())
            ++m_pos;
        const QString cmd = m_src.mid(start, m_pos - start);

        if (cmd == QLatin1String("frac") || cmd == QLatin1String("dfrac") || cmd == QLatin1String("tfrac")) {
            const QString num = parseGroupOrAtom(depth + 1).trimmed();
            const QString den = parseGroupOrAtom(depth + 1).trimmed();
            const bool nested = num.contains(QLatin1String("<sup")) || num.contains(QLatin1String("<sub")) ||
                                den.contains(QLatin1String("<sup")) || den.contains(QLatin1String("<sub")) ||
                                num.contains(QLatin1String("\u2044")) || den.contains(QLatin1String("\u2044"));
            if (nested) {
                auto needsParens = [](const QString &expr) {
                    static const QRegularExpression tagRx(QStringLiteral("<[^>]*>"));
                    QString plain = expr;
                    plain.remove(tagRx);
                    return plain.contains(QLatin1Char('+')) || plain.contains(QLatin1Char('=')) ||
                           plain.contains(QStringLiteral("\u2212"));
                };
                const QString nWrap = needsParens(num)
                                          ? QStringLiteral("<span style=\"font-style:normal;\">(</span>") + num +
                                                QStringLiteral("<span style=\"font-style:normal;\">)</span>")
                                          : num;
                const QString dWrap = needsParens(den)
                                          ? QStringLiteral("<span style=\"font-style:normal;\">(</span>") + den +
                                                QStringLiteral("<span style=\"font-style:normal;\">)</span>")
                                          : den;
                return nWrap + QStringLiteral("<span style=\"font-style:normal;\"> \u2044 </span>") + dWrap;
            }
            return QStringLiteral("<sup>") + num +
                   QStringLiteral("</sup><span style=\"font-style:normal;\">\u2044</span><sub>") + den +
                   QStringLiteral("</sub>");
        }
        if (cmd == QLatin1String("binom") || cmd == QLatin1String("dbinom") || cmd == QLatin1String("tbinom")) {
            const QString n = parseGroupOrAtom(depth + 1).trimmed();
            const QString k = parseGroupOrAtom(depth + 1).trimmed();
            return QStringLiteral("<span style=\"font-style:normal;\">C(</span>") + n +
                   QStringLiteral("<span style=\"font-style:normal;\">, </span>") + k +
                   QStringLiteral("<span style=\"font-style:normal;\">)</span>");
        }
        if (cmd == QLatin1String("sqrt")) {
            QString indexHtml;
            skipSpaces();
            if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char('[')) {
                ++m_pos;
                const qsizetype idxStart = m_pos;
                while (m_pos < m_src.size() && m_src[m_pos] != QLatin1Char(']'))
                    ++m_pos;
                const QString rawIdx = m_src.mid(idxStart, m_pos - idxStart);
                if (m_pos < m_src.size() && m_src[m_pos] == QLatin1Char(']'))
                    ++m_pos;
                indexHtml = QStringLiteral("<sup>") + LatexParser(rawIdx, false).parseAll(depth + 1) +
                            QStringLiteral("</sup>");
            }
            const QString rawArg = readRawGroup();
            const QString body = LatexParser(rawArg, false).parseAll(depth + 1).trimmed();
            const bool simple = rawArg.trimmed().size() <= 1;
            return indexHtml + QStringLiteral("<span style=\"font-style:normal;\">\u221A</span>") +
                   (simple ? body
                           : QStringLiteral("<span style=\"font-style:normal;\">(</span>") + body +
                                 QStringLiteral("<span style=\"font-style:normal;\">)</span>"));
        }
        if (cmd == QLatin1String("mathbb"))
            return mapBlackboard(readRawGroup());
        if (cmd == QLatin1String("mathcal") || cmd == QLatin1String("mathscr"))
            return mapCalligraphic(readRawGroup());
        if (cmd == QLatin1String("text") || cmd == QLatin1String("mathrm") || cmd == QLatin1String("textrm") ||
            cmd == QLatin1String("operatorname") || cmd == QLatin1String("mathsf") || cmd == QLatin1String("mathtt")) {
            const QString raw = readRawGroup();
            return QStringLiteral("<span style=\"font-style:normal;\">") + escapeHtmlText(raw) +
                   QStringLiteral("</span>");
        }
        if (cmd == QLatin1String("mathbf") || cmd == QLatin1String("textbf") || cmd == QLatin1String("bm") ||
            cmd == QLatin1String("boldsymbol")) {
            return QStringLiteral("<b style=\"font-style:normal;\">") + parseGroupOrAtom(depth + 1) +
                   QStringLiteral("</b>");
        }
        if (cmd == QLatin1String("hat") || cmd == QLatin1String("widehat"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0302");
        if (cmd == QLatin1String("bar") || cmd == QLatin1String("overline"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0304");
        if (cmd == QLatin1String("tilde") || cmd == QLatin1String("widetilde"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0303");
        if (cmd == QLatin1String("vec"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u20D7");
        if (cmd == QLatin1String("dot"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0307");
        if (cmd == QLatin1String("ddot"))
            return parseGroupOrAtom(depth + 1) + QStringLiteral("\u0308");
        if (cmd == QLatin1String("begin") || cmd == QLatin1String("end")) {
            (void)readRawGroup();  // consume environment name like {aligned} / {equation*}
            return {};
        }
        if (cmd == QLatin1String("left") || cmd == QLatin1String("right") || cmd == QLatin1String("big") ||
            cmd == QLatin1String("Big") || cmd == QLatin1String("bigg") || cmd == QLatin1String("Bigg") ||
            cmd == QLatin1String("limits") || cmd == QLatin1String("nolimits") || cmd == QLatin1String("displaystyle") ||
            cmd == QLatin1String("textstyle")) {
            skipSpaces();
            if ((cmd == QLatin1String("left") || cmd == QLatin1String("right")) && m_pos < m_src.size() &&
                m_src[m_pos] == QLatin1Char('.'))
                ++m_pos;  // invisible delimiter \left. or \right.
            return {};
        }
        if (latexSymbols().contains(cmd)) {
            const QString sym = latexSymbols().value(cmd);
            const bool isLowerGreek = !sym.isEmpty() && sym[0].unicode() >= 0x03B1 && sym[0].unicode() <= 0x03F5;
            return isLowerGreek ? sym : (QStringLiteral("<span style=\"font-style:normal;\">") + sym + QStringLiteral("</span>"));
        }
        if (uprightFunctions().contains(cmd))
            return QStringLiteral("<span style=\"font-style:normal;\">") + cmd + QStringLiteral("</span>");
        // Unknown command: emit plain text name without backslash so equations remain readable.
        return escapeHtmlText(cmd);
    }

    QString m_src;
    qsizetype m_pos = 0;
    bool m_display = false;
};

bool looksLikeCurrencySpan(const QString &inner) {
    if (inner.isEmpty())
        return true;
    // Guard e.g. "$5 to $10" or "$2.50 per book" if a stray $ appears later on the line.
    if (inner[0].isDigit() && inner.contains(QLatin1Char(' '))) {
        static const QRegularExpression mathOps(QStringLiteral("[\\\\^_=<>+*/{}\\[\\]()]"));
        if (!mathOps.match(inner).hasMatch())
            return true;
    }
    return false;
}

QString generateMathNonce(const QString &avoid) {
    QString nonce;
    do {
        const quint64 a = QRandomGenerator::global()->generate64();
        const quint64 b = QRandomGenerator::global()->generate64();
        nonce = QStringLiteral("CBMATH%1%2")
                    .arg(a, 16, 16, QLatin1Char('0'))
                    .arg(b, 16, 16, QLatin1Char('0'));
    } while (avoid.contains(nonce));
    return nonce;
}

QString stripDangerousMarkup(const QString &text) {
    if (text.isEmpty())
        return text;
    static const QRegularExpression htmlImg(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlScriptStyleBlock(
        QStringLiteral("<(?:script|style)\\b[^>]*>[\\s\\S]*?</(?:script|style)\\s*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression htmlDangerousTag(
        QStringLiteral("</?(?:script|style|iframe|object|embed|link|meta|base|svg|audio|video|source|form|input|button|"
                       "textarea|select)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression inlineImg(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression refImg(QStringLiteral("!\\[([^\\]]*)\\]\\[[^\\]]*\\]"));

    QString out = text;
    // Iterate until fixed point so doubly/triply nested malformed tags like
    // "<i<i<img >mg >mg src=...>" can never reconstruct an active tag.
    for (int pass = 0; pass < 64; ++pass) {
        const QString before = out;
        out.remove(htmlImg);
        out.remove(htmlScriptStyleBlock);
        out.remove(htmlDangerousTag);
        out.replace(inlineImg, QStringLiteral("\\1"));
        out.replace(refImg, QStringLiteral("\\1"));
        if (out == before)
            break;
    }
    out.replace(QStringLiteral("!["), QStringLiteral("!\u200B["));
    return out;
}

bool isFenceStartAt(const QString &text, qsizetype pos) {
    if (pos + 2 >= text.size() || text[pos] != QLatin1Char('`') || text[pos + 1] != QLatin1Char('`') ||
        text[pos + 2] != QLatin1Char('`')) {
        return false;
    }
    // CommonMark fenced code blocks begin at line start with at most 3 leading spaces.
    qsizetype lineStart = pos;
    while (lineStart > 0 && text[lineStart - 1] != QLatin1Char('\n'))
        --lineStart;
    if (pos - lineStart > 3)
        return false;
    for (qsizetype k = lineStart; k < pos; ++k)
        if (text[k] != QLatin1Char(' '))
            return false;
    return true;
}

// Returns the index right after the matching closing backtick run of exact length
// `ticks` before a paragraph break (\n\n) or EOF, or -1 if unclosed.
qsizetype findMatchingInlineCodeEnd(const QString &text, qsizetype openPos, qsizetype ticks) {
    const qsizetype n = text.size();
    qsizetype j = openPos + ticks;
    while (j < n) {
        if (text[j] == QLatin1Char('\n') && j + 1 < n && text[j + 1] == QLatin1Char('\n'))
            return -1;
        if (text[j] == QLatin1Char('`')) {
            qsizetype run = 0;
            while (j + run < n && text[j + run] == QLatin1Char('`'))
                ++run;
            if (run == ticks)
                return j + run;
            j += run;
            continue;
        }
        ++j;
    }
    return -1;
}

QString escapeRawHtmlOutsideCode(const QString &text) {
    if (!text.contains(QLatin1Char('<')) && !text.contains(QLatin1Char('>')))
        return text;
    QString out;
    out.reserve(text.size() + 32);
    const qsizetype n = text.size();
    qsizetype i = 0;
    bool inFence = false;
    bool atLineStart = true;
    int lineIndent = 0;
    bool inBlockquotePrefix = true;

    while (i < n) {
        if (isFenceStartAt(text, i)) {
            inFence = !inFence;
            qsizetype run = 0;
            while (i + run < n && text[i + run] == QLatin1Char('`'))
                ++run;
            out += text.mid(i, run);
            i += run;
            atLineStart = false;
            inBlockquotePrefix = false;
            continue;
        }
        if (inFence) {
            out += text[i++];
            continue;
        }
        if (text[i] == QLatin1Char('`')) {
            qsizetype ticks = 0;
            while (i + ticks < n && text[i + ticks] == QLatin1Char('`'))
                ++ticks;
            const qsizetype closeEnd = findMatchingInlineCodeEnd(text, i, ticks);
            if (closeEnd > i) {
                out += text.mid(i, closeEnd - i);
                i = closeEnd;
                atLineStart = false;
                inBlockquotePrefix = false;
                continue;
            }
            out += text.mid(i, ticks);
            i += ticks;
            atLineStart = false;
            inBlockquotePrefix = false;
            continue;
        }
        const QChar c = text[i++];
        if (c == QLatin1Char('\n')) {
            out += c;
            atLineStart = true;
            lineIndent = 0;
            inBlockquotePrefix = true;
            continue;
        }
        if (c == QLatin1Char('<')) {
            out += QStringLiteral("&lt;");
            atLineStart = false;
            inBlockquotePrefix = false;
            continue;
        }
        if (c == QLatin1Char('>')) {
            if (inBlockquotePrefix && lineIndent <= 3) {
                out += c;
                lineIndent = 0;
                atLineStart = false;
            } else {
                out += QStringLiteral("&gt;");
                atLineStart = false;
                inBlockquotePrefix = false;
            }
            continue;
        }
        if (inBlockquotePrefix && c == QLatin1Char(' ')) {
            ++lineIndent;
            if (lineIndent > 3)
                inBlockquotePrefix = false;
        } else {
            atLineStart = false;
            inBlockquotePrefix = false;
        }
        Q_UNUSED(atLineStart);
        out += c;
    }
    return out;
}

QString extractLatexWithNonces(const QString &text, const QString &nonce, QStringList &mathInnersOut,
                               QList<bool> &mathIsBlockOut, bool padBlocks) {
    if (!text.contains(QLatin1Char('$')) && !text.contains(QStringLiteral("\\(")) &&
        !text.contains(QStringLiteral("\\[")))
        return text;

    QString out;
    out.reserve(text.size() + 64);
    const qsizetype n = text.size();
    qsizetype i = 0;
    bool inFence = false;

    auto emitMath = [&](const QString &expr, bool display) -> bool {
        const QString inner = LatexParser(expr.trimmed(), display).parseAll().trimmed();
        if (inner.isEmpty())
            return false;
        const int idx = int(mathInnersOut.size());
        mathInnersOut.append(inner);
        mathIsBlockOut.append(display);
        const QString token =
            QStringLiteral("%1%2%3Z").arg(nonce, display ? QStringLiteral("B") : QStringLiteral("I"), QString::number(idx));
        if (display && padBlocks)
            out += QStringLiteral("\n\n") + token + QStringLiteral("\n\n");
        else
            out += token;
        return true;
    };

    while (i < n) {
        // Preserve fenced code blocks (``` ... ```) verbatim (e.g. copyable LaTeX output).
        if (isFenceStartAt(text, i)) {
            inFence = !inFence;
            qsizetype run = 0;
            while (i + run < n && text[i + run] == QLatin1Char('`'))
                ++run;
            out += text.mid(i, run);
            i += run;
            continue;
        }
        if (inFence) {
            out += text[i++];
            continue;
        }
        // Preserve matched inline code spans (`...`) verbatim.
        if (text[i] == QLatin1Char('`')) {
            qsizetype ticks = 0;
            while (i + ticks < n && text[i + ticks] == QLatin1Char('`'))
                ++ticks;
            const qsizetype closeEnd = findMatchingInlineCodeEnd(text, i, ticks);
            if (closeEnd > i) {
                out += text.mid(i, closeEnd - i);
                i = closeEnd;
                continue;
            }
            out += text.mid(i, ticks);
            i += ticks;
            continue;
        }
        const bool escaped = (i > 0 && text[i - 1] == QLatin1Char('\\'));

        // Display math: $$...$$
        if (!escaped && i + 1 < n && text[i] == QLatin1Char('$') && text[i + 1] == QLatin1Char('$')) {
            const qsizetype close = text.indexOf(QStringLiteral("$$"), i + 2);
            if (close > i + 2 && emitMath(text.mid(i + 2, close - (i + 2)), true)) {
                i = close + 2;
                continue;
            }
        }
        // Display math: \[...\]
        if (!escaped && i + 1 < n && text[i] == QLatin1Char('\\') && text[i + 1] == QLatin1Char('[')) {
            const qsizetype close = text.indexOf(QStringLiteral("\\]"), i + 2);
            if (close > i + 2 && emitMath(text.mid(i + 2, close - (i + 2)), true)) {
                i = close + 2;
                continue;
            }
        }
        // Inline math: \(...\)
        if (!escaped && i + 1 < n && text[i] == QLatin1Char('\\') && text[i + 1] == QLatin1Char('(')) {
            const qsizetype close = text.indexOf(QStringLiteral("\\)"), i + 2);
            if (close > i + 2 && emitMath(text.mid(i + 2, close - (i + 2)), false)) {
                i = close + 2;
                continue;
            }
        }
        // Inline math: $...$ (with strict Pandoc/KaTeX currency guards)
        if (!escaped && text[i] == QLatin1Char('$') && i + 1 < n && text[i + 1] != QLatin1Char('$') &&
            !text[i + 1].isSpace()) {
            qsizetype j = i + 1;
            qsizetype close = -1;
            while (j < n) {
                if (text[j] == QLatin1Char('\n') && j + 1 < n && text[j + 1] == QLatin1Char('\n'))
                    break;
                if (text[j] == QLatin1Char('`'))
                    break;
                if (text[j] == QLatin1Char('$') && text[j - 1] != QLatin1Char('\\')) {
                    if (!text[j - 1].isSpace() &&
                        (j + 1 >= n || (!text[j + 1].isDigit() && text[j + 1] != QLatin1Char('$')))) {
                        close = j;
                        break;
                    }
                    break;
                }
                ++j;
            }
            if (close > i + 1) {
                const QString expr = text.mid(i + 1, close - (i + 1));
                if (!looksLikeCurrencySpan(expr) && emitMath(expr, false)) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

QString sanitizeMathBgColor(const QString &mathBlockBg) {
    const QString trimmed = mathBlockBg.trimmed();
    static const QRegularExpression safeColorRx(QStringLiteral("^[#a-zA-Z0-9(),.%\\s-]+$"));
    if (trimmed.isEmpty() || trimmed.size() > 64 || !safeColorRx.match(trimmed).hasMatch())
        return QStringLiteral("rgba(128,128,128,0.12)");
    return escapeHtmlText(trimmed);
}

QString joinedStudyDocument(const QList<Companion::Chapter> &chapters, qint64 limit, int currentChapter = 0) {
    if (chapters.isEmpty() || limit <= 0)
        return {};
    auto formatChapter = [](const Companion::Chapter &ch) {
        const QString heading = ch.title.isEmpty() ? QString() : QStringLiteral("## %1\n\n").arg(ch.title);
        return heading + ch.text.trimmed() + QStringLiteral("\n\n");
    };
    qint64 total = 0;
    for (const Companion::Chapter &ch : chapters)
        total += ch.text.trimmed().size() + ch.title.size() + 8;
    if (total <= limit) {
        QString out;
        out.reserve(qsizetype(total));
        for (const Companion::Chapter &ch : chapters)
            out += formatChapter(ch);
        return out.trimmed();
    }
    // Document exceeds budget: keep opening (Abstract/Intro/Method), current section,
    // and closing findings/conclusions (deprioritizing trailing References).
    const qint64 headBudget = qint64(double(limit) * 0.60);
    const qint64 curBudget = qint64(double(limit) * 0.22);
    const qint64 tailBudget = qMax<qint64>(1024, limit - headBudget - curBudget);

    QString head;
    int lastHeadIdx = -1;
    for (int i = 0; i < chapters.size() && head.size() < headBudget; ++i) {
        QString block = formatChapter(chapters[i]);
        if (head.size() + block.size() > headBudget)
            block = block.left(qsizetype(headBudget - head.size()));
        head += block;
        lastHeadIdx = i;
    }

    // Find the last substantive chapter before References/Bibliography if present.
    int substantiveEnd = int(chapters.size()) - 1;
    static const QRegularExpression refHead(QStringLiteral("^\\s*(?:references|bibliography|works cited)\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    for (int i = int(chapters.size()) - 1; i > lastHeadIdx + 1; --i) {
        const QString titleOrLead = !chapters[i].title.isEmpty() ? chapters[i].title : chapters[i].text.left(120);
        if (refHead.match(titleOrLead).hasMatch()) {
            substantiveEnd = i - 1;
            break;
        }
    }

    QString middle;
    int curPos = -1;
    for (int i = 0; i < chapters.size(); ++i) {
        if (chapters[i].index == currentChapter) {
            curPos = i;
            break;
        }
    }
    if (curPos > lastHeadIdx && curPos <= substantiveEnd) {
        QString block = formatChapter(chapters[curPos]);
        if (block.size() > curBudget)
            block = block.left(qsizetype(curBudget));
        middle = QStringLiteral("\n[…]\n\n") + block;
    }

    QString tail;
    for (int i = substantiveEnd; i > lastHeadIdx && tail.size() < tailBudget; --i) {
        if (i == curPos)
            continue;
        QString block = formatChapter(chapters[i]);
        if (tail.size() + block.size() > tailBudget)
            block = block.right(qsizetype(tailBudget - tail.size()));
        tail.prepend(block);
    }
    if (!tail.isEmpty())
        tail.prepend(QStringLiteral("\n[…]\n\n"));

    return (head + middle + tail).trimmed();
}

QString makeSnipThumbnailDataUrl(const QByteArray &pngBytes) {
    QImage img;
    if (!img.loadFromData(pngBytes))
        return {};
    if (img.width() > 420 || img.height() > 320)
        img = img.scaled(420, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray thumbBytes;
    QBuffer buf(&thumbBytes);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(thumbBytes.toBase64());
}

QString shortBookTitle(const QString &title, qsizetype maxLen = 34) {
    const QString clean = title.simplified();
    if (clean.size() <= maxLen)
        return clean;
    qsizetype cut = clean.lastIndexOf(QLatin1Char(' '), maxLen);
    if (cut < maxLen / 2)
        cut = maxLen;
    return clean.left(cut).trimmed() + QChar(0x2026);
}

QString shelfCitationLabel(const QString &bookTitle, const QString &format, int chapter) {
    const QString title = shortBookTitle(bookTitle);
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("%1, p. %2").arg(title).arg(chapter + 1);
    return QStringLiteral("%1, ch. %2").arg(title).arg(chapter + 1);
}

}  // namespace

Companion::Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_ai(ai), m_reader(reader), m_connection(databaseConnection), m_index(ai, databaseConnection) {
    m_targetLanguage = companionSettings().value(QStringLiteral("companion/translateLanguage")).toString().trimmed();
    m_streamFlush.setSingleShot(true);
    m_streamFlush.setInterval(33);
    connect(&m_streamFlush, &QTimer::timeout, this, &Companion::flushStreamedAnswer);
    ensureTables();
    if (m_reader && m_reader->bookId() > 0)
        loadBookState(m_reader->bookId());
    connect(reader, &Reader::bookChanged, this, [this] {
        stop();
        m_feature.clear();
        m_subject.clear();
        m_answer.clear();
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_peopleWhereLocal = false;
        m_recapText.clear();
        m_recapLoaded = false;
        m_recapChapter = -1;
        m_recapOffset = -1;
        m_recapWhere.clear();
        m_recapWhereLocal = false;
        m_selectionFeature.clear();
        m_selectionSubject.clear();
        m_selectionAnswer.clear();
        m_selectionWhere.clear();
        m_selectionWhereLocal = false;
        m_conversation.clear();
        m_currentSources.clear();
        m_pendingUserTurn = false;
        m_pendingSnipPng.clear();
        m_pendingSnipUrl.clear();
        m_pendingSnipPage = -1;
        m_problem.clear();
        m_problemIsConfig = false;
        m_lastAction = {};
        m_pendingSetup = {};
        m_readingMode = QStringLiteral("fiction");
        m_scope = QStringLiteral("upto");
        m_shelfLabel.clear();
        if (m_reader && m_reader->bookId() > 0)
            loadBookState(m_reader->bookId());
        emit changed();
        emit answerChanged();
        emit conversationChanged();
        emit recapChanged();
        emit selectionChanged();
        emit readingModeChanged();
        emit scopeChanged();
        emit pendingSnipChanged();
    });
    connect(reader, &Reader::locationChanged, this, [this] {
        if (!m_reader || m_reader->bookId() <= 0)
            return;
        if (wholeBookStudyActive())
            return;
        const int bookId = m_reader->bookId();
        const int ch = m_reader->chapter();
        const qint64 end = m_reader->pageEnd();
        const qint64 start = m_reader->pageStart();
        // Cancel an in-flight chapter-scoped request (including Ask) only if the reader truly
        // navigated backward (not when pageEnd shrinks during a 2-column -> 1-column dock resize).
        if (m_busy &&
            (m_feature == QLatin1String("whoswho") || m_feature == QLatin1String("recap") ||
             m_feature == QLatin1String("ask")) &&
            m_requestChapter >= 0) {
            const bool movedBack =
                ch < m_requestChapter || (ch == m_requestChapter && m_requestStart > 0 && start < m_requestStart);
            if (movedBack) {
                if (m_feature == QLatin1String("ask"))
                    m_answer.clear();  // never commit a spoiler-tainted partial stream
                stop();
            }
        }
        bool hasStampedTurn = false;
        for (const Turn &turn : std::as_const(m_conversation)) {
            if (turn.chapter >= 0) {
                hasStampedTurn = true;
                break;
            }
        }
        if (hasStampedTurn)
            emit conversationChanged();
        bool changedAny = false;
        bool recapUpdated = false;
        if (m_peopleLoaded) {
            const bool aheadOfReader =
                m_peopleChapter > ch || (m_peopleChapter == ch && end > 0 && m_peopleOffset > end);
            if (aheadOfReader) {
                if (!loadWhosWhoSnapshot(bookId, ch, end, start, false)) {
                    m_people.clear();
                    m_peopleLoaded = false;
                    m_peopleChapter = -1;
                    m_peopleOffset = -1;
                    m_peopleKey.clear();
                    m_peopleWhere.clear();
                    m_peopleWhereLocal = false;
                } else {
                    m_where = m_peopleWhere;
                    m_whereLocal = m_peopleWhereLocal;
                }
                changedAny = true;
            }
        }
        if ((!m_peopleLoaded || m_peopleChapter < ch) && !(m_busy && m_feature == QLatin1String("whoswho"))) {
            if (loadWhosWhoSnapshot(bookId, ch, end, start, true)) {
                if (m_feature == QLatin1String("whoswho")) {
                    m_where = m_peopleWhere;
                    m_whereLocal = m_peopleWhereLocal;
                }
                changedAny = true;
            }
        }
        if (m_recapLoaded) {
            const bool recapAhead =
                m_recapChapter > ch || (m_recapChapter == ch && end > 0 && m_recapOffset > end);
            if (recapAhead) {
                if (!loadRecapSnapshot(bookId, ch, end, start, false)) {
                    m_recapText.clear();
                    m_recapLoaded = false;
                    m_recapChapter = -1;
                    m_recapOffset = -1;
                    m_recapWhere.clear();
                    m_recapWhereLocal = false;
                }
                if (m_feature == QLatin1String("recap") && !m_busy) {
                    m_answer = m_recapText;
                    m_where = m_recapWhere;
                    m_whereLocal = m_recapWhereLocal;
                    emit answerChanged();
                }
                recapUpdated = true;
                changedAny = true;
            }
        }
        if ((!m_recapLoaded || m_recapChapter < ch) && !(m_busy && m_feature == QLatin1String("recap"))) {
            if (loadRecapSnapshot(bookId, ch, end, start, true)) {
                if (m_feature == QLatin1String("recap")) {
                    m_answer = m_recapText;
                    m_where = m_recapWhere;
                    m_whereLocal = m_recapWhereLocal;
                    emit answerChanged();
                }
                recapUpdated = true;
                changedAny = true;
            }
        }
        if (recapUpdated)
            emit recapChanged();
        if (changedAny)
            emit changed();
    });
}

Companion::~Companion() { stop(); }

void Companion::ensureTables() {
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_threads ("
        " id INTEGER PRIMARY KEY,"
        " book_id INTEGER NOT NULL,"
        " seq INTEGER NOT NULL,"
        " from_user INTEGER NOT NULL,"
        " text TEXT NOT NULL,"
        " sources TEXT NOT NULL DEFAULT '[]',"
        " provenance TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT 0,"
        " snip_url TEXT NOT NULL DEFAULT '',"
        " chapter INTEGER NOT NULL DEFAULT -1,"
        " offset INTEGER NOT NULL DEFAULT -1,"
        " created_at INTEGER NOT NULL)"));
    bool hasProvenance = false;
    bool hasIsLocal = false;
    bool hasSnipUrl = false;
    bool hasChapter = false;
    bool hasOffset = false;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(companion_threads)"))) {
        while (info.next()) {
            const QString col = info.value(1).toString();
            if (col == QLatin1String("provenance"))
                hasProvenance = true;
            else if (col == QLatin1String("is_local"))
                hasIsLocal = true;
            else if (col == QLatin1String("snip_url"))
                hasSnipUrl = true;
            else if (col == QLatin1String("chapter"))
                hasChapter = true;
            else if (col == QLatin1String("offset"))
                hasOffset = true;
        }
    }
    if (!hasProvenance)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN provenance TEXT NOT NULL DEFAULT ''"));
    if (!hasIsLocal)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN is_local INTEGER NOT NULL DEFAULT 0"));
    if (!hasSnipUrl)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN snip_url TEXT NOT NULL DEFAULT ''"));
    if (!hasChapter)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN chapter INTEGER NOT NULL DEFAULT -1"));
    if (!hasOffset)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN offset INTEGER NOT NULL DEFAULT -1"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS companion_threads_book ON companion_threads (book_id, seq)"));

    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_whoswho ("
        " book_id INTEGER NOT NULL,"
        " mode TEXT NOT NULL DEFAULT 'fiction',"
        " chapter INTEGER NOT NULL,"
        " offset INTEGER NOT NULL,"
        " page_start INTEGER NOT NULL DEFAULT 0,"
        " where_label TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT -1,"
        " people TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " PRIMARY KEY (book_id, mode, chapter, offset))"));
    bool whoswhoHasLocal = false;
    bool whoswhoHasMode = false;
    QSqlQuery wInfo(db);
    if (wInfo.exec(QStringLiteral("PRAGMA table_info(companion_whoswho)"))) {
        while (wInfo.next()) {
            const QString col = wInfo.value(1).toString();
            if (col == QLatin1String("is_local"))
                whoswhoHasLocal = true;
            else if (col == QLatin1String("mode"))
                whoswhoHasMode = true;
        }
    }
    if (!whoswhoHasLocal)
        query.exec(QStringLiteral("ALTER TABLE companion_whoswho ADD COLUMN is_local INTEGER NOT NULL DEFAULT -1"));
    if (!whoswhoHasMode) {
        const bool inTx = db.transaction();
        const bool ok =
            query.exec(QStringLiteral(
                "CREATE TABLE companion_whoswho_v2 ("
                " book_id INTEGER NOT NULL,"
                " mode TEXT NOT NULL DEFAULT 'fiction',"
                " chapter INTEGER NOT NULL,"
                " offset INTEGER NOT NULL,"
                " page_start INTEGER NOT NULL DEFAULT 0,"
                " where_label TEXT NOT NULL DEFAULT '',"
                " is_local INTEGER NOT NULL DEFAULT -1,"
                " people TEXT NOT NULL,"
                " updated_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, mode, chapter, offset))")) &&
            query.exec(QStringLiteral(
                "INSERT INTO companion_whoswho_v2 "
                "(book_id, mode, chapter, offset, page_start, where_label, is_local, people, updated_at) "
                "SELECT book_id, 'fiction', chapter, offset, page_start, where_label, is_local, people, updated_at "
                "FROM companion_whoswho")) &&
            query.exec(QStringLiteral("DROP TABLE companion_whoswho")) &&
            query.exec(QStringLiteral("ALTER TABLE companion_whoswho_v2 RENAME TO companion_whoswho"));
        if (inTx)
            ok ? db.commit() : db.rollback();
    }
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS companion_whoswho_book ON companion_whoswho (book_id, mode, chapter DESC, offset DESC)"));

    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_recaps ("
        " book_id INTEGER NOT NULL,"
        " mode TEXT NOT NULL DEFAULT 'fiction',"
        " chapter INTEGER NOT NULL,"
        " offset INTEGER NOT NULL,"
        " page_start INTEGER NOT NULL DEFAULT 0,"
        " where_label TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT 0,"
        " text TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " PRIMARY KEY (book_id, mode, chapter, offset))"));
    bool recapsHasMode = false;
    QSqlQuery rInfo(db);
    if (rInfo.exec(QStringLiteral("PRAGMA table_info(companion_recaps)"))) {
        while (rInfo.next())
            if (rInfo.value(1).toString() == QLatin1String("mode"))
                recapsHasMode = true;
    }
    if (!recapsHasMode) {
        const bool inTx = db.transaction();
        const bool ok =
            query.exec(QStringLiteral(
                "CREATE TABLE companion_recaps_v2 ("
                " book_id INTEGER NOT NULL,"
                " mode TEXT NOT NULL DEFAULT 'fiction',"
                " chapter INTEGER NOT NULL,"
                " offset INTEGER NOT NULL,"
                " page_start INTEGER NOT NULL DEFAULT 0,"
                " where_label TEXT NOT NULL DEFAULT '',"
                " is_local INTEGER NOT NULL DEFAULT 0,"
                " text TEXT NOT NULL,"
                " updated_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, mode, chapter, offset))")) &&
            query.exec(QStringLiteral(
                "INSERT INTO companion_recaps_v2 "
                "(book_id, mode, chapter, offset, page_start, where_label, is_local, text, updated_at) "
                "SELECT book_id, 'fiction', chapter, offset, page_start, where_label, is_local, text, updated_at "
                "FROM companion_recaps")) &&
            query.exec(QStringLiteral("DROP TABLE companion_recaps")) &&
            query.exec(QStringLiteral("ALTER TABLE companion_recaps_v2 RENAME TO companion_recaps"));
        if (inTx)
            ok ? db.commit() : db.rollback();
    }
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS companion_recaps_book ON companion_recaps (book_id, mode, chapter DESC, offset DESC)"));
    // Clean up companion data automatically when a book is deleted from `books`.
    QSqlQuery hasBooks(db);
    if (hasBooks.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='books'")) && hasBooks.next()) {
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS companion_cleanup_book AFTER DELETE ON books BEGIN "
            "DELETE FROM companion_threads WHERE book_id = OLD.id; "
            "DELETE FROM companion_whoswho WHERE book_id = OLD.id; "
            "END"));
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS companion_cleanup_recaps AFTER DELETE ON books BEGIN "
            "DELETE FROM companion_recaps WHERE book_id = OLD.id; "
            "END"));
    }
}

QString Companion::detectReadingMode(const QString &format, const QString &title, const QStringList &tocTitles,
                                     const QString &sampleText) {
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("study");

    const QString joinedToc = tocTitles.join(QLatin1Char('\n'));
    const QString combined = title + QLatin1Char('\n') + joinedToc + QLatin1Char('\n') + sampleText;

    // Strong standalone academic/technical signals (arXiv IDs, DOIs, numbered theorems/lemmas/algorithms).
    static const QRegularExpression arxivRx(
        QStringLiteral("\\barXiv\\s*:\\s*(?:\\d{4}\\.\\d{4,5}|[a-z\\-]+/\\d{7})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression doiRx(
        QStringLiteral("(?:\\bdoi\\s*:\\s*|https?://(?:dx\\.)?doi\\.org/)10\\.\\d{4,9}/\\S+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression numberedFormalRx(
        QStringLiteral("\\b(?:Theorem|Lemma|Corollary|Proposition|Algorithm)\\s+\\d+(?:\\.\\d+)*\\b"));
    if (arxivRx.match(combined).hasMatch() || doiRx.match(combined).hasMatch() ||
        numberedFormalRx.match(combined).hasMatch()) {
        return QStringLiteral("study");
    }

    // Structural academic signals across TOC and opening prose (require >= 2 distinct signals).
    int signalsFound = 0;
    static const QRegularExpression abstractRx(
        QStringLiteral("(?:^|\\n)\\s*(?:\\d+\\.?\\s+)?abstract\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression refsRx(
        QStringLiteral("(?:^|\\n)\\s*(?:\\d+\\.?\\s+)?(?:references|bibliography|works cited)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression formalRx(
        QStringLiteral("\\b(?:Theorem|Lemma|Corollary|Algorithm|Proof\\.|Q\\.E\\.D\\.|Definition\\s+\\d)\\b"));
    static const QRegularExpression academicTitleRx(
        QStringLiteral("\\b(?:proceedings|transactions on|journal of|lecture notes|textbook|handbook|monograph)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression figureEqRx(
        QStringLiteral("\\b(?:Equation\\s*\\(?\\d|Eq\\.\\s*\\(?\\d|Fig(?:ure)?\\.\\s*\\d|Table\\s+\\d)\\b"));

    if (abstractRx.match(joinedToc).hasMatch() || abstractRx.match(sampleText).hasMatch())
        ++signalsFound;
    if (refsRx.match(joinedToc).hasMatch() || refsRx.match(sampleText).hasMatch())
        ++signalsFound;
    if (formalRx.match(combined).hasMatch())
        ++signalsFound;
    if (figureEqRx.match(sampleText).hasMatch())
        ++signalsFound;
    if (academicTitleRx.match(title).hasMatch())
        ++signalsFound;

    return signalsFound >= 2 ? QStringLiteral("study") : QStringLiteral("fiction");
}

QString Companion::loadSavedReadingMode(int bookId) const {
    if (bookId <= 0)
        return {};
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return {};
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM book_settings WHERE book_id = ? AND key = 'companion_mode'"));
    q.addBindValue(bookId);
    if (!q.exec() || !q.next())
        return {};
    const QString val = q.value(0).toString().trimmed().toLower();
    if (val == QLatin1String("study") || val == QLatin1String("fiction"))
        return val;
    return {};
}

void Companion::saveReadingMode(int bookId, const QString &mode) {
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO book_settings (book_id, key, value) VALUES (?, 'companion_mode', ?)"));
    q.addBindValue(bookId);
    q.addBindValue(mode);
    q.exec();
}

QString Companion::detectCurrentBookMode() const {
    if (!m_reader || m_reader->bookId() <= 0)
        return QStringLiteral("fiction");
    const QString format = m_reader->format();
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("study");

    QStringList tocTitles;
    for (const QVariant &entry : m_reader->contents()) {
        const QString t = entry.toMap().value(QStringLiteral("title")).toString().trimmed();
        if (!t.isEmpty())
            tocTitles.append(t);
    }
    QString sample = m_reader->passageFrom(0, 6000);
    if (sample.isEmpty()) {
        const QString path = m_reader->bookPath();
        if (!path.isEmpty()) {
            EpubBook book(path);
            if (book.open()) {
                const int maxCh = qMin(3, int(book.spine().size()));
                for (int i = 0; i < maxCh && sample.size() < 6000; ++i) {
                    const QString chunk = book.chapterText(i).left(6000 - sample.size()).trimmed();
                    if (!chunk.isEmpty())
                        sample += chunk + QLatin1Char('\n');
                }
            }
        }
    }
    return detectReadingMode(format, m_reader->title(), tocTitles, sample);
}

QList<Companion::ShelfBook> Companion::shelfBooksFor(int bookId, QString *labelOut) const {
    QList<ShelfBook> out;
    if (labelOut)
        labelOut->clear();
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return out;

    // 1. Prefer the current book's collection(s) when it belongs to one.
    if (bookId > 0) {
        QSqlQuery colQ(db);
        colQ.prepare(QStringLiteral(
            "SELECT c.id, c.name FROM collections c "
            "JOIN book_collections bc ON bc.collection_id = c.id "
            "WHERE bc.book_id = ? ORDER BY c.name COLLATE NOCASE"));
        colQ.addBindValue(bookId);
        QList<int> colIds;
        QStringList colNames;
        if (colQ.exec()) {
            while (colQ.next()) {
                colIds.append(colQ.value(0).toInt());
                colNames.append(colQ.value(1).toString());
            }
        }
        if (!colIds.isEmpty()) {
            QStringList placeholders;
            for (int i = 0; i < colIds.size(); ++i)
                placeholders.append(QStringLiteral("?"));
            QSqlQuery booksQ(db);
            booksQ.prepare(QStringLiteral(
                               "SELECT DISTINCT b.id, b.title, b.format, b.path "
                               "FROM books b "
                               "JOIN book_collections bc ON bc.book_id = b.id "
                               "WHERE bc.collection_id IN (%1) AND b.missing = 0 AND b.hidden = 0 "
                               "ORDER BY b.opened_at DESC, b.id ASC")
                               .arg(placeholders.join(QLatin1Char(','))));
            for (int cid : colIds)
                booksQ.addBindValue(cid);
            if (booksQ.exec()) {
                while (booksQ.next())
                    out.append({booksQ.value(0).toInt(), booksQ.value(1).toString(), booksQ.value(2).toString(),
                                booksQ.value(3).toString()});
            }
            if (!out.isEmpty()) {
                if (labelOut)
                    *labelOut = colNames.first();
                return out;
            }
        }
    }

    // 2. Otherwise, check sibling books in the same folder.
    QList<ShelfBook> allVisible;
    QString currentFolder;
    {
        QSqlQuery allQ(db);
        if (allQ.exec(QStringLiteral(
                "SELECT id, title, format, path FROM books "
                "WHERE missing = 0 AND hidden = 0 ORDER BY opened_at DESC, id ASC"))) {
            while (allQ.next()) {
                ShelfBook b{allQ.value(0).toInt(), allQ.value(1).toString(), allQ.value(2).toString(),
                            allQ.value(3).toString()};
                if (b.id == bookId && !b.path.isEmpty())
                    currentFolder = QFileInfo(b.path).absolutePath();
                allVisible.append(std::move(b));
            }
        }
    }
    if (!currentFolder.isEmpty()) {
        QList<ShelfBook> sameFolder;
        for (const ShelfBook &b : std::as_const(allVisible)) {
            if (!b.path.isEmpty() && QFileInfo(b.path).absolutePath() == currentFolder)
                sameFolder.append(b);
        }
        if (sameFolder.size() >= 2) {
            if (labelOut)
                *labelOut = QFileInfo(currentFolder).fileName();
            return sameFolder;
        }
    }

    // 3. Fallback: whole library.
    return allVisible;
}

void Companion::refreshShelfLabel() {
    QString label;
    if (m_reader && m_reader->bookId() > 0)
        (void)shelfBooksFor(m_reader->bookId(), &label);
    m_shelfLabel = label;
}

void Companion::setReadingMode(const QString &mode) {
    QString normalized = mode.trimmed().toLower();
    if (normalized == QLatin1String("story"))
        normalized = QStringLiteral("fiction");
    if (normalized != QLatin1String("fiction") && normalized != QLatin1String("study"))
        return;
    const bool modeChanged = (normalized != m_readingMode);
    if (m_reader && m_reader->bookId() > 0)
        saveReadingMode(m_reader->bookId(), normalized);
    if (!modeChanged)
        return;

    if (m_busy && (m_feature == QLatin1String("recap") || m_feature == QLatin1String("whoswho")))
        stop();

    m_readingMode = normalized;
    const QString targetScope = studyMode() ? QStringLiteral("book") : QStringLiteral("upto");
    const bool scopeDidChange = (m_scope != targetScope);
    m_scope = targetScope;

    // Reload mode-specific Recap/Brief and Who's Who/Glossary snapshots so modes never collide.
    m_people.clear();
    m_peopleLoaded = false;
    m_peopleChapter = -1;
    m_peopleOffset = -1;
    m_peopleKey.clear();
    m_peopleWhere.clear();
    m_peopleWhereLocal = false;
    m_recapText.clear();
    m_recapLoaded = false;
    m_recapChapter = -1;
    m_recapOffset = -1;
    m_recapWhere.clear();
    m_recapWhereLocal = false;

    if (m_reader && m_reader->bookId() > 0) {
        const int bookId = m_reader->bookId();
        loadWhosWhoSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        loadRecapSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        if (m_feature == QLatin1String("recap") && !m_busy) {
            m_answer = m_recapText;
            m_where = m_recapWhere;
            m_whereLocal = m_recapWhereLocal;
        } else if (m_feature == QLatin1String("whoswho") && !m_busy) {
            m_where = m_peopleWhere;
            m_whereLocal = m_peopleWhereLocal;
        }
    }

    emit readingModeChanged();
    if (scopeDidChange)
        emit scopeChanged();
    emit recapChanged();
    emit answerChanged();
    emit changed();
}

void Companion::setScope(const QString &scope) {
    const QString normalized = scope.trimmed().toLower();
    if (normalized != QLatin1String("upto") && normalized != QLatin1String("book") &&
        normalized != QLatin1String("shelf"))
        return;
    if (normalized == m_scope)
        return;
    const bool wasWholeStudy = wholeBookStudyActive();
    m_scope = normalized;
    if (m_scope == QLatin1String("shelf"))
        refreshShelfLabel();
    const bool nowWholeStudy = wholeBookStudyActive();
    if (studyMode() && wasWholeStudy != nowWholeStudy && m_reader && m_reader->bookId() > 0) {
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_recapText.clear();
        m_recapLoaded = false;
        m_recapChapter = -1;
        m_recapOffset = -1;
        loadWhosWhoSnapshot(m_reader->bookId(), m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        loadRecapSnapshot(m_reader->bookId(), m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        emit recapChanged();
        emit changed();
    }
    emit scopeChanged();
    emit conversationChanged();
}

void Companion::loadBookState(int bookId) {
    if (bookId <= 0)
        return;
    const QString savedMode = loadSavedReadingMode(bookId);
    m_readingMode = savedMode.isEmpty() ? detectCurrentBookMode() : savedMode;
    m_scope = studyMode() ? QStringLiteral("book") : QStringLiteral("upto");
    refreshShelfLabel();
    loadConversation(bookId);
    if (loadWhosWhoSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false) &&
        m_where.isEmpty()) {
        m_where = m_peopleWhere;
        m_whereLocal = m_peopleWhereLocal;
    }
    loadRecapSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
}

void Companion::saveConversation(int bookId) {
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    const bool inTx = db.transaction();
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
    del.addBindValue(bookId);
    bool ok = del.exec();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < m_conversation.size() && ok; ++i) {
        const Turn &turn = m_conversation[i];
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO companion_threads "
            "(book_id, seq, from_user, text, sources, provenance, is_local, snip_url, chapter, offset, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        ins.addBindValue(bookId);
        ins.addBindValue(i);
        ins.addBindValue(turn.fromUser ? 1 : 0);
        ins.addBindValue(turn.text.isNull() ? QStringLiteral("") : turn.text);
        ins.addBindValue(QString::fromUtf8(QJsonDocument(citationsToJson(turn.sources)).toJson(QJsonDocument::Compact)));
        ins.addBindValue(turn.provenance.isNull() ? QStringLiteral("") : turn.provenance);
        ins.addBindValue(turn.local ? 1 : 0);
        ins.addBindValue(turn.fromUser && !turn.snipDataUrl.isEmpty() ? turn.snipDataUrl : QStringLiteral(""));
        ins.addBindValue(turn.chapter);
        ins.addBindValue(turn.offset);
        ins.addBindValue(now);
        ok = ins.exec();
    }
    if (inTx)
        ok ? db.commit() : db.rollback();
}

void Companion::loadConversation(int bookId) {
    m_conversation.clear();
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT from_user, text, sources, provenance, is_local, snip_url, chapter, offset FROM companion_threads "
        "WHERE book_id = ? ORDER BY seq ASC, id ASC"));
    query.addBindValue(bookId);
    if (!query.exec())
        return;
    while (query.next()) {
        Turn turn;
        turn.fromUser = query.value(0).toInt() != 0;
        turn.text = query.value(1).toString();
        turn.sources = citationsFromJson(query.value(2).toString().toUtf8());
        turn.provenance = query.value(3).toString();
        turn.local = query.value(4).toInt() != 0;
        const QString savedSnip = query.value(5).toString();
        if (turn.fromUser && savedSnip.startsWith(QLatin1String("data:image/png;base64,")))
            turn.snipDataUrl = savedSnip;
        turn.chapter = query.value(6).toInt();
        turn.offset = query.value(7).toLongLong();
        m_conversation.append(std::move(turn));
    }
}

bool Companion::isTurnVisibleAtCurrentPlace(const Turn &turn) const {
    if (wholeBookStudyActive() || m_scope != QLatin1String("upto") || !m_reader || turn.chapter < 0)
        return true;
    const int ch = m_reader->chapter();
    if (turn.chapter > ch)
        return false;
    if (turn.chapter == ch) {
        const qint64 end = m_reader->pageEnd();
        if (end > 0 && turn.offset > 0 && turn.offset > end)
            return false;
    }
    return true;
}

void Companion::pruneFutureTurnsIfSpoilerFree() {
    if (wholeBookStudyActive() || m_scope != QLatin1String("upto") || !m_reader)
        return;
    QList<Turn> kept;
    kept.reserve(m_conversation.size());
    for (const Turn &turn : std::as_const(m_conversation))
        if (isTurnVisibleAtCurrentPlace(turn))
            kept.append(turn);
    if (kept.size() != m_conversation.size())
        m_conversation = std::move(kept);
}

bool Companion::loadWhosWhoSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter) {
    if (bookId <= 0)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    const QString mode = m_readingMode;
    const bool wholeStudy = wholeBookStudyActive();
    QSqlQuery query(db);
    if (wholeStudy) {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, people FROM companion_whoswho "
            "WHERE book_id = ? AND mode = ? AND chapter = -1 ORDER BY updated_at DESC LIMIT 1"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    } else {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, people FROM companion_whoswho "
            "WHERE book_id = ? AND mode = ? AND chapter >= 0 ORDER BY chapter DESC, offset DESC"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    }
    if (!query.exec())
        return false;
    while (query.next()) {
        const int savedChapter = query.value(0).toInt();
        const qint64 savedOffset = query.value(1).toLongLong();
        const qint64 savedStart = query.value(2).toLongLong();
        if (!wholeStudy) {
            // Strictly spoiler-safe: never restore a checkpoint from ahead of the reader.
            if (savedChapter > chapter)
                continue;
            if (requireSameChapter && savedChapter != chapter)
                continue;
            if (savedChapter == chapter) {
                if (pageEnd > 0) {
                    if (savedOffset > pageEnd)
                        continue;
                } else if (savedStart > pageStart) {
                    continue;
                }
            }
        }
        const QJsonArray arr = QJsonDocument::fromJson(query.value(5).toString().toUtf8()).array();
        QVariantList loaded;
        loaded.reserve(arr.size());
        for (const QJsonValue &val : arr)
            if (val.isObject())
                loaded.append(val.toObject().toVariantMap());
        m_people = loaded;
        m_peopleLoaded = true;
        m_peopleChapter = savedChapter;
        m_peopleOffset = savedOffset;
        m_peopleWhere = query.value(3).toString();
        const int savedLocal = query.value(4).toInt();
        m_peopleWhereLocal = savedLocal >= 0
                                 ? (savedLocal != 0)
                                 : m_peopleWhere.contains(QLatin1String("this computer"), Qt::CaseInsensitive);
        m_peopleKey = QStringLiteral("%1:%2:%3:%4:%5").arg(bookId).arg(mode).arg(savedChapter).arg(savedOffset).arg(m_peopleWhere);
        return true;
    }
    return false;
}

void Companion::saveWhosWhoSnapshot(int bookId, int chapter, qint64 offset, qint64 pageStart, const QString &whereLabel,
                                    bool isLocal, const QVariantList &people) {
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QJsonArray arr;
    for (const QVariant &v : people)
        arr.append(QJsonObject::fromVariantMap(v.toMap()));
    const QString json = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString mode = m_readingMode;
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO companion_whoswho "
        "(book_id, mode, chapter, offset, page_start, where_label, is_local, people, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(bookId);
    ins.addBindValue(mode);
    ins.addBindValue(chapter);
    ins.addBindValue(offset);
    ins.addBindValue(pageStart);
    ins.addBindValue(whereLabel);
    ins.addBindValue(isLocal ? 1 : 0);
    ins.addBindValue(json);
    ins.addBindValue(now);
    if (ins.exec()) {
        // Keep the 12 most recent checkpoints per (book, mode).
        QSqlQuery prune(db);
        prune.prepare(QStringLiteral(
            "DELETE FROM companion_whoswho WHERE book_id = ? AND mode = ? AND rowid NOT IN ("
            "SELECT rowid FROM companion_whoswho WHERE book_id = ? AND mode = ? ORDER BY updated_at DESC LIMIT 12)"));
        prune.addBindValue(bookId);
        prune.addBindValue(mode);
        prune.addBindValue(bookId);
        prune.addBindValue(mode);
        prune.exec();
    }
}

bool Companion::loadRecapSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter) {
    if (bookId <= 0)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    const QString mode = m_readingMode;
    const bool wholeStudy = wholeBookStudyActive();
    QSqlQuery query(db);
    if (wholeStudy) {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, text FROM companion_recaps "
            "WHERE book_id = ? AND mode = ? AND chapter = -1 ORDER BY updated_at DESC LIMIT 1"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    } else {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, text FROM companion_recaps "
            "WHERE book_id = ? AND mode = ? AND chapter >= 0 ORDER BY chapter DESC, offset DESC"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    }
    if (!query.exec())
        return false;
    while (query.next()) {
        const int savedChapter = query.value(0).toInt();
        const qint64 savedOffset = query.value(1).toLongLong();
        const qint64 savedStart = query.value(2).toLongLong();
        if (!wholeStudy) {
            // Strictly spoiler-safe: never restore a recap from ahead of the reader.
            if (savedChapter > chapter)
                continue;
            if (requireSameChapter && savedChapter != chapter)
                continue;
            if (savedChapter == chapter) {
                if (pageEnd > 0) {
                    if (savedOffset > pageEnd)
                        continue;
                } else if (savedStart > pageStart) {
                    continue;
                }
            }
        }
        const QString savedText = query.value(5).toString();
        if (savedText.trimmed().isEmpty())
            continue;
        m_recapText = savedText;
        m_recapLoaded = true;
        m_recapChapter = savedChapter;
        m_recapOffset = savedOffset;
        m_recapWhere = query.value(3).toString();
        m_recapWhereLocal = query.value(4).toInt() != 0;
        return true;
    }
    return false;
}

void Companion::saveRecapSnapshot(int bookId, int chapter, qint64 offset, qint64 pageStart, const QString &whereLabel,
                                  bool isLocal, const QString &text) {
    if (bookId <= 0 || text.trimmed().isEmpty())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString mode = m_readingMode;
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO companion_recaps "
        "(book_id, mode, chapter, offset, page_start, where_label, is_local, text, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(bookId);
    ins.addBindValue(mode);
    ins.addBindValue(chapter);
    ins.addBindValue(offset);
    ins.addBindValue(pageStart);
    ins.addBindValue(whereLabel);
    ins.addBindValue(isLocal ? 1 : 0);
    ins.addBindValue(text);
    ins.addBindValue(now);
    if (ins.exec()) {
        QSqlQuery prune(db);
        prune.prepare(QStringLiteral(
            "DELETE FROM companion_recaps WHERE book_id = ? AND mode = ? AND rowid NOT IN ("
            "SELECT rowid FROM companion_recaps WHERE book_id = ? AND mode = ? ORDER BY updated_at DESC LIMIT 12)"));
        prune.addBindValue(bookId);
        prune.addBindValue(mode);
        prune.addBindValue(bookId);
        prune.addBindValue(mode);
        prune.exec();
    }
}

void Companion::clearBookHistory(int bookId) {
    const int target = bookId > 0 ? bookId : (m_reader ? m_reader->bookId() : 0);
    if (target <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (db.isOpen()) {
        QSqlQuery q1(db);
        q1.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
        q1.addBindValue(target);
        q1.exec();
        QSqlQuery q2(db);
        q2.prepare(QStringLiteral("DELETE FROM companion_whoswho WHERE book_id = ?"));
        q2.addBindValue(target);
        q2.exec();
        QSqlQuery q3(db);
        q3.prepare(QStringLiteral("DELETE FROM companion_recaps WHERE book_id = ?"));
        q3.addBindValue(target);
        q3.exec();
    }
    if (m_reader && target == m_reader->bookId()) {
        m_conversation.clear();
        m_currentSources.clear();
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_peopleWhereLocal = false;
        m_recapText.clear();
        m_recapLoaded = false;
        m_recapChapter = -1;
        m_recapOffset = -1;
        m_recapWhere.clear();
        m_recapWhereLocal = false;
        m_selectionFeature.clear();
        m_selectionSubject.clear();
        m_selectionAnswer.clear();
        m_selectionWhere.clear();
        m_selectionWhereLocal = false;
        emit changed();
        emit answerChanged();
        emit conversationChanged();
        emit recapChanged();
        emit selectionChanged();
    }
}

QVariantList Companion::conversation() const {
    QVariantList out;
    out.reserve(m_conversation.size());
    for (const Turn &turn : m_conversation) {
        if (!isTurnVisibleAtCurrentPlace(turn))
            continue;
        QVariantMap entry{{QStringLiteral("fromUser"), turn.fromUser},
                          {QStringLiteral("text"), turn.text},
                          {QStringLiteral("sources"), citationsToVariant(turn.sources)},
                          {QStringLiteral("provenance"), turn.provenance},
                          {QStringLiteral("local"), turn.local}};
        if (turn.fromUser && !turn.snipDataUrl.isEmpty())
            entry.insert(QStringLiteral("snipUrl"), turn.snipDataUrl);
        out.append(entry);
    }
    return out;
}

QStringList Companion::suggestions() const {
    QStringList askedLower;
    for (const Turn &turn : m_conversation)
        if (turn.fromUser && isTurnVisibleAtCurrentPlace(turn))
            askedLower.append(turn.text.trimmed().toLower());

    QStringList out;
    auto addIfFresh = [&](const QString &prompt) {
        const QString clean = prompt.trimmed();
        if (clean.isEmpty() || out.size() >= 2)
            return;
        const QString low = clean.toLower();
        for (const QString &prev : askedLower)
            if (prev == low)
                return;
        if (!out.contains(clean, Qt::CaseInsensitive))
            out.append(clean);
    };

    if (m_scope == QLatin1String("shelf")) {
        addIfFresh(tr("Compare the key arguments across this shelf"));
        addIfFresh(tr("How does this document connect to others on my shelf?"));
        addIfFresh(tr("What methods or findings overlap across these documents?"));
        return out;
    }

    if (studyMode()) {
        for (const QVariant &entry : m_people) {
            const QString name = entry.toMap().value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty()) {
                const QString candidate = tr("Explain %1 in context").arg(name);
                const QString lowName = name.toLower();
                bool mentioned = false;
                for (const QString &prev : askedLower) {
                    if (prev.contains(lowName)) {
                        mentioned = true;
                        break;
                    }
                }
                if (!mentioned) {
                    addIfFresh(candidate);
                    break;
                }
            }
        }
        addIfFresh(tr("Summarize the core claims and methodology"));
        addIfFresh(tr("What are the main findings and limitations?"));
        addIfFresh(tr("Explain the key equations and notation"));
        return out;
    }

    // 1. Character prompt from already-loaded spoiler-safe Who's who entries.
    for (const QVariant &entry : m_people) {
        const QString name = entry.toMap().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            const QString candidate = tr("Who is %1?").arg(name);
            const QString lowName = name.toLower();
            bool mentioned = false;
            for (const QString &prev : askedLower) {
                if (prev.contains(lowName)) {
                    mentioned = true;
                    break;
                }
            }
            if (!mentioned) {
                addIfFresh(candidate);
                break;
            }
        }
    }

    // 2. Chapter-specific prompt when a readable chapter title is present.
    if (m_reader) {
        const QString chapterTitle = m_reader->chapterTitle().trimmed();
        if (!chapterTitle.isEmpty() && chapterTitle.size() <= 40)
            addIfFresh(tr("What has happened in %1 so far?").arg(chapterTitle));
    }

    // 3. Clean general fallbacks grounded in what has been read so far.
    addIfFresh(tr("Who are the key people so far?"));
    addIfFresh(tr("Summarize what has happened so far"));
    addIfFresh(tr("What should I remember from the last few pages?"));
    return out;
}

QVariantList Companion::currentSources() const { return citationsToVariant(m_currentSources); }

QString Companion::targetLanguage() const {
    if (!m_targetLanguage.isEmpty())
        return m_targetLanguage;
    const QString sys = readerLanguage();
    return sys.isEmpty() || sys == QLatin1String("C") ? QStringLiteral("English") : sys;
}

void Companion::setTargetLanguage(const QString &language) {
    const QString cleaned = language.trimmed();
    if (cleaned.isEmpty() || cleaned == targetLanguage())
        return;
    m_targetLanguage = cleaned;
    companionSettings().setValue(QStringLiteral("companion/translateLanguage"), m_targetLanguage);
    emit targetLanguageChanged();
}

QStringList Companion::targetLanguages() const {
    static const QStringList common = {
        QStringLiteral("English"),    QStringLiteral("Spanish"),  QStringLiteral("French"),
        QStringLiteral("German"),     QStringLiteral("Italian"),  QStringLiteral("Portuguese"),
        QStringLiteral("Dutch"),      QStringLiteral("Russian"),  QStringLiteral("Ukrainian"),
        QStringLiteral("Polish"),     QStringLiteral("Swedish"),  QStringLiteral("Turkish"),
        QStringLiteral("Greek"),      QStringLiteral("Arabic"),   QStringLiteral("Hindi"),
        QStringLiteral("Chinese"),    QStringLiteral("Japanese"), QStringLiteral("Korean"),
        QStringLiteral("Latin"),
    };
    QStringList out;
    const QString sys = readerLanguage();
    if (!sys.isEmpty() && sys != QLatin1String("C"))
        out.append(sys);
    if (!m_targetLanguage.isEmpty() && !out.contains(m_targetLanguage))
        out.append(m_targetLanguage);
    for (const QString &lang : common)
        if (!out.contains(lang))
            out.append(lang);
    return out;
}

QString Companion::cleanMarkdown(const QString &text) {
    if (text.isEmpty())
        return text;
    // 1. Iteratively strip dangerous raw HTML tags and Markdown image embeds to fixed point.
    const QString pre = stripDangerousMarkup(text);

    // 2. Replace ONLY genuine LaTeX produced by our own math formatter with per-call
    //    unforgeable random nonce placeholders BEFORE escaping raw HTML.
    const QString nonce = generateMathNonce(pre);
    QStringList mathInners;
    QList<bool> mathIsBlock;
    QString out = extractLatexWithNonces(pre, nonce, mathInners, mathIsBlock, false);

    // 3. Escape all remaining raw HTML angle brackets outside code fences/spans so
    //    untrusted prose can never inject raw tags into MarkdownText or RichText.
    out = escapeRawHtmlOutsideCode(out);

    // 4. Substitute ONLY our own generated math HTML back for the per-call nonce tokens.
    for (int i = 0; i < mathInners.size(); ++i) {
        const QString token =
            QStringLiteral("%1%2%3Z").arg(nonce, mathIsBlock[i] ? QStringLiteral("B") : QStringLiteral("I"), QString::number(i));
        const QString rendered =
            mathIsBlock[i]
                ? (QStringLiteral(
                       "<div class=\"math-block\" style=\"font-family:'Literata',Georgia,serif;font-style:italic;"
                       "background-color:rgba(128,128,128,0.12);padding:8px 12px;margin:6px 0;text-align:center;\">") +
                   mathInners[i] + QStringLiteral("</div>"))
                : (QStringLiteral(
                       "<span class=\"math-inline\" style=\"font-family:'Literata',Georgia,serif;font-style:italic;\">") +
                   mathInners[i] + QStringLiteral("</span>"));
        out.replace(token, rendered);
    }

    out.replace(QStringLiteral("!["), QStringLiteral("!\u200B["));
    return out;
}

bool Companion::hasMath(const QString &text) const {
    if (text.isEmpty())
        return false;
    if (!text.contains(QLatin1Char('$')) && !text.contains(QStringLiteral("\\(")) &&
        !text.contains(QStringLiteral("\\[")))
        return false;
    const QString pre = stripDangerousMarkup(text);
    const QString nonce = generateMathNonce(pre);
    QStringList mathInners;
    QList<bool> mathIsBlock;
    (void)extractLatexWithNonces(pre, nonce, mathInners, mathIsBlock, false);
    return !mathInners.isEmpty();
}

QString Companion::formatDisplayHtml(const QString &text, const QString &mathBlockBg) const {
    if (text.isEmpty())
        return {};

    // 1. Strip dangerous tags and Markdown images iteratively to fixed point.
    const QString pre = stripDangerousMarkup(text);

    // 2. Generate an unforgeable per-call random nonce and extract ONLY genuine LaTeX
    //    rendered by Clothbound's own LatexParser — NEVER hoisting literal
    //    <div class="math-block"> or <span class="math-inline"> from untrusted input.
    const QString nonce = generateMathNonce(pre);
    QStringList mathInners;
    QList<bool> mathIsBlock;
    QString md = extractLatexWithNonces(pre, nonce, mathInners, mathIsBlock, true);

    // 3. Escape all remaining raw HTML in untrusted Markdown BEFORE QTextDocument::setMarkdown().
    md = escapeRawHtmlOutsideCode(md);

    QTextDocument doc;
    doc.setMarkdown(md);
    QString html = doc.toHtml();

    // Extract inner body HTML so Qt Quick TextEdit inherits the panel's exact font family, size, and ink color.
    const qsizetype bodyOpen = html.indexOf(QStringLiteral("<body"));
    if (bodyOpen >= 0) {
        const qsizetype bodyStart = html.indexOf(QLatin1Char('>'), bodyOpen);
        const qsizetype bodyEnd = html.lastIndexOf(QStringLiteral("</body>"));
        if (bodyStart >= 0 && bodyEnd > bodyStart)
            html = html.mid(bodyStart + 1, bodyEnd - bodyStart - 1).trimmed();
    }
    // Defense-in-depth: iteratively strip any <img>, active links <a>, and unsafe resource attributes.
    static const QRegularExpression imgRx(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression linkHrefRx(QStringLiteral("<a\\b[^>]*>([\\s\\S]*?)</a>"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression strayLinkRx(QStringLiteral("</?a\\b[^>]*>"),
                                                QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unsafeAttrRx(
        QStringLiteral("\\s(?:href|src|background)\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    for (int pass = 0; pass < 32; ++pass) {
        const QString before = html;
        html.remove(imgRx);
        html.replace(linkHrefRx, QStringLiteral("\\1"));
        html.remove(strayLinkRx);
        html.remove(unsafeAttrRx);
        if (html == before)
            break;
    }

    // For single-paragraph snippets (e.g. Glossary definitions), remove default 12px paragraph margins.
    if (html.count(QStringLiteral("<p")) == 1 && !mathIsBlock.contains(true)) {
        static const QRegularExpression singlePMargin(QStringLiteral("margin-top:\\s*\\d+px;\\s*margin-bottom:\\s*\\d+px;"));
        html.replace(singlePMargin, QStringLiteral("margin-top:0px; margin-bottom:0px;"));
    }

    const QString safeBgColor = sanitizeMathBgColor(mathBlockBg);
    for (int i = 0; i < mathInners.size(); ++i) {
        const QString token =
            QStringLiteral("%1%2%3Z").arg(nonce, mathIsBlock[i] ? QStringLiteral("B") : QStringLiteral("I"), QString::number(i));
        if (mathIsBlock[i]) {
            // Qt's QTextDocument honors table cellpadding & td background-color for crisp tinted blocks.
            const QString snippet =
                QStringLiteral(
                    "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" "
                    "style=\"margin-top:6px;margin-bottom:6px;\">"
                    "<tr><td align=\"center\" style=\"background-color:%1;font-family:'Literata',Georgia,serif;"
                    "font-style:italic;\">%2</td></tr>"
                    "</table>")
                    .arg(safeBgColor, mathInners[i]);
            const QRegularExpression pWrap(QStringLiteral("<p[^>]*>\\s*") + QRegularExpression::escape(token) +
                                           QStringLiteral("\\s*</p>"));
            if (pWrap.match(html).hasMatch())
                html.replace(pWrap, snippet);
            else
                html.replace(token, snippet);
        } else {
            const QString snippet =
                QStringLiteral("<span style=\"font-family:'Literata',Georgia,serif;font-style:italic;\">%1</span>")
                    .arg(mathInners[i]);
            html.replace(token, snippet);
        }
    }
    return html;
}

QString Companion::readerLanguage() {
    return QLocale::languageToString(QLocale().language());
}

QList<Companion::Chapter> Companion::textUpTo(const QString &path, const QString &format, int chapter, qint64 offset,
                                              qint64 limit, std::shared_ptr<std::atomic_bool> cancel) {
    QList<Chapter> out;
    if (chapter < 0 || path.isEmpty())
        return out;
    const auto cancelled = [&cancel] { return cancel && cancel->load(); };
    if (format == QLatin1String("pdf")) {
        PdfBook book(path);
        if (!book.open())
            return out;
        const int last = qMin(chapter, book.pageCount() - 1);
        if (last < 0)
            return out;
        if (limit > 0) {
            qint64 collected = 0;
            for (int page = last; page >= 0 && !cancelled(); --page) {
                QString text = book.document()->getAllText(page).text();
                // For PDFs, pageEnd() is 0 (full visible page); only slice on an explicit positive offset.
                if (page == chapter && offset > 0)
                    text = text.left(offset);
                out.append({page, QCoreApplication::translate("Companion", "Page %1").arg(page + 1), text});
                collected += text.trimmed().size();
                if (collected >= limit + 512)
                    break;
            }
            if (cancelled())
                return {};
            std::reverse(out.begin(), out.end());
            return out;
        }
        for (int page = 0; page <= last && !cancelled(); ++page) {
            QString text = book.document()->getAllText(page).text();
            if (page == chapter && offset > 0)
                text = text.left(offset);
            out.append({page, QCoreApplication::translate("Companion", "Page %1").arg(page + 1), text});
        }
        return cancelled() ? QList<Chapter>{} : out;
    }
    EpubBook book(path);
    if (!book.open())
        return out;
    const int last = qMin(chapter, int(book.spine().size()) - 1);
    if (last < 0)
        return out;
    if (limit > 0) {
        qint64 collected = 0;
        for (int i = last; i >= 0 && !cancelled(); --i) {
            QString text = book.chapterText(i);
            if (i == chapter && offset >= 0)
                text = text.left(offset);
            if (!text.trimmed().isEmpty()) {
                out.append({i, QString(), text});
                collected += text.trimmed().size();
                if (collected >= limit + 512)
                    break;
            }
        }
        if (cancelled())
            return {};
        std::reverse(out.begin(), out.end());
        return out;
    }
    for (int i = 0; i <= last && !cancelled(); ++i) {
        QString text = book.chapterText(i);
        if (i == chapter && offset >= 0)
            text = text.left(offset);
        if (!text.trimmed().isEmpty())
            out.append({i, QString(), text});
    }
    return cancelled() ? QList<Chapter>{} : out;
}

QList<Companion::Chapter> Companion::textOfBook(const QString &path, const QString &format, qint64 limit,
                                                std::shared_ptr<std::atomic_bool> cancel) {
    QList<Chapter> out;
    if (path.isEmpty())
        return out;
    const auto cancelled = [&cancel] { return cancel && cancel->load(); };
    if (format == QLatin1String("pdf")) {
        PdfBook book(path);
        if (!book.open())
            return out;
        const int count = book.pageCount();
        qint64 collected = 0;
        for (int page = 0; page < count && !cancelled(); ++page) {
            const QString text = book.document()->getAllText(page).text();
            out.append({page, QCoreApplication::translate("Companion", "Page %1").arg(page + 1), text});
            if (limit > 0) {
                collected += text.trimmed().size();
                if (collected >= limit + 512)
                    break;
            }
        }
        return cancelled() ? QList<Chapter>{} : out;
    }
    EpubBook book(path);
    if (!book.open())
        return out;
    const int count = int(book.spine().size());
    qint64 collected = 0;
    for (int i = 0; i < count && !cancelled(); ++i) {
        const QString text = book.chapterText(i);
        if (!text.trimmed().isEmpty()) {
            out.append({i, QString(), text});
            if (limit > 0) {
                collected += text.trimmed().size();
                if (collected >= limit + 512)
                    break;
            }
        }
    }
    return cancelled() ? QList<Chapter>{} : out;
}

QString Companion::joined(const QList<Chapter> &chapters, qint64 limit) {
    QString out;
    for (qsizetype i = chapters.size() - 1; i >= 0 && out.size() < limit; --i) {
        const Chapter &chapter = chapters[i];
        QString part = chapter.text.trimmed();
        if (out.size() + part.size() > limit)
            part = part.right(limit - out.size());
        const QString heading = chapter.title.isEmpty() ? QString() : QStringLiteral("## %1\n\n").arg(chapter.title);
        out.prepend(heading + part + QStringLiteral("\n\n"));
    }
    return out.trimmed();
}

QStringList Companion::dictionary(const QString &word) {
    if (word.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty())
        return {};
    QProcess sdcv;
    sdcv.start(QStringLiteral("sdcv"),
               {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), word});
    if (!sdcv.waitForFinished(4000)) {
        sdcv.kill();
        sdcv.waitForFinished(500);
        return {};
    }
    if (sdcv.exitStatus() != QProcess::NormalExit || sdcv.exitCode() != 0)
        return {};
    return parseSdcvJson(sdcv.readAllStandardOutput(), word);
}

QString Companion::bookLine() const {
    return QStringLiteral("Book: %1, by %2.").arg(m_reader->title(), m_reader->author());
}

QString Companion::placeTitle(int chapter) const {
    const QString title = m_reader->titleOfChapter(chapter);
    if (!title.isEmpty())
        return title;
    if (m_reader->format() == QLatin1String("pdf")) {
        const QString lbl = m_reader->pdfPageLabel(chapter);
        return tr("Page %1").arg(lbl.isEmpty() ? QString::number(chapter + 1) : lbl);
    }
    return tr("Chapter %1").arg(chapter + 1);
}

bool Companion::cloudFor(const QString &feature) const {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    return provider && provider->id() != QLatin1String("local");
}

QString Companion::formatProvenance(const Ai::Provider *provider, const Ai::Reply *reply) const {
    if (!provider)
        return m_where;
    if (provider->isLocal()) {
        const QString rawModel = reply && !reply->model().isEmpty() ? reply->model() : m_ai->localTextModel();
        const QString title = AiServices::friendlyModelTitle(rawModel);
        return title.isEmpty() ? tr("On this computer · %1").arg(provider->name())
                               : tr("On this computer · %1").arg(title);
    }
    if (provider->id() == QLatin1String("local")) {
        const QString rawModel = reply && !reply->model().isEmpty() ? reply->model() : m_ai->localTextModel();
        const QString title = AiServices::friendlyModelTitle(rawModel);
        return title.isEmpty() ? tr("Sent to %1").arg(provider->name())
                               : tr("Sent to %1 · %2").arg(provider->name(), title);
    }
    const double dollars = reply ? AiUsageLedger::cost(reply->model(), reply->usage(), QDate::currentDate(), false) : 0.0;
    const QString costText = dollars < 0.01 ? tr("less than 1¢")
                           : dollars < 1.0  ? tr("about %1¢").arg(qMax(1, qRound(dollars * 100)))
                                            : QStringLiteral("$%1").arg(dollars, 0, 'f', 2);
    return tr("Gemini · sent to Google · %1").arg(costText);
}

qint64 Companion::localCharBudget(const QString &feature, bool fast, qint64 fallbackChars, double charsPerToken) const {
    Ai::TextModel *model = m_ai->textFor(feature);
    const int ctxTokens = model ? model->contextWindow(fast) : 0;
    if (ctxTokens <= 0)
        return fallbackChars;
    // Reserve headroom for instructions, JSON schema, and generated output.
    const int reserved = qMax(1024, ctxTokens / 4);
    const int usableTokens = qMax(512, ctxTokens - reserved);
    const qint64 budgeted = qint64(double(usableTokens) * charsPerToken);
    return qMin(fallbackChars, budgeted);
}

QString Companion::pendingSetupLabel() const {
    if (m_pendingSetup.feature.isEmpty())
        return {};
    if (m_pendingSetup.feature == QLatin1String("ask"))
        return tr("“%1”").arg(makeExcerpt(m_pendingSetup.text, 64));
    if (m_pendingSetup.feature == QLatin1String("snip"))
        return m_pendingSetup.text.isEmpty() ? tr("Snipped region") : tr("“%1”").arg(makeExcerpt(m_pendingSetup.text, 64));
    if (m_pendingSetup.feature == QLatin1String("define"))
        return tr("Define “%1”").arg(makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("explain"))
        return tr("Explain “%1”").arg(makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("translate"))
        return tr("Translate “%1”").arg(makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("recap"))
        return studyMode() ? tr("Study brief") : tr("Previously…");
    if (m_pendingSetup.feature == QLatin1String("whoswho"))
        return studyMode() ? tr("Glossary") : tr("Who's who");
    return {};
}

Ai::TextModel *Companion::modelFor(const QString &feature) {
    Ai::TextModel *model = m_ai->textFor(feature);
    if (!model) {
        if (!m_ai->hasKey() && !m_ai->hasLocal() && !m_lastAction.feature.isEmpty())
            m_pendingSetup = m_lastAction;
        fail(m_ai->preset() == QLatin1String("local") || m_ai->overrideFor(feature) == QLatin1String("local")
                 ? tr("This needs a model on this computer: set one up in Intelligence (✦).")
                 : tr("This needs a Gemini key or a model on this computer, in Intelligence (✦)."),
             true);
    }
    return model;
}

bool Companion::checkBudget(const QString &feature, qint64 charsEstimate, bool fast, qint64 outputTokens) {
    m_lastCheckedCost = -1.0;
    if (!cloudFor(feature))
        return true;
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    Ai::TextModel *model = provider ? provider->text() : nullptr;
    if (!model)
        return false;
    Ai::Usage usage;
    usage.inputTokens = qMax<qint64>(1, charsEstimate / 4);
    usage.outputTokens = outputTokens;
    usage.thoughtTokens = fast ? qMin<qint64>(outputTokens, 128) : qMin<qint64>(outputTokens, 400);
    const double cost = AiUsageLedger::costForProvider(provider->id(), model->textModelName(fast), usage,
                                                       QDate::currentDate(), false);
    if (!m_ai->fitsBudget(cost)) {
        fail(m_ai->describe(int(Ai::Error::Budget)), true);
        return false;
    }
    m_lastCheckedCost = cost;
    return true;
}

qint64 Companion::soFarCharsEstimate() const {
    qint64 soFar = m_reader ? m_reader->pageEnd() : 0;
    if (m_reader) {
        for (int i = 0; i < m_reader->chapter(); ++i)
            soFar += m_reader->charactersIn(i);
        // While chapter lengths are still being measured, fall back to a conservative
        // estimate so the cost readout and budget check never under-report as $0.
        if (soFar <= 0 && m_reader->chapter() > 0)
            soFar = qint64(m_reader->chapter() + 1) * 12000;
    }
    return qMin<qint64>(cloudLimit, soFar);
}

qint64 Companion::wholeBookCharsEstimate() const {
    if (!m_reader)
        return 0;
    qint64 total = 0;
    const int count = m_reader->chapterCount();
    for (int i = 0; i < count; ++i)
        total += m_reader->charactersIn(i);
    if (total <= 0)
        total = qMax<qint64>(soFarCharsEstimate(), qint64(qMax(1, count)) * (m_reader->format() == QLatin1String("pdf") ? 2800 : 12000));
    return qMin<qint64>(cloudLimit, total);
}

void Companion::flushStreamedAnswer() {
    m_streamFlush.stop();
    if (!m_streamDirty)
        return;
    m_streamDirty = false;
    emit answerChanged();
}

void Companion::begin(const QString &feature, const QString &subject) {
    stop();
    m_feature = feature;
    m_subject = subject;
    m_answer.clear();
    m_currentSources.clear();
    m_problem.clear();
    m_problemIsConfig = false;
    m_busy = true;
    m_streamDirty = false;
    m_streamNotifiedFirst = false;
    m_requestChapter = m_reader ? m_reader->chapter() : -1;
    m_requestStart = m_reader ? m_reader->pageStart() : -1;
    m_requestOffset = m_reader ? m_reader->pageEnd() : -1;
    m_where = m_ai->whereFor(feature);
    m_whereLocal = (m_ai->routeFor(feature) == QLatin1String("local") && m_ai->localIsLoopback());
    m_status = tr("Thinking…");
    emit changed();
    emit answerChanged();
}

void Companion::setStatus(const QString &status) {
    m_status = status;
    emit changed();
}

void Companion::rollbackPendingTurn() {
    if (!m_pendingUserTurn)
        return;
    m_pendingUserTurn = false;
    if (!m_answer.trimmed().isEmpty()) {
        const bool stamp = !wholeBookStudyActive() && m_scope == QLatin1String("upto") && m_requestChapter >= 0;
        m_conversation.append({false, m_answer, m_currentSources, m_where, m_whereLocal, {},
                               stamp ? m_requestChapter : -1, stamp ? m_requestOffset : -1});
        m_answer.clear();
        m_currentSources.clear();
        saveConversation(m_reader->bookId());
        emit conversationChanged();
        emit answerChanged();
    } else if (!m_conversation.isEmpty() && m_conversation.last().fromUser) {
        m_conversation.removeLast();
        m_answer.clear();
        m_currentSources.clear();
        emit conversationChanged();
        emit answerChanged();
    }
}

void Companion::fail(const QString &problem, bool isConfig) {
    flushStreamedAnswer();
    rollbackPendingTurn();
    m_problem = problem;
    m_problemIsConfig = isConfig;
    m_busy = false;
    m_requestChapter = -1;
    m_requestStart = -1;
    m_requestOffset = -1;
    emit changed();
}

void Companion::clearProblem() {
    if (m_problem.isEmpty() && !m_problemIsConfig)
        return;
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
}

void Companion::executeAction(const Action &action) {
    if (action.feature == QLatin1String("ask"))
        ask(action.text);
    else if (action.feature == QLatin1String("snip"))
        sendSnipRequest(action.snipPng, action.snipAction, action.text, action.chapter);
    else if (action.feature == QLatin1String("define"))
        define(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("explain"))
        explain(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("translate"))
        translate(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("recap"))
        refreshRecap();
    else if (action.feature == QLatin1String("whoswho"))
        refreshWhosWho();
}

void Companion::retry() {
    if (m_busy || m_lastAction.feature.isEmpty())
        return;
    const Action action = m_lastAction;
    executeAction(action);
}

void Companion::resumePendingSetup() {
    if (m_pendingSetup.feature.isEmpty())
        return;
    const Action action = m_pendingSetup;
    m_pendingSetup = {};
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
    executeAction(action);
}

void Companion::stop() {
    ++m_generation;
    m_streamFlush.stop();
    m_streamDirty = false;
    m_streamNotifiedFirst = false;
    m_requestChapter = -1;
    m_requestStart = -1;
    m_requestOffset = -1;
    m_lastCheckedCost = -1.0;
    if (m_readCancel) {
        *m_readCancel = true;
        m_readCancel.reset();
    }
    if (m_dictProcess) {
        if (m_dictProcess->state() != QProcess::NotRunning) {
            m_dictProcess->kill();
            m_dictProcess->waitForFinished(500);
        }
        m_dictProcess->deleteLater();
        m_dictProcess.clear();
    }
    m_index.cancel();
    if (m_reply) {
        QPointer<Ai::Reply> active = m_reply;
        m_reply.clear();
        if (active)
            active->abort();
    }
    rollbackPendingTurn();
    if (m_busy) {
        m_busy = false;
        emit changed();
    }
}

void Companion::readSoFar(int chapter, qint64 offset, qint64 limit, std::function<void(const QList<Chapter> &)> then) {
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, path, format, chapter, offset, limit, cancel, generation, then] {
        const QList<Chapter> chapters = textUpTo(path, format, chapter, offset, limit, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, generation, then] {
                if (!self || self->m_generation != generation)
                    return;
                // The titles the reader shows, from the contents.
                QList<Chapter> titled = chapters;
                for (Chapter &chapter : titled)
                    if (chapter.title.isEmpty())
                        chapter.title = self->m_reader->titleOfChapter(chapter.index);
                then(titled);
            },
            Qt::QueuedConnection);
    });
}

void Companion::readWholeBook(qint64 limit, std::function<void(const QList<Chapter> &)> then) {
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, path, format, limit, cancel, generation, then] {
        const QList<Chapter> chapters = textOfBook(path, format, limit, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, generation, then] {
                if (!self || self->m_generation != generation)
                    return;
                QList<Chapter> titled = chapters;
                for (Chapter &chapter : titled)
                    if (chapter.title.isEmpty())
                        chapter.title = self->m_reader->titleOfChapter(chapter.index);
                then(titled);
            },
            Qt::QueuedConnection);
    });
}

void Companion::run(const QString &feature, const Ai::TextRequest &request, std::function<void(Ai::Reply *)> done) {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    Ai::TextModel *model = provider ? provider->text() : nullptr;
    if (!model) {
        modelFor(feature);
        return;
    }
    m_where = m_ai->whereFor(feature);
    m_whereLocal = provider->isLocal();
    const bool hasImage = !request.imagePng.isEmpty();
    const double reserveCost = std::exchange(m_lastCheckedCost, -1.0);
    Ai::Reply *reply = model->generate(request);
    m_ai->track(reply, false, reserveCost);
    m_reply = reply;
    const int generation = m_generation;
    QPointer<Ai::Provider> providerGuard(provider);
    setStatus(tr("Writing…"));
    connect(reply, &Ai::Reply::textReceived, this, [this, reply, generation](const QString &delta) {
        if (reply != m_reply || generation != m_generation || delta.isEmpty())
            return;
        m_answer += delta;
        if (!m_streamNotifiedFirst) {
            m_streamNotifiedFirst = true;
            m_streamDirty = false;
            emit answerChanged();
        } else {
            m_streamDirty = true;
            if (!m_streamFlush.isActive())
                m_streamFlush.start();
        }
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, done, generation, providerGuard, feature, hasImage] {
        reply->deleteLater();
        if (reply != m_reply || generation != m_generation)
            return;
        m_reply.clear();
        flushStreamedAnswer();
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled) {
                const auto kind = reply->error().kind;
                const bool localVisionErr =
                    hasImage && providerGuard && providerGuard->id() == QLatin1String("local") &&
                    (kind == Ai::Error::Invalid || kind == Ai::Error::Unsupported) &&
                    reply->error().message == LocalModels::visionUnsupportedMessage();
                const bool isConfig = (localVisionErr || kind == Ai::Error::NoKey || kind == Ai::Error::BadKey ||
                                       kind == Ai::Error::Budget || kind == Ai::Error::NotFound ||
                                       (providerGuard && providerGuard->id() == QLatin1String("local") &&
                                        kind == Ai::Error::Offline));
                fail(localVisionErr ? reply->error().message
                                    : m_ai->describeFor(providerGuard, int(kind), reply->error().message),
                     isConfig);
            }
            return;
        }
        if (m_answer.isEmpty() && reply->result().isEmpty())
            m_answer = reply->text();
        if (!done && m_answer.trimmed().isEmpty()) {
            fail(tr("The model returned an empty answer. Try again."));
            return;
        }
        m_pendingSetup = {};
        m_where = formatProvenance(providerGuard, reply);
        m_whereLocal = providerGuard && providerGuard->isLocal();
        if (feature == QLatin1String("define") || feature == QLatin1String("explain") ||
            feature == QLatin1String("translate")) {
            m_selectionFeature = feature;
            m_selectionSubject = m_subject;
            m_selectionAnswer = m_answer;
            m_selectionWhere = m_where;
            m_selectionWhereLocal = m_whereLocal;
            emit selectionChanged();
        }
        if (done)
            done(reply);
        if (m_problem.isEmpty()) {
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit answerChanged();
            emit changed();
        }
    });
}

// --- Define, explain, translate ----------------------------------------------

void Companion::define(const QString &word, int chapter, qint64 offset) {
    const QString wanted = word.trimmed();
    m_lastAction = {QStringLiteral("define"), wanted, chapter, offset, {}, {}};
    begin(QStringLiteral("define"), wanted);
    const int generation = m_generation;
    const auto askModel = [this, wanted, chapter, offset, generation] {
        if (generation != m_generation)
            return;
        if (!modelFor(m_feature))
            return;
        if (!checkBudget(m_feature, 1600, true, 120))
            return;
        setStatus(tr("Reading the sentence…"));
        const qint64 selectionEnd = qMax<qint64>(0, offset) + wanted.size();
        qint64 end = selectionEnd;
        if (wholeBookStudyActive()) {
            end = selectionEnd + 400;
        } else if (m_reader && chapter == m_reader->chapter() && m_reader->pageEnd() > 0) {
            end = qMin<qint64>(selectionEnd + 400, qMax<qint64>(m_reader->pageEnd(), selectionEnd));
        } else if (m_reader && m_reader->format() == QLatin1String("pdf") && chapter <= m_reader->chapter()) {
            end = -1;
        }
        readSoFar(chapter, end, 2000, [this, wanted, generation](const QList<Chapter> &chapters) {
            if (generation != m_generation)
                return;
            const QString around = joined(chapters, 1200);
            Ai::TextRequest request;
            request.feature = QStringLiteral("define");
            request.fast = true;
            request.stream = true;
            const bool spoilerFree = !wholeBookStudyActive();
            request.system = instructions(
                studyMode()
                    ? QStringLiteral(
                          "You are an academic and technical reference dictionary. Format the first line as "
                          "*domain / part of speech* — concise one-line definition of how this term, acronym, or "
                          "symbol is used in this passage. Add one brief sentence on its significance if helpful. "
                          "Use LaTeX ($...$) for notation when needed. At most 70 words.")
                    : QStringLiteral(
                          "You are a literary dictionary. Format the first line as *part of speech* — concise one-line "
                          "definition for how the word is used in this passage. If the usage is archaic, rare, or "
                          "figurative, add one brief sentence after. At most 60 words."),
                spoilerFree);
            request.messages = {{true, bookLine() + QStringLiteral("\n\nPassage:\n") + around +
                                           QStringLiteral("\n\nWord: ") + wanted}};
            run(QStringLiteral("define"), request);
        });
    };

    // A dictionary on this computer answers first (asynchronously) and costs nothing.
    const QString lookupWord = cleanDictLookupWord(wanted);
    if (!lookupWord.isEmpty() && !lookupWord.contains(QLatin1Char(' ')) &&
        !QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty()) {
        setStatus(tr("Checking your dictionary…"));
        auto *sdcv = new QProcess(this);
        m_dictProcess = sdcv;
        auto handled = std::make_shared<bool>(false);
        auto *timer = new QTimer(sdcv);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, sdcv, [sdcv] {
            if (sdcv->state() != QProcess::NotRunning)
                sdcv->kill();
        });
        connect(sdcv, &QProcess::errorOccurred, this, [this, sdcv, generation, handled, askModel](QProcess::ProcessError) {
            if (*handled)
                return;
            *handled = true;
            if (sdcv == m_dictProcess)
                m_dictProcess.clear();
            sdcv->deleteLater();
            if (generation == m_generation)
                askModel();
        });
        connect(sdcv, &QProcess::finished, this,
                [this, sdcv, lookupWord, wanted, generation, handled, askModel](int code, QProcess::ExitStatus status) {
                    if (*handled)
                        return;
                    *handled = true;
                    if (sdcv == m_dictProcess)
                        m_dictProcess.clear();
                    const QByteArray output = sdcv->readAllStandardOutput();
                    sdcv->deleteLater();
                    if (generation != m_generation)
                        return;
                    const QStringList entries =
                        (status == QProcess::NormalExit && code == 0) ? parseSdcvJson(output, lookupWord) : QStringList();
                    if (!entries.isEmpty()) {
                        m_pendingSetup = {};
                        m_answer = entries.join(QStringLiteral("\n\n"));
                        m_where = tr("Dictionary on this computer");
                        m_whereLocal = true;
                        m_selectionFeature = QStringLiteral("define");
                        m_selectionSubject = wanted;
                        m_selectionAnswer = m_answer;
                        m_selectionWhere = m_where;
                        m_selectionWhereLocal = true;
                        m_busy = false;
                        m_requestChapter = -1;
                        m_requestOffset = -1;
                        emit selectionChanged();
                        emit answerChanged();
                        emit changed();
                        return;
                    }
                    askModel();
                });
        timer->start(4000);
        sdcv->start(QStringLiteral("sdcv"),
                    {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), lookupWord});
        return;
    }
    askModel();
}

void Companion::explain(const QString &text, int chapter, qint64 end) {
    const QString cleaned = text.trimmed();
    m_lastAction = {QStringLiteral("explain"), cleaned, chapter, end, {}, {}};
    begin(QStringLiteral("explain"), cleaned);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 charLimit = cloud ? 60000 : localCharBudget(m_feature, false, 12000);
    if (!checkBudget(m_feature, charLimit, false, 400))
        return;
    const int generation = m_generation;
    setStatus(tr("Reading up to here…"));
    readSoFar(chapter, end, charLimit, [this, cleaned, charLimit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const bool isCloud = cloudFor(QStringLiteral("explain"));
        const qint64 limit = isCloud ? 60000 : qMin(charLimit, localCharBudget(QStringLiteral("explain"), false, 12000));
        const QString context = joined(chapters, limit);
        if (!checkBudget(QStringLiteral("explain"), context.size() + cleaned.size(), false, 400))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("explain");
        request.stream = true;
        const bool spoilerFree = !wholeBookStudyActive();
        request.system = instructions(
            studyMode()
                ? QStringLiteral(
                      "Explain the selected passage, equation, or claim clearly for a reader studying this document: "
                      "unpack the intuition, notation, and reasoning using the surrounding text for context. Use "
                      "LaTeX ($...$ or $$...$$) for mathematical expressions when helpful. Be brief: one to three "
                      "short paragraphs.")
                : QStringLiteral(
                      "Explain the selected words to a reader in the middle of this book: what they mean, what is "
                      "happening, and any allusion, history, idiom or old usage a modern reader might miss. Use the "
                      "text before it for context. Be brief: one to three short paragraphs."),
            spoilerFree);
        request.messages = {{true, bookLine() + QStringLiteral("\n\nThe book up to the selection:\n") + context +
                                       QStringLiteral("\n\nSelected: \"") + cleaned + QStringLiteral("\"")}};
        run(QStringLiteral("explain"), request);
    });
}

void Companion::translate(const QString &text, int chapter, qint64 end) {
    const QString cleaned = text.trimmed();
    m_lastTranslateText = cleaned;
    m_lastTranslateChapter = chapter;
    m_lastTranslateEnd = end;
    m_lastAction = {QStringLiteral("translate"), cleaned, chapter, end, {}, {}};
    begin(QStringLiteral("translate"), cleaned);
    if (!modelFor(m_feature))
        return;
    if (!checkBudget(m_feature, 2400 + cleaned.size(), true, 300))
        return;
    // Translate into the explicitly chosen target language (defaulting to the reader's
    // own system language — never forcing another language on untagged PDFs or quotes).
    const QString target = targetLanguage();
    const int generation = m_generation;
    readSoFar(chapter, end, 2500, [this, cleaned, target, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const QString context = joined(chapters, 2000);
        if (!checkBudget(QStringLiteral("translate"), context.size() + cleaned.size(), true, 300))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("translate");
        request.fast = true;
        request.stream = true;
        request.system = QStringLiteral(
                             "Translate the selected words into %1, faithfully and in a natural literary style, "
                             "using the passage before them for context. If the selected words are already in %1, "
                             "render them clearly in modern %1 instead of switching languages. Give only the translation.")
                             .arg(target);
        request.messages = {{true, QStringLiteral("Context:\n") + context + QStringLiteral("\n\nTranslate:\n") + cleaned}};
        run(QStringLiteral("translate"), request);
    });
}

void Companion::retranslate() {
    if (m_lastTranslateText.isEmpty())
        return;
    translate(m_lastTranslateText, m_lastTranslateChapter, m_lastTranslateEnd);
}

// --- Ask ---------------------------------------------------------------------

QVariantMap Companion::askEstimate() const {
    const bool cloud = cloudFor(QStringLiteral("ask"));
    qint64 contextChars = 0;
    if (m_scope == QLatin1String("shelf"))
        contextChars = 12000;
    else if (m_scope == QLatin1String("book"))
        contextChars = wholeBookCharsEstimate();
    else
        contextChars = soFarCharsEstimate();
    qint64 historyChars = 0;
    for (const Turn &turn : m_conversation)
        if (isTurnVisibleAtCurrentPlace(turn))
            historyChars += turn.text.size();
    const double characters = double(contextChars) + historyChars + (hasPendingSnip() ? 4800.0 : 0.0);
    double cost = 0;
    if (cloud) {
        if (Ai::TextModel *model = m_ai->textFor(QStringLiteral("ask"))) {
            Ai::Usage usage;
            usage.inputTokens = qMax<qint64>(1, qint64(characters / 4));
            usage.outputTokens = 400;
            cost = AiUsageLedger::cost(model->textModelName(false), usage, QDate::currentDate(), false);
        }
    }
    return {{QStringLiteral("where"), m_ai->whereFor(QStringLiteral("ask"))},
            {QStringLiteral("words"), qRound(double(contextChars) / 6.0)},
            {QStringLiteral("cost"), cost},
            {QStringLiteral("local"), !cloud},
            {QStringLiteral("scope"), m_scope}};
}

void Companion::clearPendingSnip() {
    if (m_pendingSnipPng.isEmpty() && m_pendingSnipUrl.isEmpty() && m_pendingSnipPage < 0)
        return;
    m_pendingSnipPng.clear();
    m_pendingSnipUrl.clear();
    m_pendingSnipPage = -1;
    emit pendingSnipChanged();
}

void Companion::activateCitation(int bookId, int chapter, qint64 offset, const QString &quote) {
    Q_UNUSED(offset);
    if (bookId > 0 && (!m_reader || bookId != m_reader->bookId()))
        emit openBookPassage(bookId, chapter, quote);
}

void Companion::askAboutSnipBase64(const QString &base64Png, const QString &action, const QString &prompt, int page) {
    QString raw = base64Png.trimmed();
    const qsizetype comma = raw.indexOf(QLatin1Char(','));
    if (raw.startsWith(QLatin1String("data:"), Qt::CaseInsensitive) && comma > 0)
        raw = raw.mid(comma + 1).trimmed();
    const QByteArray pngBytes = QByteArray::fromBase64(raw.toLatin1());
    if (pngBytes.isEmpty()) {
        fail(tr("Couldn't read the snipped image."));
        return;
    }
    QImage decoded;
    if (!decoded.loadFromData(pngBytes)) {
        fail(tr("Couldn't decode the snipped image."));
        return;
    }
    // Re-encode clean PNG (downscaling only if excessively huge) so outbound payload & thumbnail are safe.
    QByteArray cleanPng = pngBytes;
    if (decoded.width() > 1600 || decoded.height() > 1600) {
        decoded = decoded.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        cleanPng.clear();
        QBuffer buf(&cleanPng);
        buf.open(QIODevice::WriteOnly);
        decoded.save(&buf, "PNG");
    }

    const QString normAction = action.trimmed().toLower().isEmpty() ? QStringLiteral("explain") : action.trimmed().toLower();
    const int targetPage = page >= 0 ? page : (m_reader ? m_reader->chapter() : -1);
    const QString cleanedPrompt = prompt.trimmed();

    if (normAction == QLatin1String("ask") && cleanedPrompt.isEmpty()) {
        m_pendingSnipPng = cleanPng;
        m_pendingSnipPage = targetPage;
        m_pendingSnipUrl = makeSnipThumbnailDataUrl(cleanPng);
        emit pendingSnipChanged();
        emit openAskRequested();
        return;
    }
    clearPendingSnip();
    sendSnipRequest(cleanPng, normAction, cleanedPrompt, targetPage);
}

void Companion::sendSnipRequest(const QByteArray &pngBytes, const QString &action, const QString &prompt, int page) {
    const QString normAction = action.trimmed().toLower();
    const int targetPage = page >= 0 ? page : (m_reader ? m_reader->chapter() : -1);
    const bool isPdf = m_reader && m_reader->format().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
    const QString pageSuffix =
        targetPage >= 0
            ? (isPdf ? tr(" (Page %1)").arg(targetPage + 1) : tr(" (%1)").arg(placeTitle(targetPage)))
            : QString();

    QString userLabel = prompt.trimmed();
    QString taskSystem;
    QString userPrompt;
    if (normAction == QLatin1String("latex")) {
        if (userLabel.isEmpty())
            userLabel = tr("Extract LaTeX%1").arg(pageSuffix);
        taskSystem = QStringLiteral(
            "Transcribe the mathematical expression(s) in the attached image into clean, accurate, copyable LaTeX. "
            "Provide the raw LaTeX inside a single ```latex code block first, followed by a brief note only if any "
            "symbol is ambiguous. Never invent terms not visible in the image.");
        userPrompt = userLabel;
    } else if (normAction == QLatin1String("table")) {
        if (userLabel.isEmpty())
            userLabel = tr("Extract table%1").arg(pageSuffix);
        taskSystem = QStringLiteral(
            "Extract the table in the attached image accurately as a clean Markdown table preserving all headers, "
            "rows, units, and numeric precision, followed by a compact ```csv code block for easy copying. Never "
            "omit rows or fabricate values.");
        userPrompt = userLabel;
    } else if (normAction == QLatin1String("ask") && !userLabel.isEmpty()) {
        taskSystem = QStringLiteral(
            "Answer the reader's question about the attached snipped image from the document they are reading. "
            "Ground your answer in what is visible in the image and any surrounding text provided. Use LaTeX "
            "($...$ or $$...$$) for mathematical expressions when helpful.");
        userPrompt = userLabel;
    } else {
        if (userLabel.isEmpty())
            userLabel = tr("Explain this region%1").arg(pageSuffix);
        taskSystem = QStringLiteral(
            "Examine the attached snipped image (equation, figure, chart, diagram, or passage) from the document and "
            "provide a clear, step-by-step explanation. Unpack notation, intuition, and key takeaways, using LaTeX "
            "($...$ or $$...$$) for mathematical expressions when helpful.");
        userPrompt = userLabel;
    }

    m_lastAction = {QStringLiteral("snip"), prompt.trimmed(), targetPage, 0, pngBytes, normAction};
    begin(QStringLiteral("ask"), userLabel);
    emit openAskRequested();
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud && !checkBudget(m_feature, 6400 + userPrompt.size(), false, 500))
        return;

    const bool spoilerFree = !wholeBookStudyActive();
    const int readerChapter = m_reader ? m_reader->chapter() : -1;
    const qint64 pageEnd = m_reader ? m_reader->pageEnd() : 0;
    const int safePage =
        (spoilerFree && m_reader && targetPage > readerChapter) ? readerChapter : targetPage;
    const int turnChapter = spoilerFree ? safePage : -1;
    const qint64 turnOffset = (spoilerFree && safePage == readerChapter) ? pageEnd : -1;

    pruneFutureTurnsIfSpoilerFree();
    const QString thumbUrl = makeSnipThumbnailDataUrl(pngBytes);
    Turn userTurn;
    userTurn.fromUser = true;
    userTurn.text = userLabel;
    userTurn.snipDataUrl = thumbUrl;
    userTurn.chapter = turnChapter;
    userTurn.offset = turnOffset;
    m_conversation.append(std::move(userTurn));
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();
    setStatus(tr("Inspecting snipped region…"));

    const int bookId = m_reader ? m_reader->bookId() : 0;
    const int generation = m_generation;
    const QString path = m_reader ? m_reader->bookPath() : QString();
    const QString format = m_reader ? m_reader->format() : QString();
    const QString bookHeader = m_reader ? bookLine() : QString();

    auto dispatch = [this, bookId, generation, pngBytes, taskSystem, userPrompt, bookHeader, spoilerFree, safePage,
                     turnChapter, turnOffset, isPdf](const QString &pageContext) {
        if (generation != m_generation)
            return;
        if (cloudFor(QStringLiteral("ask")) &&
            !checkBudget(QStringLiteral("ask"), 4800 + pageContext.size() + userPrompt.size(), false, 500))
            return;
        QList<Citation> sources;
        if (safePage >= 0 && !pageContext.trimmed().isEmpty()) {
            Citation cite;
            cite.bookId = bookId;
            cite.chapter = safePage;
            cite.offset = 0;
            cite.label = isPdf ? tr("Page %1").arg(safePage + 1) : placeTitle(safePage);
            cite.excerpt = makeExcerpt(pageContext);
            sources.append(cite);
        }
        m_currentSources = sources;
        emit answerChanged();

        Ai::TextRequest request;
        request.feature = QStringLiteral("ask");
        request.stream = true;
        request.imagePng = pngBytes;
        request.imageMimeType = QStringLiteral("image/png");
        request.system = instructions(taskSystem, spoilerFree, true);
        QString msgText = bookHeader;
        if (!pageContext.trimmed().isEmpty())
            msgText += QStringLiteral("\n\nSurrounding page context:\n") + pageContext.left(3000);
        msgText += QStringLiteral("\n\nRequest: ") + userPrompt;
        request.messages = {{true, msgText.trimmed()}};

        run(QStringLiteral("ask"), request, [this, bookId, turnChapter, turnOffset, generation](Ai::Reply *) {
            if (generation != m_generation)
                return;
            if (m_answer.trimmed().isEmpty()) {
                fail(tr("The model returned an empty answer. Try again."));
                return;
            }
            m_pendingUserTurn = false;
            const QString completedAnswer = m_answer;
            const QList<Citation> finalSources = m_currentSources;
            m_answer.clear();
            m_currentSources.clear();
            m_conversation.append(
                {false, completedAnswer, finalSources, m_where, m_whereLocal, {}, turnChapter, turnOffset});
            saveConversation(bookId);
            emit conversationChanged();
        });
    };

    if (path.isEmpty() || safePage < 0) {
        dispatch({});
        return;
    }
    const qint64 ctxClamp = (spoilerFree && safePage == readerChapter && pageEnd > 0) ? qMin<qint64>(3000, pageEnd) : 3000;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, path, format, safePage, ctxClamp, cancel, generation, dispatch] {
        QString ctx;
        if (!cancel->load()) {
            if (format == QLatin1String("pdf")) {
                PdfBook pdf(path);
                if (pdf.open() && safePage < pdf.pageCount())
                    ctx = pdf.pageText(safePage).left(qsizetype(ctxClamp));
            } else {
                EpubBook epub(path);
                if (epub.open() && safePage < int(epub.spine().size()))
                    ctx = epub.chapterText(safePage).left(qsizetype(ctxClamp));
            }
        }
        if (cancel->load())
            return;
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                app,
                [self, ctx, generation, dispatch] {
                    if (!self || self->m_generation != generation)
                        return;
                    dispatch(ctx);
                },
                Qt::QueuedConnection);
        }
    });
}

void Companion::newConversation() {
    stop();
    m_conversation.clear();
    m_currentSources.clear();
    m_pendingUserTurn = false;
    clearPendingSnip();
    m_answer.clear();
    m_feature = QStringLiteral("ask");
    m_problem.clear();
    m_problemIsConfig = false;
    m_pendingSetup = {};
    if (m_reader && m_reader->bookId() > 0) {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen()) {
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
            del.addBindValue(m_reader->bookId());
            del.exec();
        }
    }
    emit changed();
    emit answerChanged();
    emit conversationChanged();
}

void Companion::ask(const QString &question) {
    const QString asked = question.trimmed();
    if (!m_pendingSnipPng.isEmpty()) {
        const QByteArray png = m_pendingSnipPng;
        const int page = m_pendingSnipPage;
        clearPendingSnip();
        sendSnipRequest(png, asked.isEmpty() ? QStringLiteral("explain") : QStringLiteral("ask"), asked, page);
        return;
    }
    if (asked.isEmpty())
        return;
    if (m_scope == QLatin1String("shelf")) {
        askShelf(asked);
        return;
    }
    const bool wholeBook = (m_scope == QLatin1String("book"));
    const bool spoilerFree = !wholeBook;
    m_lastAction = {QStringLiteral("ask"), asked, m_reader ? m_reader->chapter() : 0,
                    m_reader ? m_reader->pageEnd() : 0, {}, {}};
    begin(QStringLiteral("ask"), asked);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud) {
        const double cost = askEstimate().value(QStringLiteral("cost")).toDouble();
        if (!m_ai->fitsBudget(cost)) {
            fail(m_ai->describe(int(Ai::Error::Budget)), true);
            return;
        }
    }
    const int currentChapter = m_reader->chapter();
    const qint64 currentPageEnd = m_reader->pageEnd();
    const int turnChapter = spoilerFree ? currentChapter : -1;
    const qint64 turnOffset = spoilerFree ? currentPageEnd : -1;
    pruneFutureTurnsIfSpoilerFree();
    m_conversation.append({true, asked, {}, {}, false, {}, turnChapter, turnOffset});
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();
    setStatus(cloud ? (wholeBook ? tr("Reading the whole document…") : tr("Reading the book so far…"))
                    : tr("Finding the passages that matter…"));
    const int bookId = m_reader->bookId();
    const QString format = m_reader->format();
    const int generation = m_generation;
    const qint64 readLimit = cloud ? cloudLimit : -1;

    auto onChapters = [this, asked, bookId, format, currentChapter, currentPageEnd, turnChapter, turnOffset, wholeBook,
                       spoilerFree, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        if (spoilerFree && m_reader &&
            (currentChapter > m_reader->chapter() ||
             (currentChapter == m_reader->chapter() && m_reader->pageEnd() > 0 && currentPageEnd > m_reader->pageEnd()))) {
            m_answer.clear();
            stop();
            return;
        }
        if (chapters.isEmpty()) {
            fail(wholeBook ? tr("There isn't any readable text in this document yet.")
                           : tr("There isn't any readable text up to this page yet."));
            return;
        }
        const bool isCloud = cloudFor(QStringLiteral("ask"));
        const auto send = [this, isCloud, bookId, format, chapters, spoilerFree, turnChapter, turnOffset, generation](
                              const QString &context, const QString &how, const QList<Citation> &sources) {
            if (generation != m_generation)
                return;
            m_currentSources = sources;
            emit answerChanged();
            // Build alternating conversation turns (trimmed to fit local context windows),
            // strictly filtering out any future-chapter turns in spoiler-free mode.
            const qint64 localTotal = isCloud ? cloudLimit : localCharBudget(QStringLiteral("ask"), false, localLimit);
            qint64 historyBudget = isCloud ? 100000 : qMin<qint64>(8000, qMax<qint64>(1500, localTotal / 3));
            QList<Ai::Message> turns;
            for (qsizetype i = m_conversation.size() - 1; i >= 0; --i) {
                const Turn &msg = m_conversation[i];
                if (!isTurnVisibleAtCurrentPlace(msg))
                    continue;
                if (!turns.isEmpty() && historyBudget - msg.text.size() < 0)
                    break;
                historyBudget -= msg.text.size();
                turns.prepend({msg.fromUser, msg.text});
            }
            if (!turns.isEmpty() && !turns.first().fromUser)
                turns.removeFirst();
            QList<Ai::Message> normalized;
            for (const Ai::Message &msg : turns) {
                if (!normalized.isEmpty() && normalized.last().fromUser == msg.fromUser)
                    normalized.last().text += QStringLiteral("\n\n") + msg.text;
                else
                    normalized.append(msg);
            }
            if (isCloud) {
                qint64 totalChars = context.size();
                for (const Ai::Message &msg : normalized)
                    totalChars += msg.text.size();
                if (!checkBudget(QStringLiteral("ask"), totalChars, false, 400))
                    return;
            }
            Ai::TextRequest request;
            request.feature = QStringLiteral("ask");
            request.stream = true;
            const QString task =
                studyMode()
                    ? QStringLiteral(
                          "Answer the reader's questions about the document they are studying, strictly grounded in "
                          "its text below (%1). Be clear, rigorous, and concise. Use LaTeX math ($...$ or $$...$$) "
                          "for equations and symbols when helpful, quote briefly when useful, and cite the page or "
                          "section. If the provided text doesn't address it, say so plainly.")
                          .arg(how)
                    : QStringLiteral(
                          "Answer the reader's questions about the book they are reading, from its text below (%1). "
                          "Be concise, warm, and literary. Quote briefly when it helps, and mention which chapter it "
                          "comes from. If the text doesn't say, say so plainly.")
                          .arg(how);
            request.system = instructions(task, spoilerFree, studyMode());
            request.messages = {{true, bookLine() + QStringLiteral("\n\n") + context}};
            request.messages.append({false, QStringLiteral("I've read it. What would you like to know?")});
            request.messages += normalized;
            run(QStringLiteral("ask"), request,
                [this, bookId, format, chapters, turnChapter, turnOffset, generation](Ai::Reply *) {
                    if (generation != m_generation)
                        return;
                    if (m_answer.trimmed().isEmpty()) {
                        fail(tr("The model returned an empty answer. Try again."));
                        return;
                    }
                    m_pendingUserTurn = false;
                    QList<Citation> finalSources = m_currentSources;
                    if (finalSources.isEmpty()) {
                        QList<PassageIndex::Passage> passages;
                        for (const Chapter &ch : chapters)
                            passages += PassageIndex::split(ch.index, ch.text);
                        finalSources =
                            citationsFromHits(PassageIndex::byWords(passages, m_answer, 3), chapters, format, 3);
                    }
                    const QString completedAnswer = m_answer;
                    m_answer.clear();
                    m_currentSources.clear();
                    m_conversation.append(
                        {false, completedAnswer, finalSources, m_where, m_whereLocal, {}, turnChapter, turnOffset});
                    saveConversation(bookId);
                    emit conversationChanged();
                });
        };
        if (isCloud) {
            QString fallbackQuery;
            if (m_conversation.size() >= 3) {
                for (qsizetype i = m_conversation.size() - 2; i >= 0; --i) {
                    if (m_conversation[i].fromUser) {
                        fallbackQuery = m_conversation[i].text + QLatin1Char(' ') + asked;
                        break;
                    }
                }
            }
            QPointer<Companion> self(this);
            (void)QtConcurrent::run([self, send, chapters, asked, fallbackQuery, format, wholeBook, currentChapter,
                                     generation] {
                QList<PassageIndex::Passage> passages;
                for (const Chapter &ch : chapters)
                    passages += PassageIndex::split(ch.index, ch.text);
                QList<PassageIndex::Hit> citeHits = PassageIndex::byWords(passages, asked, 4);
                if (citeHits.isEmpty() && !fallbackQuery.isEmpty())
                    citeHits = PassageIndex::byWords(passages, fallbackQuery, 4);
                const QList<Citation> cites = citationsFromHits(citeHits, chapters, format, 4);
                const QString fullText =
                    wholeBook ? joinedStudyDocument(chapters, cloudLimit, currentChapter) : joined(chapters, cloudLimit);
                const QString how = wholeBook ? QStringLiteral("the whole document")
                                              : QStringLiteral("the whole book up to the reader's place");
                if (QCoreApplication *app = QCoreApplication::instance()) {
                    QMetaObject::invokeMethod(
                        app,
                        [self, send, fullText, how, cites, generation] {
                            if (!self || self->m_generation != generation)
                                return;
                            send(fullText, how, cites);
                        },
                        Qt::QueuedConnection);
                }
            });
            return;
        }
        // On this computer: the passages closest to the question, and the
        // pages around the reader's place, budgeted to the local model's actual context window.
        QList<PassageIndex::Passage> passages;
        for (const Chapter &chapter : chapters)
            passages += PassageIndex::split(chapter.index, chapter.text);
        QPointer<Companion> self(this);
        const auto answer = [self, send, chapters, format, wholeBook, currentChapter,
                             generation](const QList<PassageIndex::Hit> &hits, bool) {
            if (!self || self->m_generation != generation)
                return;
            const QList<Citation> sources = citationsFromHits(hits, chapters, format, 4);
            const qint64 localBudget = self->localCharBudget(QStringLiteral("ask"), false, localLimit);
            const qint64 recentBudget = qMin<qint64>(4000, qMax<qint64>(1200, localBudget / 4));
            const qint64 passageBudget = qMax<qint64>(2000, localBudget - recentBudget - 2500);
            QList<PassageIndex::Hit> ordered = hits;
            std::sort(ordered.begin(), ordered.end(),
                      [](const PassageIndex::Hit &a, const PassageIndex::Hit &b) {
                          return a.passage.chapter != b.passage.chapter ? a.passage.chapter < b.passage.chapter
                                                                        : a.passage.start < b.passage.start;
                      });
            QString context = QStringLiteral("Passages that may matter:\n\n");
            qint64 used = 0;
            for (const PassageIndex::Hit &hit : ordered) {
                const QString block = passageText(hit, chapters, format) + QStringLiteral("\n\n");
                if (used > 0 && used + block.size() > passageBudget)
                    break;
                context += block;
                used += block.size();
            }
            if (wholeBook) {
                QString aroundCurrent;
                for (const Chapter &ch : chapters) {
                    if (ch.index == currentChapter) {
                        aroundCurrent = ch.text.trimmed().left(qsizetype(recentBudget));
                        break;
                    }
                }
                if (!aroundCurrent.isEmpty())
                    context += QStringLiteral("Current section in view:\n\n") + aroundCurrent;
            } else {
                context += QStringLiteral("What the reader read last:\n\n") + joined(chapters, recentBudget);
            }
            send(context,
                 wholeBook ? QStringLiteral("passages found across the whole document")
                           : QStringLiteral("passages found for the question, and the latest pages"),
                 sources);
        };
        if (m_ai->embedder()) {
            m_index.build(
                bookId, passages,
                [self, generation](double share) {
                    if (self && self->m_generation == generation)
                        self->setStatus(share > 0.0
                                            ? tr("Getting to know the book… %1%").arg(qRound(share * 100))
                                            : tr("Getting to know the book…"));
                },
                [self, bookId, passages, asked, answer, generation](const QString &) {
                    if (!self || self->m_generation != generation)
                        return;
                    self->setStatus(tr("Finding the passages that matter…"));
                    self->m_index.search(bookId, passages, asked, 8, answer);
                });
            return;
        }
        m_index.search(bookId, passages, asked, 8, answer);
    };

    if (wholeBook)
        readWholeBook(readLimit, onChapters);
    else
        readSoFar(m_reader->chapter(), m_reader->pageEnd(), readLimit, onChapters);
}

void Companion::askShelf(const QString &asked) {
    const int bookId = m_reader ? m_reader->bookId() : 0;
    m_lastAction = {QStringLiteral("ask"), asked, m_reader ? m_reader->chapter() : 0,
                    m_reader ? m_reader->pageEnd() : 0, {}, {}};
    begin(QStringLiteral("ask"), asked);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud && !checkBudget(m_feature, 12000, false, 500))
        return;

    m_conversation.append({true, asked, {}, {}, false, {}, -1, -1});
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();

    QString shelfName;
    const QList<ShelfBook> shelfBooks = shelfBooksFor(bookId, &shelfName);
    if (shelfName != m_shelfLabel) {
        m_shelfLabel = shelfName;
        emit scopeChanged();
    }
    QSet<int> shelfIds;
    QHash<int, ShelfBook> byId;
    for (const ShelfBook &b : shelfBooks) {
        shelfIds.insert(b.id);
        byId.insert(b.id, b);
    }

    setStatus(shelfName.isEmpty() ? tr("Searching across your library…")
                                  : tr("Searching across %1…").arg(shelfName));

    const int generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);

    auto resolvePassages = [this, self, asked, bookId, shelfBooks, byId, shelfName, cancel,
                            generation](const QList<PassageIndex::LibraryHit> &libHits) {
        if (!self || self->m_generation != generation || cancel->load())
            return;
        self->setStatus(tr("Reading matching passages…"));

        struct ResolvedBlock {
            int bookId = 0;
            QString bookTitle;
            QString format;
            int chapter = 0;
            qint64 offset = 0;
            QString label;
            QString text;
        };

        (void)QtConcurrent::run([self, asked, bookId, shelfBooks, byId, libHits, shelfName, cancel, generation] {
            QList<ResolvedBlock> blocks;
            QSet<QPair<int, int>> seenBookChapter;

            auto readChapterText = [](const ShelfBook &book, int chapter) -> QString {
                if (book.path.isEmpty() || chapter < 0)
                    return {};
                if (book.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
                    PdfBook pdf(book.path);
                    if (pdf.open() && chapter < pdf.pageCount())
                        return pdf.pageText(chapter);
                } else {
                    EpubBook epub(book.path);
                    if (epub.open() && chapter < int(epub.spine().size()))
                        return epub.chapterText(chapter);
                }
                return {};
            };

            // 1. Resolve semantic hits from PassageIndex::searchLibrary().
            for (const PassageIndex::LibraryHit &hit : libHits) {
                if (cancel->load())
                    return;
                if (!byId.contains(hit.bookId))
                    continue;
                if (seenBookChapter.contains({hit.bookId, hit.chapter}))
                    continue;
                const ShelfBook book = byId.value(hit.bookId);
                const QString chText = readChapterText(book, hit.chapter);
                if (chText.trimmed().isEmpty())
                    continue;
                QString excerpt;
                if (hit.end > hit.start && hit.start >= 0 && hit.start < chText.size())
                    excerpt = chText.mid(qsizetype(hit.start), qsizetype(qMin<qint64>(1400, hit.end - hit.start))).trimmed();
                if (excerpt.isEmpty())
                    excerpt = chText.trimmed().left(1200);
                seenBookChapter.insert({hit.bookId, hit.chapter});
                blocks.append({book.id, book.title, book.format, hit.chapter, qMax<qint64>(0, hit.start),
                               shelfCitationLabel(book.title, book.format, hit.chapter), excerpt});
                if (blocks.size() >= 8)
                    break;
            }

            // 2. Fallback / supplement via lexical BM25 across shelf books when vector hits are sparse
            // (e.g., no embedder configured or newly-added papers not yet vector-indexed).
            if (blocks.size() < 4 && !shelfBooks.isEmpty()) {
                struct LexicalEntry {
                    ShelfBook book;
                    PassageIndex::Hit hit;
                };
                QList<LexicalEntry> candidates;
                const int maxBooksToScan = qMin<int>(14, int(shelfBooks.size()));
                for (int bi = 0; bi < maxBooksToScan && !cancel->load(); ++bi) {
                    const ShelfBook &book = shelfBooks[bi];
                    const QList<Chapter> chs = textOfBook(book.path, book.format, 120000, cancel);
                    if (cancel->load() || chs.isEmpty())
                        continue;
                    QList<PassageIndex::Passage> passages;
                    for (const Chapter &ch : chs)
                        passages += PassageIndex::split(ch.index, ch.text, 1100);
                    const QList<PassageIndex::Hit> hits = PassageIndex::byWords(passages, asked, 2);
                    for (const PassageIndex::Hit &h : hits)
                        candidates.append({book, h});
                }
                if (cancel->load())
                    return;
                std::sort(candidates.begin(), candidates.end(), [](const LexicalEntry &a, const LexicalEntry &b) {
                    return a.hit.score > b.hit.score;
                });
                for (const LexicalEntry &entry : candidates) {
                    if (seenBookChapter.contains({entry.book.id, entry.hit.passage.chapter}))
                        continue;
                    seenBookChapter.insert({entry.book.id, entry.hit.passage.chapter});
                    blocks.append({entry.book.id, entry.book.title, entry.book.format, entry.hit.passage.chapter,
                                   entry.hit.passage.start,
                                   shelfCitationLabel(entry.book.title, entry.book.format, entry.hit.passage.chapter),
                                   entry.hit.passage.text});
                    if (blocks.size() >= 8)
                        break;
                }
            }
            if (cancel->load())
                return;

            QList<Citation> citations;
            QString context;
            for (const ResolvedBlock &b : blocks) {
                context += QStringLiteral("[%1]\n%2\n\n").arg(b.label, b.text);
                if (citations.size() < 6) {
                    Citation cite;
                    cite.bookId = b.bookId;
                    cite.bookTitle = b.bookTitle;
                    cite.chapter = b.chapter;
                    cite.offset = b.offset;
                    cite.label = b.label;
                    cite.excerpt = makeExcerpt(b.text);
                    citations.append(cite);
                }
            }

            if (QCoreApplication *app = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(
                    app,
                    [self, bookId, context, citations, shelfName, generation] {
                        if (!self || self->m_generation != generation)
                            return;
                        if (context.trimmed().isEmpty()) {
                            self->fail(tr("No matching passages were found on this shelf. Try another question or "
                                          "switch to Whole book."));
                            return;
                        }
                        const bool isCloud = self->cloudFor(QStringLiteral("ask"));
                        const qint64 budgetChars =
                            isCloud ? cloudLimit : self->localCharBudget(QStringLiteral("ask"), false, localLimit);
                        const QString trimmedContext = context.left(qsizetype(qMax<qint64>(2000, budgetChars - 2500)));
                        if (isCloud && !self->checkBudget(QStringLiteral("ask"), trimmedContext.size(), false, 500))
                            return;

                        self->m_currentSources = citations;
                        emit self->answerChanged();

                        QList<Ai::Message> turns;
                        qint64 histBudget = isCloud ? 32000 : 4000;
                        for (qsizetype i = self->m_conversation.size() - 1; i >= 0; --i) {
                            const Turn &msg = self->m_conversation[i];
                            if (!turns.isEmpty() && histBudget - msg.text.size() < 0)
                                break;
                            histBudget -= msg.text.size();
                            turns.prepend({msg.fromUser, msg.text});
                        }
                        if (!turns.isEmpty() && !turns.first().fromUser)
                            turns.removeFirst();

                        Ai::TextRequest request;
                        request.feature = QStringLiteral("ask");
                        request.stream = true;
                        const QString whereDesc =
                            shelfName.isEmpty() ? QStringLiteral("the reader's library")
                                                : QStringLiteral("the shelf \"%1\"").arg(shelfName);
                        request.system = instructions(
                            QStringLiteral(
                                "Synthesize a grounded answer across multiple documents from %1 using only the "
                                "excerpts below. Cite sources inline using exact bracketed labels like "
                                "[Book Title, p. X] (matching the labels above). Compare, connect, or contrast "
                                "documents when helpful, and use LaTeX ($...$ or $$...$$) for mathematical "
                                "expressions when relevant. If the excerpts don't cover the question, say so plainly.")
                                .arg(whereDesc),
                            false, true);
                        request.messages = {
                            {true, QStringLiteral("Currently open: %1\n\nExcerpts from %2:\n\n%3")
                                       .arg(self->bookLine(), whereDesc, trimmedContext)}};
                        request.messages.append(
                            {false, QStringLiteral("I've reviewed those excerpts. What would you like to know?")});
                        request.messages += turns;

                        self->run(QStringLiteral("ask"), request, [self, bookId, generation](Ai::Reply *) {
                            if (!self || self->m_generation != generation)
                                return;
                            if (self->m_answer.trimmed().isEmpty()) {
                                self->fail(tr("The model returned an empty answer. Try again."));
                                return;
                            }
                            self->m_pendingUserTurn = false;
                            const QString completedAnswer = self->m_answer;
                            const QList<Citation> finalSources = self->m_currentSources;
                            self->m_answer.clear();
                            self->m_currentSources.clear();
                            self->m_conversation.append(
                                {false, completedAnswer, finalSources, self->m_where, self->m_whereLocal, {}, -1, -1});
                            self->saveConversation(bookId);
                            emit self->conversationChanged();
                        });
                    },
                    Qt::QueuedConnection);
            }
        });
    };

    m_index.searchLibrary(asked, 10, resolvePassages, shelfIds);
}

// --- Previously…, Who's who --------------------------------------------------

void Companion::recap() { runRecap(false); }

void Companion::refreshRecap() { runRecap(true); }

void Companion::runRecap(bool forceRefresh) {
    const int bookId = m_reader->bookId();
    const int chapter = m_reader->chapter();
    const qint64 pageEnd = m_reader->pageEnd();
    const qint64 pageStart = m_reader->pageStart();
    const bool isStudy = studyMode();
    const bool wholeStudy = wholeBookStudyActive();
    const int cacheChapter = wholeStudy ? -1 : chapter;
    const qint64 cacheOffset = wholeStudy ? 0 : pageEnd;
    m_lastAction = {QStringLiteral("recap"), QString(), chapter, pageEnd, {}, {}};
    begin(QStringLiteral("recap"), wholeStudy ? m_reader->title() : placeTitle(chapter));
    if (!forceRefresh) {
        const bool memoryValid =
            m_recapLoaded && !m_recapText.trimmed().isEmpty() &&
            (wholeStudy ? (m_recapChapter == -1) : (m_recapChapter == chapter && (pageEnd <= 0 || m_recapOffset <= pageEnd)));
        if (memoryValid) {
            m_answer = m_recapText;
            m_where = m_recapWhere;
            m_whereLocal = m_recapWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit answerChanged();
            emit changed();
            return;
        }
        if (loadRecapSnapshot(bookId, chapter, pageEnd, pageStart, true)) {
            m_answer = m_recapText;
            m_where = m_recapWhere;
            m_whereLocal = m_recapWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit recapChanged();
            emit answerChanged();
            emit changed();
            return;
        }
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 estChars = wholeStudy ? wholeBookCharsEstimate() : soFarCharsEstimate();
    if (cloud && !checkBudget(m_feature, estChars, false, 500))
        return;
    const qint64 limit = cloud ? cloudLimit : localCharBudget(m_feature, false, localLimit);
    const int generation = m_generation;
    setStatus(wholeStudy ? tr("Reading the document…") : tr("Reading the book so far…"));

    auto onChapters = [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, isStudy, wholeStudy,
                       limit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        // Spoiler-safety guard in Fiction mode or "upto" scope if the reader jumped backward while reading disk.
        if (!wholeStudy && m_reader &&
            (chapter > m_reader->chapter() ||
             (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd()))) {
            stop();
            return;
        }
        const bool isCloud = cloudFor(QStringLiteral("recap"));
        const qint64 effectiveLimit =
            isCloud ? cloudLimit : qMin(limit, localCharBudget(QStringLiteral("recap"), false, localLimit));
        QString titles;
        for (const Chapter &ch : chapters)
            if (!ch.title.isEmpty())
                titles += QStringLiteral("- ") + ch.title + QLatin1Char('\n');
        const QString text = wholeStudy ? joinedStudyDocument(chapters, effectiveLimit, chapter)
                                        : joined(chapters, effectiveLimit);
        if (text.trimmed().isEmpty()) {
            fail(tr("There isn't any text to summarize yet. Turn a page or two first."));
            return;
        }
        if (!checkBudget(QStringLiteral("recap"), text.size() + titles.size(), false, 500))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("recap");
        request.stream = true;
        if (isStudy) {
            request.system = instructions(
                QStringLiteral(
                    "Write a crisp, structured Study Brief for this document. Organize it into four concise "
                    "sections with bold lead-ins on separate paragraphs:\n"
                    "**Core Claims** — the central thesis, problem statement, or chapter objectives.\n"
                    "**Method** — the approach, model, proofs, experimental setup, or analytical framework.\n"
                    "**Key Findings** — the main results, theorems, or quantitative takeaways.\n"
                    "**Limitations** — caveats, assumptions, edge cases, or open questions.\n"
                    "(If this is a textbook chapter rather than a paper, adapt the four leads to a tight chapter "
                    "study guide.) Use standard LaTeX math ($...$ or $$...$$) for key equations or notation when "
                    "helpful. Stay strictly grounded in the provided text."),
                !wholeStudy, true);
        } else {
            request.system = instructions(
                QStringLiteral(
                    "Write \"Previously…\" for a reader coming back to this book: recap the story so far in 2 to 4 "
                    "crisp, engaging paragraphs, focusing most on the latest events and ending right where they "
                    "stopped. Name the key people and tensions that matter right now."),
                true, false);
        }
        request.messages = {{true, bookLine() +
                                       (wholeStudy ? QStringLiteral("\n\nSections:\n")
                                                   : QStringLiteral("\n\nChapters so far:\n")) +
                                       titles + QStringLiteral("\n") + text}};
        run(QStringLiteral("recap"), request,
            [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, wholeStudy, generation](Ai::Reply *) {
                if (generation != m_generation)
                    return;
                if (m_answer.trimmed().isEmpty()) {
                    fail(tr("The model returned an empty answer. Try again."));
                    return;
                }
                if (!wholeStudy && m_reader &&
                    (chapter > m_reader->chapter() ||
                     (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd())))
                    return;
                m_recapText = m_answer;
                m_recapLoaded = true;
                m_recapChapter = cacheChapter;
                m_recapOffset = cacheOffset;
                m_recapWhere = m_where;
                m_recapWhereLocal = m_whereLocal;
                saveRecapSnapshot(bookId, cacheChapter, cacheOffset, wholeStudy ? 0 : pageStart, m_where, m_whereLocal,
                                  m_recapText);
                emit recapChanged();
            });
    };

    if (wholeStudy)
        readWholeBook(limit, onChapters);
    else
        readSoFar(chapter, pageEnd, limit, onChapters);
}

void Companion::refreshWhosWho() {
    m_peopleKey.clear();
    m_peopleWhere.clear();
    m_peopleWhereLocal = false;
    runWhosWho(true);
}

void Companion::whosWho() { runWhosWho(false); }

void Companion::runWhosWho(bool forceRefresh) {
    const int bookId = m_reader->bookId();
    const int chapter = m_reader->chapter();
    const qint64 pageEnd = m_reader->pageEnd();
    const qint64 pageStart = m_reader->pageStart();
    const QString format = m_reader->format();
    const bool isStudy = studyMode();
    const bool wholeStudy = wholeBookStudyActive();
    const int cacheChapter = wholeStudy ? -1 : chapter;
    const qint64 cacheOffset = wholeStudy ? 0 : pageEnd;
    const QString where = m_ai->whereFor(QStringLiteral("whoswho"));
    const QString key = QStringLiteral("%1:%2:%3:%4:%5").arg(bookId).arg(m_readingMode).arg(cacheChapter).arg(cacheOffset).arg(where);
    m_lastAction = {QStringLiteral("whoswho"), QString(), chapter, pageEnd, {}, {}};
    begin(QStringLiteral("whoswho"), wholeStudy ? m_reader->title() : placeTitle(chapter));
    if (!forceRefresh) {
        const bool memoryValid =
            m_peopleLoaded &&
            (wholeStudy ? (m_peopleChapter == -1) : (m_peopleChapter == chapter && (pageEnd <= 0 || m_peopleOffset <= pageEnd))) &&
            (key == m_peopleKey || !m_people.isEmpty());
        if (memoryValid) {
            m_where = m_peopleWhere.isEmpty() ? where : m_peopleWhere;
            m_whereLocal = m_peopleWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit changed();
            return;
        }
        if (loadWhosWhoSnapshot(bookId, chapter, pageEnd, pageStart, true)) {
            m_where = m_peopleWhere.isEmpty() ? where : m_peopleWhere;
            m_whereLocal = m_peopleWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit changed();
            return;
        }
    }
    // Never leave a later-chapter snapshot visible if the reader jumped backward in spoiler-free mode.
    if (!wholeStudy && (m_peopleChapter > chapter || (m_peopleChapter == chapter && pageEnd > 0 && m_peopleOffset > pageEnd))) {
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_peopleWhereLocal = false;
        emit changed();
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 estChars = wholeStudy ? wholeBookCharsEstimate() : soFarCharsEstimate();
    if (cloud && !checkBudget(m_feature, estChars, false, 800))
        return;
    // Budget strictly against the local model's actual context window (2.5 chars/token for JSON/non-Latin safety).
    const qint64 readLimit = cloud ? cloudLimit : localCharBudget(m_feature, false, localWhosWhoLimit, 2.5);
    const int generation = m_generation;
    setStatus(wholeStudy ? tr("Scanning notation & terms…") : tr("Reading the book so far…"));

    auto onChapters = [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, isStudy, wholeStudy,
                       format, key, readLimit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        if (!wholeStudy && m_reader &&
            (chapter > m_reader->chapter() ||
             (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd()))) {
            stop();
            return;
        }
        const bool isCloud = cloudFor(QStringLiteral("whoswho"));
        const qint64 effectiveLimit =
            isCloud ? cloudLimit
                    : qMin(readLimit, localCharBudget(QStringLiteral("whoswho"), false, localWhosWhoLimit, 2.5));
        const QString text = wholeStudy ? joinedStudyDocument(chapters, effectiveLimit, chapter)
                                        : joined(chapters, effectiveLimit);
        if (text.trimmed().isEmpty()) {
            m_people.clear();
            m_peopleLoaded = true;
            m_peopleChapter = cacheChapter;
            m_peopleOffset = cacheOffset;
            m_peopleKey = key;
            m_peopleWhere.clear();
            m_peopleWhereLocal = false;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit changed();
            return;
        }
        if (!checkBudget(QStringLiteral("whoswho"), text.size(), false, 800))
            return;
        const QJsonObject item{{"type", "object"},
                               {"properties", QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                                                          {"also", QJsonObject{{"type", "string"}}},
                                                          {"who", QJsonObject{{"type", "string"}}},
                                                          {"chapter", QJsonObject{{"type", "string"}}}}},
                               {"required", QJsonArray{"name", "also", "who", "chapter"}},
                               {"additionalProperties", false}};
        const QString arrayKey = isStudy ? QStringLiteral("terms") : QStringLiteral("people");
        Ai::TextRequest request;
        request.feature = QStringLiteral("whoswho");
        // Use the full context window on local models (never forcing 24K chars into num_ctx: 8192).
        request.fast = false;
        request.schema = QJsonObject{{"type", "object"},
                                     {"properties", QJsonObject{{arrayKey, QJsonObject{{"type", "array"}, {"items", item}}}}},
                                     {"required", QJsonArray{arrayKey}},
                                     {"additionalProperties", false}};
        if (isStudy) {
            request.system = instructions(
                QStringLiteral(
                    "Extract a crisp Glossary & Notation reference from this document, most central first, at most 30 "
                    "entries: mathematical symbols (written in clean Unicode such as θ, ℒ, ∇, α, Σ), acronyms, and "
                    "key domain terms. For each entry set: \"name\" the symbol, acronym, or term itself, \"also\" "
                    "its expansion, alternate symbol, or notation (or \"\"), \"who\" one concise, accurate definition "
                    "grounded in the document (inline LaTeX $...$ is welcome), \"chapter\" the exact section heading "
                    "or page number where it is first defined or introduced."),
                !wholeStudy, false);
        } else {
            request.system = instructions(
                QStringLiteral(
                    "List the notable people (and named animals or beings) introduced in the book so far, most "
                    "important first, at most 30: \"name\" as most often called, \"also\" other names or titles "
                    "(or \"\"), \"who\" one crisp, spoiler-free sentence on who they are and their role so far, "
                    "\"chapter\" the exact chapter title or number where they first appear."),
                true, false);
        }
        request.messages = {{true, bookLine() + QStringLiteral("\n\n") + text}};
        run(QStringLiteral("whoswho"), request,
            [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, isStudy, wholeStudy, format,
             chapters, key, generation](Ai::Reply *reply) {
                if (generation != m_generation)
                    return;
                if (!wholeStudy && m_reader &&
                    (chapter > m_reader->chapter() ||
                     (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd())))
                    return;
                QJsonObject result = reply->result();
                if (result.isEmpty())
                    result = extractJsonObject(reply->text());
                const QJsonValue entriesVal = result.contains(QStringLiteral("terms"))
                                                  ? result.value(QStringLiteral("terms"))
                                                  : result.value(QStringLiteral("people"));
                if (result.isEmpty() || !entriesVal.isArray()) {
                    fail(isStudy ? tr("Glossary couldn't be read from the model's answer. Try again.")
                                 : tr("Who's who couldn't be read from the model's answer. Try again."));
                    return;
                }
                m_people.clear();
                // Names as the book prints them, without its note markers ("Filby*").
                static const QRegularExpression marker(QStringLiteral("[*†‡§¹²³⁴⁵⁶⁷⁸⁹⁰]+(?=\\s|$)"));
                for (const QJsonValue &value : entriesVal.toArray()) {
                    QVariantMap itemMap = value.toObject().toVariantMap();
                    if (!isStudy) {
                        for (const char *field : {"name", "also"})
                            itemMap[QLatin1String(field)] =
                                itemMap.value(QLatin1String(field)).toString().remove(marker).trimmed();
                    } else {
                        for (const char *field : {"name", "also"})
                            itemMap[QLatin1String(field)] = itemMap.value(QLatin1String(field)).toString().trimmed();
                    }
                    const QString name = itemMap.value(QStringLiteral("name")).toString();
                    if (name.isEmpty())
                        continue;
                    const QString also = itemMap.value(QStringLiteral("also")).toString();
                    const QString llmChapter = itemMap.value(QStringLiteral("chapter")).toString();
                    const FirstSeen seen = resolveFirstSeen(chapters, name, also, llmChapter, format, isStudy);
                    itemMap.insert(QStringLiteral("chapter"), seen.label);
                    itemMap.insert(QStringLiteral("chapterIndex"), seen.chapterIndex);
                    itemMap.insert(QStringLiteral("offset"), seen.offset);
                    m_people.append(itemMap);
                }
                m_peopleLoaded = true;
                m_peopleChapter = cacheChapter;
                m_peopleOffset = cacheOffset;
                m_peopleKey = key;
                m_peopleWhere = m_where;
                m_peopleWhereLocal = m_whereLocal;
                m_answer.clear();
                saveWhosWhoSnapshot(bookId, cacheChapter, cacheOffset, wholeStudy ? 0 : pageStart, m_where,
                                    m_whereLocal, m_people);
            });
    };

    if (wholeStudy)
        readWholeBook(readLimit, onChapters);
    else
        readSoFar(chapter, pageEnd, readLimit, onChapters);
}
