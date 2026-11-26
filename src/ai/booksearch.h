#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QVariantList>
#include <atomic>
#include <memory>

class AiServices;
class Library;
class PassageIndex;

// Finding words in the open book. Every chapter's text is searched the way
// the page counts it, so a hit's place is exact. Case, accents and curly
// quotes don't matter ("naive" finds "naïve", "don't" finds "don’t").
// Results arrive chapter by chapter from a worker thread; a new search, or
// another book, drops the old one.
class BookSearch : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString query READ query NOTIFY changed)
    Q_PROPERTY(bool searching READ searching NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // Stopped at the limit; there are more.
    Q_PROPERTY(bool capped READ capped NOTIFY changed)
    // Each hit: {chapter, start, end, before, match, after}.
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)
    // Searching by meaning: passages close to what's described, not words.
    Q_PROPERTY(bool byMeaning READ byMeaning WRITE setByMeaning NOTIFY changed)
    Q_PROPERTY(bool canSearchByMeaning READ canSearchByMeaning NOTIFY changed)
    // Optional spoiler shield for semantic search: only search up to the reader's current place.
    Q_PROPERTY(bool spoilerFree READ spoilerFree WRITE setSpoilerFree NOTIFY changed)
    // While a book is read for meaning the first time: "Reading the book… 40%".
    Q_PROPERTY(QString preparing READ preparing NOTIFY changed)
    Q_PROPERTY(QString where READ where NOTIFY changed)
    // Search by meaning across the whole library (the palette's "By meaning").
    Q_PROPERTY(QVariantList libraryResults READ libraryResults NOTIFY libraryResultsChanged)
    Q_PROPERTY(int booksPrepared READ booksPrepared NOTIFY changed)

public:
    static constexpr int limit = 2000;

    // Decoupled from concrete Reader to avoid a library <-> reader module cycle:
    // `reader` may be a Reader* (implicitly upcast to QObject*) or any QObject
    // exposing bookId/format/chapter/pageStart/pageEnd/(bookPath|path) properties
    // and an optional bookChanged() signal.
    explicit BookSearch(QObject *reader, QObject *parent = nullptr);
    ~BookSearch() override;

    // Optional explicit Library binding for resolving bookPath from bookId.
    void setLibrary(Library *library);

    QString query() const { return m_query; }
    bool searching() const { return m_searching; }
    int count() const { return int(m_results.size()); }
    bool capped() const { return m_capped; }
    QVariantList results() const { return m_results; }
    int current() const { return m_current; }
    void setCurrent(int current);

    Q_INVOKABLE void find(const QString &query);
    Q_INVOKABLE void clear();
    // The hits in a chapter, as {start, end} spans for highlighting.
    Q_INVOKABLE QVariantList inChapter(int chapter) const;
    // Finds passages close in meaning to `query` in every book prepared for
    // the current embedder; results arrive in libraryResults as
    // [{bookId, chapter, start, end, score}].
    Q_INVOKABLE void searchLibrary(const QString &query);
    QVariantList libraryResults() const { return m_libraryResults; }
    int booksPrepared() const;

    void setMeaning(AiServices *ai, const QString &databaseConnection);
    // Shares a single PassageIndex across multiple BookSession instances so
    // vector memory is never duplicated across open windows/tabs.
    void setMeaning(AiServices *ai, PassageIndex *sharedIndex);
    PassageIndex *passageIndex() const { return m_index.data(); }
    bool byMeaning() const { return m_byMeaning; }
    void setByMeaning(bool byMeaning);
    bool canSearchByMeaning() const;
    bool spoilerFree() const { return m_spoilerFree; }
    void setSpoilerFree(bool spoilerFree);
    QString preparing() const { return m_preparing; }
    QString where() const;

    // Lowercase, without accents, with plain quotes and dashes, and the same
    // length, so offsets in it are offsets in the text.
    static QString fold(const QString &text);
    // Where a (folded) query is found in (folded) text.
    static QList<QPair<qint64, qint64>> matches(const QString &text, const QString &query, int limit);
    // A hit with a little of the text either side, cut at words.
    static QVariantMap hit(const QString &text, int chapter, qint64 start, qint64 end);
    // Sentence-aligned highlight span within [passageStart, passageEnd] best matching `query`.
    static QPair<qint64, qint64> sentenceSpan(const QString &chapterText, qint64 passageStart, qint64 passageEnd,
                                              const QString &query);

signals:
    void changed();
    void currentChanged();
    void libraryResultsChanged();

private slots:
    void onReaderBookChanged();

private:
    struct BookCache;

    void findByMeaning(const QString &query);
    QString readerBookPath() const;
    QString readerFormat() const;
    int readerBookId() const;
    int readerChapter() const;
    qint64 readerPageStart() const;
    qint64 readerPageEnd() const;

    QPointer<QObject> m_reader;
    QPointer<Library> m_library;
    QPointer<AiServices> m_ai;
    QPointer<PassageIndex> m_index;
    bool m_ownsIndex = false;
    bool m_indexOpActive = false;
    QVariantList m_libraryResults;
    int m_libraryRun = 0;
    bool m_byMeaning = false;
    bool m_spoilerFree = false;
    QString m_preparing;
    int m_meaningRun = 0;
    QString m_query;
    bool m_searching = false;
    bool m_capped = false;
    QVariantList m_results;
    int m_current = -1;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<BookCache> m_cache;
};
