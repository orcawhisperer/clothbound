#include "booksearch.h"

#include "epubbook.h"
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
    if (wanted.size() < 2 || path.isEmpty() || m_reader->format() != QStringLiteral("epub")) {
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
    QThreadPool::globalInstance()->start([self, cancel, path, folded] {
        // Its own copy of the book, so the page can go on reading from its own.
        EpubBook book(path);
        int found = 0;
        if (book.open()) {
            const int chapters = int(book.spine().size());
            for (int chapter = 0; chapter < chapters && !*cancel && found < limit; ++chapter) {
                const QString text = book.chapterText(chapter);
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
