#include "ai/bookcontextreader.h"

#include "formats/epubbook.h"
#include "formats/pdfbook.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QPair>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

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

QString shortBookTitle(const QString &title, qsizetype maxLen = 34) {
    const QString clean = title.simplified();
    if (clean.size() <= maxLen)
        return clean;
    qsizetype cut = clean.lastIndexOf(QLatin1Char(' '), maxLen);
    if (cut < maxLen / 2)
        cut = maxLen;
    return clean.left(cut).trimmed() + QChar(0x2026);
}

QString readSingleChapterText(const QString &path, const QString &format, int chapter) {
    if (path.isEmpty() || chapter < 0)
        return {};
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(path);
        if (pdf.open() && chapter < pdf.pageCount())
            return pdf.pageText(chapter);
    } else {
        EpubBook epub(path);
        if (epub.open() && chapter < int(epub.spine().size()))
            return epub.chapterText(chapter);
    }
    return {};
}

}  // namespace

QList<BookContextReader::Chapter> BookContextReader::textUpTo(const QString &path, const QString &format, int chapter,
                                                              qint64 offset, qint64 limit,
                                                              std::shared_ptr<std::atomic_bool> cancel) {
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

QList<BookContextReader::Chapter> BookContextReader::textOfBook(const QString &path, const QString &format, qint64 limit,
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

QString BookContextReader::readPageContext(const QString &path, const QString &format, int pageOrChapter, qint64 maxChars,
                                           const std::shared_ptr<std::atomic_bool> &cancel) {
    if (path.isEmpty() || pageOrChapter < 0 || maxChars <= 0 || (cancel && cancel->load()))
        return {};
    return readSingleChapterText(path, format, pageOrChapter).left(qsizetype(maxChars));
}

QString BookContextReader::readOpeningSample(const QString &path, const QString &format, qint64 maxChars) {
    if (path.isEmpty() || maxChars <= 0)
        return {};
    QString sample;
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(path);
        if (pdf.open()) {
            const int maxPages = qMin(3, pdf.pageCount());
            for (int i = 0; i < maxPages && sample.size() < maxChars; ++i) {
                const QString chunk = pdf.pageText(i).left(qsizetype(maxChars) - sample.size()).trimmed();
                if (!chunk.isEmpty())
                    sample += chunk + QLatin1Char('\n');
            }
        }
        return sample;
    }
    EpubBook book(path);
    if (book.open()) {
        const int maxCh = qMin(3, int(book.spine().size()));
        for (int i = 0; i < maxCh && sample.size() < maxChars; ++i) {
            const QString chunk = book.chapterText(i).left(qsizetype(maxChars) - sample.size()).trimmed();
            if (!chunk.isEmpty())
                sample += chunk + QLatin1Char('\n');
        }
    }
    return sample;
}

QString BookContextReader::detectReadingMode(const QString &format, const QString &title, const QStringList &tocTitles,
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

QString BookContextReader::joined(const QList<Chapter> &chapters, qint64 limit) {
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

QString BookContextReader::joinedStudyDocument(const QList<Chapter> &chapters, qint64 limit, int currentChapter) {
    if (chapters.isEmpty() || limit <= 0)
        return {};
    auto formatChapter = [](const Chapter &ch) {
        const QString heading = ch.title.isEmpty() ? QString() : QStringLiteral("## %1\n\n").arg(ch.title);
        return heading + ch.text.trimmed() + QStringLiteral("\n\n");
    };
    qint64 total = 0;
    for (const Chapter &ch : chapters)
        total += ch.text.trimmed().size() + ch.title.size() + 8;
    if (total <= limit) {
        QString out;
        out.reserve(qsizetype(total));
        for (const Chapter &ch : chapters)
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

QString BookContextReader::chapterDisplayLabel(const QList<Chapter> &chapters, int chapterIndex, const QString &format) {
    for (const Chapter &chapter : chapters)
        if (chapter.index == chapterIndex && !chapter.title.isEmpty())
            return chapter.title;
    return format == QLatin1String("pdf")
               ? QCoreApplication::translate("Companion", "Page %1").arg(chapterIndex + 1)
               : QCoreApplication::translate("Companion", "Chapter %1").arg(chapterIndex + 1);
}

QString BookContextReader::passageText(const PassageIndex::Hit &hit, const QList<Chapter> &chapters,
                                       const QString &format) {
    return QStringLiteral("[%1]\n%2")
        .arg(chapterDisplayLabel(chapters, hit.passage.chapter, format), hit.passage.text);
}

QString BookContextReader::makeExcerpt(const QString &text, qsizetype maxChars) {
    QString clean = text.simplified();
    if (clean.size() <= maxChars)
        return clean;
    qsizetype cut = clean.lastIndexOf(QLatin1Char(' '), maxChars);
    if (cut < maxChars / 2)
        cut = maxChars;
    return clean.left(cut).trimmed() + QChar(0x2026);
}

QString BookContextReader::makeChipQuote(const QString &text) {
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

QList<CompanionStore::Citation> BookContextReader::citationsFromHits(const QList<PassageIndex::Hit> &hits,
                                                                     const QList<Chapter> &chapters,
                                                                     const QString &format, int maxCount) {
    QList<CompanionStore::Citation> out;
    for (const PassageIndex::Hit &hit : hits) {
        bool duplicate = false;
        for (const CompanionStore::Citation &existing : out) {
            if (existing.chapter == hit.passage.chapter && qAbs(existing.offset - hit.passage.start) < 400) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        CompanionStore::Citation cite;
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

BookContextReader::FirstSeen BookContextReader::resolveFirstSeen(const QList<Chapter> &chapters, const QString &name,
                                                                 const QString &also, const QString &llmChapter,
                                                                 const QString &format, bool studyGlossary) {
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
        for (const Chapter &ch : chapters) {
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
        for (const Chapter &ch : chapters) {
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
            for (const Chapter &ch : chapters) {
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

QVariantList BookContextReader::parseWhosWhoEntries(const QJsonArray &entries, const QList<Chapter> &chapters,
                                                    const QString &format, bool isStudy) {
    QVariantList out;
    // Names as the book prints them, without its note markers ("Filby*").
    static const QRegularExpression marker(QStringLiteral("[*†‡§¹²³⁴⁵⁶⁷⁸⁹⁰]+(?=\\s|$)"));
    for (const QJsonValue &value : entries) {
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
        out.append(itemMap);
    }
    return out;
}

QJsonObject BookContextReader::extractJsonObject(const QString &raw) {
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

QString BookContextReader::shelfCitationLabel(const QString &bookTitle, const QString &format, int chapter) {
    const QString title = shortBookTitle(bookTitle);
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("%1, p. %2").arg(title).arg(chapter + 1);
    return QStringLiteral("%1, ch. %2").arg(title).arg(chapter + 1);
}

BookContextReader::ShelfContextResult BookContextReader::resolveShelfContext(
    const QList<CompanionStore::ShelfBook> &shelfBooks, const QList<PassageIndex::LibraryHit> &libHits,
    const QString &asked, const std::shared_ptr<std::atomic_bool> &cancel) {
    ShelfContextResult result;
    const auto cancelled = [&cancel] { return cancel && cancel->load(); };

    struct ResolvedBlock {
        int bookId = 0;
        QString bookTitle;
        QString format;
        int chapter = 0;
        qint64 offset = 0;
        QString label;
        QString text;
    };

    QHash<int, CompanionStore::ShelfBook> byId;
    for (const CompanionStore::ShelfBook &b : shelfBooks)
        byId.insert(b.id, b);

    QList<ResolvedBlock> blocks;
    QSet<QPair<int, int>> seenBookChapter;

    // 1. Resolve semantic hits from PassageIndex::searchLibrary().
    for (const PassageIndex::LibraryHit &hit : libHits) {
        if (cancelled())
            return {};
        if (!byId.contains(hit.bookId))
            continue;
        if (seenBookChapter.contains({hit.bookId, hit.chapter}))
            continue;
        const CompanionStore::ShelfBook book = byId.value(hit.bookId);
        const QString chText = readSingleChapterText(book.path, book.format, hit.chapter);
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

    // 2. Fallback / supplement via lexical BM25 across shelf books when vector hits are sparse.
    if (blocks.size() < 4 && !shelfBooks.isEmpty()) {
        struct LexicalEntry {
            CompanionStore::ShelfBook book;
            PassageIndex::Hit hit;
        };
        QList<LexicalEntry> candidates;
        const int maxBooksToScan = qMin<int>(14, int(shelfBooks.size()));
        for (int bi = 0; bi < maxBooksToScan && !cancelled(); ++bi) {
            const CompanionStore::ShelfBook &book = shelfBooks[bi];
            const QList<Chapter> chs = textOfBook(book.path, book.format, 120000, cancel);
            if (cancelled() || chs.isEmpty())
                continue;
            QList<PassageIndex::Passage> passages;
            for (const Chapter &ch : chs)
                passages += PassageIndex::split(ch.index, ch.text, 1100);
            const QList<PassageIndex::Hit> hits = PassageIndex::byWords(passages, asked, 2);
            for (const PassageIndex::Hit &h : hits)
                candidates.append({book, h});
        }
        if (cancelled())
            return {};
        std::sort(candidates.begin(), candidates.end(),
                  [](const LexicalEntry &a, const LexicalEntry &b) { return a.hit.score > b.hit.score; });
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
    if (cancelled())
        return {};

    for (const ResolvedBlock &b : blocks) {
        result.context += QStringLiteral("[%1]\n%2\n\n").arg(b.label, b.text);
        if (result.citations.size() < 6) {
            CompanionStore::Citation cite;
            cite.bookId = b.bookId;
            cite.bookTitle = b.bookTitle;
            cite.chapter = b.chapter;
            cite.offset = b.offset;
            cite.label = b.label;
            cite.excerpt = makeExcerpt(b.text);
            result.citations.append(cite);
        }
    }
    return result;
}

BookContextReader::LocalAskContext BookContextReader::buildLocalAskContext(const QList<PassageIndex::Hit> &hits,
                                                                           const QList<Chapter> &chapters,
                                                                           const QString &format, bool wholeBook,
                                                                           int currentChapter, qint64 localBudget) {
    LocalAskContext result;
    result.sources = citationsFromHits(hits, chapters, format, 4);
    const qint64 recentBudget = qMin<qint64>(4000, qMax<qint64>(1200, localBudget / 4));
    const qint64 passageBudget = qMax<qint64>(2000, localBudget - recentBudget - 2500);

    QList<PassageIndex::Hit> ordered = hits;
    std::sort(ordered.begin(), ordered.end(), [](const PassageIndex::Hit &a, const PassageIndex::Hit &b) {
        return a.passage.chapter != b.passage.chapter ? a.passage.chapter < b.passage.chapter
                                                      : a.passage.start < b.passage.start;
    });

    result.context = QStringLiteral("Passages that may matter:\n\n");
    qint64 used = 0;
    for (const PassageIndex::Hit &hit : ordered) {
        const QString block = passageText(hit, chapters, format) + QStringLiteral("\n\n");
        if (used > 0 && used + block.size() > passageBudget)
            break;
        result.context += block;
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
            result.context += QStringLiteral("Current section in view:\n\n") + aroundCurrent;
        result.how = QStringLiteral("passages found across the whole document");
    } else {
        result.context += QStringLiteral("What the reader read last:\n\n") + joined(chapters, recentBudget);
        result.how = QStringLiteral("passages found for the question, and the latest pages");
    }
    return result;
}

BookContextReader::LocalAskContext BookContextReader::buildCloudAskContext(const QList<Chapter> &chapters,
                                                                           const QString &asked,
                                                                           const QString &fallbackQuery,
                                                                           const QString &format, bool wholeBook,
                                                                           int currentChapter, qint64 limit) {
    LocalAskContext result;
    QList<PassageIndex::Passage> passages;
    for (const Chapter &ch : chapters)
        passages += PassageIndex::split(ch.index, ch.text);
    QList<PassageIndex::Hit> citeHits = PassageIndex::byWords(passages, asked, 4);
    if (citeHits.isEmpty() && !fallbackQuery.isEmpty())
        citeHits = PassageIndex::byWords(passages, fallbackQuery, 4);
    result.sources = citationsFromHits(citeHits, chapters, format, 4);
    result.context = wholeBook ? joinedStudyDocument(chapters, limit, currentChapter) : joined(chapters, limit);
    result.how = wholeBook ? QStringLiteral("the whole document")
                           : QStringLiteral("the whole book up to the reader's place");
    return result;
}

QList<CompanionStore::Citation> BookContextReader::citationsForAnswer(const QList<Chapter> &chapters,
                                                                      const QString &format, const QString &answer,
                                                                      int maxCount) {
    QList<PassageIndex::Passage> passages;
    for (const Chapter &ch : chapters)
        passages += PassageIndex::split(ch.index, ch.text);
    return citationsFromHits(PassageIndex::byWords(passages, answer, maxCount), chapters, format, maxCount);
}

QString BookContextReader::formatChapterTitles(const QList<Chapter> &chapters) {
    QString titles;
    for (const Chapter &ch : chapters)
        if (!ch.title.isEmpty())
            titles += QStringLiteral("- ") + ch.title + QLatin1Char('\n');
    return titles;
}

QString BookContextReader::systemInstructions(const QString &task, const QString &language, bool spoilerFree,
                                              bool allowHeadings) {
    static constexpr const char *kNoSpoilers =
        "The reader is partway through the book. You see only what they have read so far. Never reveal, hint at, or "
        "guess what happens later, even if you know the book; if they ask about something not yet in the text, say "
        "it hasn't come up yet.";
    QString out = task;
    if (spoilerFree)
        out += QStringLiteral("\n\n") + QString::fromLatin1(kNoSpoilers);
    out += allowHeadings
               ? QStringLiteral("\nWrite in %1. Be concise, well-structured, and clear.").arg(language)
               : QStringLiteral("\nWrite in %1. Plain prose, no headings; short paragraphs.").arg(language);
    return out;
}

QString BookContextReader::defineTaskPrompt(bool isStudy) {
    return isStudy
               ? QStringLiteral(
                     "You are an academic and technical reference dictionary. Format the first line as "
                     "*domain / part of speech* — concise one-line definition of how this term, acronym, or "
                     "symbol is used in this passage. Add one brief sentence on its significance if helpful. "
                     "Use LaTeX ($...$) for notation when needed. At most 70 words.")
               : QStringLiteral(
                     "You are a literary dictionary. Format the first line as *part of speech* — concise one-line "
                     "definition for how the word is used in this passage. If the usage is archaic, rare, or "
                     "figurative, add one brief sentence after. At most 60 words.");
}

QString BookContextReader::explainTaskPrompt(bool isStudy) {
    return isStudy
               ? QStringLiteral(
                     "Explain the selected passage, equation, or claim clearly for a reader studying this document: "
                     "unpack the intuition, notation, and reasoning using the surrounding text for context. Use "
                     "LaTeX ($...$ or $$...$$) for mathematical expressions when helpful. Be brief: one to three "
                     "short paragraphs.")
               : QStringLiteral(
                     "Explain the selected words to a reader in the middle of this book: what they mean, what is "
                     "happening, and any allusion, history, idiom or old usage a modern reader might miss. Use the "
                     "text before it for context. Be brief: one to three short paragraphs.");
}

QString BookContextReader::askTaskPrompt(bool isStudy, const QString &how) {
    return isStudy
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
}

QString BookContextReader::shelfAskTaskPrompt(const QString &whereDesc) {
    return QStringLiteral(
               "Synthesize a grounded answer across multiple documents from %1 using only the "
               "excerpts below. Cite sources inline using exact bracketed labels like "
               "[Book Title, p. X] (matching the labels above). Compare, connect, or contrast "
               "documents when helpful, and use LaTeX ($...$ or $$...$$) for mathematical "
               "expressions when relevant. If the excerpts don't cover the question, say so plainly.")
        .arg(whereDesc);
}

QString BookContextReader::recapTaskPrompt(bool isStudy) {
    return isStudy
               ? QStringLiteral(
                     "Write a crisp, structured Study Brief for this document. Organize it into four concise "
                     "sections with bold lead-ins on separate paragraphs:\n"
                     "**Core Claims** — the central thesis, problem statement, or chapter objectives.\n"
                     "**Method** — the approach, model, proofs, experimental setup, or analytical framework.\n"
                     "**Key Findings** — the main results, theorems, or quantitative takeaways.\n"
                     "**Limitations** — caveats, assumptions, edge cases, or open questions.\n"
                     "(If this is a textbook chapter rather than a paper, adapt the four leads to a tight chapter "
                     "study guide.) Use standard LaTeX math ($...$ or $$...$$) for key equations or notation when "
                     "helpful. Stay strictly grounded in the provided text.")
               : QStringLiteral(
                     "Write \"Previously…\" for a reader coming back to this book: recap the story so far in 2 to 4 "
                     "crisp, engaging paragraphs, focusing most on the latest events and ending right where they "
                     "stopped. Name the key people and tensions that matter right now.");
}

QString BookContextReader::whosWhoTaskPrompt(bool isStudy) {
    return isStudy
               ? QStringLiteral(
                     "Extract a crisp Glossary & Notation reference from this document, most central first, at most 30 "
                     "entries: mathematical symbols (written in clean Unicode such as θ, ℒ, ∇, α, Σ), acronyms, and "
                     "key domain terms. For each entry set: \"name\" the symbol, acronym, or term itself, \"also\" "
                     "its expansion, alternate symbol, or notation (or \"\"), \"who\" one concise, accurate definition "
                     "grounded in the document (inline LaTeX $...$ is welcome), \"chapter\" the exact section heading "
                     "or page number where it is first defined or introduced.")
               : QStringLiteral(
                     "List the notable people (and named animals or beings) introduced in the book so far, most "
                     "important first, at most 30: \"name\" as most often called, \"also\" other names or titles "
                     "(or \"\"), \"who\" one crisp, spoiler-free sentence on who they are and their role so far, "
                     "\"chapter\" the exact chapter title or number where they first appear.");
}

QString BookContextReader::snipTaskPrompt(const QString &normAction, const QString &userLabel) {
    if (normAction == QLatin1String("latex")) {
        return QStringLiteral(
            "Transcribe the mathematical expression(s) in the attached image into clean, accurate, copyable LaTeX. "
            "Provide the raw LaTeX inside a single ```latex code block first, followed by a brief note only if any "
            "symbol is ambiguous. Never invent terms not visible in the image.");
    }
    if (normAction == QLatin1String("table")) {
        return QStringLiteral(
            "Extract the table in the attached image accurately as a clean Markdown table preserving all headers, "
            "rows, units, and numeric precision, followed by a compact ```csv code block for easy copying. Never "
            "omit rows or fabricate values.");
    }
    if (normAction == QLatin1String("ask") && !userLabel.isEmpty()) {
        return QStringLiteral(
            "Answer the reader's question about the attached snipped image from the document they are reading. "
            "Ground your answer in what is visible in the image and any surrounding text provided. Use LaTeX "
            "($...$ or $$...$$) for mathematical expressions when helpful.");
    }
    return QStringLiteral(
        "Examine the attached snipped image (equation, figure, chart, diagram, or passage) from the document and "
        "provide a clear, step-by-step explanation. Unpack notation, intuition, and key takeaways, using LaTeX "
        "($...$ or $$...$$) for mathematical expressions when helpful.");
}

QJsonObject BookContextReader::whosWhoSchema(bool isStudy) {
    const QJsonObject item{{"type", "object"},
                           {"properties", QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                                                      {"also", QJsonObject{{"type", "string"}}},
                                                      {"who", QJsonObject{{"type", "string"}}},
                                                      {"chapter", QJsonObject{{"type", "string"}}}}},
                           {"required", QJsonArray{"name", "also", "who", "chapter"}},
                           {"additionalProperties", false}};
    const QString arrayKey = isStudy ? QStringLiteral("terms") : QStringLiteral("people");
    return QJsonObject{{"type", "object"},
                       {"properties", QJsonObject{{arrayKey, QJsonObject{{"type", "array"}, {"items", item}}}}},
                       {"required", QJsonArray{arrayKey}},
                       {"additionalProperties", false}};
}

QString BookContextReader::makeSnipThumbnailDataUrl(const QByteArray &pngBytes) {
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

bool BookContextReader::decodeAndNormalizeSnipBase64(const QString &base64Png, QByteArray *cleanPngOut,
                                                     QString *errorOut) {
    QString raw = base64Png.trimmed();
    const qsizetype comma = raw.indexOf(QLatin1Char(','));
    if (raw.startsWith(QLatin1String("data:"), Qt::CaseInsensitive) && comma > 0)
        raw = raw.mid(comma + 1).trimmed();
    const QByteArray pngBytes = QByteArray::fromBase64(raw.toLatin1());
    if (pngBytes.isEmpty()) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("Companion", "Couldn't read the snipped image.");
        return false;
    }
    QImage decoded;
    if (!decoded.loadFromData(pngBytes)) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("Companion", "Couldn't decode the snipped image.");
        return false;
    }
    QByteArray cleanPng = pngBytes;
    if (decoded.width() > 1600 || decoded.height() > 1600) {
        decoded = decoded.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        cleanPng.clear();
        QBuffer buf(&cleanPng);
        buf.open(QIODevice::WriteOnly);
        decoded.save(&buf, "PNG");
    }
    if (cleanPngOut)
        *cleanPngOut = std::move(cleanPng);
    return true;
}
