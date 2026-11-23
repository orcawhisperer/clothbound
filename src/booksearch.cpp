#include "booksearch.h"

#include "aiservices.h"
#include "companion.h"
#include "epubbook.h"
#include "passageindex.h"
#include "pdfbook.h"
#include "reader.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

namespace {
struct Normalized {
    QString text;
    QList<qint64> starts;
    QList<qint64> ends;
};

// Normalizes chapter or query text for matching while keeping exact UTF-16
// source offsets for every emitted character: collapses whitespace runs
// (including PDF "\r\n" and HTML indentation), strips soft hyphens, PDFium
// U+FFFE line-break hyphens and combining marks, expands common ligatures,
// and folds case, accents, quotes and dashes.
Normalized normalizeText(const QString &raw) {
    Normalized out;
    out.text.reserve(raw.size());
    out.starts.reserve(raw.size());
    out.ends.reserve(raw.size());
    auto emitChar = [&](QChar ch, qint64 start, qint64 end) {
        out.text.append(ch);
        out.starts.append(start);
        out.ends.append(end);
    };
    const qsizetype n = raw.size();
    for (qsizetype i = 0; i < n; ++i) {
        QChar c = raw[i];
        // PDFium marks a word broken across lines with U+FFFE before \r\n.
        if (c == QChar(0xFFFE)) {
            if (i + 1 < n && raw[i + 1] == QLatin1Char('\r'))
                ++i;
            if (i + 1 < n && raw[i + 1] == QLatin1Char('\n'))
                ++i;
            continue;
        }
        if (c == QChar(0x00AD))
            continue;
        if (c.category() == QChar::Mark_NonSpacing) {
            if (!out.ends.isEmpty())
                out.ends.back() = i + 1;
            continue;
        }
        if (c.isSpace()) {
            if (out.text.isEmpty() || out.text.back() != QLatin1Char(' '))
                emitChar(QLatin1Char(' '), i, i + 1);
            else
                out.ends.back() = i + 1;
            continue;
        }
        switch (c.unicode()) {
        case 0xFB00: emitChar('f', i, i + 1); emitChar('f', i, i + 1); continue;
        case 0xFB01: emitChar('f', i, i + 1); emitChar('i', i, i + 1); continue;
        case 0xFB02: emitChar('f', i, i + 1); emitChar('l', i, i + 1); continue;
        case 0xFB03: emitChar('f', i, i + 1); emitChar('f', i, i + 1); emitChar('i', i, i + 1); continue;
        case 0xFB04: emitChar('f', i, i + 1); emitChar('f', i, i + 1); emitChar('l', i, i + 1); continue;
        case 0x00DF: emitChar('s', i, i + 1); emitChar('s', i, i + 1); continue;
        default: break;
        }
        while (c.decompositionTag() == QChar::Canonical && !c.decomposition().isEmpty())
            c = c.decomposition().front();
        switch (c.unicode()) {
        case 0x2018: case 0x2019: case 0x201B: case 0x2032:
            c = '\'';
            break;
        case 0x201C: case 0x201D: case 0x201F: case 0x2033:
            c = '"';
            break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212:
            c = '-';
            break;
        default:
            break;
        }
        emitChar(c.toLower(), i, i + 1);
    }
    return out;
}

QList<QPair<qint64, qint64>> matchNormalized(const Normalized &haystack, const QString &needle, int limit) {
    QList<QPair<qint64, qint64>> found;
    if (needle.isEmpty() || haystack.text.isEmpty())
        return found;
    qsizetype at = haystack.text.indexOf(needle);
    while (at >= 0 && found.size() < limit) {
        const qint64 start = haystack.starts[at];
        const qint64 end = haystack.ends[at + needle.size() - 1];
        found.append({start, end});
        at = haystack.text.indexOf(needle, at + needle.size());
    }
    return found;
}

bool isTooShort(const QString &wanted) {
    if (wanted.isEmpty())
        return true;
    if (wanted.size() >= 2)
        return false;
    const QChar::Script script = wanted.front().script();
    return script != QChar::Script_Han && script != QChar::Script_Hiragana &&
           script != QChar::Script_Katakana && script != QChar::Script_Hangul;
}
}  // namespace

struct BookSearch::BookCache {
    QMutex mutex;
    QString path;
    bool pdf = false;
    int chapters = -1;
    QHash<int, QString> texts;
    QHash<int, Normalized> normalized;
};

BookSearch::BookSearch(Reader *reader, QObject *parent) : QObject(parent), m_reader(reader) {
    connect(reader, &Reader::bookChanged, this, [this] {
        m_cache.reset();
        clear();
    });
}

BookSearch::~BookSearch() {
    if (m_cancel)
        *m_cancel = true;
    ++m_meaningRun;
    if (m_index)
        m_index->cancel();
}

QString BookSearch::fold(const QString &text) {
    QString folded(text.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < text.size(); ++i) {
        QChar c = text[i];
        if (c.isSpace()) {
            folded[i] = ' ';
            continue;
        }
        // "é" is "e" and a mark: the letter alone (and again, for letters
        // with more than one mark).
        while (c.decompositionTag() == QChar::Canonical && !c.decomposition().isEmpty())
            c = c.decomposition().front();
        switch (c.unicode()) {
        case 0x2018: case 0x2019: case 0x201B: case 0x2032:
            c = '\'';
            break;
        case 0x201C: case 0x201D: case 0x201F: case 0x2033:
            c = '"';
            break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212:
            c = '-';
            break;
        default:
            break;
        }
        folded[i] = c.toLower();
    }
    return folded;
}

QList<QPair<qint64, qint64>> BookSearch::matches(const QString &text, const QString &query, int limit) {
    QList<QPair<qint64, qint64>> found;
    if (query.isEmpty())
        return found;
    qsizetype at = text.indexOf(query);
    while (at >= 0 && found.size() < limit) {
        found.append({at, at + query.size()});
        at = text.indexOf(query, at + query.size());
    }
    return found;
}

QVariantMap BookSearch::hit(const QString &text, int chapter, qint64 start, qint64 end) {
    constexpr qsizetype before = 48, after = 80;
    qsizetype from = qMax<qsizetype>(0, start - before);
    if (from > 0 && from < text.size() && text[from].isLowSurrogate())
        --from;
    if (from > 0) {
        // Start at a word boundary (any whitespace, not only ASCII space).
        qsizetype space = -1;
        for (qsizetype i = from; i < start; ++i)
            if (text[i].isSpace()) {
                space = i;
                break;
            }
        if (space >= 0)
            from = space + 1;
    }
    qsizetype to = qMin<qsizetype>(text.size(), end + after);
    if (to > 0 && to < text.size() && text[to].isLowSurrogate())
        ++to;
    if (to < text.size()) {
        qsizetype space = -1;
        for (qsizetype i = to - 1; i > end; --i)
            if (text[i].isSpace()) {
                space = i;
                break;
            }
        if (space > end)
            to = space;
    }
    auto cleanSnippet = [](QString s) {
        s.remove(QStringLiteral("\uFFFE\r\n"));
        s.remove(QStringLiteral("\uFFFE\n"));
        s.remove(QChar(0xFFFE));
        s.remove(QChar(0x00AD));
        return s.simplified();
    };
    QString lead = cleanSnippet(text.mid(from, start - from));
    QString tail = text.mid(end, to - end);
    const bool spaceAfter = !tail.isEmpty() && tail.front().isSpace();
    tail = cleanSnippet(tail);
    if (from > 0)
        lead.prepend(QChar(0x2026));
    if (to < text.size())
        tail.append(QChar(0x2026));
    if (!lead.isEmpty() && from < start && text[start - 1].isSpace())
        lead.append(' ');
    if (spaceAfter && !tail.isEmpty())
        tail.prepend(' ');
    return {{"chapter", chapter},
            {"start", start},
            {"end", end},
            {"before", lead},
            {"match", cleanSnippet(text.mid(start, end - start))},
            {"after", tail}};
}

void BookSearch::find(const QString &query) {
    if (m_byMeaning) {
        findByMeaning(query);
        return;
    }
    const QString wanted = query.simplified();
    if (wanted == m_query && (m_searching || !m_results.isEmpty()))
        return;
    if (m_cancel)
        *m_cancel = true;
    ++m_meaningRun;
    if (m_index)
        m_index->cancel();
    m_preparing.clear();
    m_query = wanted;
    m_results.clear();
    m_capped = false;
    m_current = -1;
    m_searching = false;
    const QString path = m_reader->bookPath();
    const QString needle = normalizeText(wanted).text.trimmed();
    // Single Latin characters match everything; single CJK characters are words.
    if (isTooShort(wanted) || needle.isEmpty() || path.isEmpty()) {
        emit changed();
        emit currentChanged();
        return;
    }
    m_searching = true;
    emit changed();
    emit currentChanged();

    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const bool pdf = m_reader->format() == QStringLiteral("pdf");
    if (!m_cache || m_cache->path != path || m_cache->pdf != pdf) {
        m_cache = std::make_shared<BookCache>();
        m_cache->path = path;
        m_cache->pdf = pdf;
    }
    auto cache = m_cache;
    QPointer<BookSearch> self(this);
    QThreadPool::globalInstance()->start([self, cancel, path, needle, pdf, cache] {
        std::unique_ptr<EpubBook> epub;
        std::unique_ptr<PdfBook> pages;
        auto ensureOpen = [&]() -> bool {
            if (pdf) {
                if (!pages) {
                    pages = std::make_unique<PdfBook>(path);
                    if (!pages->open())
                        return false;
                }
                return true;
            }
            if (!epub) {
                epub = std::make_unique<EpubBook>(path);
                if (!epub->open())
                    return false;
            }
            return true;
        };

        int chapters = -1;
        {
            QMutexLocker lock(&cache->mutex);
            chapters = cache->chapters;
        }
        if (chapters < 0 && ensureOpen()) {
            chapters = pdf ? pages->pageCount() : int(epub->spine().size());
            QMutexLocker lock(&cache->mutex);
            cache->chapters = chapters;
        }

        int found = 0;
        for (int chapter = 0; chapter < chapters && !*cancel && found < limit; ++chapter) {
            QString text;
            Normalized norm;
            bool cached = false;
            {
                QMutexLocker lock(&cache->mutex);
                auto it = cache->normalized.constFind(chapter);
                if (it != cache->normalized.cend()) {
                    norm = *it;
                    text = cache->texts.value(chapter);
                    cached = true;
                }
            }
            if (!cached) {
                if (!ensureOpen())
                    break;
                text = pdf ? pages->pageText(chapter) : epub->chapterText(chapter);
                norm = normalizeText(text);
                QMutexLocker lock(&cache->mutex);
                cache->texts.insert(chapter, text);
                cache->normalized.insert(chapter, norm);
            }
            QVariantList hits;
            for (const auto &[start, end] : matchNormalized(norm, needle, limit - found))
                hits.append(hit(text, chapter, start, end));
            found += int(hits.size());
            if (hits.isEmpty())
                continue;
            QMetaObject::invokeMethod(
                qApp,
                [self, cancel, hits] {
                    if (!self || *cancel)
                        return;
                    self->m_results += hits;
                    emit self->changed();
                },
                Qt::QueuedConnection);
        }
        const bool capped = found >= limit;
        QMetaObject::invokeMethod(
            qApp,
            [self, cancel, capped] {
                if (!self || *cancel)
                    return;
                self->m_searching = false;
                self->m_capped = capped;
                emit self->changed();
            },
            Qt::QueuedConnection);
    });
}

void BookSearch::clear() {
    if (m_cancel)
        *m_cancel = true;
    m_cancel.reset();
    ++m_meaningRun;
    if (m_index)
        m_index->cancel();
    const bool had = !m_query.isEmpty() || !m_results.isEmpty() || m_searching || !m_preparing.isEmpty();
    m_query.clear();
    m_results.clear();
    m_preparing.clear();
    m_searching = false;
    m_capped = false;
    m_current = -1;
    if (had) {
        emit changed();
        emit currentChanged();
    }
}

void BookSearch::setCurrent(int current) {
    if (m_results.isEmpty())
        current = -1;
    else
        current = (current % int(m_results.size()) + int(m_results.size())) % int(m_results.size());
    if (current == m_current)
        return;
    m_current = current;
    emit currentChanged();
}

QVariantList BookSearch::inChapter(int chapter) const {
    QVariantList spans;
    for (const QVariant &value : m_results) {
        const QVariantMap hit = value.toMap();
        if (hit.value("chapter").toInt() == chapter)
            spans.append(QVariantMap{{"chapter", chapter}, {"start", hit.value("start")}, {"end", hit.value("end")}});
    }
    return spans;
}

// --- By meaning -----------------------------------------------------------------

void BookSearch::setMeaning(AiServices *ai, const QString &databaseConnection) {
    m_ai = ai;
    m_index = new PassageIndex(ai, databaseConnection, this);
    connect(ai, &AiServices::changed, this, &BookSearch::changed);
    connect(ai, &AiServices::routingChanged, this, &BookSearch::changed);
}

bool BookSearch::canSearchByMeaning() const { return m_ai && m_ai->embedder(); }

QString BookSearch::where() const { return m_ai ? m_ai->whereFor(QStringLiteral("search")) : QString(); }

void BookSearch::setByMeaning(bool byMeaning) {
    if (byMeaning == m_byMeaning)
        return;
    m_byMeaning = byMeaning;
    const QString again = m_query;
    clear();
    emit changed();
    if (!again.isEmpty())
        find(again);
}

void BookSearch::findByMeaning(const QString &query) {
    const QString wanted = query.trimmed();
    if (m_cancel)
        *m_cancel = true;
    if (m_index)
        m_index->cancel();
    m_query = wanted;
    m_results.clear();
    m_current = -1;
    m_capped = false;
    m_preparing.clear();
    const int run = ++m_meaningRun;
    if (wanted.size() < 3 || !canSearchByMeaning() || m_reader->bookPath().isEmpty()) {
        m_searching = false;
        emit changed();
        emit currentChanged();
        return;
    }
    m_searching = true;
    emit changed();
    emit currentChanged();
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int bookId = m_reader->bookId();
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    QPointer<BookSearch> self(this);
    QThreadPool::globalInstance()->start([self, cancel, path, format, run, wanted, bookId] {
        if (*cancel)
            return;
        // The whole book, as search sees it.
        const QList<Companion::Chapter> chapters = Companion::textUpTo(path, format, 1 << 30, -1);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, cancel, chapters, run, wanted, bookId] {
                if (!self || *cancel || run != self->m_meaningRun)
                    return;
                QList<PassageIndex::Passage> passages;
                QHash<int, QString> texts;
                for (const Companion::Chapter &chapter : chapters) {
                    passages += PassageIndex::split(chapter.index, chapter.text);
                    texts.insert(chapter.index, chapter.text);
                }
                const auto search = [self, cancel, run, wanted, bookId, passages, texts] {
                    self->m_index->search(
                        bookId, passages, wanted, 25, [self, cancel, run, texts](const QList<PassageIndex::Hit> &hits, bool byMeaning) {
                            if (!self || *cancel || run != self->m_meaningRun)
                                return;
                            constexpr double minMeaningScore = 0.25;
                            for (const PassageIndex::Hit &found : hits) {
                                if (byMeaning && found.score < minMeaningScore)
                                    continue;
                                QVariantMap hit = BookSearch::hit(texts.value(found.passage.chapter), found.passage.chapter,
                                                                  found.passage.start,
                                                                  qMin(found.passage.end, found.passage.start + 160));
                                hit.insert(QStringLiteral("score"), found.score);
                                self->m_results.append(hit);
                            }
                            self->m_searching = false;
                            self->m_preparing.clear();
                            emit self->changed();
                            emit self->currentChanged();
                        });
                };
                if (self->m_index->indexed(bookId) >= passages.size()) {
                    search();
                    return;
                }
                self->m_preparing = tr("Reading the book for meaning…");
                emit self->changed();
                self->m_index->build(
                    bookId, passages,
                    [self, cancel, run](double share) {
                        if (!self || *cancel || run != self->m_meaningRun)
                            return;
                        self->m_preparing = tr("Reading the book for meaning… %1%").arg(qRound(share * 100));
                        emit self->changed();
                    },
                    [self, cancel, run, search](const QString &problem) {
                        if (!self || *cancel || run != self->m_meaningRun)
                            return;
                        if (!problem.isEmpty()) {
                            self->m_preparing = problem;
                            self->m_searching = false;
                            emit self->changed();
                            return;
                        }
                        search();
                    });
            },
            Qt::QueuedConnection);
    });
}
