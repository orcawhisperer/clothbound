#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class AiServices;
namespace Ai { class Reply; }

// A book cut into passages of a paragraph or a few, each with its vector
// from an embedder (kept in the library's database, so a book is indexed
// once per model). Finds the passages closest in meaning to a question, or,
// without an embedder, the ones sharing its rarer words. Never past a limit
// the caller sets: the reader's place, for anything spoiler-safe.
class PassageIndex : public QObject {
    Q_OBJECT

public:
    struct Passage {
        int chapter = 0;
        qint64 start = 0;
        qint64 end = 0;
        QString text;
    };
    struct Hit {
        Passage passage;
        double score = 0;
    };

    PassageIndex(AiServices *ai, const QString &databaseConnection, QObject *parent = nullptr);
    ~PassageIndex() override { cancel(); }

    // Paragraph-aligned passages of about `size` characters.
    static QList<Passage> split(int chapter, const QString &text, int size = 1100);
    // Rare shared words: for when there's no embedder.
    static QList<Hit> byWords(const QList<Passage> &passages, const QString &query, int count);
    static double cosine(const QList<float> &a, const QList<float> &b);

    // Embeds what isn't yet (calling `progress` with the share done), then
    // `done` (with an error message, or "").
    void build(int bookId, const QList<Passage> &passages, std::function<void(double)> progress,
               std::function<void(const QString &)> done);
    // The `count` passages closest to `query` among `passages` (which the
    // caller has already cut at the reader's place). Uses vectors when the
    // embedder has them all, words otherwise.
    void search(int bookId, const QList<Passage> &passages, const QString &query, int count,
                std::function<void(const QList<Hit> &, bool byMeaning)> done);
    // How many of a book's passages have vectors for the current embedder.
    int indexed(int bookId) const;
    void cancel();

private:
    QString model() const;
    QHash<QString, QList<float>> load(int bookId, const QString &modelName = {}) const;
    bool store(int bookId, const QString &modelName, const Passage &passage, const QList<float> &vector);
    static QString keyOf(const Passage &passage);
    void embedNext(int bookId);

    AiServices *m_ai;
    QString m_connection;
    // The build under way.
    QString m_buildModel;
    QList<Passage> m_todo;
    int m_total = 0;
    std::function<void(double)> m_progress;
    std::function<void(const QString &)> m_done;
    QPointer<Ai::Reply> m_reply;
    QPointer<Ai::Reply> m_searchReply;
    int m_generation = 0;
};
