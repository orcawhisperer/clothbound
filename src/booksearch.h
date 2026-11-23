#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QVariantList>
#include <atomic>
#include <memory>

class AiServices;
class PassageIndex;
class Reader;

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
    // While a book is read for meaning the first time: "Reading the book… 40%".
    Q_PROPERTY(QString preparing READ preparing NOTIFY changed)
    Q_PROPERTY(QString where READ where NOTIFY changed)

public:
    static constexpr int limit = 2000;

    explicit BookSearch(Reader *reader, QObject *parent = nullptr);
    ~BookSearch() override;

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

    void setMeaning(AiServices *ai, const QString &databaseConnection);
    bool byMeaning() const { return m_byMeaning; }
    void setByMeaning(bool byMeaning);
    bool canSearchByMeaning() const;
    QString preparing() const { return m_preparing; }
    QString where() const;

    // Lowercase, without accents, with plain quotes and dashes, and the same
    // length, so offsets in it are offsets in the text.
    static QString fold(const QString &text);
    // Where a (folded) query is found in (folded) text.
    static QList<QPair<qint64, qint64>> matches(const QString &text, const QString &query, int limit);
    // A hit with a little of the text either side, cut at words.
    static QVariantMap hit(const QString &text, int chapter, qint64 start, qint64 end);

signals:
    void changed();
    void currentChanged();

private:
    void findByMeaning(const QString &query);

    Reader *m_reader;
    AiServices *m_ai = nullptr;
    PassageIndex *m_index = nullptr;
    bool m_byMeaning = false;
    QString m_preparing;
    int m_meaningRun = 0;
    QString m_query;
    bool m_searching = false;
    bool m_capped = false;
    QVariantList m_results;
    int m_current = -1;
    std::shared_ptr<std::atomic_bool> m_cancel;
};
