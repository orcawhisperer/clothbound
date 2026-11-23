#include "librarymodel.h"

#include "library.h"

#include <algorithm>
#include <tuple>

WorksModel::WorksModel(Library *library) : QAbstractListModel(library), m_library(library) {}

int WorksModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_works.size()); }

QHash<int, QByteArray> WorksModel::roleNames() const {
    return {{KeyRole, "key"},
            {BookIdRole, "bookId"},
            {TitleRole, "title"},
            {AuthorRole, "author"},
            {EditionLineRole, "editionLine"},
            {EditionsRole, "editions"},
            {CoverRole, "cover"},
            {ProgressRole, "progress"},
            {FinishedRole, "finished"},
            {FormatRole, "format"},
            {ProblemRole, "problem"},
            {YearRole, "year"},
            {OpenedRole, "opened"},
            {SeriesRole, "series"},
            {SeriesIndexRole, "seriesIndex"},
            {MinutesLeftRole, "minutesLeft"},
            {SecondsReadRole, "secondsRead"},
            {AddedAtRole, "addedAt"}};
}

QVariant WorksModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_works.size())
        return {};
    const Work &work = m_works[index.row()];
    const BookRecord book = m_books.value(work.representative);
    switch (role) {
    case KeyRole:
        return work.key;
    case BookIdRole:
        return book.id;
    case TitleRole:
        return book.title;
    case AuthorRole:
        return book.author;
    case EditionLineRole:
        return book.editionLine();
    case EditionsRole:
        return int(work.books.size());
    case CoverRole:
        return m_library->coverUrl(book.id);
    case ProgressRole:
        return book.progress;
    case FinishedRole:
        return book.finishedAt > 0;
    case FormatRole:
        return book.format;
    case ProblemRole:
        return book.problem;
    case YearRole:
        return book.year;
    case OpenedRole:
        return book.openedAt > 0;
    case SeriesRole:
        return book.series;
    case SeriesIndexRole:
        return book.seriesIndex;
    case MinutesLeftRole:
        return m_library->minutesLeft(book.id);
    case SecondsReadRole:
        return double(book.secondsRead);
    case AddedAtRole:
        return double(book.addedAt);
    }
    return {};
}

QVariantMap WorksModel::get(int row) const {
    QVariantMap map;
    if (row < 0 || row >= m_works.size())
        return map;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return map;
}

int WorksModel::rowOfBook(int bookId) const {
    return m_bookRow.value(bookId, -1);
}

void WorksModel::setSearchText(const QString &text) {
    if (text == m_search)
        return;
    m_search = text;
    emit searchTextChanged();
    rebuild();
}

void WorksModel::setSortOrder(const QString &order) {
    if (order == m_sort)
        return;
    m_sort = order;
    emit sortOrderChanged();
    rebuild();
}

void WorksModel::setGrouped(bool grouped) {
    if (grouped == m_grouped)
        return;
    m_grouped = grouped;
    emit groupedChanged();
    rebuild();
}

void WorksModel::setStatusFilter(const QString &status) {
    const QString normalized = (status == QLatin1String("all")) ? QString() : status.trimmed().toLower();
    if (normalized == m_statusFilter)
        return;
    m_statusFilter = normalized;
    emit statusFilterChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setFormatFilter(const QString &format) {
    const QString normalized = (format == QLatin1String("all")) ? QString() : format.trimmed().toLower();
    if (normalized == m_formatFilter)
        return;
    m_formatFilter = normalized;
    emit formatFilterChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setSeriesFilter(const QString &series) {
    const QString normalized = series.trimmed();
    if (normalized == m_seriesFilter)
        return;
    m_seriesFilter = normalized;
    emit seriesFilterChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setCollectionId(int id) {
    const int normalized = qMax(0, id);
    if (normalized == m_collectionId)
        return;
    m_collectionId = normalized;
    emit collectionIdChanged();
    emit filtersChanged();
    rebuild();
}

bool WorksModel::hasActiveFilters() const {
    return !m_statusFilter.isEmpty() || !m_formatFilter.isEmpty() || !m_seriesFilter.isEmpty() || m_collectionId > 0;
}

void WorksModel::clearFilters() {
    const bool statusChanged = !m_statusFilter.isEmpty();
    const bool formatChanged = !m_formatFilter.isEmpty();
    const bool seriesChanged = !m_seriesFilter.isEmpty();
    const bool colChanged = m_collectionId > 0;
    if (!statusChanged && !formatChanged && !seriesChanged && !colChanged)
        return;
    m_statusFilter.clear();
    m_formatFilter.clear();
    m_seriesFilter.clear();
    m_collectionId = 0;
    if (statusChanged)
        emit statusFilterChanged();
    if (formatChanged)
        emit formatFilterChanged();
    if (seriesChanged)
        emit seriesFilterChanged();
    if (colChanged)
        emit collectionIdChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setBooks(const QHash<int, BookRecord> &books) {
    m_books = books;
    m_searchable.clear();
    m_searchable.reserve(m_books.size());
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it)
        m_searchable.insert(it.key(), searchableFor(*it));
    rebuildSeriesList();
    rebuild();
}

void WorksModel::setCollections(const QHash<int, QSet<int>> &collectionBooks) {
    m_collectionBooks = collectionBooks;
    if (m_collectionId > 0)
        rebuild();
}

void WorksModel::updateBook(const BookRecord &book) {
    const auto oldIt = m_books.constFind(book.id);
    const bool existed = oldIt != m_books.cend();
    const QString searchKey = searchableFor(book);
    const bool seriesChanged = !existed || oldIt->series != book.series || oldIt->seriesIndex != book.seriesIndex;
    const bool shapeChanged = !existed || oldIt->visible() != book.visible() || oldIt->workKey != book.workKey ||
                              oldIt->openedAt != book.openedAt || oldIt->addedAt != book.addedAt ||
                              oldIt->finishedAt != book.finishedAt || m_sort == QLatin1String("progress") ||
                              (m_sort == QLatin1String("series") && seriesChanged) ||
                              !m_statusFilter.isEmpty() || !m_seriesFilter.isEmpty() ||
                              m_searchable.value(book.id) != searchKey;
    m_books.insert(book.id, book);
    m_searchable.insert(book.id, searchKey);
    if (seriesChanged)
        rebuildSeriesList();
    if (!shapeChanged) {
        const int row = m_bookRow.value(book.id, -1);
        if (row >= 0 && row < m_works.size())
            emit dataChanged(index(row), index(row));
        return;
    }
    rebuild();
}

void WorksModel::rebuildSeriesList() {
    QSet<QString> unique;
    for (const auto &book : std::as_const(m_books))
        if (book.visible() && !book.series.trimmed().isEmpty())
            unique.insert(book.series.trimmed());
    QStringList list(unique.cbegin(), unique.cend());
    std::sort(list.begin(), list.end(),
              [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
    if (list != m_seriesList) {
        m_seriesList = list;
        emit seriesListChanged();
    }
}

bool WorksModel::passesFilters(const BookRecord &book) const {
    if (!m_statusFilter.isEmpty()) {
        if (m_statusFilter == QLatin1String("reading")) {
            if (book.finishedAt > 0 || book.openedAt <= 0)
                return false;
        } else if (m_statusFilter == QLatin1String("unread")) {
            if (book.finishedAt > 0 || book.openedAt > 0)
                return false;
        } else if (m_statusFilter == QLatin1String("finished")) {
            if (book.finishedAt <= 0)
                return false;
        } else if (m_statusFilter == QLatin1String("want")) {
            if (book.wantAt <= 0 || book.finishedAt > 0)
                return false;
        } else if (m_statusFilter == QLatin1String("listening")) {
            if (book.listenedAt <= 0)
                return false;
        }
    }
    if (!m_formatFilter.isEmpty() && book.format.compare(m_formatFilter, Qt::CaseInsensitive) != 0)
        return false;
    if (!m_seriesFilter.isEmpty()) {
        if (m_seriesFilter == QLatin1String("*")) {
            if (book.series.trimmed().isEmpty())
                return false;
        } else if (book.series.trimmed().compare(m_seriesFilter, Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    if (m_collectionId > 0) {
        const auto it = m_collectionBooks.constFind(m_collectionId);
        if (it == m_collectionBooks.cend() || !it->contains(book.id))
            return false;
    }
    return true;
}

QString WorksModel::searchable(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD).toLower();
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed)
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c.isLetterOrNumber() ? c : QChar(' '));
    return out;
}

QString WorksModel::searchableFor(const BookRecord &book) {
    return searchable(QStringList{book.title, book.author, book.editor, book.publisher,
                                  book.year > 0 ? QString::number(book.year) : QString(), book.series, book.format,
                                  book.subjects}
                          .join(QLatin1Char(' ')));
}

bool WorksModel::matches(int id, const QStringList &tokens) const {
    const QString haystack = m_searchable.value(id);
    for (const QString &token : tokens)
        if (!haystack.contains(token))
            return false;
    return true;
}

void WorksModel::rebuild() {
    const QStringList tokens = searchable(m_search).split(' ', Qt::SkipEmptyParts);
    QHash<QString, Work> groups;
    QStringList order;
    for (const auto &book : std::as_const(m_books)) {
        if (!book.visible() || !passesFilters(book))
            continue;
        const QString key = m_grouped && !book.workKey.isEmpty() ? book.workKey
                                                                  : QStringLiteral("book:%1").arg(book.id);
        if (!groups.contains(key)) {
            groups.insert(key, Work{key, {}, 0, 0, 0});
            order.append(key);
        }
        groups[key].books.append(book.id);
    }

    QList<Work> works;
    for (const QString &key : std::as_const(order)) {
        Work work = groups.value(key);
        bool any = tokens.isEmpty();
        for (const int id : std::as_const(work.books))
            any = any || matches(id, tokens);
        if (!any)
            continue;
        // The edition shown is the one being read; otherwise a readable
        // EPUB with a cover, the newest first.
        auto score = [this](int id) {
            const BookRecord &b = m_books[id];
            return std::make_tuple(b.openedAt, b.problem.isEmpty(), b.format == "epub", !b.cover.isEmpty(), b.year, -id);
        };
        work.representative = *std::max_element(work.books.cbegin(), work.books.cend(),
                                                [&](int a, int b) { return score(a) < score(b); });
        for (const int id : std::as_const(work.books)) {
            work.openedAt = qMax(work.openedAt, m_books[id].openedAt);
            work.addedAt = qMax(work.addedAt, m_books[id].addedAt);
        }
        works.append(work);
    }

    auto title = [this](const Work &w) -> const QString & { return m_books[w.representative].sortTitle; };
    auto byTitle = [&](const Work &a, const Work &b) {
        const int c = title(a).localeAwareCompare(title(b));
        return c != 0 ? c < 0 : a.key < b.key;
    };
    if (m_sort == "title") {
        std::sort(works.begin(), works.end(), byTitle);
    } else if (m_sort == "author") {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            const int c = m_books[a.representative].sortAuthor.localeAwareCompare(m_books[b.representative].sortAuthor);
            return c != 0 ? c < 0 : byTitle(a, b);
        });
    } else if (m_sort == "series") {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            const BookRecord &ba = m_books[a.representative];
            const BookRecord &bb = m_books[b.representative];
            const QString sa = ba.series.trimmed();
            const QString sb = bb.series.trimmed();
            const bool hasA = !sa.isEmpty();
            const bool hasB = !sb.isEmpty();
            if (hasA != hasB)
                return hasA;
            if (hasA) {
                const int sc = sa.localeAwareCompare(sb);
                if (sc != 0)
                    return sc < 0;
                const bool idxA = ba.seriesIndex > 0;
                const bool idxB = bb.seriesIndex > 0;
                if (idxA != idxB)
                    return idxA;
                if (ba.seriesIndex != bb.seriesIndex)
                    return ba.seriesIndex < bb.seriesIndex;
                return byTitle(a, b);
            }
            const int ac = ba.sortAuthor.localeAwareCompare(bb.sortAuthor);
            return ac != 0 ? ac < 0 : byTitle(a, b);
        });
    } else if (m_sort == "added") {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            return a.addedAt != b.addedAt ? a.addedAt > b.addedAt : byTitle(a, b);
        });
    } else if (m_sort == "progress") {
        // Books under way first, the furthest along leading; then unread;
        // finished last.
        auto rank = [this](const Work &w) {
            const BookRecord &b = m_books[w.representative];
            return b.finishedAt > 0 ? 2 : b.progress > 0 ? 0 : 1;
        };
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb)
                return ra < rb;
            const double pa = m_books[a.representative].progress, pb = m_books[b.representative].progress;
            return pa != pb ? pa > pb : byTitle(a, b);
        });
    } else {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            if (a.openedAt != b.openedAt)
                return a.openedAt > b.openedAt;
            return a.addedAt != b.addedAt ? a.addedAt > b.addedAt : byTitle(a, b);
        });
    }

    // The same works in the same order only need repainting, which keeps
    // the grid's scroll position and selection while a book is read.
    bool sameShape = works.size() == m_works.size();
    for (int i = 0; sameShape && i < works.size(); ++i)
        sameShape = works[i].key == m_works[i].key && works[i].books == m_works[i].books;
    auto rebuildRowIndex = [this] {
        m_bookRow.clear();
        for (int row = 0; row < m_works.size(); ++row)
            for (const int id : std::as_const(m_works[row].books))
                m_bookRow.insert(id, row);
    };
    if (sameShape) {
        m_works = works;
        rebuildRowIndex();
        if (!m_works.isEmpty())
            emit dataChanged(index(0), index(int(m_works.size()) - 1));
        return;
    }
    beginResetModel();
    m_works = works;
    rebuildRowIndex();
    endResetModel();
    emit countChanged();
}
