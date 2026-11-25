#include "ai/passageindex.h"

#include "ai/aiservices.h"
#include "ai/aiusage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <vector>

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

// 8-wide float32 SLP-vectorizable dot product + normB^2 kernel.
// Explicit 8-lane accumulation lets GCC auto-vectorize to NEON fmla.4s / SSE mulps+addps
// even at -O2 without -ffast-math, preserving strict IEEE-754 semantics.
inline void accumulateDotAndNormB8(const float *a, const float *b, qsizetype n, float d[8], float nb[8],
                                   float &tailDot, float &tailNormB) {
    qsizetype i = 0;
    for (; i + 8 <= n; i += 8) {
        for (int k = 0; k < 8; ++k) {
            const float vb = b[i + k];
            d[k] += a[i + k] * vb;
            nb[k] += vb * vb;
        }
    }
    for (; i < n; ++i) {
        const float vb = b[i];
        tailDot += a[i] * vb;
        tailNormB += vb * vb;
    }
}

inline float reduce8(const float v[8], float tail) {
    return (((v[0] + v[1]) + (v[2] + v[3])) + ((v[4] + v[5]) + (v[6] + v[7]))) + tail;
}

inline double finalizeUnitQueryCosine(float dot, float normBSq) {
    if (!std::isfinite(normBSq) || normBSq <= 0.0f)
        return 0.0;
    // Pre-normalized unit vectors skip std::sqrt(); legacy unnormalized BLOBs divide cleanly.
    if (std::abs(normBSq - 1.0f) <= 1e-5f)
        return std::clamp(double(dot), -1.0, 1.0);
    return std::clamp(double(dot) / std::sqrt(double(normBSq)), -1.0, 1.0);
}

bool openPassageDbReadOnly(const QString &dbPath, const QString &connName, QSqlDatabase *outDb) {
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath))
        return false;
    auto tryOpen = [&](const QString &name, const QString &opts) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(name);
        db.setConnectOptions(opts);
        Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
        if (!db.open())
            return false;
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
        pragma.exec(QStringLiteral("PRAGMA query_only=ON"));
        QSqlQuery probe(db);
        if (!probe.exec(QStringLiteral("SELECT 1 FROM sqlite_master LIMIT 1"))) {
            db.close();
            return false;
        }
        if (outDb)
            *outDb = db;
        return true;
    };
    if (tryOpen(dbPath, QStringLiteral("QSQLITE_OPEN_READONLY")))
        return true;
    QSqlDatabase::removeDatabase(connName);

    const QFileInfo walInfo(dbPath + QStringLiteral("-wal"));
    if (QFileInfo(dbPath).isReadable() && (!walInfo.exists() || walInfo.size() == 0)) {
        const QString uri = QUrl::fromLocalFile(QFileInfo(dbPath).absoluteFilePath()).toString(QUrl::FullyEncoded) +
                            QStringLiteral("?mode=ro&immutable=1");
        if (tryOpen(uri, QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI")))
            return true;
        QSqlDatabase::removeDatabase(connName);
    }
    return false;
}

}  // namespace

PassageIndex::PassageIndex(AiServices *ai, const QString &databaseConnection, QObject *parent, bool readOnly)
    : QObject(parent),
      m_ai(ai),
      m_connection(databaseConnection),
      m_readOnly(readOnly),
      m_cache(std::make_shared<VectorCache>()) {
    if (m_readOnly)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
                              "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_passage_vectors_model_book ON passage_vectors(model, book_id)"));
    QSqlQuery hasBooks(db);
    if (hasBooks.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='books'")) &&
        hasBooks.next()) {
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS passage_vectors_cleanup_book AFTER DELETE ON books BEGIN "
            "DELETE FROM passage_vectors WHERE book_id = OLD.id; "
            "END"));
        pruneOrphans();
    }
}

int PassageIndex::pruneOrphans() {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_readOnly);
    if (m_readOnly)
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return 0;
    QSqlQuery hasBooks(db);
    if (!hasBooks.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='books'")) ||
        !hasBooks.next()) {
        return 0;
    }
    QSqlQuery del(db);
    if (!del.exec(QStringLiteral("DELETE FROM passage_vectors WHERE book_id NOT IN (SELECT id FROM books)")))
        return 0;
    const int removed = qMax(0, del.numRowsAffected());
    if (removed > 0) {
        QMutexLocker lock(&m_cache->mutex);
        m_cache->loaded = false;
        m_cache->vectors.clear();
    }
    return removed;
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

QList<float> PassageIndex::normalize(const QList<float> &vector) {
    const qsizetype n = vector.size();
    if (n <= 0)
        return vector;
    const float *src = vector.constData();
    alignas(32) float acc[8] = {};
    qsizetype i = 0;
    for (; i + 8 <= n; i += 8) {
        for (int k = 0; k < 8; ++k) {
            const float v = src[i + k];
            acc[k] += v * v;
        }
    }
    float tail = 0.0f;
    for (; i < n; ++i)
        tail += src[i] * src[i];
    const float normSq = reduce8(acc, tail);
    if (!std::isfinite(normSq) || normSq <= 0.0f)
        return vector;
    if (std::abs(normSq - 1.0f) <= 1e-6f)
        return vector;
    const float invNorm = float(1.0 / std::sqrt(double(normSq)));
    QList<float> out(n);
    float *dst = out.data();
    for (qsizetype j = 0; j < n; ++j)
        dst[j] = src[j] * invNorm;
    return out;
}

double PassageIndex::cosine(const QList<float> &a, const QList<float> &b) {
    const qsizetype n = a.size();
    if (n != b.size() || n == 0)
        return 0;
    const float *pa = a.constData();
    const float *pb = b.constData();
    alignas(32) float d[8] = {};
    alignas(32) float na[8] = {};
    alignas(32) float nb[8] = {};
    qsizetype i = 0;
    for (; i + 8 <= n; i += 8) {
        for (int k = 0; k < 8; ++k) {
            const float va = pa[i + k];
            const float vb = pb[i + k];
            d[k] += va * vb;
            na[k] += va * va;
            nb[k] += vb * vb;
        }
    }
    float td = 0.0f, tna = 0.0f, tnb = 0.0f;
    for (; i < n; ++i) {
        const float va = pa[i];
        const float vb = pb[i];
        td += va * vb;
        tna += va * va;
        tnb += vb * vb;
    }
    const float dot = reduce8(d, td);
    const float normASq = reduce8(na, tna);
    const float normBSq = reduce8(nb, tnb);
    if (normASq <= 0.0f || normBSq <= 0.0f)
        return 0.0;
    if (std::abs(normASq - 1.0f) <= 1e-5f && std::abs(normBSq - 1.0f) <= 1e-5f)
        return std::clamp(double(dot), -1.0, 1.0);
    return std::clamp(double(dot) / std::sqrt(double(normASq) * double(normBSq)), -1.0, 1.0);
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
        QSqlDatabase db;
        if (openPassageDbReadOnly(dbPath, connName, &db)) {
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
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                app,
                [self, cache, bookId, modelName, loaded, stillValid = std::move(stillValid), then = std::move(then)] {
                    if (!self || (stillValid && !stillValid()))
                        return;
                    {
                        QMutexLocker lock(&cache->mutex);
                        cache->bookId = bookId;
                        cache->model = modelName;
                        cache->vectors = loaded;
                        cache->loaded = true;
                    }
                    then(loaded);
                },
                Qt::QueuedConnection);
        }
    });
}

int PassageIndex::indexed(int bookId) const {
    Q_ASSERT(QThread::currentThread() == thread());
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
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_readOnly);
    if (m_readOnly)
        return false;
    const QList<float> unitVec = normalize(vector);
    const QString key = keyOf(passage);
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors (book_id, model, passage, vector) VALUES (?, ?, ?, ?)"));
    query.addBindValue(bookId);
    query.addBindValue(modelName);
    query.addBindValue(key);
    query.addBindValue(pack(unitVec));
    if (!query.exec())
        return false;
    QMutexLocker lock(&m_cache->mutex);
    if (m_cache->loaded && m_cache->bookId == bookId && m_cache->model == modelName)
        m_cache->vectors.insert(key, unitVec);
    return true;
}

void PassageIndex::build(int bookId, const QList<Passage> &passages, std::function<void(double)> progress,
                         std::function<void(const QString &)> done) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_readOnly);
    if (m_readOnly) {
        if (done)
            done(tr("The index is open read-only."));
        return;
    }
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
                const double cost = AiUsageLedger::costForProvider(
                    activeProvider->id(), activeEmbedder->embeddingModelName(), est, QDate::currentDate(), false);
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
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_readOnly);
    if (m_readOnly)
        return;
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
    const QList<Passage> batch = m_todo.mid(0, 32);
    double batchCost = 0.0;
    if (provider->id() != QLatin1String("local")) {
        qint64 batchChars = 0;
        for (const Passage &passage : batch)
            batchChars += passage.text.size();
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, batchChars / 4);
        batchCost = AiUsageLedger::costForProvider(provider->id(), embedder->embeddingModelName(), est,
                                                   QDate::currentDate(), false);
        if (!m_ai->fitsBudget(batchCost)) {
            m_todo.clear();
            auto done = std::exchange(m_done, nullptr);
            if (done)
                done(m_ai->describe(int(Ai::Error::Budget)));
            return;
        }
    }
    if (m_progress && m_total > 0)
        m_progress(1.0 - double(m_todo.size()) / m_total);
    QStringList texts;
    texts.reserve(batch.size());
    for (const Passage &passage : batch)
        texts.append(passage.text);
    Ai::Reply *reply = embedder->embed(texts, false);
    m_ai->track(reply, false, batchCost);
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
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_generation;
    ++m_searchGeneration;
    ++m_libraryGeneration;
    if (m_reply)
        m_reply->abort();
    if (m_searchReply)
        m_searchReply->abort();
    if (m_libraryReply)
        m_libraryReply->abort();
    m_todo.clear();
    m_buildModel.clear();
    m_done = nullptr;
    m_progress = nullptr;
}

void PassageIndex::search(int bookId, const QList<Passage> &passages, const QString &query, int count,
                          std::function<void(const QList<Hit> &, bool)> done) {
    Q_ASSERT(QThread::currentThread() == thread());
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
    double queryCost = 0.0;
    if (provider->id() != QLatin1String("local")) {
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, query.size() / 4);
        queryCost = AiUsageLedger::costForProvider(provider->id(), embedder->embeddingModelName(), est,
                                                   QDate::currentDate(), false);
        if (!m_ai->fitsBudget(queryCost)) {
            runWordFallback();
            return;
        }
    }
    const int existingCount = indexed(bookId);
    if (existingCount <= 0) {
        runWordFallback();
        return;
    }
    loadVectorsAsync(
        bookId, modelName, existingCount, [this, searchGen] { return searchGen == m_searchGeneration; },
        [this, passages, query, count, done, searchGen, runWordFallback](const QHash<QString, QList<float>> &vectors) {
            if (searchGen != m_searchGeneration)
                return;
            Ai::Provider *activeProvider = m_ai->embedderProvider();
            Ai::Embedder *activeEmbedder = m_ai->embedder();
            if (!activeProvider || !activeEmbedder || passages.isEmpty() || vectors.isEmpty()) {
                runWordFallback();
                return;
            }
            // Compute SHA-1 key once per passage and use any available vectors
            // (partial vector coverage still participates in hybrid search instead
            // of discarding all vectors if a single passage is missing).
            QList<QList<float>> matchedVectors;
            matchedVectors.reserve(passages.size());
            int matchedCount = 0;
            for (const Passage &passage : passages) {
                const auto it = vectors.constFind(keyOf(passage));
                if (it != vectors.cend() && !it->isEmpty()) {
                    matchedVectors.append(*it);
                    ++matchedCount;
                } else {
                    matchedVectors.append(QList<float>{});
                }
            }
            if (matchedCount == 0) {
                runWordFallback();
                return;
            }
            double activeQueryCost = 0.0;
            if (activeProvider->id() != QLatin1String("local")) {
                Ai::Usage est;
                est.inputTokens = qMax<qint64>(1, query.size() / 4);
                activeQueryCost = AiUsageLedger::costForProvider(activeProvider->id(),
                                                                 activeEmbedder->embeddingModelName(), est,
                                                                 QDate::currentDate(), false);
                if (!m_ai->fitsBudget(activeQueryCost)) {
                    runWordFallback();
                    return;
                }
            }
            Ai::Reply *reply = activeEmbedder->embed({query}, true);
            m_ai->track(reply, false, activeQueryCost);
            m_searchReply = reply;
            connect(reply, &Ai::Reply::finished, this,
                    [this, reply, passages, matchedVectors = std::move(matchedVectors), query, count, done, searchGen,
                     runWordFallback] {
                        reply->deleteLater();
                        if (searchGen != m_searchGeneration)
                            return;
                        if (reply->error() || reply->vectors().isEmpty()) {
                            runWordFallback();
                            return;
                        }
                        const QList<float> asked = reply->vectors().first();
                        QPointer<PassageIndex> self(this);
                        (void)QtConcurrent::run([self, passages, matchedVectors, asked, query, count, done, searchGen] {
                            const QList<float> unitAsked = PassageIndex::normalize(asked);
                            const qsizetype dim = unitAsked.size();
                            const float *askedData = unitAsked.constData();
                            float askedNormSq = 0.0f;
                            for (qsizetype i = 0; i < dim; ++i)
                                askedNormSq += askedData[i] * askedData[i];
                            const bool validQuery = dim > 0 && std::isfinite(askedNormSq) && askedNormSq > 0.0f;

                            QList<double> cosines;
                            cosines.reserve(passages.size());
                            for (const QList<float> &vec : matchedVectors) {
                                if (!validQuery || vec.size() != dim) {
                                    cosines.append(0.0);
                                    continue;
                                }
                                alignas(32) float d[8] = {};
                                alignas(32) float nb[8] = {};
                                float td = 0.0f, tnb = 0.0f;
                                accumulateDotAndNormB8(askedData, vec.constData(), dim, d, nb, td, tnb);
                                cosines.append(finalizeUnitQueryCosine(reduce8(d, td), reduce8(nb, tnb)));
                            }
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

namespace {

double cosineWithRawBlob(const float *unitAsked, qsizetype dim, const char *blobData, qsizetype blobBytes) {
    if (dim <= 0 || blobBytes != dim * qsizetype(sizeof(float)))
        return 0.0;
    alignas(32) float d[8] = {};
    alignas(32) float nb[8] = {};
    float td = 0.0f;
    float tnb = 0.0f;
    constexpr qsizetype kBlock = 256;
    alignas(32) float buf[kBlock];
    qsizetype i = 0;
    while (i < dim) {
        const qsizetype take = qMin(kBlock, dim - i);
        std::memcpy(buf, blobData + i * qsizetype(sizeof(float)), size_t(take) * sizeof(float));
        accumulateDotAndNormB8(unitAsked + i, buf, take, d, nb, td, tnb);
        i += take;
    }
    return finalizeUnitQueryCosine(reduce8(d, td), reduce8(nb, tnb));
}

bool parsePassageKey(const QString &key, int *chapterOut, qint64 *startOut, qint64 *endOut) {
    const qsizetype c1 = key.indexOf(QLatin1Char(':'));
    if (c1 <= 0)
        return false;
    const qsizetype c2 = key.indexOf(QLatin1Char(':'), c1 + 1);
    if (c2 <= c1 + 1)
        return false;
    const qsizetype c3 = key.indexOf(QLatin1Char(':'), c2 + 1);
    const QStringView v = key;
    bool ok0 = false, ok1 = false, ok2 = false;
    const int ch = v.sliced(0, c1).toInt(&ok0);
    const qint64 st = v.sliced(c1 + 1, c2 - c1 - 1).toLongLong(&ok1);
    const qint64 en = (c3 < 0 ? v.sliced(c2 + 1) : v.sliced(c2 + 1, c3 - c2 - 1)).toLongLong(&ok2);
    if (!ok0 || !ok1 || !ok2)
        return false;
    *chapterOut = ch;
    *startOut = st;
    *endOut = en;
    return true;
}

QList<PassageIndex::LibraryHit> rankLibraryInDb(QSqlDatabase &db, const QString &model, const QList<float> &asked,
                                                int count, double minCosine, const QSet<int> &bookIds) {
    QList<PassageIndex::LibraryHit> best;
    if (!db.isOpen() || model.isEmpty() || asked.isEmpty() || count <= 0)
        return best;

    const QList<float> unitAsked = PassageIndex::normalize(asked);
    const qsizetype dim = unitAsked.size();
    const float *askedData = unitAsked.constData();
    float askedNormSq = 0.0f;
    for (qsizetype i = 0; i < dim; ++i)
        askedNormSq += askedData[i] * askedData[i];
    if (!std::isfinite(askedNormSq) || askedNormSq <= 0.0f)
        return best;

    struct HeapEntry {
        double score = 0.0;
        int bookId = 0;
        int chapter = 0;
    };
    struct MinScoreCmp {
        bool operator()(const HeapEntry &a, const HeapEntry &b) const { return a.score > b.score; }
    };

    // Keep at most `count` distinct (bookId, chapter) winners in a bounded min-heap
    // so large libraries never allocate O(M) hits or full-sort every row.
    QHash<QPair<int, int>, PassageIndex::LibraryHit> bestByChapter;
    bestByChapter.reserve(count + 1);
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, MinScoreCmp> minHeap;
    double minHeapScore = -1.0;

    auto pruneStaleHeapTop = [&] {
        while (!minHeap.empty()) {
            const HeapEntry &top = minHeap.top();
            const auto it = bestByChapter.constFind({top.bookId, top.chapter});
            if (it == bestByChapter.cend() || it->score != top.score)
                minHeap.pop();
            else
                break;
        }
    };
    auto rebuildHeapIfBloated = [&] {
        if (minHeap.size() <= size_t(count) * 4 + 64)
            return;
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, MinScoreCmp> fresh;
        for (auto it = bestByChapter.cbegin(); it != bestByChapter.cend(); ++it)
            fresh.push({it->score, it->bookId, it->chapter});
        minHeap.swap(fresh);
    };
    auto refreshFloor = [&] {
        pruneStaleHeapTop();
        minHeapScore = (int(bestByChapter.size()) >= count && !minHeap.empty()) ? minHeap.top().score : -1.0;
    };

    auto scanQuery = [&](QSqlQuery &query) {
        if (!query.exec())
            return;
        while (query.next()) {
            const QByteArray blob = query.value(2).toByteArray();
            const double score = cosineWithRawBlob(askedData, dim, blob.constData(), blob.size());
            if (score < minCosine)
                continue;
            if (int(bestByChapter.size()) >= count && score <= minHeapScore)
                continue;

            int chapter = 0;
            qint64 start = 0;
            qint64 end = 0;
            if (!parsePassageKey(query.value(1).toString(), &chapter, &start, &end))
                continue;

            const int bookId = query.value(0).toInt();
            const QPair<int, int> key{bookId, chapter};
            auto it = bestByChapter.find(key);
            if (it != bestByChapter.end()) {
                if (score > it->score) {
                    it->start = start;
                    it->end = end;
                    it->score = score;
                    minHeap.push({score, bookId, chapter});
                    rebuildHeapIfBloated();
                    refreshFloor();
                }
                continue;
            }
            if (int(bestByChapter.size()) >= count) {
                pruneStaleHeapTop();
                if (!minHeap.empty() && score <= minHeap.top().score) {
                    minHeapScore = minHeap.top().score;
                    continue;
                }
                if (!minHeap.empty()) {
                    const HeapEntry worst = minHeap.top();
                    minHeap.pop();
                    bestByChapter.remove({worst.bookId, worst.chapter});
                }
            }
            bestByChapter.insert(key, {bookId, chapter, start, end, score});
            minHeap.push({score, bookId, chapter});
            rebuildHeapIfBloated();
            refreshFloor();
        }
    };

    if (bookIds.isEmpty()) {
        QSqlQuery query(db);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral("SELECT book_id, passage, vector FROM passage_vectors WHERE model = ?"));
        query.addBindValue(model);
        scanQuery(query);
    } else {
        QList<int> idList(bookIds.cbegin(), bookIds.cend());
        std::sort(idList.begin(), idList.end());
        constexpr qsizetype kChunkSize = 500;
        for (qsizetype off = 0; off < idList.size(); off += kChunkSize) {
            const qsizetype take = qMin(kChunkSize, idList.size() - off);
            QString sql;
            sql.reserve(80 + take * 2);
            sql += QStringLiteral("SELECT book_id, passage, vector FROM passage_vectors WHERE model = ? AND book_id IN (");
            for (qsizetype i = 0; i < take; ++i) {
                if (i > 0)
                    sql += QLatin1Char(',');
                sql += QLatin1Char('?');
            }
            sql += QLatin1Char(')');
            QSqlQuery query(db);
            query.setForwardOnly(true);
            query.prepare(sql);
            query.addBindValue(model);
            for (qsizetype i = 0; i < take; ++i)
                query.addBindValue(idList[off + i]);
            scanQuery(query);
        }
    }

    best.reserve(bestByChapter.size());
    for (auto it = bestByChapter.cbegin(); it != bestByChapter.cend(); ++it)
        best.append(it.value());
    std::sort(best.begin(), best.end(), [](const PassageIndex::LibraryHit &a, const PassageIndex::LibraryHit &b) {
        return a.score > b.score;
    });
    return best;
}
}  // namespace

QList<PassageIndex::LibraryHit> PassageIndex::rankLibrary(const QString &dbPath, const QString &model,
                                                          const QList<float> &asked, int count, double minCosine,
                                                          const QSet<int> &bookIds) {
    QList<LibraryHit> best;
    if (dbPath.isEmpty() || model.isEmpty() || asked.isEmpty())
        return best;
    const QString connName = QStringLiteral("pi-library-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db;
        if (openPassageDbReadOnly(dbPath, connName, &db)) {
            best = rankLibraryInDb(db, model, asked, count, minCosine, bookIds);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return best;
}

int PassageIndex::booksPrepared() const {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString name = model();
    if (name.isEmpty())
        return 0;
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT COUNT(DISTINCT book_id) FROM passage_vectors WHERE model = ?"));
    query.addBindValue(name);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

void PassageIndex::searchLibrary(const QString &query, int count, std::function<void(const QList<LibraryHit> &)> done,
                                 const QSet<int> &bookIds) {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_libraryGeneration;
    if (m_libraryReply)
        m_libraryReply->abort();
    const int libGen = m_libraryGeneration;
    Ai::Provider *provider = m_ai->embedderProvider();
    Ai::Embedder *embedder = m_ai->embedder();
    const QString name = model();
    if (!provider || !embedder || name.isEmpty() || query.trimmed().isEmpty() || booksPrepared() == 0) {
        done({});
        return;
    }
    double queryCost = 0.0;
    if (provider->id() != QLatin1String("local")) {
        Ai::Usage est;
        est.inputTokens = qMax<qint64>(1, query.size() / 4);
        queryCost = AiUsageLedger::costForProvider(provider->id(), embedder->embeddingModelName(), est,
                                                   QDate::currentDate(), false);
        if (!m_ai->fitsBudget(queryCost)) {
            done({});
            return;
        }
    }
    Ai::Reply *reply = embedder->embed({query}, true);
    m_ai->track(reply, false, queryCost);
    m_libraryReply = reply;
    const QString path = databasePath();
    QPointer<PassageIndex> self(this);
    connect(reply, &Ai::Reply::finished, this, [self, reply, path, name, count, done, bookIds, libGen] {
        reply->deleteLater();
        if (!self || libGen != self->m_libraryGeneration)
            return;
        if (reply->error() || reply->vectors().isEmpty()) {
            done({});
            return;
        }
        const QList<float> asked = reply->vectors().first();
        if (path.isEmpty() || path == QLatin1String(":memory:")) {
            QSqlDatabase db = QSqlDatabase::database(self->m_connection, false);
            done(rankLibraryInDb(db, name, asked, count, minSemanticScore, bookIds));
            return;
        }
        (void)QtConcurrent::run([self, path, name, asked, count, done, bookIds, libGen] {
            const QList<LibraryHit> hits = rankLibrary(path, name, asked, count, minSemanticScore, bookIds);
            if (QCoreApplication *app = QCoreApplication::instance())
                QMetaObject::invokeMethod(
                    app,
                    [self, hits, done, libGen] {
                        if (self && libGen == self->m_libraryGeneration)
                            done(hits);
                    },
                    Qt::QueuedConnection);
        });
    });
}
