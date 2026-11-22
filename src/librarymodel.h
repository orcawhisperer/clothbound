#pragma once

#include "bookrecord.h"

#include <QAbstractListModel>
#include <QHash>

class Library;

// The library grid: one row per work, its editions gathered behind the one
// most worth showing, filtered by the search field and sorted as asked.
class WorksModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString sortOrder READ sortOrder WRITE setSortOrder NOTIFY sortOrderChanged)
    Q_PROPERTY(bool grouped READ grouped WRITE setGrouped NOTIFY groupedChanged)
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
    int count() const { return int(m_works.size()); }

    void setBooks(const QHash<int, BookRecord> &books);
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowOfBook(int bookId) const;

    // Lowercase without accents, so "Gödel" is found by "godel".
    static QString searchable(const QString &text);

signals:
    void searchTextChanged();
    void sortOrderChanged();
    void groupedChanged();
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
    bool matches(const BookRecord &book, const QStringList &tokens) const;

    Library *m_library;
    QHash<int, BookRecord> m_books;
    QList<Work> m_works;
    QString m_search;
    QString m_sort = QStringLiteral("recent");
    bool m_grouped = true;
};
