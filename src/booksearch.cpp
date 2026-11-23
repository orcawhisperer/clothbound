#include "booksearch.h"

#include "epubbook.h"
#include "aiservices.h"
#include "companion.h"
#include "passageindex.h"
#include "pdfbook.h"
#include "reader.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>

BookSearch::BookSearch(Reader *reader, QObject *parent) : QObject(parent), m_reader(reader) {
    connect(reader, &Reader::bookChanged, this, &BookSearch::clear);
}

BookSearch::~BookSearch() {
    if (m_cancel)
        *m_cancel = true;
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
    if (from > 0) {
        // Start at a word.
        const qsizetype space = text.indexOf(' ', from);
        if (space >= 0 && space < start)
            from = space + 1;
    }
    qsizetype to = qMin<qsizetype>(text.size(), end + after);
    if (to < text.size()) {
        const qsizetype space = text.lastIndexOf(' ', to);
        if (space > end)
            to = space;
    }
    QString lead = text.mid(from, start - from).simplified();
    QString tail = text.mid(end, to - end);
    const bool spaceAfter = !tail.isEmpty() && tail.front().isSpace();
    tail = tail.simplified();
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
            {"match", text.mid(start, end - start).simplified()},
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
    m_query = wanted;
    m_results.clear();
    m_capped = false;
    m_current = -1;
    m_searching = false;
    const QString path = m_reader->bookPath();
    // A letter or two finds everything; it waits for a little more.
    if (wanted.size() < 2 || path.isEmpty()) {
        emit changed();
        emit currentChanged();
        return;
    }
    m_searching = true;
    emit changed();
    emit currentChanged();

    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const QString folded = fold(wanted);
    QPointer<BookSearch> self(this);
    const bool pdf = m_reader->format() == QStringLiteral("pdf");
    QThreadPool::globalInstance()->start([self, cancel, path, folded, pdf] {
        // Its own copy of the book, so the page can go on reading from its own.
        EpubBook book(pdf ? QString() : path);
        PdfBook pages(pdf ? path : QString());
        int found = 0;
        if (pdf ? pages.open() : book.open()) {
            const int chapters = pdf ? pages.pageCount() : int(book.spine().size());
            for (int chapter = 0; chapter < chapters && !*cancel && found < limit; ++chapter) {
                const QString text = pdf ? pages.pageText(chapter) : book.chapterText(chapter);
                QVariantList hits;
                for (const auto &[start, end] : matches(fold(text), folded, limit - found))
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
    const bool had = !m_query.isEmpty() || !m_results.isEmpty() || m_searching;
    m_query.clear();
    m_results.clear();
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
            spans.append(QVariantMap{{"start", hit.value("start")}, {"end", hit.value("end")}});
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
    m_query = wanted;
    m_results.clear();
    m_current = -1;
    m_capped = false;
    const int run = ++m_meaningRun;
    if (wanted.size() < 3 || !canSearchByMeaning() || m_reader->bookPath().isEmpty()) {
        m_searching = false;
        emit changed();
        emit currentChanged();
        return;
    }
    m_searching = true;
    m_preparing.clear();
    emit changed();
    emit currentChanged();
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int bookId = m_reader->bookId();
    QPointer<BookSearch> self(this);
    QThreadPool::globalInstance()->start([self, path, format, run, wanted, bookId] {
        // The whole book, as search sees it.
        const QList<Companion::Chapter> chapters = Companion::textUpTo(path, format, 1 << 30, -1);
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, run, wanted, bookId] {
                if (!self || run != self->m_meaningRun)
                    return;
                QList<PassageIndex::Passage> passages;
                QHash<int, QString> texts;
                for (const Companion::Chapter &chapter : chapters) {
                    passages += PassageIndex::split(chapter.index, chapter.text);
                    texts.insert(chapter.index, chapter.text);
                }
                const auto search = [self, run, wanted, bookId, passages, texts] {
                    self->m_index->search(bookId, passages, wanted, 25, [self, run, texts](const QList<PassageIndex::Hit> &hits, bool) {
                        if (!self || run != self->m_meaningRun)
                            return;
                        for (const PassageIndex::Hit &found : hits) {
                            QVariantMap hit = BookSearch::hit(texts.value(found.passage.chapter), found.passage.chapter,
                                                              found.passage.start, qMin(found.passage.end, found.passage.start + 160));
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
                    [self, run](double share) {
                        if (!self || run != self->m_meaningRun)
                            return;
                        self->m_preparing = tr("Reading the book for meaning… %1%").arg(qRound(share * 100));
                        emit self->changed();
                    },
                    [self, run, search](const QString &problem) {
                        if (!self || run != self->m_meaningRun)
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
