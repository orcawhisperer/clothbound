#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickImageResponse>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#ifndef CLOTHBOUND_TSAN_ACTIVE
#if defined(__linux__)
#if defined(__SANITIZE_THREAD__)
#define CLOTHBOUND_TSAN_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CLOTHBOUND_TSAN_ACTIVE 1
#endif
#endif
#endif
#endif

#ifdef CLOTHBOUND_TSAN_ACTIVE
#include <errno.h>
#include <pthread.h>
#include <time.h>

// Qt 6.12's QThread::wait() calls glibc's pthread_clockjoin_np(), which neither
// GCC nor Clang's libtsan intercepts yet (only pthread_join / pthread_tryjoin_np /
// pthread_timedjoin_np are intercepted). Without forwarding to the intercepted join
// primitives, TSan never sees QThread joins, causing spurious CHECK failures in
// sanitizer_thread_registry.cpp:186 when glibc recycles a joined pthread_t address.
extern "C" __attribute__((visibility("default"))) int pthread_clockjoin_np(pthread_t thread, void **retval,
                                                                           clockid_t clockid,
                                                                           const struct timespec *abstime) {
    if (!abstime)
        return pthread_join(thread, retval);
    if (clockid == CLOCK_REALTIME)
        return pthread_timedjoin_np(thread, retval, abstime);

    struct timespec nowClk {};
    if (clock_gettime(clockid, &nowClk) != 0)
        return pthread_join(thread, retval);

    long long diffSec = static_cast<long long>(abstime->tv_sec) - static_cast<long long>(nowClk.tv_sec);
    long long diffNsec = static_cast<long long>(abstime->tv_nsec) - static_cast<long long>(nowClk.tv_nsec);
    if (diffNsec < 0) {
        --diffSec;
        diffNsec += 1000000000LL;
    }
    if (diffSec < 0 || (diffSec == 0 && diffNsec == 0))
        return pthread_tryjoin_np(thread, retval);
    if (diffSec > 3600)
        return pthread_join(thread, retval);

    struct timespec nowRt {};
    if (clock_gettime(CLOCK_REALTIME, &nowRt) != 0)
        return pthread_join(thread, retval);

    struct timespec rtDeadline {};
    rtDeadline.tv_sec = static_cast<time_t>(static_cast<long long>(nowRt.tv_sec) + diffSec);
    long long totalNsec = static_cast<long long>(nowRt.tv_nsec) + diffNsec;
    if (totalNsec >= 1000000000LL) {
        ++rtDeadline.tv_sec;
        totalNsec -= 1000000000LL;
    }
    rtDeadline.tv_nsec = static_cast<long>(totalNsec);
    return pthread_timedjoin_np(thread, retval, &rtDeadline);
}
#endif

#include "audio/audiocache.h"
#include "audio/speechplayer.h"
#include "fakeserver.h"
#include "formats/bookscheme.h"
#include "formats/coverprovider.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "library/libraryindex.h"
#include "online/urlguard.h"
#include "reader/readerimageprovider.h"
#include "testbooks.h"

namespace {

QByteArray makeSinePcm(int sampleRate, int samples, double freqHz = 440.0) {
    QByteArray pcm(samples * 2, Qt::Uninitialized);
    auto *dst = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < samples; ++i) {
        const double v = std::sin(2.0 * 3.14159265358979323846 * freqHz * double(i) / double(sampleRate));
        dst[i] = qint16(v * 12000.0);
    }
    return pcm;
}

QByteArray makePngBytes(int w, int h, const QColor &fill) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(fill);
    // Draw a contrasting stripe so recoloring/contentBox/scaling have real features.
    for (int y = h / 4; y < (h * 3) / 4; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = w / 4; x < (w * 3) / 4; ++x)
            line[x] = qRgba(20, 20, 20, 255);
    }
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

QByteArray makeRichEpub(const QString &titlePrefix, const QByteArray &png1, const QByteArray &png2) {
    const QByteArray pfx = titlePrefix.toUtf8();
    const QByteArray opf =
        QByteArray("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                   "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                   "    <dc:identifier id=\"uid\">urn:uuid:tsan-stress-") +
        pfx +
        QByteArray("</dc:identifier>\n"
                   "    <dc:title>") +
        pfx +
        QByteArray("</dc:title>\n"
                   "    <dc:creator>Stress Author</dc:creator>\n"
                   "    <dc:language>en</dc:language>\n"
                   "  </metadata>\n"
                   "  <manifest>\n"
                   "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                   "    <item id=\"ch1\" href=\"text/ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                   "    <item id=\"ch2\" href=\"text/ch2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                   "    <item id=\"img1\" href=\"images/plate1.png\" media-type=\"image/png\"/>\n"
                   "    <item id=\"img2\" href=\"images/plate2.png\" media-type=\"image/png\"/>\n"
                   "    <item id=\"svg1\" href=\"images/diagram.svg\" media-type=\"image/svg+xml\"/>\n"
                   "  </manifest>\n"
                   "  <spine><itemref idref=\"ch1\"/><itemref idref=\"ch2\"/></spine>\n"
                   "</package>");

    const QByteArray nav =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
        "<body><nav epub:type=\"toc\"><ol>\n"
        "  <li><a href=\"text/ch1.xhtml\">First</a></li>\n"
        "  <li><a href=\"text/ch2.xhtml\">Second</a></li>\n"
        "</ol></nav></body></html>";

    const QByteArray ch1 = TestBooks::chapter(
        "First",
        QByteArray("<h1>") + pfx +
            QByteArray(" Harbor</h1>\n<p>Lantern keepers watch the northern channel across twilight tides.</p>\n"
                       "<p>Starlight reflections dance upon granite piers and distant lighthouses.</p>"));
    const QByteArray ch2 = TestBooks::chapter(
        "Second",
        "<p>Almanac entries record barometric pressure, ocean currents, and coastal fog signals.</p>");
    const QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"128\" height=\"128\" viewBox=\"0 0 128 128\">\n"
        "<rect x=\"8\" y=\"8\" width=\"112\" height=\"112\" fill=\"#3e5f99\"/>"
        "<circle cx=\"64\" cy=\"64\" r=\"36\" fill=\"#f6f1e7\"/></svg>";

    return TestBooks::epub({
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/nav.xhtml"), nav},
        {QStringLiteral("OEBPS/text/ch1.xhtml"), ch1},
        {QStringLiteral("OEBPS/text/ch2.xhtml"), ch2},
        {QStringLiteral("OEBPS/images/plate1.png"), png1},
        {QStringLiteral("OEBPS/images/plate2.png"), png2},
        {QStringLiteral("OEBPS/images/diagram.svg"), svg},
    });
}

}  // namespace

class ConcurrencyTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qputenv("CLOTHBOUND_MUTE", "1");
        QVERIFY(m_dir.isValid());
    }

    // 1. SpeechPlayer: 4-thread stress hammering appendAudio, setRate, setVolume,
    //    play/pause, clear, and live WSOLA worker thread simultaneously.
    void stressSpeechPlayer() {
        SpeechPlayer player(24000);
        const QByteArray chunkEven = makeSinePcm(24000, 1200, 330.0);
        const QByteArray chunkOdd = chunkEven.left(chunkEven.size() - 1);  // odd byte count stresses trailingByte

        std::atomic_bool stop{false};
        std::atomic<int> tracksAdded{0};
        std::atomic<int> rateChanges{0};

        // Thread 1: producer adding tracks, streaming odd/even PCM chunks, and finishing tracks.
        std::thread producer([&] {
            int nextId = 1;
            for (int iter = 0; iter < 80 && !stop.load(std::memory_order_relaxed); ++iter) {
                const int id = nextId++;
                player.addTrack(id, (iter % 3) * 128);
                tracksAdded.fetch_add(1, std::memory_order_relaxed);
                for (int b = 0; b < 5; ++b) {
                    player.appendAudio(id, (b & 1) ? chunkOdd : chunkEven);
                    std::this_thread::yield();
                }
                player.appendAudio(id, QByteArray(1, '\0'));  // flush trailing byte
                player.finishTrack(id);
            }
        });

        // Thread 2: rate, volume, and mute mutator (triggers WSOLA reseed/rewind).
        std::thread rateVol([&] {
            constexpr double rates[] = {0.5, 0.85, 1.0, 1.25, 1.5, 2.0, 2.5, 1.0, 1.75};
            for (int i = 0; i < 160 && !stop.load(std::memory_order_relaxed); ++i) {
                player.setRate(rates[i % int(std::size(rates))]);
                rateChanges.fetch_add(1, std::memory_order_relaxed);
                player.setVolume(double(i % 11) / 10.0);
                player.setMuted((i & 3) == 0);
                Q_UNUSED(player.volume());
                std::this_thread::yield();
            }
        });

        // Thread 3: transport controller toggling play/pause and reading state mirrors.
        // Calling play() from a worker thread exercises m_worker (WSOLA) deterministically
        // without depending on headless audio daemon availability.
        std::thread transport([&] {
            for (int i = 0; i < 160 && !stop.load(std::memory_order_relaxed); ++i) {
                if ((i % 4) != 3)
                    player.play();
                else
                    player.pause();
                Q_UNUSED(player.isPlaying());
                Q_UNUSED(player.starved());
                Q_UNUSED(player.drainedOut());
                Q_UNUSED(player.hasTrack((i % 10) + 1));
                Q_UNUSED(player.trackSamples((i % 10) + 1));
                Q_UNUSED(player.trackComplete((i % 10) + 1));
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        });

        // Thread 4: periodic clear() + fresh track injection to stress generation resets.
        std::thread resetter([&] {
            for (int i = 0; i < 24 && !stop.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                player.clear();
                const int id = 10000 + i;
                player.addTrack(id);
                player.appendAudio(id, chunkEven);
                player.appendAudio(id, chunkEven);
                player.finishTrack(id);
                player.play();
            }
        });

        producer.join();
        rateVol.join();
        transport.join();
        resetter.join();
        stop.store(true, std::memory_order_relaxed);

        // Drain queued signals and verify clean final state.
        player.pause();
        player.clear();
        QCoreApplication::processEvents();
        QVERIFY(tracksAdded.load() == 80);
        QVERIFY(rateChanges.load() == 160);
        QVERIFY(!player.isPlaying());
        QVERIFY(player.drainedOut());
    }

    // 2. AudioCache: concurrent store() + lookup() + clear() + evict() + decode() stress
    //    across multiple thread-affine AudioCache instances (plus background QtConcurrent
    //    trash removal, ffmpeg QProcess encoders/decodes, and static keyFor() hammering).
    void stressAudioCache() {
        const QByteArray pcm = makeSinePcm(24000, 6000, 520.0);  // 250 ms mono PCM

        // Hammer thread-safe AudioCache::keyFor() across 4 threads simultaneously.
        {
            std::vector<std::thread> keyThreads;
            std::atomic<int> hashed{0};
            for (int t = 0; t < 4; ++t) {
                keyThreads.emplace_back([&, t] {
                    for (int i = 0; i < 250; ++i) {
                        const QString k = AudioCache::keyFor(QStringLiteral("builtin"), QStringLiteral("m"),
                                                             QStringLiteral("v%1").arg(t), QString(),
                                                             QStringLiteral("Sentence %1").arg(i));
                        if (k.size() == 40)
                            hashed.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (auto &th : keyThreads)
                th.join();
            QCOMPARE(hashed.load(), 1000);
        }

        // Run 3 concurrent thread-affine AudioCache workers (each with its own QSqlDatabase
        // connection and tight capBytes to force continuous eviction + overlapping clear()
        // QtConcurrent trash removal + concurrent ffmpeg encode/decode QProcesses).
        std::atomic<int> completedWorkers{0};
        std::atomic<int> storesTriggered{0};
        std::atomic<int> clearsTriggered{0};

        std::vector<std::thread> threads;
        for (int w = 0; w < 3; ++w) {
            const QString base = m_dir.filePath(QStringLiteral("audiocache_%1").arg(w));
            QDir().mkpath(base);
            threads.emplace_back([&, w, base] {
                const QString connName = QStringLiteral("tsan-audiocache-%1-%2")
                                             .arg(w)
                                             .arg(QUuid::createUuid().toString(QUuid::Id128));
                {
                    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
                    db.setDatabaseName(base + QStringLiteral("/cache.sqlite"));
                    if (!db.open())
                        return;
                    {
                        QSqlQuery q(db);
                        q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
                        q.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
                    }

                    // Construct QEventLoop first so the adopted std::thread has an active
                    // Qt event dispatcher before AudioCache creates QTimer / QProcess children.
                    QEventLoop threadLoop;
                    // Tight 4 KiB cap triggers evict() on nearly every indexed clip.
                    const QString audioDir = base + QStringLiteral("/audio");
                    AudioCache cache(audioDir, connName, nullptr, 4096);

                    QStringList keys;
                    for (int i = 0; i < 10; ++i) {
                        keys.append(AudioCache::keyFor(QStringLiteral("builtin"), QStringLiteral("v1"),
                                                       QStringLiteral("voice%1").arg(w), QString(),
                                                       QStringLiteral("Clip %1-%2").arg(w).arg(i)));
                    }

                    for (int round = 0; round < 3; ++round) {
                        for (int i = 0; i < keys.size(); ++i) {
                            cache.store(keys[i], pcm, 24000, {1200, 3600, 6000});
                            storesTriggered.fetch_add(1, std::memory_order_relaxed);
                            Q_UNUSED(cache.isStoring(keys[i]));
                            Q_UNUSED(cache.lookup(keys[i]));
                            Q_UNUSED(cache.lookupPath(keys[i]));
                            cache.storeDirections(keys[i], QStringLiteral("{\"tone\":\"calm\"}"));
                            Q_UNUSED(cache.directions(keys[i]));
                            if ((i % 3) == 2)
                                threadLoop.processEvents(QEventLoop::AllEvents, 15);
                        }

                        // Let a couple of encodes complete so lookup/decode/evict exercise real files.
                        QElapsedTimer waitEncode;
                        waitEncode.start();
                        while (cache.hasPendingStores() && waitEncode.elapsed() < 800) {
                            threadLoop.processEvents(QEventLoop::AllEvents, 20);
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }

                        const auto batch = cache.clipInfoBatch(keys);
                        for (auto it = batch.cbegin(); it != batch.cend(); ++it) {
                            Q_UNUSED(cache.lookup(it.key()));
                            Q_UNUSED(cache.seconds(it.key()));
                            Q_UNUSED(cache.boundaries(it.key()));
                            // Ephemeral context destroyed immediately on odd entries to test cancellation.
                            auto ctx = std::make_unique<QObject>();
                            cache.decode(it.value().first, 24000, ctx.get(), [](const QByteArray &, bool) {});
                            if (round == 1)
                                ctx.reset();
                            else
                                threadLoop.processEvents(QEventLoop::AllEvents, 10);
                        }

                        cache.remove(keys.first());
                        cache.clear();
                        clearsTriggered.fetch_add(1, std::memory_order_relaxed);
                        // Immediately store while async trash removal is running.
                        cache.store(keys.last(), pcm, 24000);
                        threadLoop.processEvents(QEventLoop::AllEvents, 25);
                    }
                    // Destroy AudioCache while encodes/decodes/clear trash may still be active.
                }
                QSqlDatabase::removeDatabase(connName);
                completedWorkers.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto &t : threads)
            t.join();

        QCOMPARE(completedWorkers.load(), 3);
        QVERIFY(storesTriggered.load() >= 90);
        QCOMPARE(clearsTriggered.load(), 9);
    }

    // 3. LibraryIndex: concurrent update() + rebuild() + search() + passage() + cancel()
    //    + teardown stress against real EPUB and PDF books in SQLite WAL mode.
    void stressLibraryIndex() {
        const QByteArray png1 = makePngBytes(32, 32, QColor(0x20, 0x40, 0x80));
        const QByteArray png2 = makePngBytes(32, 32, QColor(0x80, 0x40, 0x20));

        QList<LibraryIndex::Source> books;
        for (int i = 1; i <= 4; ++i) {
            const QString path = m_dir.filePath(QStringLiteral("idx_book_%1.epub").arg(i));
            QVERIFY(TestBooks::write(path, makeRichEpub(QStringLiteral("Volume%1").arg(i), png1, png2)));
            books.append({i, path, QStringLiteral("epub"), 1000 + i});
        }
        const QString pdfPath = m_dir.filePath(QStringLiteral("idx_book_5.pdf"));
        QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Harbor Manual", "Ada Marsh")));
        books.append({5, pdfPath, QStringLiteral("pdf"), 1005});

        const QString dbPath = m_dir.filePath(QStringLiteral("library_stress.sqlite"));
        LibraryIndex index(dbPath);
        QVERIFY(index.available());

        // Start background reader/writer threads hitting static indexBook() & matchExpression()
        // against the same WAL SQLite database while main thread drives update/search/passage/cancel/rebuild.
        std::atomic_bool done{false};
        std::atomic_bool externalCancel{false};
        std::vector<std::thread> helpers;
        auto joinHelpers = qScopeGuard([&] {
            done.store(true, std::memory_order_relaxed);
            externalCancel.store(true, std::memory_order_relaxed);
            for (auto &t : helpers)
                if (t.joinable())
                    t.join();
        });

        helpers.emplace_back([&] {
            int iter = 0;
            while (!done.load(std::memory_order_relaxed) && iter < 200) {
                const QString expr = LibraryIndex::matchExpression(QStringLiteral("lantern harbor \"tide\" %1").arg(iter));
                Q_UNUSED(expr);
                ++iter;
            }
        });
        helpers.emplace_back([&] {
            for (int i = 0; i < 6 && !done.load(std::memory_order_relaxed); ++i) {
                externalCancel.store((i % 2) == 1, std::memory_order_relaxed);
                LibraryIndex::Source extra = books[i % books.size()];
                extra.id = 100 + (i % 2);
                extra.mtime = 2000 + i;
                LibraryIndex::indexBook(dbPath, extra, &externalCancel);
            }
        });

        // Main-thread churn: update -> search/passage -> cancel -> rebuild -> update subsets.
        for (int cycle = 0; cycle < 6; ++cycle) {
            QList<LibraryIndex::Source> batch = books;
            for (auto &b : batch)
                b.mtime += cycle;
            if ((cycle % 2) == 1)
                batch.removeLast();

            index.update(batch);
            for (int p = 0; p < 5; ++p) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                Q_UNUSED(index.search(QStringLiteral("lantern"), 5));
                Q_UNUSED(index.search(QStringLiteral("almanac"), 5, 1));
                Q_UNUSED(index.passage(1, 0, 0, 80));
                Q_UNUSED(index.bookCount());
                Q_UNUSED(index.megabytes());
                QThread::msleep(4);
            }
            if (cycle == 2)
                index.cancel();
            else if (cycle == 4)
                index.rebuild();
        }

        // Wait for current indexing batch to complete and confirm FTS search still works.
        index.update(books);
        QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 8000);
        const QVariantList hits = index.search(QStringLiteral("lantern"), 10);
        QVERIFY2(!hits.isEmpty(),
                 qPrintable(QStringLiteral("Expected FTS5 hits after churn: bookCount=%1 done=%2 total=%3")
                                .arg(index.bookCount())
                                .arg(index.done())
                                .arg(index.total())));
        QVERIFY(!index.passage(1, 0, 0, 60).isEmpty());

        done.store(true, std::memory_order_relaxed);
        externalCancel.store(true, std::memory_order_relaxed);
        for (auto &t : helpers)
            t.join();
        helpers.clear();

        // Teardown stress: destroy LibraryIndex immediately while background indexing is in flight.
        for (int i = 0; i < 4; ++i) {
            const QString tearDb = m_dir.filePath(QStringLiteral("library_tear_%1.sqlite").arg(i));
            {
                LibraryIndex transient(tearDb);
                transient.update(books);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                // ~LibraryIndex() cancels and drains m_pool synchronously.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 4. PdfPages: concurrent base page renders + high-zoom viewport tile renders
    //    + prefetch + cancel + remove(token) + teardown stress.
    void stressPdfPages() {
        const QString pathA = m_dir.filePath(QStringLiteral("pages_a.pdf"));
        const QString pathB = m_dir.filePath(QStringLiteral("pages_b.pdf"));
        QVERIFY(TestBooks::write(pathA, TestBooks::pdf("Book A", "Author A")));
        QVERIFY(TestBooks::write(pathB, TestBooks::pdf("Book B", "Author B")));

        auto bookA = std::make_shared<PdfBook>(pathA);
        auto bookB = std::make_shared<PdfBook>(pathB);
        QVERIFY(bookA->open());
        QVERIFY(bookB->open());

        const QString tokA = QStringLiteral("tsan-pdf-a");
        const QString tokB = QStringLiteral("tsan-pdf-b");
        auto cleanupTokens = qScopeGuard([&] {
            PdfPages::remove(tokA);
            PdfPages::remove(tokB);
        });

        PdfPages::add(tokA, bookA);
        PdfPages::add(tokB, bookB);

        // Keep all QQuickImageResponse objects owned by the thread that created them
        // until after ~PdfPages() drains its worker pool so run() never touches freed memory.
        struct TrackedPdfResp {
            std::unique_ptr<QQuickImageResponse> resp;
            bool stableToken = false;
            bool cancelled = false;
        };
        std::vector<TrackedPdfResp> mainResponses;
        {
            PdfPages pages;

            // Worker threads hammering static token add/remove, prefetch/cancelPrefetch,
            // and pure recolor/tile-geometry helpers concurrently with live PDFium renders.
            std::atomic_bool stopWorkers{false};
            std::thread tokenChurn([&] {
                for (int i = 0; i < 20 && !stopWorkers.load(std::memory_order_relaxed); ++i) {
                    const QString &tok = (i & 1) ? tokA : tokB;
                    PdfPages::prefetch(tok, {0}, QSize(320, 480), QStringLiteral("?bg=%231e1f24&fg=%23d6d2c9"));
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    if ((i % 4) == 2) {
                        PdfPages::remove(tok);
                        PdfPages::add(tok, (i & 1) ? bookA : bookB);
                    } else if ((i % 4) == 3) {
                        PdfPages::cancelPrefetch();
                    }
                }
            });

            std::thread pixelChurn([&] {
                for (int i = 0; i < 20; ++i) {
                    QImage img(64, 96, QImage::Format_ARGB32_Premultiplied);
                    img.fill(Qt::white);
                    PdfPages::compositeAndRecolor(img, QColor(QStringLiteral("#1e1f24")),
                                                  QColor(QStringLiteral("#d6d2c9")));
                    Q_UNUSED(PdfPages::contentBox(img));
                    QUrlQuery q(QStringLiteral("tx=0.1&ty=0.1&tw=0.4&th=0.4&fullw=4000"));
                    const auto spec = PdfPages::parseTileSpec(q);
                    Q_UNUSED(PdfPages::tileGeometryFor(QSizeF(200, 300), spec, QSize(1024, 1024)));
                }
            });

            // Issue interleaved base-page and high-zoom viewport tile requests on the main thread.
            for (int i = 0; i < 24; ++i) {
                const bool isA = (i & 1) != 0;
                const QString &tok = isA ? tokA : tokB;
                QString reqId;
                QSize reqSize;
                if ((i % 2) == 0) {
                    // Base page render across multiple widths/themes
                    const int w = 240 + (i % 4) * 60;
                    reqId = QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tok);
                    reqSize = QSize(w, 0);
                } else {
                    // High-zoom viewport tile render
                    const double tx = 0.05 * double(i % 5);
                    reqId = QStringLiteral("%1/0?bg=%23f6f1e7&fg=%2326221d&tx=%2&ty=0.1&tw=0.45&th=0.45&fullw=3600")
                                .arg(tok)
                                .arg(tx, 0, 'f', 2);
                    reqSize = QSize(512, 512);
                }
                std::unique_ptr<QQuickImageResponse> resp(pages.requestImageResponse(reqId, reqSize));
                QVERIFY(resp != nullptr);
                const bool cancelThis = ((i % 5) == 4);
                if (cancelThis)
                    resp->cancel();
                mainResponses.push_back({std::move(resp), !isA, cancelThis});
                if ((i % 4) == 0)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }

            // Trigger remove(token) while renders are still queued/active, then tear down PdfPages.
            PdfPages::remove(tokA);
            stopWorkers.store(true, std::memory_order_relaxed);
            tokenChurn.join();
            pixelChurn.join();
            // ~PdfPages() drains m_pool synchronously before mainResponses are destroyed!
        }

        QCoreApplication::processEvents();
        int renderedCount = 0;
        for (const auto &item : mainResponses) {
            std::unique_ptr<QQuickTextureFactory> tf(item.resp->textureFactory());
            if (tf && !tf->image().isNull())
                ++renderedCount;
        }
        QVERIFY(renderedCount > 0);
        mainResponses.clear();
    }

    // 5. BookScheme: concurrent resolveRequest() / resolveRequestAsync() + add()/remove()
    //    + context-destruction cancellation + teardown stress.
    void stressBookScheme() {
        const QByteArray png1 = makePngBytes(24, 24, QColor(0x33, 0x66, 0x99));
        const QByteArray png2 = makePngBytes(24, 24, QColor(0x99, 0x33, 0x66));
        const QString epubPath1 = m_dir.filePath(QStringLiteral("scheme1.epub"));
        const QString epubPath2 = m_dir.filePath(QStringLiteral("scheme2.epub"));
        QVERIFY(TestBooks::write(epubPath1, makeRichEpub(QStringLiteral("Scheme1"), png1, png2)));
        QVERIFY(TestBooks::write(epubPath2, makeRichEpub(QStringLiteral("Scheme2"), png2, png1)));

        auto book1 = std::make_shared<EpubBook>(epubPath1);
        auto book2 = std::make_shared<EpubBook>(epubPath2);
        QVERIFY(book1->open());
        QVERIFY(book2->open());

        BookScheme scheme;
        scheme.requestStarted(nullptr);  // null-job guard

        const QString stableTok = scheme.add(book1);
        std::atomic<int> asyncCallbacks{0};
        std::atomic<int> syncHits{0};

        // 4 threads concurrently hammering add/remove, sync resolveRequest, async resolveRequestAsync,
        // and BookScheme::isWellFormed.
        std::atomic<int> runningThreads{4};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                auto dec = qScopeGuard([&] { runningThreads.fetch_sub(1, std::memory_order_release); });
                for (int i = 0; i < 40; ++i) {
                    const QString ephemeralTok = scheme.add((i & 1) ? book1 : book2);
                    const QUrl chUrl(BookScheme::baseUrl(ephemeralTok) + QStringLiteral("OEBPS/text/ch1.xhtml"));
                    const QUrl imgUrl(BookScheme::baseUrl(stableTok) + QStringLiteral("OEBPS/images/plate1.png"));
                    const QUrl relUrl(QStringLiteral("clothbound://local/OEBPS/text/ch2.xhtml"));
                    const QByteArray ref =
                        (BookScheme::baseUrl(stableTok) + QStringLiteral("OEBPS/text/ch1.xhtml")).toUtf8();

                    const auto r1 = scheme.resolveRequest(chUrl);
                    const auto r2 = scheme.resolveRequest(imgUrl);
                    const auto r3 = scheme.resolveRequest(relUrl, ref);
                    const auto rBad = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://evil/book/x")));
                    if (r1.ok && r2.ok && r3.ok && rBad.invalidUrl)
                        syncHits.fetch_add(1, std::memory_order_relaxed);

                    Q_UNUSED(BookScheme::isWellFormed(r1.data));

                    // Async request with ephemeral context destroyed immediately on odd iterations
                    // to race QObject::destroyed cancellation against worker completion!
                    auto ctx = std::make_unique<QObject>();
                    scheme.resolveRequestAsync(
                        imgUrl, ref, ctx.get(),
                        [&](BookScheme::ResolvedResource) { asyncCallbacks.fetch_add(1, std::memory_order_relaxed); });
                    if ((i + t) & 1)
                        ctx.reset();

                    // Unregister ephemeral book while async work may be active.
                    scheme.remove(ephemeralTok);
                    if (ctx) {
                        std::this_thread::yield();
                        ctx.reset();
                    }
                }
            });
        }

        // Pump main-thread event loop while worker threads run so queued async callbacks deliver live.
        while (runningThreads.load(std::memory_order_acquire) > 0) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (auto &th : threads)
            th.join();
        QCoreApplication::processEvents();
        QVERIFY(syncHits.load() > 0);

        // Teardown stress: fire async requests then immediately destroy BookScheme mid-flight.
        for (int round = 0; round < 4; ++round) {
            {
                BookScheme transient;
                const QString tok = transient.add(book1);
                const QUrl url(BookScheme::baseUrl(tok) + QStringLiteral("OEBPS/text/ch1.xhtml"));
                for (int k = 0; k < 20; ++k) {
                    transient.resolveRequestAsync(url, {}, nullptr, [](BookScheme::ResolvedResource) {});
                }
                transient.remove(tok);
                // ~BookScheme() sets m_shuttingDown, clears pool, and joins waitForDone().
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        QCoreApplication::processEvents();
        Q_UNUSED(asyncCallbacks.load());
    }

    // 6. CoverProvider & ReaderImageProvider: concurrent image requests, cache eviction,
    //    mid-flight cancel(), and remove(token) / teardown stress.
    void stressCoverAndReaderImageProviders() {
        // --- Part A: CoverProvider ---
        const QString coversDir = m_dir.filePath(QStringLiteral("covers_stress"));
        const QString pruneDir = m_dir.filePath(QStringLiteral("covers_prune"));
        QDir().mkpath(coversDir);
        QDir().mkpath(pruneDir);

        QStringList coverPaths;
        for (int i = 0; i < 8; ++i) {
            const QString p = coversDir + QStringLiteral("/cover_%1.png").arg(i);
            QVERIFY(TestBooks::write(p, makePngBytes(128, 192, QColor(30 + i * 25, 90, 180 - i * 15))));
            coverPaths.append(p);
        }
        for (int i = 0; i < 24; ++i) {
            const QString p = pruneDir + QStringLiteral("/old_%1.png").arg(i);
            QVERIFY(TestBooks::write(p, makePngBytes(32, 32, QColor(100, 100, 100))));
        }

        {
            CoverProvider coverProvider;
            struct TrackedCoverResp {
                std::unique_ptr<QQuickImageResponse> resp;
                bool cancelled = false;
            };
            std::vector<TrackedCoverResp> coverResponses;

            std::vector<std::thread> renderThreads;
            for (int t = 0; t < 3; ++t) {
                renderThreads.emplace_back([&, t] {
                    for (int i = 0; i < 16; ++i) {
                        const QString &p = coverPaths[(i + t) % coverPaths.size()];
                        const QImage img =
                            CoverProvider::renderCoverImage(p, QSize(120 + (i % 4) * 20, 180), 5.0, 3.0, 120.0);
                        Q_UNUSED(img);
                    }
                    CoverProvider::pruneCoverCacheDirectory(pruneDir, 2048, 4);
                });
            }

            int expectedValidCovers = 0;
            for (int i = 0; i < 32; ++i) {
                const QString &p = coverPaths[i % coverPaths.size()];
                const QString id = QString::fromUtf8(QUrl::toPercentEncoding(p, "/")) +
                                   QStringLiteral("?r=%1&sr=3&bw=140").arg((i % 2) ? 5 : 0);
                std::unique_ptr<QQuickImageResponse> resp(
                    coverProvider.requestImageResponse(id, QSize(120 + (i % 5) * 16, 180)));
                QVERIFY(resp != nullptr);
                const bool cancelThis = ((i % 6) == 5);
                if (cancelThis)
                    resp->cancel();
                else
                    ++expectedValidCovers;
                coverResponses.push_back({std::move(resp), cancelThis});
            }

            for (auto &th : renderThreads)
                th.join();
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();

            int validCovers = 0;
            for (const auto &item : coverResponses) {
                std::unique_ptr<QQuickTextureFactory> tf(item.resp->textureFactory());
                if (!item.cancelled) {
                    QVERIFY(tf != nullptr);
                    QVERIFY(!tf->image().isNull());
                    ++validCovers;
                }
            }
            QCOMPARE(validCovers, expectedValidCovers);
            coverResponses.clear();
        }

        // --- Part B: ReaderImageProvider ---
        const QByteArray imgLarge1 = makePngBytes(256, 256, QColor(0x2a, 0x6f, 0x97));
        const QByteArray imgLarge2 = makePngBytes(256, 256, QColor(0x97, 0x4b, 0x2a));
        const QString epubA = m_dir.filePath(QStringLiteral("readerimg_a.epub"));
        const QString epubB = m_dir.filePath(QStringLiteral("readerimg_b.epub"));
        QVERIFY(TestBooks::write(epubA, makeRichEpub(QStringLiteral("ImgA"), imgLarge1, imgLarge2)));
        QVERIFY(TestBooks::write(epubB, makeRichEpub(QStringLiteral("ImgB"), imgLarge2, imgLarge1)));

        auto bookA = std::make_shared<EpubBook>(epubA);
        auto bookB = std::make_shared<EpubBook>(epubB);
        QVERIFY(bookA->open());
        QVERIFY(bookB->open());

        const QString tokA = QStringLiteral("tsan-img-a");
        const QString tokB = QStringLiteral("tsan-img-b");
        auto cleanupImgTokens = qScopeGuard([&] {
            ReaderImageProvider::remove(tokA);
            ReaderImageProvider::remove(tokB);
        });

        ReaderImageProvider::add(tokA, bookA);
        ReaderImageProvider::add(tokB, bookB);

        struct TrackedImgResp {
            std::unique_ptr<QQuickImageResponse> resp;
            bool missing = false;
            bool cancelled = false;
        };
        std::vector<TrackedImgResp> imgResponses;
        {
            ReaderImageProvider imgProvider;
            QVERIFY(ReaderImageProvider::isRegistered());
            std::atomic_bool stopChurn{false};

            std::thread tokenAndDecodeChurn([&] {
                for (int i = 0; i < 24 && !stopChurn.load(std::memory_order_relaxed); ++i) {
                    const auto &bk = (i & 1) ? bookA : bookB;
                    const QString entry =
                        (i % 3 == 0) ? QStringLiteral("OEBPS/images/diagram.svg") : QStringLiteral("OEBPS/images/plate1.png");
                    const QImage decoded = ReaderImageProvider::decodeBookImage(bk, entry, QSize(256, 256));
                    Q_UNUSED(decoded);
                    if ((i % 5) == 4) {
                        const QString &tok = (i & 1) ? tokA : tokB;
                        ReaderImageProvider::remove(tok);
                        ReaderImageProvider::add(tok, bk);
                    }
                }
            });

            const QStringList entries{QStringLiteral("OEBPS/images/plate1.png"),
                                      QStringLiteral("OEBPS/images/plate2.png"),
                                      QStringLiteral("OEBPS/images/diagram.svg"),
                                      QStringLiteral("OEBPS/images/missing.png")};
            for (int i = 0; i < 32; ++i) {
                const QString &tok = (i & 1) ? tokA : tokB;
                const QString &entry = entries[i % entries.size()];
                const QString id = tok + QLatin1Char('/') + QString::fromUtf8(QUrl::toPercentEncoding(entry, "/"));
                std::unique_ptr<QQuickImageResponse> resp(
                    imgProvider.requestImageResponse(id, QSize(128 + (i % 4) * 64, 128 + (i % 4) * 64)));
                QVERIFY(resp != nullptr);
                const bool cancelThis = ((i % 5) == 3);
                if (cancelThis)
                    resp->cancel();
                imgResponses.push_back({std::move(resp), (i % entries.size()) == 3, cancelThis});
            }

            ReaderImageProvider::remove(tokA);
            stopChurn.store(true, std::memory_order_relaxed);
            tokenAndDecodeChurn.join();
            // ~ReaderImageProvider() calls m_pool.waitForDone() before imgResponses are destroyed.
        }
        QCoreApplication::processEvents();
        int decodedImages = 0;
        for (const auto &item : imgResponses) {
            std::unique_ptr<QQuickTextureFactory> tf(item.resp->textureFactory());
            if (item.missing)
                QVERIFY(tf == nullptr);
            else if (tf && !tf->image().isNull())
                ++decodedImages;
        }
        QVERIFY(decodedImages > 0);
        imgResponses.clear();
    }

    // 7. UrlGuard: concurrent parseHostLiteral() / SSRF checks + fetchPinned() / fetchPinnedSync()
    //    against a live local FakeServer running on its own thread.
    void stressUrlGuard() {
        auto resetHooks = qScopeGuard([] {
            UrlGuard::setHostResolverForTests(nullptr);
            UrlGuard::setConnectAddressOverrideForTests(nullptr);
        });

        // Host FakeServer on a dedicated QThread so client threads can connect concurrently
        // without blocking the server's accept/readyRead event loop.
        QMutex originMutex;
        QString serverOrigin;
        std::atomic_bool serverReady{false};
        std::atomic_bool serverStop{false};

        std::thread serverThread([&] {
            // Construct QEventLoop first so the adopted std::thread has an active
            // Qt event dispatcher before FakeServer's QTcpServer::listen() registers notifiers.
            QEventLoop loop;
            FakeServer server;
            server.routes.insert("/ok", {200, "text/plain", "clothbound-tsan-ok"});
            server.routes.insert("/payload", {200, "application/octet-stream", QByteArray(4096, 'Z')});
            {
                QMutexLocker lock(&originMutex);
                serverOrigin = server.origin();
            }
            serverReady.store(true, std::memory_order_release);

            QTimer poll;
            poll.setInterval(5);
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (serverStop.load(std::memory_order_acquire))
                    loop.quit();
            });
            poll.start();
            loop.exec();
        });
        auto joinServer = qScopeGuard([&] {
            serverStop.store(true, std::memory_order_release);
            if (serverThread.joinable())
                serverThread.join();
        });
        while (!serverReady.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        QString origin;
        {
            QMutexLocker lock(&originMutex);
            origin = serverOrigin;
        }
        QVERIFY(origin.startsWith(QLatin1String("http://127.0.0.1:")));

        const QStringList hostCorpus{
            QStringLiteral("127.0.0.1"),
            QStringLiteral("0177.0.0.1"),
            QStringLiteral("0x7f000001"),
            QStringLiteral("2130706433"),
            QStringLiteral("169.254.169.254"),
            QStringLiteral("10.0.0.1"),
            QStringLiteral("192.168.1.5"),
            QStringLiteral("[::1]"),
            QStringLiteral("::ffff:127.0.0.1"),
            QStringLiteral("64:ff9b::169.254.169.254"),
            QStringLiteral("fe80::1%eth0"),
            QStringLiteral("93.184.216.34"),
            QStringLiteral("2606:2800:220:1:248:1893:25c8:1946"),
            QStringLiteral("example.org"),
            QStringLiteral("localhost."),
            QStringLiteral("metadata.google.internal"),
        };

        std::atomic<int> okFetches{0};
        std::atomic<int> blockedFetches{0};
        std::atomic<int> asyncFetches{0};

        std::vector<std::thread> clients;
        for (int t = 0; t < 4; ++t) {
            clients.emplace_back([&, t] {
                // 1) Pure host-literal & SSRF classification stress.
                for (int i = 0; i < 60; ++i) {
                    const QString &h = hostCorpus[(i + t) % hostCorpus.size()];
                    const QHostAddress addr = UrlGuard::parseHostLiteral(h);
                    if (!addr.isNull())
                        Q_UNUSED(UrlGuard::isPrivateHostAddress(addr));
                    Q_UNUSED(UrlGuard::isLoopbackHost(h));
                    const QUrl u(QStringLiteral("http://%1/test").arg(h));
                    Q_UNUSED(UrlGuard::isAllowedUrl(u, false));
                    Q_UNUSED(UrlGuard::isLocalNetworkUrl(u));
                }

                // 2) Concurrent fetchPinnedSync() against local FakeServer: allowed + SSRF-blocked.
                for (int i = 0; i < 6; ++i) {
                    PinnedHttpOptions allowOpts;
                    allowOpts.url = QUrl(origin + ((i & 1) ? QStringLiteral("/payload") : QStringLiteral("/ok")));
                    allowOpts.allowLocalNetwork = true;
                    allowOpts.timeoutMs = 5000;
                    const PinnedHttpResponse resp = UrlGuard::fetchPinnedSync(allowOpts);
                    if (resp.ok && resp.httpStatus == 200)
                        okFetches.fetch_add(1, std::memory_order_relaxed);

                    PinnedHttpOptions blockOpts;
                    blockOpts.url = QUrl(origin + QStringLiteral("/ok"));
                    blockOpts.allowLocalNetwork = false;
                    blockOpts.timeoutMs = 5000;
                    const PinnedHttpResponse blocked = UrlGuard::fetchPinnedSync(blockOpts);
                    if (!blocked.ok && blocked.blockedByGuard)
                        blockedFetches.fetch_add(1, std::memory_order_relaxed);
                }

                // 3) Concurrent async fetchPinned() burst inside a thread-local QEventLoop
                //    (including one request aborted immediately mid-flight).
                QEventLoop loop;
                int pending = 3;
                QList<PinnedHttpRequest *> reqs;
                for (int k = 0; k < 3; ++k) {
                    PinnedHttpOptions opts;
                    opts.url = QUrl(origin + QStringLiteral("/ok"));
                    opts.allowLocalNetwork = true;
                    opts.timeoutMs = 5000;
                    reqs.append(UrlGuard::fetchPinned(opts, [&](PinnedHttpResponse resp) {
                        if (resp.ok && resp.body == "clothbound-tsan-ok")
                            asyncFetches.fetch_add(1, std::memory_order_relaxed);
                        if (--pending == 0)
                            loop.quit();
                    }));
                }
                // Extra request aborted mid-flight to exercise abort() teardown path.
                PinnedHttpOptions abortOpts;
                abortOpts.url = QUrl(origin + QStringLiteral("/payload"));
                abortOpts.allowLocalNetwork = true;
                abortOpts.timeoutMs = 5000;
                std::unique_ptr<PinnedHttpRequest> doomed(UrlGuard::fetchPinned(abortOpts, [](PinnedHttpResponse) {}));
                doomed->abort();
                doomed.reset();

                QTimer safety;
                safety.setSingleShot(true);
                QObject::connect(&safety, &QTimer::timeout, &loop, &QEventLoop::quit);
                safety.start(5000);
                if (pending > 0)
                    loop.exec();
                qDeleteAll(reqs);
            });
        }

        for (auto &c : clients)
            c.join();

        serverStop.store(true, std::memory_order_release);
        serverThread.join();

        QCOMPARE(okFetches.load(), 24);
        QCOMPARE(blockedFetches.load(), 24);
        QCOMPARE(asyncFetches.load(), 12);
    }

private:
    QTemporaryDir m_dir;
};

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clothbound_concurrency_tests"));
    ConcurrencyTests tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_concurrency.moc"
