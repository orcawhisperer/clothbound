#pragma once

#include "bookrecord.h"

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QStringList>

class Library;

// The library grid: one row per work, its editions gathered behind the one
// most worth showing, filtered by the search field and chips, and sorted as asked.
class WorksModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString sortOrder READ sortOrder WRITE setSortOrder NOTIFY sortOrderChanged)
    Q_PROPERTY(bool grouped READ grouped WRITE setGrouped NOTIFY groupedChanged)
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY statusFilterChanged)
    Q_PROPERTY(QString formatFilter READ formatFilter WRITE setFormatFilter NOTIFY formatFilterChanged)
    Q_PROPERTY(QString seriesFilter READ seriesFilter WRITE setSeriesFilter NOTIFY seriesFilterChanged)
    Q_PROPERTY(int collectionId READ collectionId WRITE setCollectionId NOTIFY collectionIdChanged)
    Q_PROPERTY(bool hasActiveFilters READ hasActiveFilters NOTIFY filtersChanged)
    Q_PROPERTY(QStringList seriesList READ seriesList NOTIFY seriesListChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        BookIdRole,
        TitleRole,
        AuthorRole,
        EditionLineRole,
        EditionsRole,
        CoverRole,
        ProgressRole,
        FinishedRole,
        FormatRole,
        ProblemRole,
        YearRole,
        OpenedRole,
        SeriesRole,
        SeriesIndexRole,
        MinutesLeftRole,
        SecondsReadRole,
        AddedAtRole,
        PagesRole,
        WantToReadRole,
        ListeningRole,
        DownloadedRole,
        AudiobookRole,
    };

    explicit WorksModel(Library *library);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString searchText() const { return m_search; }
    void setSearchText(const QString &text);
    QString sortOrder() const { return m_sort; }
    void setSortOrder(const QString &order);
    bool grouped() const { return m_grouped; }
    void setGrouped(bool grouped);
    QString statusFilter() const { return m_statusFilter; }
    void setStatusFilter(const QString &status);
    QString formatFilter() const { return m_formatFilter; }
    void setFormatFilter(const QString &format);
    QString seriesFilter() const { return m_seriesFilter; }
    void setSeriesFilter(const QString &series);
    int collectionId() const { return m_collectionId; }
    void setCollectionId(int id);
    bool hasActiveFilters() const;
    QStringList seriesList() const { return m_seriesList; }
    int count() const { return int(m_works.size()); }

    Q_INVOKABLE void clearFilters();

    void setBooks(const QHash<int, BookRecord> &books);
    void setCollections(const QHash<int, QSet<int>> &collectionBooks);
    void setAudioSets(const QSet<int> &downloaded, const QSet<int> &audiobooks);
    // Fast in-place update for progress/position changes without rebuilding the
    // whole grid unless sort order or visibility is affected.
    void updateBook(const BookRecord &book);
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowOfBook(int bookId) const;

    // Lowercase without accents, so "Gödel" is found by "godel".
    static QString searchable(const QString &text);

signals:
    void searchTextChanged();
    void sortOrderChanged();
    void groupedChanged();
    void statusFilterChanged();
    void formatFilterChanged();
    void seriesFilterChanged();
    void collectionIdChanged();
    void filtersChanged();
    void seriesListChanged();
    void countChanged();

private:
    struct Work {
        QString key;
        QList<int> books;
        int representative = 0;
        qint64 openedAt = 0;
        qint64 addedAt = 0;
    };

    void rebuild();
    void rebuildSeriesList();
    bool passesFilters(const BookRecord &book) const;
    bool matches(int id, const QStringList &tokens) const;
    static QString searchableFor(const BookRecord &book);

    Library *m_library;
    QHash<int, BookRecord> m_books;
    QHash<int, QSet<int>> m_collectionBooks;
    QSet<int> m_downloaded;
    QSet<int> m_audiobooks;
    QHash<int, QString> m_searchable;
    QHash<int, int> m_bookRow;
    QList<Work> m_works;
    QStringList m_seriesList;
    QString m_search;
    QString m_sort = QStringLiteral("recent");
    QString m_statusFilter;
    QString m_formatFilter;
    QString m_seriesFilter;
    int m_collectionId = 0;
    bool m_grouped = true;
};
