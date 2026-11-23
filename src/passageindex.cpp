#include "passageindex.h"

#include "aiservices.h"
#include "aiusage.h"

#include <QCryptographicHash>
#include <QDate>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <cmath>

namespace {

QStringList wordsOf(const QString &text) {
    static const QRegularExpression word(QStringLiteral("[\\p{L}\\p{N}']{3,}"));
    QStringList out;
    auto it = word.globalMatch(text.toLower());
    while (it.hasNext())
        out.append(it.next().captured());
    return out;
}

const QSet<QString> &stopWords() {
    static const QSet<QString> words = {
        QStringLiteral("the"),  QStringLiteral("and"),  QStringLiteral("that"), QStringLiteral("was"),
        QStringLiteral("for"),  QStringLiteral("with"), QStringLiteral("his"),  QStringLiteral("her"),
        QStringLiteral("had"),  QStringLiteral("but"),  QStringLiteral("not"),  QStringLiteral("you"),
        QStringLiteral("what"), QStringLiteral("who"),  QStringLiteral("why"),  QStringLiteral("how"),
        QStringLiteral("does"), QStringLiteral("did"),  QStringLiteral("this"), QStringLiteral("from"),
        QStringLiteral("they"), QStringLiteral("there"), QStringLiteral("were"), QStringLiteral("have"),
        QStringLiteral("him"),  QStringLiteral("she"),  QStringLiteral("are"),  QStringLiteral("when"),
    };
    return words;
}

QByteArray pack(const QList<float> &vector) {
    return QByteArray(reinterpret_cast<const char *>(vector.constData()), vector.size() * qsizetype(sizeof(float)));
}

QList<float> unpack(const QByteArray &blob) {
    QList<float> vector(blob.size() / qsizetype(sizeof(float)));
    memcpy(vector.data(), blob.constData(), vector.size() * sizeof(float));
    return vector;
}

}  // namespace

PassageIndex::PassageIndex(AiServices *ai, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_ai(ai), m_connection(databaseConnection) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
                              "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))"));
}

QList<PassageIndex::Passage> PassageIndex::split(int chapter, const QString &text, int size) {
    QList<Passage> out;
    size = qMax(64, size);
    Passage current;
    current.chapter = chapter;
    current.start = -1;
    qint64 at = 0;
    const auto flush = [&] {
        if (current.start >= 0 && !current.text.trimmed().isEmpty()) {
            qsizetype lead = 0;
            while (lead < current.text.size() && current.text[lead].isSpace())
                ++lead;
            current.start += lead;
            current.text = current.text.trimmed();
            current.end = current.start + current.text.size();
            out.append(current);
        }
        current = Passage();
        current.chapter = chapter;
        current.start = -1;
    };
    for (const QString &paragraph : text.split(QLatin1Char('\n'))) {
        const qint64 start = at;
        at += paragraph.size() + 1;
        if (paragraph.trimmed().isEmpty())
            continue;
        // Subdivide oversized single-line paragraphs (e.g. minified EPUBs) at
        // sentence or word boundaries so embedder token limits are respected.
        if (paragraph.size() > size) {
            flush();
            qsizetype pos = 0;
            while (pos < paragraph.size()) {
                while (pos < paragraph.size() && paragraph[pos].isSpace())
                    ++pos;
                if (pos >= paragraph.size())
                    break;
                qsizetype end = qMin<qsizetype>(paragraph.size(), pos + size);
                if (end < paragraph.size()) {
                    const qsizetype floor = pos + size / 2;
                    qsizetype cut = -1;
                    for (qsizetype i = end - 1; i >= floor; --i) {
                        const QChar c = paragraph[i];
                        if ((c == QLatin1Char('.') || c == QLatin1Char('?') || c == QLatin1Char('!')) &&
                            (i + 1 < paragraph.size() && paragraph[i + 1].isSpace())) {
                            cut = i + 1;
                            break;
                        }
                    }
                    if (cut < 0) {
                        for (qsizetype i = end; i >= floor; --i) {
                            if (i < paragraph.size() && paragraph[i].isSpace()) {
                                cut = i;
                                break;
                            }
                        }
                    }
                    if (cut > pos)
                        end = cut;
                }
                QString piece = paragraph.mid(pos, end - pos);
                while (!piece.isEmpty() && piece.back().isSpace())
                    piece.chop(1);
                if (!piece.isEmpty())
                    out.append({chapter, start + pos, start + pos + piece.size(), piece});
                pos = end;
            }
            continue;
        }
        if (current.start >= 0 && current.text.size() + paragraph.size() > size)
            flush();
        if (current.start < 0)
            current.start = start;
        current.text += paragraph + QLatin1Char('\n');
        current.end = start + paragraph.size();
        if (current.text.size() >= size)
            flush();
    }
    flush();
    return out;
}

QList<PassageIndex::Hit> PassageIndex::byWords(const QList<Passage> &passages, const QString &query, int count) {
    QStringList terms;
    for (const QString &word : wordsOf(query))
        if (!stopWords().contains(word) && !terms.contains(word))
            terms.append(word);
    if (terms.isEmpty() || passages.isEmpty())
        return {};
    // How many passages each term is in: rarer words count for more.
    QHash<QString, int> seen;
    QList<QHash<QString, int>> counts;
    counts.reserve(passages.size());
    for (const Passage &passage : passages) {
        QHash<QString, int> here;
        for (const QString &word : wordsOf(passage.text))
            if (terms.contains(word))
                ++here[word];
        for (auto it = here.cbegin(); it != here.cend(); ++it)
            ++seen[it.key()];
        counts.append(here);
    }
    QList<Hit> hits;
    for (int i = 0; i < passages.size(); ++i) {
        double score = 0;
        for (auto it = counts[i].cbegin(); it != counts[i].cend(); ++it) {
            const double idf = std::log(1.0 + double(passages.size()) / (1 + seen.value(it.key())));
            score += idf * (1.0 + std::log(double(it.value())));
        }
        if (score > 0)
            hits.append({passages[i], score});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
    return hits.mid(0, count);
}

double PassageIndex::cosine(const QList<float> &a, const QList<float> &b) {
    if (a.size() != b.size() || a.isEmpty())
        return 0;
    double dot = 0, na = 0, nb = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * b[i];
        na += double(a[i]) * a[i];
        nb += double(b[i]) * b[i];
    }
    return na > 0 && nb > 0 ? dot / std::sqrt(na * nb) : 0;
}

QString PassageIndex::model() const {
    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    return provider && embedder ? provider->id() + QLatin1Char('/') + embedder->embeddingModelName() : QString();
}

QString PassageIndex::keyOf(const Passage &passage) {
    const QByteArray digest = QCryptographicHash::hash(passage.text.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    return QStringLiteral("%1:%2:%3:%4")
        .arg(passage.chapter)
        .arg(passage.start)
        .arg(passage.end)
        .arg(QString::fromLatin1(digest));
}

QHash<QString, QList<float>> PassageIndex::load(int bookId, const QString &modelName) const {
    QHash<QString, QList<float>> out;
    const QString name = modelName.isEmpty() ? model() : modelName;
    if (name.isEmpty())
        return out;
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT passage, vector FROM passage_vectors WHERE book_id = ? AND model = ?"));
    query.addBindValue(bookId);
    query.addBindValue(name);
    query.exec();
    while (query.next())
        out.insert(query.value(0).toString(), unpack(query.value(1).toByteArray()));
    return out;
}

int PassageIndex::indexed(int bookId) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM passage_vectors WHERE book_id = ? AND model = ?"));
    query.addBindValue(bookId);
    query.addBindValue(model());
    query.exec();
    return query.next() ? query.value(0).toInt() : 0;
}

bool PassageIndex::store(int bookId, const QString &modelName, const Passage &passage, const QList<float> &vector) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors (book_id, model, passage, vector) VALUES (?, ?, ?, ?)"));
    query.addBindValue(bookId);
    query.addBindValue(modelName);
    query.addBindValue(keyOf(passage));
    query.addBindValue(pack(vector));
    return query.exec();
}

void PassageIndex::build(int bookId, const QList<Passage> &passages, std::function<void(double)> progress,
                         std::function<void(const QString &)> done) {
    cancel();
    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    if (!provider || !embedder) {
        if (done)
            done(tr("There's no embedding model: add a Gemini key or choose one in Intelligence."));
        return;
    }
    m_buildModel = provider->id() + QLatin1Char('/') + embedder->embeddingModelName();
    const QHash<QString, QList<float>> have = load(bookId, m_buildModel);
    m_todo.clear();
    qint64 todoChars = 0;
    for (const Passage &passage : passages) {
        if (!have.contains(keyOf(passage))) {
            m_todo.append(passage);
            todoChars += passage.text.size();
        }
    }
    if (!m_todo.isEmpty() && provider->id() != QLatin1String("local")) {
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, todoChars / 4);
        const double cost = AiUsageLedger::cost(embedder->embeddingModelName(), est, QDate::currentDate(), false);
        if (!m_ai->fitsBudget(cost)) {
            m_todo.clear();
            if (done)
                done(m_ai->describe(int(Ai::Error::Budget)));
            return;
        }
    }
    m_total = int(m_todo.size());
    m_progress = std::move(progress);
    m_done = std::move(done);
    embedNext(bookId);
}

void PassageIndex::embedNext(int bookId) {
    if (m_todo.isEmpty()) {
        auto done = std::exchange(m_done, nullptr);
        if (done)
            done(QString());
        return;
    }
    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    if (!provider || !embedder || m_buildModel.isEmpty()) {
        m_todo.clear();
        auto done = std::exchange(m_done, nullptr);
        if (done)
            done(tr("The embedding model is no longer available."));
        return;
    }
    const QString activeModel = provider->id() + QLatin1Char('/') + embedder->embeddingModelName();
    if (activeModel != m_buildModel) {
        m_todo.clear();
        auto done = std::exchange(m_done, nullptr);
        if (done)
            done(tr("The embedding model changed while reading the book. Try again."));
        return;
    }
    if (m_progress && m_total > 0)
        m_progress(1.0 - double(m_todo.size()) / m_total);
    const QList<Passage> batch = m_todo.mid(0, 32);
    QStringList texts;
    for (const Passage &passage : batch)
        texts.append(passage.text);
    Ai::Reply *reply = embedder->embed(texts, false);
    m_ai->track(reply);
    m_reply = reply;
    const int generation = m_generation;
    const QString modelName = m_buildModel;
    QPointer<Ai::Provider> providerGuard(provider);
    connect(reply, &Ai::Reply::finished, this, [this, reply, batch, bookId, generation, modelName, providerGuard] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        if (reply->error() || reply->vectors().size() != batch.size()) {
            auto done = std::exchange(m_done, nullptr);
            if (done)
                done(reply->error() ? m_ai->describeFor(providerGuard, int(reply->error().kind), reply->error().message)
                                    : tr("The embedding model answered for the wrong number of passages."));
            return;
        }
        QSqlDatabase db = QSqlDatabase::database(m_connection);
        const bool inTx = db.transaction();
        bool ok = true;
        for (int i = 0; i < batch.size() && ok; ++i)
            ok = store(bookId, modelName, batch[i], reply->vectors()[i]);
        if (inTx)
            ok = ok ? db.commit() : (db.rollback(), false);
        if (!ok) {
            auto done = std::exchange(m_done, nullptr);
            if (done)
                done(tr("Couldn't save the book's index."));
            return;
        }
        m_todo.remove(0, batch.size());
        embedNext(bookId);
    });
}

void PassageIndex::cancel() {
    ++m_generation;
    if (m_reply)
        m_reply->abort();
    if (m_searchReply)
        m_searchReply->abort();
    m_todo.clear();
    m_buildModel.clear();
    m_done = nullptr;
    m_progress = nullptr;
}

void PassageIndex::search(int bookId, const QList<Passage> &passages, const QString &query, int count,
                          std::function<void(const QList<Hit> &, bool)> done) {
    if (m_searchReply)
        m_searchReply->abort();
    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    const QString modelName = provider && embedder ? provider->id() + QLatin1Char('/') + embedder->embeddingModelName() : QString();
    const QHash<QString, QList<float>> vectors = load(bookId, modelName);
    bool complete = provider && embedder && !passages.isEmpty();
    for (const Passage &passage : passages)
        if (!vectors.contains(keyOf(passage))) {
            complete = false;
            break;
        }
    if (complete && provider->id() != QLatin1String("local")) {
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, query.size() / 4);
        const double cost = AiUsageLedger::cost(embedder->embeddingModelName(), est, QDate::currentDate(), false);
        if (!m_ai->fitsBudget(cost))
            complete = false;
    }
    if (!complete) {
        done(byWords(passages, query, count), false);
        return;
    }
    Ai::Reply *reply = embedder->embed({query}, true);
    m_ai->track(reply);
    m_searchReply = reply;
    const int generation = m_generation;
    connect(reply, &Ai::Reply::finished, this, [this, reply, passages, vectors, query, count, done, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        if (reply->error() || reply->vectors().isEmpty()) {
            done(byWords(passages, query, count), false);
            return;
        }
        const QList<float> asked = reply->vectors().first();
        QList<Hit> hits;
        hits.reserve(passages.size());
        for (const Passage &passage : passages)
            hits.append({passage, cosine(asked, vectors.value(keyOf(passage)))});
        std::partial_sort(hits.begin(), hits.begin() + qMin<qsizetype>(count, hits.size()), hits.end(),
                          [](const Hit &a, const Hit &b) { return a.score > b.score; });
        done(hits.mid(0, count), true);
    });
}
