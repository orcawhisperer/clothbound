#pragma once

#include <QAtomicInteger>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>

// The words of every book, searchable at once: an SQLite FTS5 index of each
// chapter's text (as the reader sees it, so offsets jump to the right place),
// kept in the cache where it can always be rebuilt. Books are read into it
// one at a time on a background thread; a changed file is read again, and a
// removed one dropped.
class LibraryIndex : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool indexing READ indexing NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY progressChanged)
    Q_PROPERTY(int bookCount READ bookCount NOTIFY progressChanged)
    Q_PROPERTY(double megabytes READ megabytes NOTIFY progressChanged)

public:
    // A book to index: where it is and when it last changed.
    struct Source {
        int id = 0;
        QString path;
        QString format;
        qint64 mtime = 0;
    };

    explicit LibraryIndex(const QString &path = {}, QObject *parent = nullptr);
    ~LibraryIndex() override;

    bool available() const { return m_available; }
    bool indexing() const { return m_indexing; }
    int done() const { return m_done; }
    int total() const { return m_total; }
    int bookCount() const;
    double megabytes() const;

    // Brings the index in line with the library: new and changed books are
    // read in, books no longer listed are dropped.
    void update(const QList<Source> &books);
    // [{bookId, chapter, offset, snippet}], best first; the snippet marks the
    // matched words with ‹ and ›. Pass `bookId > 0` to scope to one book.
    Q_INVOKABLE QVariantList search(const QString &query, int limit = 8, int bookId = 0) const;
    Q_INVOKABLE void rebuild();
    // The text of a chapter between two offsets, for showing a passage.
    Q_INVOKABLE QString passage(int bookId, int chapter, qint64 start, qint64 end) const;
    void cancel();

    // The FTS5 query for what someone typed: each word must appear, the last
    // as a prefix, quotes and operators taken literally.
    static QString matchExpression(const QString &query);
    // Reads one book's chapters into the index at `databasePath`; false when
    // the book can't be opened.
    static bool indexBook(const QString &databasePath, const Source &book, const QAtomicInteger<bool> *cancel = nullptr);

signals:
    void progressChanged();

private:
    bool ensureDatabase() const;
    void next();

    QString m_path;
    QString m_connection;
    mutable bool m_available = false;
    bool m_indexing = false;
    int m_done = 0;
    int m_total = 0;
    QList<Source> m_queue;
    QList<Source> m_last;
    std::shared_ptr<QAtomicInteger<bool>> m_cancel;
};
