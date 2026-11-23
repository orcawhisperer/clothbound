#include "ai/passageindex.h"

#include "ai/aiservices.h"
#include "ai/aiusage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

QStringList wordsOf(const QString &text) {
    QString normalized = text.toLower();
    normalized.replace(QChar(0x2019), QLatin1Char('\''));
    normalized.replace(QChar(0x2018), QLatin1Char('\''));
    static const QRegularExpression word(QStringLiteral("[\\p{L}\\p{N}']{2,}"));
    QStringList out;
    auto it = word.globalMatch(normalized);
    while (it.hasNext()) {
        QString tok = it.next().captured();
        while (tok.startsWith(QLatin1Char('\'')))
            tok.remove(0, 1);
        while (tok.endsWith(QLatin1Char('\'')))
            tok.chop(1);
        if (tok.size() > 3 && tok.endsWith(QLatin1String("'s")))
            tok.chop(2);
        if (tok.size() >= 2)
            out.append(tok);
    }
    return out;
}

const QSet<QString> &stopWords() {
    static const QSet<QString> words = {
        QStringLiteral("am"),   QStringLiteral("an"),   QStringLiteral("as"),   QStringLiteral("at"),
        QStringLiteral("be"),   QStringLiteral("by"),   QStringLiteral("do"),   QStringLiteral("go"),
        QStringLiteral("he"),   QStringLiteral("if"),   QStringLiteral("in"),   QStringLiteral("is"),
        QStringLiteral("it"),   QStringLiteral("me"),   QStringLiteral("my"),   QStringLiteral("no"),
        QStringLiteral("of"),   QStringLiteral("on"),   QStringLiteral("or"),   QStringLiteral("so"),
        QStringLiteral("to"),   QStringLiteral("up"),   QStringLiteral("us"),   QStringLiteral("we"),
        QStringLiteral("the"),  QStringLiteral("and"),  QStringLiteral("that"), QStringLiteral("was"),
        QStringLiteral("for"),  QStringLiteral("with"), QStringLiteral("his"),  QStringLiteral("her"),
        QStringLiteral("had"),  QStringLiteral("but"),  QStringLiteral("not"),  QStringLiteral("you"),
        QStringLiteral("what"), QStringLiteral("who"),  QStringLiteral("why"),  QStringLiteral("how"),
        QStringLiteral("does"), QStringLiteral("did"),  QStringLiteral("this"), QStringLiteral("from"),
        QStringLiteral("they"), QStringLiteral("there"), QStringLiteral("were"), QStringLiteral("have"),
        QStringLiteral("him"),  QStringLiteral("she"),  QStringLiteral("are"),  QStringLiteral("when"),
        QStringLiteral("about"), QStringLiteral("into"), QStringLiteral("their"), QStringLiteral("which"),
    };
    return words;
}

QList<double> bm25Scores(const QList<PassageIndex::Passage> &passages, const QString &query) {
    QList<double> scores(passages.size(), 0.0);
    if (passages.isEmpty())
        return scores;
    QStringList terms;
    for (const QString &w : wordsOf(query))
        if (!stopWords().contains(w) && !terms.contains(w))
            terms.append(w);
    if (terms.isEmpty()) {
        for (const QString &w : wordsOf(query))
            if (!terms.contains(w))
                terms.append(w);
    }
    if (terms.isEmpty())
        return scores;

    const QSet<QString> termSet(terms.cbegin(), terms.cend());
    QHash<QString, int> df;
    QList<QHash<QString, int>> tfList;
    QList<int> docLen;
    tfList.reserve(passages.size());
    docLen.reserve(passages.size());
    qint64 totalLen = 0;
    for (const PassageIndex::Passage &passage : passages) {
        QHash<QString, int> here;
        const QStringList words = wordsOf(passage.text);
        docLen.append(int(words.size()));
        totalLen += words.size();
        for (const QString &w : words)
            if (termSet.contains(w))
                ++here[w];
        for (auto it = here.cbegin(); it != here.cend(); ++it)
            ++df[it.key()];
        tfList.append(std::move(here));
    }
    const double avgdl = qMax(1.0, double(totalLen) / double(passages.size()));
    const double N = double(passages.size());
    constexpr double k1 = 1.5;
    constexpr double b = 0.75;
    for (qsizetype i = 0; i < passages.size(); ++i) {
        if (tfList[i].isEmpty())
            continue;
        double score = 0.0;
        const double dl = double(docLen[i]);
        for (auto it = tfList[i].cbegin(); it != tfList[i].cend(); ++it) {
            const double d = double(df.value(it.key()));
            const double idf = std::log(1.0 + (N - d + 0.5) / (d + 0.5));
            const double tf = double(it.value());
            score += idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl / avgdl));
        }
        scores[i] = score;
    }
    return scores;
}

QByteArray pack(const QList<float> &vector) {
    return QByteArray(reinterpret_cast<const char *>(vector.constData()), vector.size() * qsizetype(sizeof(float)));
}

QList<float> unpack(const QByteArray &blob) {
    QList<float> vector(blob.size() / qsizetype(sizeof(float)));
    if (!vector.isEmpty())
        std::memcpy(vector.data(), blob.constData(), size_t(vector.size()) * sizeof(float));
    return vector;
}

}  // namespace

PassageIndex::PassageIndex(AiServices *ai, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_ai(ai), m_connection(databaseConnection), m_cache(std::make_shared<VectorCache>()) {
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
    if (passages.isEmpty() || count <= 0)
        return {};
    const QList<double> scores = bm25Scores(passages, query);
    double maxScore = 0.0;
    for (double s : scores)
        if (s > maxScore)
            maxScore = s;
    if (maxScore <= 0.0)
        return {};
    QList<Hit> hits;
    for (qsizetype i = 0; i < passages.size(); ++i) {
        if (scores[i] > 0.0)
            hits.append({passages[i], scores[i], 0.0, scores[i] / maxScore, QStringLiteral("lexical")});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
    return hits.mid(0, count);
}

QList<PassageIndex::Hit> PassageIndex::fuseHybrid(const QList<Passage> &passages, const QList<double> &cosines,
                                                  const QString &query, int count, double minCosine) {
    if (passages.isEmpty() || count <= 0 || cosines.size() != passages.size())
        return {};

    // 1. Rank semantic candidates passing the cosine similarity cutoff.
    QList<qsizetype> semOrder;
    semOrder.reserve(passages.size());
    for (qsizetype i = 0; i < passages.size(); ++i)
        if (cosines[i] >= minCosine)
            semOrder.append(i);
    std::sort(semOrder.begin(), semOrder.end(),
              [&cosines](qsizetype a, qsizetype b) { return cosines[a] > cosines[b]; });
    QHash<qsizetype, int> semRank;
    semRank.reserve(semOrder.size());
    for (int r = 0; r < semOrder.size(); ++r)
        semRank.insert(semOrder[r], r + 1);

    // 2. Rank lexical BM25 candidates.
    const QList<double> bm25 = bm25Scores(passages, query);
    double maxBm25 = 0.0;
    QList<qsizetype> lexOrder;
    for (qsizetype i = 0; i < passages.size(); ++i) {
        if (bm25[i] > 0.0) {
            lexOrder.append(i);
            if (bm25[i] > maxBm25)
                maxBm25 = bm25[i];
        }
    }
    std::sort(lexOrder.begin(), lexOrder.end(), [&bm25](qsizetype a, qsizetype b) { return bm25[a] > bm25[b]; });
    QHash<qsizetype, int> lexRank;
    lexRank.reserve(lexOrder.size());
    for (int r = 0; r < lexOrder.size(); ++r)
        lexRank.insert(lexOrder[r], r + 1);

    // 3. Reciprocal Rank Fusion (semantic weight 1.0, lexical weight 0.7, k = 60).
    constexpr double k = 60.0;
    constexpr double wSem = 1.0;
    constexpr double wLex = 0.7;
    struct Candidate {
        qsizetype index = 0;
        double rrf = 0.0;
        double relevance = 0.0;
        double semantic = 0.0;
        double lexical = 0.0;
        QString kind;
    };
    QSet<qsizetype> unionIdx;
    for (qsizetype idx : semOrder)
        unionIdx.insert(idx);
    for (qsizetype idx : lexOrder)
        unionIdx.insert(idx);

    QList<Candidate> candidates;
    candidates.reserve(unionIdx.size());
    for (qsizetype idx : unionIdx) {
        const int rSem = semRank.value(idx, 0);
        const int rLex = lexRank.value(idx, 0);
        const bool hasSem = rSem > 0;
        const bool hasLex = rLex > 0;
        double rrf = 0.0;
        if (hasSem)
            rrf += wSem / (k + double(rSem));
        if (hasLex)
            rrf += wLex / (k + double(rLex));
        const double sem = qBound(0.0, cosines[idx], 1.0);
        const double lex = maxBm25 > 0.0 ? qBound(0.0, bm25[idx] / maxBm25, 1.0) : 0.0;
        double rel = 0.0;
        QString kind;
        if (hasSem && hasLex) {
            kind = QStringLiteral("hybrid");
            rel = qBound(0.0, 0.65 * sem + 0.35 * lex + 0.08, 0.99);
        } else if (hasSem) {
            kind = QStringLiteral("semantic");
            rel = sem;
        } else {
            kind = QStringLiteral("lexical");
            rel = qBound(0.0, 0.25 + 0.45 * lex, 0.70);
        }
        candidates.append({idx, rrf, rel, sem, lex, kind});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (!qFuzzyCompare(a.rrf, b.rrf))
            return a.rrf > b.rrf;
        if (!qFuzzyCompare(a.semantic, b.semantic))
            return a.semantic > b.semantic;
        return a.relevance > b.relevance;
    });
    const qsizetype take = qMin<qsizetype>(count, candidates.size());
    QList<Hit> out;
    out.reserve(take);
    for (qsizetype i = 0; i < take; ++i) {
        const Candidate &c = candidates[i];
        out.append({passages[c.index], c.relevance, c.semantic, c.lexical, c.kind});
    }
    return out;
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

QString PassageIndex::databasePath() const {
    return QSqlDatabase::database(m_connection, false).databaseName();
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

QHash<QString, QList<float>> PassageIndex::loadFromFile(const QString &dbPath, int bookId, const QString &modelName) {
    QHash<QString, QList<float>> out;
    if (dbPath.isEmpty() || modelName.isEmpty())
        return out;
    const QString connName = QStringLiteral("pi-read-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT passage, vector FROM passage_vectors WHERE book_id = ? AND model = ?"));
            query.addBindValue(bookId);
            query.addBindValue(modelName);
            if (query.exec()) {
                while (query.next())
                    out.insert(query.value(0).toString(), unpack(query.value(1).toByteArray()));
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return out;
}

void PassageIndex::loadVectorsAsync(int bookId, const QString &modelName, int expectedCount,
                                    std::function<bool()> stillValid,
                                    std::function<void(const QHash<QString, QList<float>> &)> then) {
    if (modelName.isEmpty()) {
        then({});
        return;
    }
    {
        QMutexLocker lock(&m_cache->mutex);
        if (m_cache->loaded && m_cache->bookId == bookId && m_cache->model == modelName &&
            m_cache->vectors.size() == expectedCount) {
            const QHash<QString, QList<float>> cached = m_cache->vectors;
            lock.unlock();
            then(cached);
            return;
        }
    }
    const QString dbPath = databasePath();
    if (dbPath.isEmpty() || dbPath == QLatin1String(":memory:")) {
        const QHash<QString, QList<float>> loaded = load(bookId, modelName);
        {
            QMutexLocker lock(&m_cache->mutex);
            m_cache->bookId = bookId;
            m_cache->model = modelName;
            m_cache->vectors = loaded;
            m_cache->loaded = true;
        }
        then(loaded);
        return;
    }
    QPointer<PassageIndex> self(this);
    auto cache = m_cache;
    (void)QtConcurrent::run([self, cache, dbPath, bookId, modelName, stillValid = std::move(stillValid),
                             then = std::move(then)]() mutable {
        const QHash<QString, QList<float>> loaded = loadFromFile(dbPath, bookId, modelName);
        {
            QMutexLocker lock(&cache->mutex);
            cache->bookId = bookId;
            cache->model = modelName;
            cache->vectors = loaded;
            cache->loaded = true;
        }
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                app,
                [self, loaded, stillValid = std::move(stillValid), then = std::move(then)] {
                    if (!self || (stillValid && !stillValid()))
                        return;
                    then(loaded);
                },
                Qt::QueuedConnection);
        }
    });
}

int PassageIndex::indexed(int bookId) const {
    const QString name = model();
    if (name.isEmpty())
        return 0;
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM passage_vectors WHERE book_id = ? AND model = ?"));
    query.addBindValue(bookId);
    query.addBindValue(name);
    query.exec();
    return query.next() ? query.value(0).toInt() : 0;
}

bool PassageIndex::store(int bookId, const QString &modelName, const Passage &passage, const QList<float> &vector) {
    const QString key = keyOf(passage);
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors (book_id, model, passage, vector) VALUES (?, ?, ?, ?)"));
    query.addBindValue(bookId);
    query.addBindValue(modelName);
    query.addBindValue(key);
    query.addBindValue(pack(vector));
    if (!query.exec())
        return false;
    QMutexLocker lock(&m_cache->mutex);
    if (m_cache->loaded && m_cache->bookId == bookId && m_cache->model == modelName)
        m_cache->vectors.insert(key, vector);
    return true;
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
    m_progress = std::move(progress);
    m_done = std::move(done);
    const int generation = m_generation;
    const int existingCount = indexed(bookId);
    const QString buildModel = m_buildModel;
    loadVectorsAsync(
        bookId, buildModel, existingCount, [this, generation] { return generation == m_generation; },
        [this, bookId, passages, generation, buildModel](const QHash<QString, QList<float>> &have) {
            if (generation != m_generation)
                return;
            Ai::Provider *activeProvider = m_ai->embedderProvider();
            Ai::Embedder *activeEmbedder = m_ai->embedder();
            if (!activeProvider || !activeEmbedder) {
                auto cb = std::exchange(m_done, nullptr);
                if (cb)
                    cb(tr("The embedding model is no longer available."));
                return;
            }
            m_todo.clear();
            qint64 todoChars = 0;
            for (const Passage &passage : passages) {
                if (!have.contains(keyOf(passage))) {
                    m_todo.append(passage);
                    todoChars += passage.text.size();
                }
            }
            if (!m_todo.isEmpty() && activeProvider->id() != QLatin1String("local")) {
                Ai::Usage est;
                est.inputTokens = qMax<qint64>(1, todoChars / 4);
                const double cost =
                    AiUsageLedger::cost(activeEmbedder->embeddingModelName(), est, QDate::currentDate(), false);
                if (!m_ai->fitsBudget(cost)) {
                    m_todo.clear();
                    auto cb = std::exchange(m_done, nullptr);
                    if (cb)
                        cb(m_ai->describe(int(Ai::Error::Budget)));
                    return;
                }
            }
            m_total = int(m_todo.size());
            embedNext(bookId);
        });
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
            {
                QMutexLocker lock(&m_cache->mutex);
                if (m_cache->bookId == bookId && m_cache->model == modelName)
                    m_cache->loaded = false;
            }
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
    ++m_searchGeneration;
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
    ++m_searchGeneration;
    if (m_searchReply)
        m_searchReply->abort();
    const int searchGen = m_searchGeneration;
    auto runWordFallback = [this, passages, query, count, done, searchGen] {
        QPointer<PassageIndex> self(this);
        (void)QtConcurrent::run([self, passages, query, count, done, searchGen] {
            const QList<Hit> hits = byWords(passages, query, count);
            if (QCoreApplication *app = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(
                    app,
                    [self, hits, done, searchGen] {
                        if (!self || searchGen != self->m_searchGeneration)
                            return;
                        done(hits, false);
                    },
                    Qt::QueuedConnection);
            }
        });
    };

    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    const QString modelName =
        provider && embedder ? provider->id() + QLatin1Char('/') + embedder->embeddingModelName() : QString();
    if (!provider || !embedder || passages.isEmpty()) {
        runWordFallback();
        return;
    }
    if (provider->id() != QLatin1String("local")) {
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, query.size() / 4);
        const double cost = AiUsageLedger::cost(embedder->embeddingModelName(), est, QDate::currentDate(), false);
        if (!m_ai->fitsBudget(cost)) {
            runWordFallback();
            return;
        }
    }
    const int existingCount = indexed(bookId);
    loadVectorsAsync(
        bookId, modelName, existingCount, [this, searchGen] { return searchGen == m_searchGeneration; },
        [this, passages, query, count, done, searchGen, runWordFallback](const QHash<QString, QList<float>> &vectors) {
            if (searchGen != m_searchGeneration)
                return;
            Ai::Provider *activeProvider = m_ai->embedderProvider();
            Ai::Embedder *activeEmbedder = m_ai->embedder();
            bool complete = activeProvider && activeEmbedder && !passages.isEmpty();
            if (complete) {
                for (const Passage &passage : passages) {
                    if (!vectors.contains(keyOf(passage))) {
                        complete = false;
                        break;
                    }
                }
            }
            if (!complete) {
                runWordFallback();
                return;
            }
            Ai::Reply *reply = activeEmbedder->embed({query}, true);
            m_ai->track(reply);
            m_searchReply = reply;
            connect(reply, &Ai::Reply::finished, this,
                    [this, reply, passages, vectors, query, count, done, searchGen, runWordFallback] {
                        reply->deleteLater();
                        if (searchGen != m_searchGeneration)
                            return;
                        if (reply->error() || reply->vectors().isEmpty()) {
                            runWordFallback();
                            return;
                        }
                        const QList<float> asked = reply->vectors().first();
                        QPointer<PassageIndex> self(this);
                        (void)QtConcurrent::run([self, passages, vectors, asked, query, count, done, searchGen] {
                            QList<double> cosines;
                            cosines.reserve(passages.size());
                            for (const Passage &passage : passages)
                                cosines.append(cosine(asked, vectors.value(keyOf(passage))));
                            const QList<Hit> hits = fuseHybrid(passages, cosines, query, count);
                            if (QCoreApplication *app = QCoreApplication::instance()) {
                                QMetaObject::invokeMethod(
                                    app,
                                    [self, hits, done, searchGen] {
                                        if (!self || searchGen != self->m_searchGeneration)
                                            return;
                                        done(hits, true);
                                    },
                                    Qt::QueuedConnection);
                            }
                        });
                    });
        });
}

// --- Across the library ---------------------------------------------------------

QList<PassageIndex::LibraryHit> PassageIndex::rankLibrary(const QString &dbPath, const QString &model,
                                                          const QList<float> &asked, int count, double minCosine) {
    QList<LibraryHit> hits;
    if (dbPath.isEmpty() || model.isEmpty() || asked.isEmpty())
        return hits;
    const QString connName = QStringLiteral("pi-library-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT book_id, passage, vector FROM passage_vectors WHERE model = ?"));
            query.addBindValue(model);
            query.exec();
            while (query.next()) {
                const double score = cosine(asked, unpack(query.value(2).toByteArray()));
                if (score < minCosine)
                    continue;
                // The key is chapter:start:end:digest.
                const QStringList parts = query.value(1).toString().split(QLatin1Char(':'));
                if (parts.size() < 3)
                    continue;
                hits.append({query.value(0).toInt(), parts[0].toInt(), parts[1].toLongLong(), parts[2].toLongLong(), score});
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    std::sort(hits.begin(), hits.end(), [](const LibraryHit &a, const LibraryHit &b) { return a.score > b.score; });
    // One passage per chapter of a book, so a single chapter can't fill the list.
    QList<LibraryHit> best;
    QSet<QPair<int, int>> seen;
    for (const LibraryHit &hit : std::as_const(hits)) {
        if (seen.contains({hit.bookId, hit.chapter}))
            continue;
        seen.insert({hit.bookId, hit.chapter});
        best.append(hit);
        if (best.size() >= count)
            break;
    }
    return best;
}

int PassageIndex::booksPrepared() const {
    const QString name = model();
    if (name.isEmpty())
        return 0;
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT COUNT(DISTINCT book_id) FROM passage_vectors WHERE model = ?"));
    query.addBindValue(name);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

void PassageIndex::searchLibrary(const QString &query, int count, std::function<void(const QList<LibraryHit> &)> done) {
    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    const QString name = model();
    if (!provider || !embedder || name.isEmpty() || query.trimmed().isEmpty() || booksPrepared() == 0) {
        done({});
        return;
    }
    if (provider->id() != QLatin1String("local")) {
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, query.size() / 4);
        const double cost = AiUsageLedger::cost(embedder->embeddingModelName(), est, QDate::currentDate(), false);
        if (!m_ai->fitsBudget(cost)) {
            done({});
            return;
        }
    }
    Ai::Reply *reply = embedder->embed({query}, true);
    m_ai->track(reply);
    const QString path = databasePath();
    QPointer<PassageIndex> self(this);
    connect(reply, &Ai::Reply::finished, this, [self, reply, path, name, count, done] {
        reply->deleteLater();
        if (!self || reply->error() || reply->vectors().isEmpty()) {
            done({});
            return;
        }
        const QList<float> asked = reply->vectors().first();
        (void)QtConcurrent::run([self, path, name, asked, count, done] {
            const QList<LibraryHit> hits = rankLibrary(path, name, asked, count);
            if (QCoreApplication *app = QCoreApplication::instance())
                QMetaObject::invokeMethod(app, [self, hits, done] { if (self) done(hits); }, Qt::QueuedConnection);
        });
    });
}
