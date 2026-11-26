#include "ai/ai.h"
#include "ai/aiservices.h"
#include "ai/booksearch.h"
#include "ai/booksession.h"
#include "ai/companion.h"
#include "ai/companionstore.h"
#include "ai/illustrator.h"
#include "ai/keystore.h"
#include "ai/passageindex.h"
#include "audio/audiocache.h"
#include "audio/cast.h"
#include "audio/download.h"
#include "audio/narration.h"
#include "audio/voicecatalog.h"
#include "audio/voicepacks.h"
#include "formats/bookscheme.h"
#include "library/library.h"
#include "library/marks.h"
#include "reader/reader.h"
#include "testbooks.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>

namespace {

QByteArray pngBytes(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::darkBlue);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

class FakeSpeechReply : public Ai::Reply {
    Q_OBJECT
public:
    explicit FakeSpeechReply(const QByteArray &pcm, int rate = 24000, QObject *parent = nullptr)
        : Ai::Reply(parent), m_pcm(pcm) {
        m_sampleRate = rate;
        QTimer::singleShot(0, this, [this] {
            if (m_aborted)
                return;
            m_audio = m_pcm;
            emit audioReceived(m_pcm);
            finish();
        });
    }
    void abort() override {
        m_aborted = true;
        finish({Ai::Error::Cancelled, QStringLiteral("Cancelled.")});
    }

private:
    QByteArray m_pcm;
    bool m_aborted = false;
};

class FakeSpeechProvider : public Ai::Provider, public Ai::SpeechSynthesizer {
    Q_OBJECT
public:
    explicit FakeSpeechProvider(QObject *parent = nullptr) : Ai::Provider(parent) {}
    QString id() const override { return QStringLiteral("fake"); }
    QString name() const override { return QStringLiteral("Fake"); }
    QString privacyNote() const override { return {}; }
    bool isLocal() const override { return true; }
    bool needsKey() const override { return false; }
    void setKey(const QString &) override {}
    bool hasKey() const override { return true; }
    Ai::Reply *check() override { return new FakeSpeechReply({}, 24000, this); }
    Ai::SpeechSynthesizer *speech() override { return this; }
    QString speechModelName(bool) const override { return QStringLiteral("fake-tts"); }
    bool supportsStyle() const override { return false; }
    Ai::Reply *listVoices(const QString &) override { return new FakeSpeechReply({}, 24000, this); }
    Ai::Reply *synthesize(const Ai::SpeechRequest &) override {
        // 0.25s of quiet 24kHz mono PCM16.
        QByteArray pcm(24000 / 4 * 2, '\0');
        return new FakeSpeechReply(pcm, 24000, this);
    }
};

QString sampleSpeakableJson() {
    const QJsonArray arr{
        QJsonObject{{QStringLiteral("start"), 0},
                    {QStringLiteral("end"), 44},
                    {QStringLiteral("kind"), QStringLiteral("paragraph")},
                    {QStringLiteral("text"), QStringLiteral("The lamp's light shone across the harbor.")}},
        QJsonObject{{QStringLiteral("start"), 45},
                    {QStringLiteral("end"), 88},
                    {QStringLiteral("kind"), QStringLiteral("paragraph")},
                    {QStringLiteral("text"), QStringLiteral("A storm gathered beyond the outer shoals.")}},
    };
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QByteArray twoChapterEpub(const QByteArray &title, const QByteArray &ch1Body, const QByteArray &ch2Body) {
    return TestBooks::epub({
        {"OEBPS/content.opf",
         "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
         "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
         "<dc:title>" + title + "</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en</dc:language>"
         "</metadata>"
         "<manifest>"
         "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
         "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
         "</manifest>"
         "<spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>"},
        {"OEBPS/c1.xhtml", TestBooks::chapter("Chapter One", ch1Body)},
        {"OEBPS/c2.xhtml", TestBooks::chapter("Chapter Two", ch2Body)},
    });
}

}  // namespace

class TestP0BookSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qputenv("CLOTHBOUND_MUTE", "1");
        qunsetenv("GEMINI_API_KEY");
    }

    // 1. Two BookSessions open Book A and Book B simultaneously with independent
    //    chapters, progress, marks, BookSearch, and a single shared PassageIndex.
    void twoSessionsOpenDistinctBooksSimultaneously() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString epubA = dir.filePath(QStringLiteral("bookA.epub"));
        const QString epubB = dir.filePath(QStringLiteral("bookB.epub"));
        const QString pdfC = dir.filePath(QStringLiteral("bookC.pdf"));
        QVERIFY(TestBooks::write(
            epubA, twoChapterEpub("Lantern Keeper",
                                  "<p>The brass lantern glowed in the harbor mist.</p>",
                                  "<p>Second chapter of the lantern story continues.</p>")));
        QVERIFY(TestBooks::write(
            epubB, twoChapterEpub("Salt Roads",
                                  "<p>Caravans crossed the dry salt flats at dawn.</p>",
                                  "<p>Second chapter of the salt roads expedition.</p>")));
        QVERIFY(TestBooks::write(pdfC, TestBooks::pdf("Optics Primer", "Clara Vance")));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookA = library.addBookFile(epubA);
        const int bookB = library.addBookFile(epubB);
        const int bookC = library.addBookFile(pdfC);
        QVERIFY(bookA > 0 && bookB > 0 && bookC > 0);
        QVERIFY(bookA != bookB && bookB != bookC);

        BookScheme scheme;
        FakeSpeechProvider provider;
        MemoryKeyStore keys;
        AiServices ai(&provider, &keys, library.database().connectionName());
        PassageIndex sharedIndex(&ai, library.database().connectionName());

        BookSession sessionA(&library, &scheme, &ai, &sharedIndex, dir.filePath(QStringLiteral("pics")));
        BookSession sessionB(&library, &scheme, &ai, &sharedIndex, dir.filePath(QStringLiteral("pics")));

        // Verify ALL per-session AI components share the ONE PassageIndex instance.
        QCOMPARE(sessionA.passageIndex(), &sharedIndex);
        QCOMPARE(sessionB.passageIndex(), &sharedIndex);
        QCOMPARE(sessionA.companion()->passageIndex(), &sharedIndex);
        QCOMPARE(sessionB.companion()->passageIndex(), &sharedIndex);
        QCOMPARE(sessionA.bookSearch()->passageIndex(), &sharedIndex);
        QCOMPARE(sessionB.bookSearch()->passageIndex(), &sharedIndex);

        QVERIFY(sessionA.openBook(bookA));
        QVERIFY(sessionB.openBook(bookB));
        QCOMPARE(sessionA.bookId(), bookA);
        QCOMPARE(sessionB.bookId(), bookB);
        QCOMPARE(sessionA.reader()->title(), QStringLiteral("Lantern Keeper"));
        QCOMPARE(sessionB.reader()->title(), QStringLiteral("Salt Roads"));
        QVERIFY(sessionA.reader()->manifest().value(QStringLiteral("base")).toString() !=
                sessionB.reader()->manifest().value(QStringLiteral("base")).toString());

        // Move sessionA to chapter 1 and add a highlight; sessionB stays at chapter 0 with 0 marks.
        sessionA.reader()->relocated({{QStringLiteral("chapter"), 1},
                                      {QStringLiteral("offset"), 12},
                                      {QStringLiteral("end"), 40},
                                      {QStringLiteral("page"), 0},
                                      {QStringLiteral("pages"), 1},
                                      {QStringLiteral("snippet"), QStringLiteral("Second chapter")}});
        sessionA.reader()->flushPosition();
        QCOMPARE(sessionA.reader()->chapter(), 1);
        QCOMPARE(sessionB.reader()->chapter(), 0);

        const int markA = sessionA.marks()->addHighlight(
            0, 4, 17, QStringLiteral("brass lantern"), QStringLiteral("yellow"), QStringLiteral("Ch. 1"),
            QStringLiteral("The"), QStringLiteral("glowed"));
        QVERIFY(markA > 0);
        QCOMPARE(sessionA.marks()->count(), 1);
        QCOMPARE(sessionB.marks()->count(), 0);

        const int markB = sessionB.marks()->addBookmark(0, 0, QStringLiteral("Caravans crossed"), QStringLiteral("Ch. 1"));
        QVERIFY(markB > 0);
        QCOMPARE(sessionA.marks()->count(), 1);
        QCOMPARE(sessionB.marks()->count(), 1);

        // Concurrent BookSearch in each session searches only its own book.
        sessionA.bookSearch()->find(QStringLiteral("lantern"));
        sessionB.bookSearch()->find(QStringLiteral("caravans"));
        QTRY_VERIFY_WITH_TIMEOUT(!sessionA.bookSearch()->searching() && !sessionB.bookSearch()->searching(), 5000);
        QVERIFY(sessionA.bookSearch()->count() >= 1);
        QVERIFY(sessionB.bookSearch()->count() >= 1);

        // Opening the same PDF simultaneously in two sessions yields distinct collision-free tokens.
        BookSession pdf1(&library, &scheme);
        BookSession pdf2(&library, &scheme);
        QVERIFY(pdf1.openBook(bookC));
        QVERIFY(pdf2.openBook(bookC));
        QVERIFY(!pdf1.reader()->pdfSource().isEmpty());
        QVERIFY(pdf1.reader()->pdfSource() != pdf2.reader()->pdfSource());
        pdf1.close();
        QVERIFY(pdf2.isOpen());
        QCOMPARE(pdf2.reader()->pdfPageText(0).contains(QStringLiteral("Hello reader")), true);
    }

    // 2. Book A stays actively narrating (and downloading / cast-bound) while
    //    Book B opens, turns pages, and closes — proving narration.cpp:117 regression is gone.
    void bookAStaysNarratingWhileBookBOpensTurnsPagesAndCloses() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString epubA = dir.filePath(QStringLiteral("narrA.epub"));
        const QString epubB = dir.filePath(QStringLiteral("narrB.epub"));
        QVERIFY(TestBooks::write(
            epubA, twoChapterEpub("Book Alpha",
                                  "<p>The lamp's light shone across the harbor. A storm gathered beyond the outer shoals.</p>",
                                  "<p>Morning returned calmly to the lighthouse.</p>")));
        QVERIFY(TestBooks::write(
            epubB, twoChapterEpub("Book Beta",
                                  "<p>Another book read simultaneously in a second tab.</p>",
                                  "<p>Turning pages in Book Beta must not disturb Book Alpha.</p>")));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookA = library.addBookFile(epubA);
        const int bookB = library.addBookFile(epubB);
        QVERIFY(bookA > 0 && bookB > 0);

        BookScheme scheme;
        FakeSpeechProvider provider;
        MemoryKeyStore keys;
        AiServices ai(&provider, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        VoicePacks packs(dir.filePath(QStringLiteral("voices")));

        BookSession sessionA(&library, &scheme, &ai);
        QVERIFY(sessionA.openBook(bookA));

        VoiceCatalog catalog(&ai, &cache, sessionA.reader(), library.database().connectionName(),
                             dir.filePath(QStringLiteral("voices.json")));
        Cast cast(&ai, &cache, sessionA.reader(), &library, &catalog, &packs);
        Narration narration(&ai, &cache, nullptr, &library);
        narration.setCast(&cast);
        Download download(&ai, &cache, nullptr, &narration);
        download.setCast(&cast);

        // Bind narration and download to Session A and start playback on Book A.
        narration.bindReader(sessionA.reader());
        narration.setVoice(QStringLiteral("sulafat"), QStringLiteral("Sulafat"));
        QCOMPARE(narration.boundReader(), sessionA.reader());
        QCOMPARE(narration.boundBookId(), bookA);

        QSignalSpy blocksSpy(&narration, &Narration::blocksWanted);
        narration.startFor(sessionA.reader(), 0, 0);
        QVERIFY(narration.active());
        QVERIFY(narration.playing());
        QCOMPARE(narration.boundBookId(), bookA);
        QCOMPARE(blocksSpy.count(), 1);

        sessionA.reader()->speakableReady(0, sampleSpeakableJson());
        QCoreApplication::processEvents();
        QVERIFY(narration.active());
        QVERIFY(narration.playing());

        // Start a download scoped to Session A as well.
        download.startFor(sessionA.reader(), 0, 0);
        QVERIFY(download.active());
        QCOMPARE(download.bookId(), bookA);

        // Open, read, turn pages, and close Book B in a second session!
        {
            BookSession sessionB(&library, &scheme, &ai);
            QVERIFY(sessionB.openBook(bookB));
            sessionB.reader()->relocated({{QStringLiteral("chapter"), 1},
                                          {QStringLiteral("offset"), 10},
                                          {QStringLiteral("end"), 30},
                                          {QStringLiteral("page"), 0},
                                          {QStringLiteral("pages"), 1}});
            sessionB.reader()->flushPosition();

            // Unbinding a non-bound reader MUST NOT disturb Book A's narration!
            narration.unbindReader(sessionB.reader());

            QVERIFY(narration.active());
            QVERIFY(narration.playing());
            QCOMPARE(narration.boundReader(), sessionA.reader());
            QCOMPARE(narration.boundBookId(), bookA);
            QVERIFY(download.active());
            QCOMPARE(download.bookId(), bookA);
            QCOMPARE(cast.bookId(), bookA);

            sessionB.close();
            QVERIFY(narration.active());
            QVERIFY(narration.playing());
            QCOMPARE(narration.boundBookId(), bookA);
            QVERIFY(download.active());
        }

        // Session B is now completely destroyed; Book A is STILL actively narrating and downloading!
        QCoreApplication::processEvents();
        QVERIFY(narration.active());
        QVERIFY(narration.playing());
        QCOMPARE(narration.boundReader(), sessionA.reader());
        QCOMPARE(narration.boundBookId(), bookA);
        QVERIFY(download.active());
        QCOMPARE(download.bookId(), bookA);

        // Explicitly transitioning narration to Session C stops Book A and binds to Book B cleanly.
        BookSession sessionC(&library, &scheme, &ai);
        QVERIFY(sessionC.openBook(bookB));
        narration.startFor(sessionC.reader(), 0, 0);
        QVERIFY(narration.active());
        QCOMPARE(narration.boundReader(), sessionC.reader());
        QCOMPARE(narration.boundBookId(), bookB);
        QCOMPARE(cast.reader(), sessionC.reader());
        QCOMPARE(cast.bookId(), bookB);

        // Closing the bound Session C stops narration cleanly.
        sessionC.close();
        QVERIFY(!narration.active());
        QCOMPARE(narration.boundBookId(), 0);
        download.cancel();
    }

    // 3. Opening the SAME Book A in two BookSessions propagates marksChanged and
    //    bookPositionChanged cleanly without infinite feedback loops, and verifies
    //    EPUB + PDF quote+context anchoring.
    void sameBookInTwoSessionsPropagatesMarksAndPositionWithoutLoops() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString epubA = dir.filePath(QStringLiteral("shared.epub"));
        QVERIFY(TestBooks::write(
            epubA, twoChapterEpub("Shared Volume",
                                  "<p>First chapter text with a memorable passage to highlight.</p>",
                                  "<p>Second chapter text for cross-window reading position sync.</p>")));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookA = library.addBookFile(epubA);
        QVERIFY(bookA > 0);

        BookScheme scheme;
        BookSession s1(&library, &scheme);
        BookSession s2(&library, &scheme);
        QVERIFY(s1.openBook(bookA));
        QVERIFY(s2.openBook(bookA));

        QSignalSpy libMarksSpy(&library, &Library::marksChanged);
        QSignalSpy libPosSpy(&library, &Library::bookPositionChanged);
        QSignalSpy m1Spy(s1.marks(), &Marks::changed);
        QSignalSpy m2Spy(s2.marks(), &Marks::changed);
        QSignalSpy r1SyncSpy(s1.reader(), &Reader::externalPositionSynced);
        QSignalSpy r2SyncSpy(s2.reader(), &Reader::externalPositionSynced);

        // Add highlight in s1 -> propagates ONCE to s2 with zero echo back to s1.
        const int hid = s1.marks()->addHighlight(
            0, 28, 45, QStringLiteral("memorable passage"), QStringLiteral("yellow"), QStringLiteral("Ch. 1"),
            QStringLiteral("with a"), QStringLiteral("to highlight"));
        QVERIFY(hid > 0);
        QCOMPARE(libMarksSpy.count(), 1);
        QCOMPARE(libMarksSpy.first().at(0).toInt(), bookA);
        QCOMPARE(libMarksSpy.first().at(1).value<QObject *>(), static_cast<QObject *>(&s1));
        QCOMPARE(m1Spy.count(), 1);
        QCOMPARE(m2Spy.count(), 1);
        QCOMPARE(s2.marks()->count(), 1);
        QCOMPARE(s2.marks()->mark(hid).value(QStringLiteral("text")).toString(), QStringLiteral("memorable passage"));

        // Edit note in s2 -> propagates ONCE back to s1 with zero loop.
        s2.marks()->setNote(hid, QStringLiteral("Cross-tab note"));
        QCOMPARE(libMarksSpy.count(), 2);
        QCOMPARE(libMarksSpy.last().at(1).value<QObject *>(), static_cast<QObject *>(&s2));
        QCOMPARE(m1Spy.count(), 2);
        QCOMPARE(m2Spy.count(), 2);
        QCOMPARE(s1.marks()->mark(hid).value(QStringLiteral("note")).toString(), QStringLiteral("Cross-tab note"));

        // Delete in s1 -> s2 drops it cleanly.
        s1.marks()->remove(hid);
        QCOMPARE(libMarksSpy.count(), 3);
        QCOMPARE(s1.marks()->count(), 0);
        QCOMPARE(s2.marks()->count(), 0);

        // Position change in s1 -> propagates ONCE to s2 with zero loop back to s1.
        s1.reader()->relocated({{QStringLiteral("chapter"), 1},
                                {QStringLiteral("offset"), 22},
                                {QStringLiteral("end"), 55},
                                {QStringLiteral("page"), 0},
                                {QStringLiteral("pages"), 1},
                                {QStringLiteral("snippet"), QStringLiteral("Second chapter text")}});
        s1.reader()->flushPosition();

        QCOMPARE(libPosSpy.count(), 1);
        QCOMPARE(libPosSpy.first().at(0).toInt(), bookA);
        QCOMPARE(libPosSpy.first().at(1).toInt(), 1);
        QCOMPARE(libPosSpy.first().at(3).value<QObject *>(), static_cast<QObject *>(&s1));
        QCOMPARE(r1SyncSpy.count(), 0);
        QCOMPARE(r2SyncSpy.count(), 1);
        QCOMPARE(s2.reader()->chapter(), 1);
        QCOMPARE(s2.reader()->pageStart(), qint64(22));
        QCOMPARE(s2.reader()->progress(), s1.reader()->progress());

        // Verify EPUB & PDF (PDFium U+FFFE\r\n + PDFKit ligatures & hyphenated wraps) quote anchoring.
        const QString pdfiumPage = QStringLiteral("First line of inter\uFFFE\r\nferometer data and office light.");
        const auto pdfiumSpan = Marks::locateQuote(pdfiumPage, QStringLiteral("interferometer"),
                                                   QStringLiteral("line of"), QStringLiteral("data"), 0);
        QVERIFY(pdfiumSpan.first >= 0);
        QCOMPARE(pdfiumPage.mid(pdfiumSpan.first, pdfiumSpan.second - pdfiumSpan.first),
                 QStringLiteral("inter\uFFFE\r\nferometer"));

        const QString pdfkitPage =
            QStringLiteral("A well-\nknown of\uFB01ce inter-\nferometer in \u201Clight\u201D (second office).");
        const int hPdf1 = s1.marks()->addHighlight(
            0, 0, 10, QStringLiteral("office interferometer"), QStringLiteral("green"), QStringLiteral("p. 1"),
            QStringLiteral("well-known"), QStringLiteral("in \"light\""));
        const int hPdf2 = s1.marks()->addHighlight(
            0, 0, 10, QStringLiteral("well-known"), QStringLiteral("blue"), QStringLiteral("p. 1"),
            QStringLiteral("A"), QStringLiteral("office"));
        QVERIFY(hPdf1 > 0 && hPdf2 > 0);
        const QVariantList anchored = s2.marks()->anchorHighlightsIn(0, pdfkitPage);
        QCOMPARE(anchored.size(), 2);
        for (const QVariant &v : anchored) {
            const QVariantMap m = v.toMap();
            QVERIFY(m.value(QStringLiteral("found")).toBool());
            const qint64 st = m.value(QStringLiteral("start")).toLongLong();
            const qint64 en = m.value(QStringLiteral("end")).toLongLong();
            const QString slice = pdfkitPage.mid(st, en - st);
            if (m.value(QStringLiteral("id")).toInt() == hPdf1)
                QCOMPARE(slice, QStringLiteral("of\uFB01ce inter-\nferometer"));
            else
                QCOMPARE(slice, QStringLiteral("well-\nknown"));
        }
    }

    // 4. Destroying a BookSession cleanly detaches QPointer<Reader> from
    //    Narration/Cast/Download with zero dangling pointers, plus Illustrator
    //    injected save chooser & headless copyText safety.
    void destroyingSessionDetachesPointersAndIllustratorChooser() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString epubA = dir.filePath(QStringLiteral("teardown.epub"));
        QVERIFY(TestBooks::write(epubA, twoChapterEpub("Ephemeral", "<p>Short text for teardown.</p>", "<p>End.</p>")));

        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        const int bookA = library.addBookFile(epubA);
        QVERIFY(bookA > 0);

        BookScheme scheme;
        FakeSpeechProvider provider;
        MemoryKeyStore keys;
        AiServices ai(&provider, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        VoicePacks packs(dir.filePath(QStringLiteral("voices")));
        VoiceCatalog catalog(&ai, &cache, nullptr, library.database().connectionName(),
                             dir.filePath(QStringLiteral("voices.json")));
        Cast cast(&ai, &cache, nullptr, &library, &catalog, &packs);
        Narration narration(&ai, &cache, nullptr, &library);
        narration.setCast(&cast);
        Download download(&ai, &cache, nullptr, &narration);
        download.setCast(&cast);

        // Verify Illustrator::setSaveFileChooser works cleanly without D-Bus.
        const QString picsDir = dir.filePath(QStringLiteral("pics"));
        QVERIFY(QDir().mkpath(picsDir));
        const QString srcImg = picsDir + QStringLiteral("/sample.png");
        QVERIFY(TestBooks::write(srcImg, pngBytes(16, 16)));
        const QString dstImg = dir.filePath(QStringLiteral("exported.png"));

        {
            auto session = std::make_unique<BookSession>(&library, &scheme, &ai, nullptr, picsDir);
            QVERIFY(session->openBook(bookA));

            // Headless copyText never crashes.
            session->reader()->copyText(QStringLiteral("safe copy"));

            // Bind Narration, Cast, Download to the heap session.
            narration.startFor(session->reader(), 0, 0);
            session->reader()->speakableReady(0, sampleSpeakableJson());
            download.startFor(session->reader(), 0, 0);
            QVERIFY(narration.active());
            QVERIFY(download.active());
            QCOMPARE(narration.boundReader(), session->reader());
            QCOMPARE(cast.reader(), session->reader());
            QCOMPARE(download.reader(), session->reader());

            // Insert a picture record and verify injected SaveFileChooser saves it cleanly.
            CompanionStore store(library.database().connectionName());
            store.ensurePictureTable();
            CompanionStore::PictureRecord rec;
            rec.bookId = bookA;
            rec.chapter = 0;
            rec.offset = 0;
            rec.caption = QStringLiteral("Harbor");
            rec.prompt = QStringLiteral("Engraving");
            rec.file = srcImg;
            rec.created = 1700000000;
            rec.style = QStringLiteral("engraving");
            rec.passage = QStringLiteral("Short text for teardown.");
            const int picId = store.insertPicture(rec);
            QVERIFY(picId > 0);

            bool chooserInvoked = false;
            session->illustrator()->setSaveFileChooser(
                [&chooserInvoked, dstImg](const QString &, const QString &, const QString &,
                                          std::function<void(const QString &, bool)> done) {
                    chooserInvoked = true;
                    done(dstImg, false);
                });
            QSignalSpy savedSpy(session->illustrator(), &Illustrator::savedCopy);
            session->illustrator()->chooseSaveCopy(picId);
            QTRY_COMPARE_WITH_TIMEOUT(savedSpy.count(), 1, 2000);
            QVERIFY(chooserInvoked);
            QCOMPARE(savedSpy.first().at(0).toString(), dstImg);
            QVERIFY(QFile::exists(dstImg));

            // Destroy session while Narration/Cast/Download are bound!
            session.reset();
        }

        QCoreApplication::processEvents();
        QVERIFY(narration.boundReader() == nullptr);
        QCOMPARE(narration.boundBookId(), 0);
        QVERIFY(!narration.active());
        QVERIFY(cast.reader() == nullptr);
        QCOMPARE(cast.bookId(), 0);
        QVERIFY(download.reader() == nullptr);
        QVERIFY(!download.active());

        // Subsequent calls after teardown must be completely safe (zero dangling dereferences).
        narration.start(0, 0);
        QVERIFY(!narration.active());
        narration.setVoice(QStringLiteral("sulafat"), QStringLiteral("Sulafat"));
        cast.setEnabled(false);
        QVERIFY(download.estimate(0, 0).value(QStringLiteral("chapters")).toInt() == 0);
    }
};

QTEST_MAIN(TestP0BookSession)
#include "tst_p0_book_session.moc"
