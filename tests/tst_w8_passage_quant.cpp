#include "ai/ai.h"
#include "ai/aiservices.h"
#include "ai/keystore.h"
#include "ai/localmodels.h"
#include "ai/passageindex.h"
#include "fakeserver.h"
#include "library/library.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <sys/stat.h>

namespace {

// Minimal headless Ai::Provider stub so this test links strictly against clothbound_headless + Qt6::Test
// (zero WebEngine / Qml / Quick / Multimedia dependency).
class StubCloudProvider final : public Ai::Provider {
public:
    explicit StubCloudProvider(QObject *parent = nullptr) : Ai::Provider(parent) {}
    QString id() const override { return QStringLiteral("stub"); }
    QString name() const override { return QStringLiteral("Stub"); }
    QString privacyNote() const override { return QStringLiteral("Stub provider"); }
    bool isLocal() const override { return false; }
    bool needsKey() const override { return true; }
    void setKey(const QString &) override {}
    bool hasKey() const override { return false; }
    Ai::Reply *check() override { return nullptr; }
};

QByteArray packFp32(const QList<float> &v) {
    return QByteArray(reinterpret_cast<const char *>(v.constData()), v.size() * qsizetype(sizeof(float)));
}

double exactDoubleCosine(const QList<float> &a, const QList<float> &b) {
    if (a.size() != b.size() || a.isEmpty())
        return 0.0;
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * double(b[i]);
        na += double(a[i]) * double(a[i]);
        nb += double(b[i]) * double(b[i]);
    }
    if (!std::isfinite(dot) || !std::isfinite(na) || !std::isfinite(nb) || na <= 0.0 || nb <= 0.0)
        return 0.0;
    return std::clamp(dot / std::sqrt(na * nb), -1.0, 1.0);
}

QList<float> randomUniformUnit(std::mt19937 &rng, int dim) {
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    QList<float> v(dim);
    double n2 = 0.0;
    for (int i = 0; i < dim; ++i) {
        v[i] = dist(rng);
        n2 += double(v[i]) * double(v[i]);
    }
    const float inv = float(1.0 / std::sqrt(n2));
    for (int i = 0; i < dim; ++i)
        v[i] *= inv;
    return v;
}

QList<float> randomGaussianUnit(std::mt19937 &rng, int dim) {
    std::normal_distribution<float> dist(0.0f, 1.0f);
    QList<float> v(dim);
    double n2 = 0.0;
    for (int i = 0; i < dim; ++i) {
        v[i] = dist(rng);
        n2 += double(v[i]) * double(v[i]);
    }
    const float inv = float(1.0 / std::sqrt(n2));
    for (int i = 0; i < dim; ++i)
        v[i] *= inv;
    return v;
}

QList<float> mixUnitVectors(const QList<float> &anchor, const QList<float> &noise, float mix) {
    const qsizetype dim = anchor.size();
    const float nw = std::sqrt(std::max(0.0f, 1.0f - mix * mix));
    QList<float> out(dim);
    double n2 = 0.0;
    for (qsizetype i = 0; i < dim; ++i) {
        out[i] = mix * anchor[i] + nw * noise[i];
        n2 += double(out[i]) * double(out[i]);
    }
    const float inv = n2 > 0.0 ? float(1.0 / std::sqrt(n2)) : 1.0f;
    for (qsizetype i = 0; i < dim; ++i)
        out[i] *= inv;
    return out;
}

QByteArray embedJsonResponse(const QList<QList<float>> &vectors) {
    QJsonArray outer;
    for (const QList<float> &vec : vectors) {
        QJsonArray arr;
        for (float f : vec)
            arr.append(double(f));
        outer.append(arr);
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("embeddings"), outer}}).toJson(QJsonDocument::Compact);
}

QByteArray makeQ8BlobRaw(float scale, const QByteArray &payload) {
    QByteArray blob(PassageIndex::kQuantizedHeaderBytes + payload.size(), Qt::Uninitialized);
    blob[0] = 'Q';
    blob[1] = '8';
    blob[2] = '\0';
    blob[3] = '\1';
    quint32 bits = 0;
    std::memcpy(&bits, &scale, sizeof(bits));
    qToLittleEndian<quint32>(bits, blob.data() + 4);
    if (!payload.isEmpty())
        std::memcpy(blob.data() + PassageIndex::kQuantizedHeaderBytes, payload.constData(), size_t(payload.size()));
    return blob;
}

}  // namespace

class W8PassageQuantTests : public QObject {
    Q_OBJECT

private slots:
    // (a) 3.9x+ BLOB size compression on 768-dim embeddings + binary layout verification.
    void blobSizeCompressionAndLayout() {
        std::mt19937 rng(0x768001u);
        const QList<float> v768 = randomGaussianUnit(rng, 768);
        const QByteArray fp32 = packFp32(v768);
        const QByteArray q8 = PassageIndex::quantize(v768);

        QCOMPARE(fp32.size(), qsizetype(768 * sizeof(float)));  // 3,072 bytes
        QCOMPARE(q8.size(), qsizetype(8 + 768));                // 776 bytes
        const double compressionRatio = double(fp32.size()) / double(q8.size());
        QVERIFY2(compressionRatio >= 3.95, qPrintable(QString::number(compressionRatio)));

        // Verify explicit 4-byte header magic ('Q','8','\0','\1') and finite positive LE float scale.
        QCOMPARE(q8[0], 'Q');
        QCOMPARE(q8[1], '8');
        QCOMPARE(q8[2], '\0');
        QCOMPARE(q8[3], '\1');
        QVERIFY(PassageIndex::isQuantizedBlob(q8));
        QVERIFY(!PassageIndex::isQuantizedBlob(fp32));

        // Round-trip dequantize produces a unit-norm 768-dim vector closely matching original.
        const QList<float> restored = PassageIndex::dequantize(q8);
        QCOMPARE(restored.size(), 768);
        double normSq = 0.0;
        for (float f : restored)
            normSq += double(f) * double(f);
        QVERIFY2(std::abs(std::sqrt(normSq) - 1.0) < 1e-5, qPrintable(QString::number(normSq)));
        const double selfCos = exactDoubleCosine(v768, restored);
        QVERIFY2(selfCos > 0.999, qPrintable(QString::number(selfCos)));

        // Check additional production and non-multiple-of-16 dimensions (exercises SIMD tail loop).
        for (const int dim : {1, 3, 15, 16, 17, 31, 33, 63, 384, 769, 1024, 1536, 3072}) {
            const QList<float> v = randomUniformUnit(rng, dim);
            const QByteArray qb = PassageIndex::quantize(v);
            QCOMPARE(qb.size(), qsizetype(8 + dim));
            QVERIFY(PassageIndex::isQuantizedBlob(qb));
            const QList<float> dq = PassageIndex::dequantize(qb);
            QCOMPARE(dq.size(), dim);
            QVERIFY2(exactDoubleCosine(v, dq) > 0.998, qPrintable(QStringLiteral("dim=%1").arg(dim)));
        }

        // Unnormalized input scales identically to normalized input.
        QList<float> scaled = v768;
        for (float &x : scaled)
            x *= 42.5f;
        QCOMPARE(PassageIndex::quantize(scaled), q8);
    }

    // (b) Cosine similarity error < 0.005 vs exact fp32 across random 768-dim vectors + top-K ranking agreement.
    void cosineAccuracyAndTopKAgreement768() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        constexpr int kDim = 768;

        std::mt19937 rng(0x20261125u);
        const QList<float> query = randomGaussianUnit(rng, kDim);

        const QString fp32Path = dir.filePath(QStringLiteral("fp32.sqlite"));
        const QString q8Path = dir.filePath(QStringLiteral("q8.sqlite"));
        const QString cFp = QStringLiteral("w8-acc-fp32");
        const QString cQ8 = QStringLiteral("w8-acc-q8");

        constexpr int kRows = 600;
        QList<double> exactScores(kRows);
        {
            QSqlDatabase dbFp = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), cFp);
            dbFp.setDatabaseName(fp32Path);
            QVERIFY(dbFp.open());
            QSqlDatabase dbQ8 = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), cQ8);
            dbQ8.setDatabaseName(q8Path);
            QVERIFY(dbQ8.open());

            PassageIndex initFp(nullptr, cFp);
            PassageIndex initQ8(nullptr, cQ8);
            Q_UNUSED(initFp);
            Q_UNUSED(initQ8);

            QVERIFY(dbFp.transaction());
            QVERIFY(dbQ8.transaction());
            QSqlQuery insFp(dbFp);
            insFp.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, 'm', ?, ?)"));
            QSqlQuery insQ8(dbQ8);
            insQ8.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, 'm', ?, ?)"));

            for (int r = 0; r < kRows; ++r) {
                QList<float> v;
                if (r == 0) {
                    v = query;  // exact match (cosine = 1.0)
                } else if (r == 1) {
                    v = query;
                    for (float &x : v)
                        x = -x;  // antipodal (-1.0)
                } else if (r < 22) {
                    // Distinct top-20 spaced cosines in [0.40, 0.97] so top-K order is unambiguous.
                    const float mix = 0.40f + float(r - 2) * 0.0285f;
                    v = mixUnitVectors(query, randomGaussianUnit(rng, kDim), mix);
                } else if (r % 2 == 0) {
                    v = randomUniformUnit(rng, kDim);
                } else {
                    v = randomGaussianUnit(rng, kDim);
                }
                exactScores[r] = exactDoubleCosine(query, v);

                // Verify dequantized cosine error is strictly < 0.005 for every single vector.
                const QByteArray q8Blob = PassageIndex::quantize(v);
                const QList<float> dq = PassageIndex::dequantize(q8Blob);
                const double dqErr = std::abs(exactDoubleCosine(query, dq) - exactScores[r]);
                QVERIFY2(dqErr < 0.005,
                         qPrintable(QStringLiteral("row %1 dqErr=%2").arg(r).arg(dqErr, 0, 'f', 6)));

                const QString key = QStringLiteral("0:0:100:r%1").arg(r);
                insFp.addBindValue(r + 1);
                insFp.addBindValue(key);
                insFp.addBindValue(packFp32(v));
                QVERIFY(insFp.exec());

                insQ8.addBindValue(r + 1);
                insQ8.addBindValue(key);
                insQ8.addBindValue(q8Blob);
                QVERIFY(insQ8.exec());
            }
            QVERIFY(dbFp.commit());
            QVERIFY(dbQ8.commit());
            dbFp.close();
            dbQ8.close();
        }
        QSqlDatabase::removeDatabase(cFp);
        QSqlDatabase::removeDatabase(cQ8);

        // Compare full-table rankLibrary scores for EVERY row above -1.0 via minCosine=-1.1.
        const auto allFp = PassageIndex::rankLibrary(fp32Path, QStringLiteral("m"), query, kRows, -1.1);
        const auto allQ8 = PassageIndex::rankLibrary(q8Path, QStringLiteral("m"), query, kRows, -1.1);
        QCOMPARE(allFp.size(), kRows);
        QCOMPARE(allQ8.size(), kRows);

        QHash<int, double> q8ByBook;
        for (const auto &h : allQ8)
            q8ByBook.insert(h.bookId, h.score);

        double maxErr = 0.0;
        for (int r = 0; r < kRows; ++r) {
            QVERIFY(q8ByBook.contains(r + 1));
            const double err = std::abs(q8ByBook.value(r + 1) - exactScores[r]);
            maxErr = std::max(maxErr, err);
            QVERIFY2(err < 0.005,
                     qPrintable(QStringLiteral("book %1 exact=%2 q8=%3 err=%4")
                                    .arg(r + 1)
                                    .arg(exactScores[r], 0, 'f', 6)
                                    .arg(q8ByBook.value(r + 1), 0, 'f', 6)
                                    .arg(err, 0, 'f', 6)));
        }
        // Self-match exact 1.0, antipodal exact -1.0.
        QVERIFY2(std::abs(q8ByBook.value(1) - 1.0) < 1e-6, qPrintable(QString::number(q8ByBook.value(1))));
        QVERIFY2(std::abs(q8ByBook.value(2) + 1.0) < 1e-6, qPrintable(QString::number(q8ByBook.value(2))));

        // Top-12 ranking agreement (bounded min-heap path with default minSemanticScore = 0.25).
        const auto topFp = PassageIndex::rankLibrary(fp32Path, QStringLiteral("m"), query, 12, 0.25);
        const auto topQ8 = PassageIndex::rankLibrary(q8Path, QStringLiteral("m"), query, 12, 0.25);
        QCOMPARE(topFp.size(), 12);
        QCOMPARE(topQ8.size(), 12);
        for (int i = 0; i < 12; ++i) {
            QCOMPARE(topQ8[i].bookId, topFp[i].bookId);
            QVERIFY2(std::abs(topQ8[i].score - topFp[i].score) < 0.005,
                     qPrintable(QStringLiteral("rank %1 fp=%2 q8=%3").arg(i).arg(topFp[i].score).arg(topQ8[i].score)));
        }

        // Verify bookIds pushdown works identically on Q8 DB (including read-only chmod 0444 file!).
        QVERIFY(::chmod(q8Path.toUtf8().constData(), 0444) == 0);
        auto restorePerm = qScopeGuard([&] { ::chmod(q8Path.toUtf8().constData(), 0644); });
        const QSet<int> shelf{1, 10, 15, 20};
        const auto shelfHits = PassageIndex::rankLibrary(q8Path, QStringLiteral("m"), query, 10, 0.25, shelf);
        QCOMPARE(shelfHits.size(), 4);
        QCOMPARE(shelfHits[0].bookId, 1);
        for (const auto &h : shelfHits)
            QVERIFY(shelf.contains(h.bookId));
    }

    // (c) Seamless backward compatibility reading legacy fp32 BLOBs mixed with new Q8 BLOBs
    //     in the same passage_vectors table (both rankLibrary AND per-book search / VectorCache).
    void mixedLegacyFp32AndQ8InSameTable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        constexpr int kDim = 768;

        std::mt19937 rng(0xCAFEBABEu);
        const QList<float> query = randomGaussianUnit(rng, kDim);

        // 1. Mixed formats inside rankLibrary() on the exact same passage_vectors table.
        const QString dbPath = dir.filePath(QStringLiteral("mixed.sqlite"));
        const QString conn = QStringLiteral("w8-mixed-lib");
        const QList<float> vRank1Q8 = mixUnitVectors(query, randomGaussianUnit(rng, kDim), 0.95f);
        const QList<float> vRank2LegacyUnnorm = [&] {
            QList<float> v = mixUnitVectors(query, randomGaussianUnit(rng, kDim), 0.80f);
            for (float &x : v)
                x *= 12.5f;  // legacy unnormalized fp32 BLOB
            return v;
        }();
        const QList<float> vRank3Q8 = mixUnitVectors(query, randomGaussianUnit(rng, kDim), 0.65f);
        const QList<float> vRank4LegacyUnit = mixUnitVectors(query, randomGaussianUnit(rng, kDim), 0.50f);
        const QList<float> vLowQ8 = randomUniformUnit(rng, kDim);

        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            PassageIndex init(nullptr, conn);
            Q_UNUSED(init);

            QSqlQuery ins(db);
            ins.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, 'm', ?, ?)"));
            auto addRow = [&](int bookId, const QString &key, const QByteArray &blob) {
                ins.addBindValue(bookId);
                ins.addBindValue(key);
                ins.addBindValue(blob);
                QVERIFY(ins.exec());
            };
            addRow(101, QStringLiteral("0:0:100:q1"), PassageIndex::quantize(vRank1Q8));
            addRow(102, QStringLiteral("1:0:100:f2"), packFp32(vRank2LegacyUnnorm));
            addRow(103, QStringLiteral("2:0:100:q3"), PassageIndex::quantize(vRank3Q8));
            addRow(104, QStringLiteral("3:0:100:f4"), packFp32(vRank4LegacyUnit));
            addRow(105, QStringLiteral("4:0:100:q5"), PassageIndex::quantize(vLowQ8));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        const auto hits = PassageIndex::rankLibrary(dbPath, QStringLiteral("m"), query, 10, 0.25);
        QCOMPARE(hits.size(), 4);
        QCOMPARE(hits[0].bookId, 101);  // Q8 (~0.95)
        QCOMPARE(hits[1].bookId, 102);  // legacy unnormalized fp32 (~0.80)
        QCOMPARE(hits[2].bookId, 103);  // Q8 (~0.65)
        QCOMPARE(hits[3].bookId, 104);  // legacy unit fp32 (~0.50)
        QVERIFY(std::abs(hits[0].score - exactDoubleCosine(query, vRank1Q8)) < 0.005);
        QVERIFY(std::abs(hits[1].score - exactDoubleCosine(query, vRank2LegacyUnnorm)) < 1e-5);
        QVERIFY(std::abs(hits[2].score - exactDoubleCosine(query, vRank3Q8)) < 0.005);
        QVERIFY(std::abs(hits[3].score - exactDoubleCosine(query, vRank4LegacyUnit)) < 1e-5);

        // 2. End-to-end PassageIndex::build() + per-book PassageIndex::search() with mixed Q8 + legacy fp32 in ONE book.
        Library lib(dir.filePath(QStringLiteral("lib_data")), dir.filePath(QStringLiteral("lib_cache")));
        QVERIFY(lib.open());
        // Seed book rows 77 and 88 in `books` so PassageIndex's orphan-pruning trigger preserves their vectors.
        {
            QSqlQuery insBook(lib.database());
            insBook.prepare(QStringLiteral(
                "INSERT INTO books (id, path, format, size, mtime, title, sort_title, author, sort_author, "
                "publisher, year, language, series, series_index, work_key, chapters, pages, length, cover, "
                "added_at, opened_at, finished_at, progress, position, seconds_read, missing, hidden, loose, want_at, listened_at) "
                "VALUES (?, ?, 'epub', 1024, 1, 'Book', 'book', 'Author', 'author', '', 2025, 'en', '', 0, ?, 1, 0, 1000, '', "
                "1, 0, 0, 0, '', 0, 0, 0, 0, 0, 0)"));
            for (const int bid : {77, 88}) {
                insBook.addBindValue(bid);
                insBook.addBindValue(QStringLiteral("/fake/w8_book_%1.epub").arg(bid));
                insBook.addBindValue(QStringLiteral("work-w8-%1").arg(bid));
                QVERIFY(insBook.exec());
            }
        }

        FakeServer srv;
        StubCloudProvider stubCloud;
        MemoryKeyStore keys;
        AiServices ai(&stubCloud, &keys, lib.database().connectionName());
        LocalModels lm;
        LocalModels::Server scfg;
        scfg.kind = LocalModels::Ollama;
        scfg.url = QUrl(srv.origin());
        lm.configure(scfg, QStringLiteral("qwen3.5:4b"), QStringLiteral("nomic-embed-text"));
        ai.setLocalModels(&lm);
        ai.setPreset(QStringLiteral("local"));

        PassageIndex pi(&ai, lib.database().connectionName());
        const QList<PassageIndex::Passage> passages{
            {0, 0, 40, QStringLiteral("Ancient mariners charted northern constellations.")},
            {0, 45, 90, QStringLiteral("Bakers kneaded sourdough at dawn in the valley.")},
            {0, 95, 140, QStringLiteral("Seafaring astronomers refined sextant mirrors.")},
        };
        const QList<float> vP0 = mixUnitVectors(query, randomGaussianUnit(rng, kDim), 0.72f);  // will be stored as Q8 by build()
        const QList<float> vP1 = randomUniformUnit(rng, kDim);                                // low-sim Q8
        const QList<float> vP2 = mixUnitVectors(query, randomGaussianUnit(rng, kDim), 0.92f);  // manually seeded as legacy fp32!

        // Seed passage 2 manually as a legacy unnormalized fp32 BLOB BEFORE calling build().
        {
            QList<float> legacyUnnormP2 = vP2;
            for (float &x : legacyUnnormP2)
                x *= 5.0f;
            const QByteArray digest =
                QCryptographicHash::hash(passages[2].text.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
            const QString keyP2 = QStringLiteral("%1:%2:%3:%4")
                                      .arg(passages[2].chapter)
                                      .arg(passages[2].start)
                                      .arg(passages[2].end)
                                      .arg(QString::fromLatin1(digest));
            QSqlQuery ins(lib.database());
            ins.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, ?, ?, ?)"));
            ins.addBindValue(77);
            ins.addBindValue(QStringLiteral("local/nomic-embed-text"));
            ins.addBindValue(keyP2);
            ins.addBindValue(packFp32(legacyUnnormP2));
            QVERIFY(ins.exec());
        }

        // build() skips existing legacy passage 2 and embeds passages 0 & 1 -> stored as 776-byte tagged Q8!
        srv.responses.append({200, "application/json", embedJsonResponse({vP0, vP1})});
        QString buildErr = QStringLiteral("unset");
        pi.build(77, passages, nullptr, [&](const QString &e) { buildErr = e; });
        QTRY_COMPARE_WITH_TIMEOUT(buildErr, QString(), 5000);
        QCOMPARE(pi.indexed(77), 3);

        // Verify SQLite literally contains 2 Q8 rows (776 B each) + 1 legacy fp32 row (3,072 B).
        int q8Rows = 0;
        int fp32Rows = 0;
        {
            QSqlQuery q(lib.database());
            QVERIFY(q.exec(QStringLiteral("SELECT vector FROM passage_vectors WHERE book_id = 77")));
            while (q.next()) {
                const QByteArray b = q.value(0).toByteArray();
                if (PassageIndex::isQuantizedBlob(b)) {
                    QCOMPARE(b.size(), qsizetype(8 + kDim));
                    ++q8Rows;
                } else {
                    QCOMPARE(b.size(), qsizetype(kDim * sizeof(float)));
                    ++fp32Rows;
                }
            }
        }
        QCOMPARE(q8Rows, 2);
        QCOMPARE(fp32Rows, 1);

        // Search via warm VectorCache AND via a cold PassageIndex instance (reads from disk via loadFromFile).
        for (int cold = 0; cold < 2; ++cold) {
            std::unique_ptr<PassageIndex> coldIdx;
            PassageIndex *activeIdx = &pi;
            if (cold == 1) {
                coldIdx = std::make_unique<PassageIndex>(&ai, lib.database().connectionName());
                activeIdx = coldIdx.get();
            }
            srv.responses.append({200, "application/json", embedJsonResponse({query})});
            QList<PassageIndex::Hit> found;
            bool byMeaning = false;
            bool finished = false;
            activeIdx->search(77, passages, QStringLiteral("celestial navigation"), 3,
                              [&](const QList<PassageIndex::Hit> &h, bool m) {
                                  found = h;
                                  byMeaning = m;
                                  finished = true;
                              });
            QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
            QVERIFY(byMeaning);
            QVERIFY(found.size() >= 2);
            // Passage 2 (legacy fp32, cosine ~0.92) ranks #1, Passage 0 (Q8, cosine ~0.72) ranks #2!
            QCOMPARE(found[0].passage.text, passages[2].text);
            QCOMPARE(found[1].passage.text, passages[0].text);
            QVERIFY(std::abs(found[0].semantic - exactDoubleCosine(query, vP2)) < 1e-4);
            QVERIFY(std::abs(found[1].semantic - exactDoubleCosine(query, vP0)) < 0.005);
        }

        // Also verify tiny vectors (< kMinQuantizeDims) stay raw normalized fp32 when stored via build().
        srv.responses.append({200, "application/json", QByteArrayLiteral("{\"embeddings\":[[3.0,4.0,0.0]]}")});
        buildErr = QStringLiteral("unset");
        pi.build(88, {{0, 0, 12, QStringLiteral("Tiny vector.")}}, nullptr, [&](const QString &e) { buildErr = e; });
        QTRY_COMPARE_WITH_TIMEOUT(buildErr, QString(), 5000);
        {
            QSqlQuery q(lib.database());
            QVERIFY(q.exec(QStringLiteral("SELECT vector FROM passage_vectors WHERE book_id = 88")));
            QVERIFY(q.next());
            const QByteArray b = q.value(0).toByteArray();
            QCOMPARE(b.size(), qsizetype(3 * sizeof(float)));
            QVERIFY(!PassageIndex::isQuantizedBlob(b));
        }

        // Verify AFTER DELETE ON books trigger still cleans up Q8 passage_vectors rows.
        {
            QSqlQuery del(lib.database());
            QVERIFY(del.exec(QStringLiteral("DELETE FROM books WHERE id = 77")));
            QCOMPARE(pi.indexed(77), 0);
        }
    }

    // (d) Zero-vector / near-zero / NaN / Inf / truncated-BLOB / extreme-dim defensive handling.
    void defensiveZeroNearZeroNanAndTruncatedBlobs() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        constexpr int kDim = 768;

        // 1. Empty / zero / near-zero / NaN / Inf quantize & dequantize.
        QVERIFY(PassageIndex::quantize({}).isEmpty());
        QVERIFY(PassageIndex::dequantize({}).isEmpty());
        QVERIFY(!PassageIndex::isQuantizedBlob({}));

        const QList<float> zeroVec(kDim, 0.0f);
        const QByteArray zeroBlob = PassageIndex::quantize(zeroVec);
        QCOMPARE(zeroBlob.size(), qsizetype(8 + kDim));
        QVERIFY(PassageIndex::isQuantizedBlob(zeroBlob));
        const QList<float> zeroOut = PassageIndex::dequantize(zeroBlob);
        QCOMPARE(zeroOut.size(), kDim);
        for (float f : zeroOut)
            QCOMPARE(f, 0.0f);

        const QList<float> nearZeroVec(kDim, 1e-38f);
        const QByteArray nearZeroBlob = PassageIndex::quantize(nearZeroVec);
        QCOMPARE(nearZeroBlob.size(), qsizetype(8 + kDim));
        QVERIFY(PassageIndex::isQuantizedBlob(nearZeroBlob));
        for (float f : PassageIndex::dequantize(nearZeroBlob)) {
            QVERIFY(std::isfinite(f));
            QCOMPARE(f, 0.0f);
        }

        QList<float> nanVec(kDim, 0.1f);
        nanVec[42] = std::numeric_limits<float>::quiet_NaN();
        const QByteArray nanBlob = PassageIndex::quantize(nanVec);
        QVERIFY(PassageIndex::isQuantizedBlob(nanBlob));
        for (float f : PassageIndex::dequantize(nanBlob)) {
            QVERIFY(std::isfinite(f));
            QCOMPARE(f, 0.0f);
        }

        QList<float> infVec(kDim, 0.1f);
        infVec[17] = std::numeric_limits<float>::infinity();
        const QByteArray infBlob = PassageIndex::quantize(infVec);
        QVERIFY(PassageIndex::isQuantizedBlob(infBlob));
        for (float f : PassageIndex::dequantize(infBlob)) {
            QVERIFY(std::isfinite(f));
            QCOMPARE(f, 0.0f);
        }

        // Small normal vector (1e-12) and huge vector (1e25) quantize cleanly without underflow/overflow.
        std::mt19937 rng(0xDEF001u);
        const QList<float> unitRef = randomGaussianUnit(rng, kDim);
        QList<float> smallNormal = unitRef;
        QList<float> hugeNormal = unitRef;
        for (int i = 0; i < kDim; ++i) {
            smallNormal[i] *= 1e-12f;
            hugeNormal[i] *= 1e25f;
        }
        QVERIFY(exactDoubleCosine(unitRef, PassageIndex::dequantize(PassageIndex::quantize(smallNormal))) > 0.999);
        QVERIFY(exactDoubleCosine(unitRef, PassageIndex::dequantize(PassageIndex::quantize(hugeNormal))) > 0.999);

        // PassageIndex::cosine() never returns NaN on NaN/Inf inputs.
        QCOMPARE(PassageIndex::cosine(nanVec, unitRef), 0.0);
        QCOMPARE(PassageIndex::cosine(infVec, unitRef), 0.0);
        QCOMPARE(PassageIndex::cosine(zeroVec, unitRef), 0.0);

        // 2. Malformed, truncated, and corrupt-scale BLOBs in dequantize() and rankLibrary().
        const QByteArray truncHeaderOnly = makeQ8BlobRaw(0.005f, QByteArray{});
        const QByteArray truncQ8Short = PassageIndex::quantize(unitRef).left(8 + 100);
        const QByteArray truncQ8MinusOne = PassageIndex::quantize(unitRef).left(8 + kDim - 1);
        const QByteArray corruptNanScale = makeQ8BlobRaw(std::numeric_limits<float>::quiet_NaN(), QByteArray(kDim, '\1'));
        const QByteArray corruptInfScale = makeQ8BlobRaw(std::numeric_limits<float>::infinity(), QByteArray(kDim, '\1'));
        const QByteArray corruptNegScale = makeQ8BlobRaw(-0.01f, QByteArray(kDim, '\1'));
        const QByteArray truncFp32 = packFp32(unitRef).left(kDim * int(sizeof(float)) - 1);
        const QByteArray tinyJunk("Q8", 2);
        const QByteArray wrongDimQ8 = PassageIndex::quantize(randomUniformUnit(rng, 384));
        const QByteArray legacyNanFp32 = packFp32(nanVec);
        // External Q8 payload with -128 bytes (must not overflow int16/int32 under UBSan).
        const QByteArray minInt8Payload(kDim, char(-128));
        const QByteArray minInt8Q8 = makeQ8BlobRaw(float(1.0 / std::sqrt(double(kDim) * 128.0 * 128.0)), minInt8Payload);

        QVERIFY(!PassageIndex::isQuantizedBlob(truncHeaderOnly));
        QVERIFY(!PassageIndex::isQuantizedBlob(corruptNanScale));
        QVERIFY(!PassageIndex::isQuantizedBlob(corruptInfScale));
        QVERIFY(!PassageIndex::isQuantizedBlob(corruptNegScale));
        QVERIFY(!PassageIndex::isQuantizedBlob(tinyJunk));
        QVERIFY(PassageIndex::dequantize(truncHeaderOnly).isEmpty());
        QVERIFY(PassageIndex::dequantize(corruptNanScale).isEmpty());
        QVERIFY(PassageIndex::dequantize(corruptInfScale).isEmpty());
        QVERIFY(PassageIndex::dequantize(corruptNegScale).isEmpty());
        QVERIFY(PassageIndex::dequantize(truncFp32).isEmpty());
        QVERIFY(PassageIndex::dequantize(tinyJunk).isEmpty());

        const QString edgeDb = dir.filePath(QStringLiteral("edge.sqlite"));
        const QString edgeConn = QStringLiteral("w8-edge-db");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), edgeConn);
            db.setDatabaseName(edgeDb);
            QVERIFY(db.open());
            PassageIndex init(nullptr, edgeConn);
            Q_UNUSED(init);
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, 'm', ?, ?)"));
            auto add = [&](int id, const QByteArray &blob) {
                ins.addBindValue(id);
                ins.addBindValue(QStringLiteral("0:0:50:e%1").arg(id));
                ins.addBindValue(blob);
                QVERIFY(ins.exec());
            };
            add(1, QByteArray("", 0));  // non-null 0-byte BLOB
            add(2, tinyJunk);
            add(3, truncHeaderOnly);
            add(4, truncQ8Short);
            add(5, truncQ8MinusOne);
            add(6, corruptNanScale);
            add(7, corruptInfScale);
            add(8, corruptNegScale);
            add(9, truncFp32);
            add(10, wrongDimQ8);
            add(11, zeroBlob);
            add(12, nearZeroBlob);
            add(13, nanBlob);
            add(14, infBlob);
            add(15, legacyNanFp32);
            add(16, minInt8Q8);
            add(99, PassageIndex::quantize(unitRef));  // sole valid high-similarity hit
            db.close();
        }
        QSqlDatabase::removeDatabase(edgeConn);

        // Querying with unitRef must cleanly ignore every malformed/zero/NaN row and return book 99 (~1.0).
        const auto edgeHits = PassageIndex::rankLibrary(edgeDb, QStringLiteral("m"), unitRef, 10, 0.25);
        QCOMPARE(edgeHits.size(), 1);
        QCOMPARE(edgeHits[0].bookId, 99);
        QVERIFY(std::isfinite(edgeHits[0].score));
        QVERIFY(std::abs(edgeHits[0].score - 1.0) < 1e-5);

        // Querying with zero / near-zero / NaN / Inf query vectors must return empty cleanly.
        QVERIFY(PassageIndex::rankLibrary(edgeDb, QStringLiteral("m"), zeroVec, 10, -1.0).isEmpty());
        QVERIFY(PassageIndex::rankLibrary(edgeDb, QStringLiteral("m"), nearZeroVec, 10, -1.0).isEmpty());
        QVERIFY(PassageIndex::rankLibrary(edgeDb, QStringLiteral("m"), nanVec, 10, -1.0).isEmpty());
        QVERIFY(PassageIndex::rankLibrary(edgeDb, QStringLiteral("m"), infVec, 10, -1.0).isEmpty());

        // 3. Multi-block dotInt8_16 overflow guard (dim = 40,000 > 32,768) under UBSan/ASan.
        constexpr int kHugeDim = 40000;
        const QList<float> hugeVec = randomUniformUnit(rng, kHugeDim);
        const QString hugeDb = dir.filePath(QStringLiteral("huge_dim.sqlite"));
        const QString hugeConn = QStringLiteral("w8-huge-dim");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), hugeConn);
            db.setDatabaseName(hugeDb);
            QVERIFY(db.open());
            PassageIndex init(nullptr, hugeConn);
            Q_UNUSED(init);
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (1, 'm', '0:0:10:h1', ?)"));
            ins.addBindValue(PassageIndex::quantize(hugeVec));
            QVERIFY(ins.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(hugeConn);
        const auto hugeHits = PassageIndex::rankLibrary(hugeDb, QStringLiteral("m"), hugeVec, 5, 0.25);
        QCOMPARE(hugeHits.size(), 1);
        QVERIFY(std::abs(hugeHits[0].score - 1.0) < 1e-5);
    }
};

QTEST_GUILESS_MAIN(W8PassageQuantTests)
#include "tst_w8_passage_quant.moc"
