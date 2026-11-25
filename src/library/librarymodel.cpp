#include "library/librarymodel.h"

#include "library/library.h"

#include <QDateTime>
#include <QThread>
#include <algorithm>
#include <tuple>

WorksModel::WorksModel(Library *library, QObject *parent)
    : QAbstractListModel(parent ? parent : library),
      m_library(library),
      m_usesLibraryStore(library != nullptr) {}

const QHash<int, BookRecord> &WorksModel::booksMap() const {
    return (m_usesLibraryStore && m_library) ? m_library->bookRecords() : m_standaloneBooks;
}

const BookRecord *WorksModel::findBook(int id) const {
    if (m_usesLibraryStore && m_library)
        return m_library->bookRecord(id);
    const auto it = m_standaloneBooks.constFind(id);
    return it != m_standaloneBooks.cend() ? &(*it) : nullptr;
}

int WorksModel::rowCount(const QModelIndex &parent) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return parent.isValid() ? 0 : int(m_works.size());
}

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
            {AddedAtRole, "addedAt"},
            {PagesRole, "pages"},
            {WantToReadRole, "wantToRead"},
            {ListeningRole, "listening"},
            {DownloadedRole, "downloaded"},
            {AudiobookRole, "audiobook"},
            {ArticleRole, "article"}};
}

QVariant WorksModel::data(const QModelIndex &index, int role) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!index.isValid() || index.row() < 0 || index.row() >= m_works.size())
        return {};
    Q_ASSERT(index.row() >= 0 && index.row() < m_works.size());
    const Work &work = m_works[index.row()];
    Q_ASSERT(work.representative > 0);
    const BookRecord *bookPtr = findBook(work.representative);
    if (!bookPtr)
        return {};
    const BookRecord &book = *bookPtr;
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
        return m_library ? m_library->coverUrl(book.id) : book.cover;
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
        return m_library ? m_library->minutesLeft(book.id) : 0;
    case SecondsReadRole:
        return double(book.secondsRead);
    case AddedAtRole:
        return double(book.addedAt);
    case PagesRole:
        return book.pages;
    case WantToReadRole:
        return book.wantAt > 0 && book.finishedAt == 0;
    case ListeningRole:
        return book.listenedAt > 0;
    case DownloadedRole:
        return m_downloaded.contains(book.id);
    case AudiobookRole:
        return m_audiobooks.contains(book.id);
    case ArticleRole:
        return m_articles.contains(book.id);
    }
    return {};
}

QVariantMap WorksModel::get(int row) const {
    Q_ASSERT(QThread::currentThread() == thread());
    QVariantMap map;
    if (row < 0 || row >= m_works.size())
        return map;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return map;
}

int WorksModel::rowOfBook(int bookId) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_bookRow.value(bookId, -1);
}

void WorksModel::setSearchText(const QString &text) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (text == m_search)
        return;
    m_search = text;
    emit searchTextChanged();
    rebuild();
}

void WorksModel::setSortOrder(const QString &order) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (order == m_sort)
        return;
    m_sort = order;
    emit sortOrderChanged();
    rebuild();
}

void WorksModel::setGrouped(bool grouped) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (grouped == m_grouped)
        return;
    m_grouped = grouped;
    emit groupedChanged();
    rebuild();
}

void WorksModel::setStatusFilter(const QString &status) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString normalized = (status == QLatin1String("all")) ? QString() : status.trimmed().toLower();
    if (normalized == m_statusFilter)
        return;
    m_statusFilter = normalized;
    emit statusFilterChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setFormatFilter(const QString &format) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString normalized = (format == QLatin1String("all")) ? QString() : format.trimmed().toLower();
    if (normalized == m_formatFilter)
        return;
    m_formatFilter = normalized;
    emit formatFilterChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setSeriesFilter(const QString &series) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString normalized = series.trimmed();
    if (normalized == m_seriesFilter)
        return;
    m_seriesFilter = normalized;
    emit seriesFilterChanged();
    emit filtersChanged();
    rebuild();
}

void WorksModel::setCollectionId(int id) {
    Q_ASSERT(QThread::currentThread() == thread());
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
    Q_ASSERT(QThread::currentThread() == thread());
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

void WorksModel::rebuildSearchCache() {
    const QHash<int, BookRecord> &books = booksMap();
    m_searchable.clear();
    m_searchable.reserve(books.size());
    for (auto it = books.cbegin(); it != books.cend(); ++it)
        m_searchable.insert(it.key(), searchableFor(*it));
}

void WorksModel::syncFromLibrary() {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(m_library != nullptr);
    m_usesLibraryStore = true;
    if (!m_standaloneBooks.isEmpty())
        m_standaloneBooks.clear();
    rebuildSearchCache();
    rebuildSeriesList();
    rebuild();
}

void WorksModel::setBooks(const QHash<int, BookRecord> &books) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_library && &books == &m_library->bookRecords()) {
        syncFromLibrary();
        return;
    }
    m_usesLibraryStore = false;
    m_standaloneBooks = books;
    rebuildSearchCache();
    rebuildSeriesList();
    rebuild();
}

void WorksModel::setBooks(const QList<BookRecord> &books) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_usesLibraryStore = false;
    m_standaloneBooks.clear();
    m_standaloneBooks.reserve(books.size());
    for (const BookRecord &book : books)
        m_standaloneBooks.insert(book.id, book);
    rebuildSearchCache();
    rebuildSeriesList();
    rebuild();
}

void WorksModel::setCollections(const QHash<int, QSet<int>> &collectionBooks, bool rebuildNow) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_collectionBooks = collectionBooks;
    if (rebuildNow && m_collectionId > 0)
        rebuild();
}

void WorksModel::setAudioSets(const QSet<int> &downloaded, const QSet<int> &audiobooks) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_downloaded == downloaded && m_audiobooks == audiobooks)
        return;
    m_downloaded = downloaded;
    m_audiobooks = audiobooks;
    if (m_statusFilter == QLatin1String("downloaded") || m_statusFilter == QLatin1String("audiobooks"))
        rebuild();
    else if (!m_works.isEmpty())
        emit dataChanged(index(0), index(int(m_works.size()) - 1), {DownloadedRole, AudiobookRole});
}

void WorksModel::setArticleSet(const QSet<int> &articles) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_articles == articles)
        return;
    m_articles = articles;
    if (m_statusFilter == QLatin1String("articles") || m_formatFilter == QLatin1String("epub"))
        rebuild();
    else if (!m_works.isEmpty())
        emit dataChanged(index(0), index(int(m_works.size()) - 1), {ArticleRole});
}

void WorksModel::updateBook(const BookRecord &book, const BookRecord *previous) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool sharedStore = (m_usesLibraryStore && m_library != nullptr);
    BookRecord old;
    if (sharedStore) {
        if (!m_searchable.contains(book.id) || previous == nullptr) {
            m_searchable.insert(book.id, searchableFor(book));
            rebuildSeriesList();
            rebuild();
            return;
        }
        old = *previous;
    } else {
        const auto oldIt = m_standaloneBooks.constFind(book.id);
        if (oldIt == m_standaloneBooks.cend()) {
            m_standaloneBooks.insert(book.id, book);
            m_searchable.insert(book.id, searchableFor(book));
            if (book.visible() && !book.series.trimmed().isEmpty())
                rebuildSeriesList();
            rebuild();
            return;
        }
        old = previous ? *previous : *oldIt;
    }

    const bool oldShown = old.visible() && passesFilters(old);
    const bool newShown = book.visible() && passesFilters(book);
    const bool membershipChanged = (oldShown != newShown);
    const bool seriesChanged = old.series != book.series || old.seriesIndex != book.seriesIndex;

    const bool searchFieldsChanged =
        old.title != book.title || old.author != book.author || old.editor != book.editor ||
        old.publisher != book.publisher || old.year != book.year || old.series != book.series ||
        old.format != book.format || old.subjects != book.subjects;
    bool searchMatchChanged = false;
    if (searchFieldsChanged) {
        const QString searchKey = searchableFor(book);
        if (m_searchable.value(book.id) != searchKey) {
            m_searchable.insert(book.id, searchKey);
            searchMatchChanged = !m_search.isEmpty();
        }
    }

    const bool repScoreChanged =
        old.openedAt != book.openedAt || old.problem.isEmpty() != book.problem.isEmpty() ||
        (old.format == QLatin1String("epub")) != (book.format == QLatin1String("epub")) ||
        old.cover.isEmpty() != book.cover.isEmpty() || old.year != book.year;

    bool sortKeyChanged = false;
    if (m_statusFilter == QLatin1String("recent")) {
        sortKeyChanged = old.addedAt != book.addedAt || old.sortTitle != book.sortTitle;
    } else if (m_sort == QLatin1String("title")) {
        sortKeyChanged = old.sortTitle != book.sortTitle;
    } else if (m_sort == QLatin1String("author")) {
        sortKeyChanged = old.sortAuthor != book.sortAuthor || old.sortTitle != book.sortTitle;
    } else if (m_sort == QLatin1String("series")) {
        sortKeyChanged = seriesChanged || old.sortAuthor != book.sortAuthor || old.sortTitle != book.sortTitle;
    } else if (m_sort == QLatin1String("added")) {
        sortKeyChanged = old.addedAt != book.addedAt || old.sortTitle != book.sortTitle;
    } else if (m_sort == QLatin1String("progress")) {
        sortKeyChanged = old.progress != book.progress || old.finishedAt != book.finishedAt ||
                         old.sortTitle != book.sortTitle;
    } else {
        sortKeyChanged = old.openedAt != book.openedAt || old.addedAt != book.addedAt ||
                         old.sortTitle != book.sortTitle;
    }

    const bool shapeChanged =
        membershipChanged || old.workKey != book.workKey || repScoreChanged || sortKeyChanged || searchMatchChanged;
    if (!sharedStore)
        m_standaloneBooks.insert(book.id, book);
    if (seriesChanged || old.visible() != book.visible())
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
    const QHash<int, BookRecord> &books = booksMap();
    QSet<QString> unique;
    for (const auto &book : books)
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
        } else if (m_statusFilter == QLatin1String("recent")) {
            const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - recentWindowSecs;
            if (book.addedAt <= 0 || book.addedAt < cutoff)
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
        } else if (m_statusFilter == QLatin1String("downloaded")) {
            if (!m_downloaded.contains(book.id))
                return false;
        } else if (m_statusFilter == QLatin1String("audiobooks")) {
            if (!m_audiobooks.contains(book.id))
                return false;
        } else if (m_statusFilter == QLatin1String("articles")) {
            if (!m_articles.contains(book.id))
                return false;
        }
    }
    if (!m_formatFilter.isEmpty()) {
        if (book.format.compare(m_formatFilter, Qt::CaseInsensitive) != 0)
            return false;
        if (m_formatFilter == QLatin1String("epub") && m_articles.contains(book.id))
            return false;
    }
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
    Q_ASSERT(QThread::currentThread() == thread());
    const QHash<int, BookRecord> &books = booksMap();
    auto recordOf = [&books](int id) -> const BookRecord & {
        const auto it = books.constFind(id);
        Q_ASSERT(it != books.cend());
        return *it;
    };
    const QStringList tokens = searchable(m_search).split(' ', Qt::SkipEmptyParts);
    QHash<QString, Work> groups;
    QStringList order;
    for (const auto &book : books) {
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
        auto score = [&recordOf](int id) {
            const BookRecord &b = recordOf(id);
            return std::make_tuple(b.openedAt, b.problem.isEmpty(), b.format == "epub", !b.cover.isEmpty(), b.year, -id);
        };
        work.representative = *std::max_element(work.books.cbegin(), work.books.cend(),
                                                [&](int a, int b) { return score(a) < score(b); });
        for (const int id : std::as_const(work.books)) {
            const BookRecord &b = recordOf(id);
            work.openedAt = qMax(work.openedAt, b.openedAt);
            work.addedAt = qMax(work.addedAt, b.addedAt);
        }
        works.append(work);
    }

    auto title = [&recordOf](const Work &w) -> const QString & { return recordOf(w.representative).sortTitle; };
    auto byTitle = [&](const Work &a, const Work &b) {
        const int c = title(a).localeAwareCompare(title(b));
        return c != 0 ? c < 0 : a.key < b.key;
    };
    if (m_statusFilter == QLatin1String("recent")) {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            if (a.addedAt != b.addedAt)
                return a.addedAt > b.addedAt;
            return a.representative != b.representative ? a.representative > b.representative : byTitle(a, b);
        });
    } else if (m_sort == "title") {
        std::sort(works.begin(), works.end(), byTitle);
    } else if (m_sort == "author") {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            const int c = recordOf(a.representative).sortAuthor.localeAwareCompare(recordOf(b.representative).sortAuthor);
            return c != 0 ? c < 0 : byTitle(a, b);
        });
    } else if (m_sort == "series") {
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            const BookRecord &ba = recordOf(a.representative);
            const BookRecord &bb = recordOf(b.representative);
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
        auto rank = [&recordOf](const Work &w) {
            const BookRecord &b = recordOf(w.representative);
            return b.finishedAt > 0 ? 2 : b.progress > 0 ? 0 : 1;
        };
        std::sort(works.begin(), works.end(), [&](const Work &a, const Work &b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb)
                return ra < rb;
            const double pa = recordOf(a.representative).progress, pb = recordOf(b.representative).progress;
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
    auto rebuildRowIndex = [this, &books] {
        m_bookRow.clear();
        for (int row = 0; row < m_works.size(); ++row) {
            Q_ASSERT(!m_works[row].books.isEmpty());
            Q_ASSERT(books.contains(m_works[row].representative));
            for (const int id : std::as_const(m_works[row].books))
                m_bookRow.insert(id, row);
            Q_ASSERT(m_bookRow.value(m_works[row].representative, -1) == row);
        }
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
