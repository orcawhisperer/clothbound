#include "audio/audiooutput.h"
#include "audio/speechplayer.h"
#include "formats/bookinfo.h"
#include "formats/bookresourcestore.h"
#include "formats/epubbook.h"
#include "formats/pdfrecolor.h"
#include "library/scanner.h"
#include "testbooks.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

class TestP0HeadlessCore : public QObject {
    Q_OBJECT

private slots:
    void bookResourceStoreIsolationCspFontsAndFallbackToken();
    void pdfRecolorAndContentBoxHeadless();
    void unzippedEpubDirectoryAndTraversalRejection();
    void scannerTreatsUnzippedEpubBundleAsSingleLeaf();
    void speechPlayerCustomAudioOutputBackend();
};

namespace {

QByteArray tinyPng(QColor color = Qt::darkBlue) {
    QImage img(12, 16, QImage::Format_RGB32);
    img.fill(color);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return out;
}

bool writeTextFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

bool makeSymlink(const QString &target, const QString &linkPath) {
#ifdef Q_OS_UNIX
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(linkPath).constData()) == 0;
#else
    return QFile::link(target, linkPath);
#endif
}

// Creates an Apple Books-style unzipped `.epub/` directory package on disk.
bool createUnzippedEpubBundle(const QString &bundleDir, const QByteArray &coverPng,
                              const QString &title = QStringLiteral("Apple Books Bundle")) {
    const QByteArray containerXml = R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>)";

    const QByteArray opfTemplate = R"(<?xml version="1.0" encoding="utf-8"?>
<package version="2.0" unique-identifier="uid" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>__TITLE__</dc:title>
    <dc:creator opf:role="aut">Clara Vance</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="uid">urn:isbn:978-0-00-000001-9</dc:identifier>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
    <item id="ch1" href="text/chapter%201.xhtml" media-type="application/xhtml+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="ch1"/>
  </spine>
</package>)";
    QByteArray opf = opfTemplate;
    opf.replace("__TITLE__", title.toUtf8());

    const QByteArray ncx = R"(<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <navMap>
    <navPoint id="n1" playOrder="1"><navLabel><text>Opening</text></navLabel><content src="text/chapter%201.xhtml"/></navPoint>
  </navMap>
</ncx>)";

    const QByteArray ch1 = TestBooks::chapter("Opening", "<p>Unzipped Apple Books chapter content.</p>");
    const QByteArray css = "p { line-height: 1.5; }";

    return writeTextFile(bundleDir + QStringLiteral("/mimetype"), "application/epub+zip") &&
           writeTextFile(bundleDir + QStringLiteral("/META-INF/container.xml"), containerXml) &&
           writeTextFile(bundleDir + QStringLiteral("/OEBPS/content.opf"), opf) &&
           writeTextFile(bundleDir + QStringLiteral("/OEBPS/toc.ncx"), ncx) &&
           writeTextFile(bundleDir + QStringLiteral("/OEBPS/text/chapter 1.xhtml"), ch1) &&
           writeTextFile(bundleDir + QStringLiteral("/OEBPS/images/cover.png"), coverPng) &&
           writeTextFile(bundleDir + QStringLiteral("/OEBPS/style.css"), css);
}

// Fake in-memory AudioOutputBackend proving SpeechPlayer drives playback
// cleanly through the abstract backend seam without Qt6::Multimedia.
class FakeAudioOutputBackend : public AudioOutputBackend {
public:
    struct Telemetry {
        std::mutex mutex;
        int openCalls = 0;
        int suspendCalls = 0;
        int resumeCalls = 0;
        int resetCalls = 0;
        int stopCalls = 0;
        double lastVolume = -1.0;
        QByteArray receivedPcm;
    };

    explicit FakeAudioOutputBackend(std::shared_ptr<Telemetry> telemetry) : m_telemetry(std::move(telemetry)) {}

    bool open(int sampleRate, int channels = 1) override {
        Q_UNUSED(sampleRate);
        Q_UNUSED(channels);
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        ++m_telemetry->openCalls;
        m_open = true;
        m_state = State::Active;
        return true;
    }

    qint64 write(const char *data, qint64 len) override {
        if (!m_open || !data || len <= 0)
            return 0;
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        m_telemetry->receivedPcm.append(data, qsizetype(len));
        m_state = State::Active;
        return len;
    }

    void suspend() override {
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        ++m_telemetry->suspendCalls;
        m_state = State::Suspended;
    }

    void resume() override {
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        ++m_telemetry->resumeCalls;
        m_state = State::Active;
    }

    bool reset() override {
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        ++m_telemetry->resetCalls;
        m_state = State::Active;
        return m_open;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        ++m_telemetry->stopCalls;
        m_open = false;
        m_state = State::Stopped;
    }

    qint64 bytesFree() const override { return m_bufferBytes; }
    qint64 bufferSize() const override { return m_bufferBytes; }
    qint64 bufferedBytes() const override { return 0; }
    State state() const override { return m_state; }

    void setVolume(double volume) override {
        std::lock_guard<std::mutex> lock(m_telemetry->mutex);
        m_telemetry->lastVolume = volume;
    }

    bool isOpen() const override { return m_open; }

private:
    static constexpr qint64 m_bufferBytes = 48000;
    std::shared_ptr<Telemetry> m_telemetry;
    bool m_open = false;
    State m_state = State::Stopped;
};

}  // namespace

void TestP0HeadlessCore::bookResourceStoreIsolationCspFontsAndFallbackToken() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray coverPng = tinyPng(Qt::darkRed);
    const QByteArray rootImg = tinyPng(Qt::darkGreen);

    // Book A: well-formed XHTML, malformed XHTML, and a root-level `/images/root-pic.png`.
    const QByteArray epubA = TestBooks::epub({
        {"OEBPS/content.opf", R"(<?xml version="1.0" encoding="utf-8"?>
<package version="2.0" unique-identifier="uid" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Book A</dc:title></metadata>
  <manifest>
    <item id="ok" href="ok.xhtml" media-type="application/xhtml+xml"/>
    <item id="bad" href="bad.xhtml" media-type="application/xhtml+xml"/>
    <item id="cov" href="images/cover.png" media-type="image/png"/>
  </manifest>
  <spine><itemref idref="ok"/><itemref idref="bad"/></spine>
</package>)"},
        {"OEBPS/ok.xhtml", TestBooks::chapter("OK", "<p>Well formed.</p>")},
        {"OEBPS/bad.xhtml", "<html><body><p>Unclosed paragraph<br></body></html>"},
        {"OEBPS/images/cover.png", coverPng},
        {"images/root-pic.png", rootImg},
    });
    const QString pathA = dir.filePath(QStringLiteral("a.epub"));
    QVERIFY(TestBooks::write(pathA, epubA));

    // Book B: EPUB 3 with Adobe-obfuscated embedded font.
    const QByteArray plainFont = QByteArray("OTTO") + QByteArray(1200, 'F');
    const QByteArray adobeKey = QByteArray::fromHex("123456789abcdef0123456789abcdef0");
    QByteArray obfFont = plainFont;
    for (qsizetype i = 0; i < qMin<qsizetype>(1024, obfFont.size()); ++i)
        obfFont[i] = char(obfFont[i] ^ adobeKey[i % adobeKey.size()]);
    const QString pathB = dir.filePath(QStringLiteral("b.epub"));
    QVERIFY(TestBooks::write(pathB, TestBooks::epub3(obfFont)));

    auto bookA = std::make_shared<EpubBook>(pathA);
    QVERIFY(bookA->open());
    auto bookB = std::make_shared<EpubBook>(pathB);
    QVERIFY(bookB->open());

    BookResourceStore store;
    const QString tokenA = store.registerBook(pathA, bookA);
    const QString tokenB = store.registerBook(pathB, bookB);
    QVERIFY(!tokenA.isEmpty());
    QVERIFY(!tokenB.isEmpty());
    QVERIFY(tokenA != tokenB);
    QCOMPARE(BookResourceStore::baseUrl(tokenA), QStringLiteral("clothbound://local/book/%1/").arg(tokenA));

    // 1. Well-formed XHTML vs malformed XHTML fallback + strict CSP & nosniff headers.
    const auto okRes = store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/ok.xhtml").arg(tokenA)));
    QVERIFY(okRes.ok());
    QCOMPARE(okRes.status, 200);
    QCOMPARE(okRes.contentType, QByteArray("application/xhtml+xml"));
    QCOMPARE(BookResourceStore::headerValue(okRes.headers, "X-Content-Type-Options"), QByteArray("nosniff"));
    const QByteArray bookCsp = BookResourceStore::headerValue(okRes.headers, "Content-Security-Policy");
    QVERIFY(bookCsp.contains("script-src 'none'"));
    QVERIFY(bookCsp.contains("object-src 'none'"));
    QVERIFY(bookCsp.contains("form-action 'none'"));

    const auto badRes = store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/bad.xhtml").arg(tokenA)));
    QVERIFY(badRes.ok());
    QCOMPARE(badRes.contentType, QByteArray("text/html"));
    QCOMPARE(BookResourceStore::headerValue(badRes.headers, "Content-Security-Policy"), bookCsp);

    // App headers verify strict reader surface CSP.
    const QByteArray appCsp = BookResourceStore::headerValue(BookResourceStore::appHeaderList(), "Content-Security-Policy");
    QVERIFY(appCsp.contains("script-src 'self'"));
    QVERIFY(!appCsp.contains("'unsafe-eval'"));
    QCOMPARE(BookResourceStore::headerValue(BookResourceStore::appHeaderList(), "X-Content-Type-Options"),
             QByteArray("nosniff"));

    // 2. Embedded font de-obfuscation through BookResourceStore.
    const auto fontRes = store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/fonts/f.otf").arg(tokenB)));
    QVERIFY(fontRes.ok());
    QCOMPARE(fontRes.contentType, QByteArray("font/otf"));
    QCOMPARE(fontRes.body, plainFont);
    QVERIFY(BookResourceStore::headerValue(fontRes.headers, "Content-Security-Policy").contains("script-src 'none'"));

    // 3. Token isolation: tokenA cannot read bookB files; unknown token never falls back to fallbackToken.
    QVERIFY(!store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/fonts/f.otf").arg(tokenA))).ok());
    const auto badTok = store.resolve(QUrl(QStringLiteral("clothbound://local/book/deadbeef/OEBPS/ok.xhtml")),
                                      {}, tokenA);
    QVERIFY(!badTok.ok());
    QCOMPARE(badTok.status, 404);

    // 4. WebKit empty-Referer fix: root-relative paths fall back cleanly to `fallbackToken` when Referer is empty.
    const QUrl rootRelUrl(QStringLiteral("clothbound://local/images/root-pic.png"));
    // Without Referer and without fallbackToken -> 404.
    const auto noRefNoFallback = store.resolve(rootRelUrl);
    QVERIFY(!noRefNoFallback.ok());
    QCOMPARE(noRefNoFallback.status, 404);

    // Empty Referer + fallbackToken -> 200 with exact image bytes and book CSP.
    const auto emptyRefFallback = store.resolve(rootRelUrl, {}, tokenA);
    QVERIFY(emptyRefFallback.ok());
    QCOMPARE(emptyRefFallback.status, 200);
    QCOMPARE(emptyRefFallback.contentType, QByteArray("image/png"));
    QCOMPARE(emptyRefFallback.body, rootImg);
    QCOMPARE(BookResourceStore::headerValue(emptyRefFallback.headers, "Content-Security-Policy"), bookCsp);

    // Valid Referer takes precedence over a different fallbackToken.
    const QByteArray refA = QStringLiteral("clothbound://local/book/%1/OEBPS/ok.xhtml").arg(tokenA).toUtf8();
    const auto refWins = store.resolve(rootRelUrl, refA, tokenB);
    QVERIFY(refWins.ok());
    QCOMPARE(refWins.body, rootImg);

    // Non-empty cross-origin Referer is REFUSED even when fallbackToken is supplied.
    const auto evilRef = store.resolve(rootRelUrl, "https://evil.example/book/" + tokenA.toUtf8() + "/ok.xhtml", tokenA);
    QVERIFY(!evilRef.ok());
    QCOMPARE(evilRef.status, 404);

    // Invalid host -> 400; path traversal -> 404.
    const auto wrongHost = store.resolve(QUrl(QStringLiteral("clothbound://evil/book/%1/OEBPS/ok.xhtml").arg(tokenA)));
    QVERIFY(!wrongHost.ok());
    QCOMPARE(wrongHost.status, 400);
    QVERIFY(!store.resolve(QUrl(QStringLiteral("clothbound://local/../../etc/passwd")), {}, tokenA).ok());
    QVERIFY(!store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/../../etc/passwd").arg(tokenA))).ok());

    // 5. Concurrent reads from multiple threads are safe.
    std::atomic_int okCount{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                const auto r1 = store.resolve(rootRelUrl, {}, tokenA);
                const auto r2 = store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/fonts/f.otf").arg(tokenB)));
                if (r1.ok() && r1.body == rootImg && r2.ok() && r2.body == plainFont)
                    ++okCount;
            }
        });
    }
    for (auto &th : workers)
        th.join();
    QCOMPARE(okCount.load(), 80);

    // 6. Unregistering tokenA invalidates only tokenA while keeping tokenB alive.
    store.unregisterBook(tokenA);
    QVERIFY(!store.contains(tokenA));
    QVERIFY(!store.resolve(rootRelUrl, {}, tokenA).ok());
    QVERIFY(store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/fonts/f.otf").arg(tokenB))).ok());
}

void TestP0HeadlessCore::pdfRecolorAndContentBoxHeadless() {
    auto lumaOf = [](QRgb px) {
        return (qRed(px) * 299 + qGreen(px) * 587 + qBlue(px) * 114) / 1000;
    };

    // 1. Dark-theme recolor preserves readability of dark text and colored links.
    QImage darkImg(8, 2, QImage::Format_RGB32);
    darkImg.fill(qRgb(255, 255, 255));
    darkImg.setPixel(0, 0, qRgb(0, 0, 0));         // black ink
    darkImg.setPixel(1, 0, qRgb(10, 30, 100));     // dark navy hyperlink
    darkImg.setPixel(2, 0, qRgb(110, 15, 20));     // dark burgundy heading
    darkImg.setPixel(3, 0, qRgb(0, 0, 255));       // saturated blue link
    const QColor darkBg(QStringLiteral("#1E1D1B"));
    const QColor lightFg(QStringLiteral("#DCD5C8"));
    PdfRecolor::recolor(darkImg, darkBg, lightFg);

    QCOMPARE(darkImg.pixel(7, 1), darkBg.rgb());
    QCOMPARE(darkImg.pixel(0, 0), lightFg.rgb());
    const int bgL = lumaOf(darkBg.rgb());
    for (int x = 0; x < 4; ++x) {
        const int outL = lumaOf(darkImg.pixel(x, 0));
        QVERIFY2(outL - bgL >= 85,
                 qPrintable(QStringLiteral("pixel %1 contrast too low: out=%2 bg=%3").arg(x).arg(outL).arg(bgL)));
    }

    // 2. Sepia/light theme compositeAndRecolor handles premultiplied ARGB32 cleanly.
    QImage argb(4, 2, QImage::Format_ARGB32_Premultiplied);
    argb.fill(Qt::transparent);
    argb.setPixel(0, 0, qRgba(0, 0, 0, 255));        // opaque black ink
    argb.setPixel(1, 0, qRgba(255, 255, 255, 255));  // opaque white paper
    const QColor sepiaBg(QStringLiteral("#F4ECD8"));
    const QColor sepiaFg(QStringLiteral("#2B2520"));
    PdfRecolor::compositeAndRecolor(argb, sepiaBg, sepiaFg);
    QCOMPARE(argb.format(), QImage::Format_RGB32);
    QCOMPARE(argb.pixel(0, 0), sepiaFg.rgb());
    QCOMPARE(argb.pixel(1, 0), sepiaBg.rgb());
    QCOMPARE(argb.pixel(3, 1), sepiaBg.rgb());  // transparent composited onto paper -> bg

    // 3. contentBox locates ink bounds accurately and returns empty on blank page.
    QImage blank(50, 50, QImage::Format_RGB32);
    blank.fill(Qt::white);
    QVERIFY(PdfRecolor::contentBox(blank).isEmpty());

    QImage page(100, 100, QImage::Format_RGB32);
    page.fill(Qt::white);
    for (int y = 30; y < 90; ++y)
        for (int x = 20; x < 60; ++x)
            page.setPixel(x, y, qRgb(20, 20, 20));
    const QRectF box = PdfRecolor::contentBox(page);
    QVERIFY(qAbs(box.left() - 0.20) < 1e-6);
    QVERIFY(qAbs(box.top() - 0.30) < 1e-6);
    QVERIFY(qAbs(box.width() - 0.40) < 1e-6);
    QVERIFY(qAbs(box.height() - 0.60) < 1e-6);
}

void TestP0HeadlessCore::unzippedEpubDirectoryAndTraversalRejection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString secretPath = dir.filePath(QStringLiteral("secret.txt"));
    QVERIFY(writeTextFile(secretPath, "TOP SECRET OUTSIDE BUNDLE"));

    const QString bundleDir = dir.filePath(QStringLiteral("Apple Books Sample.epub"));
    const QByteArray coverPng = tinyPng(Qt::darkMagenta);
    QVERIFY(createUnzippedEpubBundle(bundleDir, coverPng));

    // Place a malicious symlink inside the bundle pointing outside the bundle root.
    const QString escapeLink = bundleDir + QStringLiteral("/OEBPS/escape.xhtml");
    QVERIFY(makeSymlink(secretPath, escapeLink));

    // A plain directory without META-INF/container.xml is NOT an unzipped EPUB package.
    const QString plainDir = dir.filePath(QStringLiteral("plain.epub"));
    QVERIFY(QDir().mkpath(plainDir));
    QVERIFY(!EpubBook::isUnzippedPackage(plainDir));
    QVERIFY(EpubBook::isUnzippedPackage(bundleDir));

    // Open unzipped `.epub/` directory directly with EpubBook.
    auto book = std::make_shared<EpubBook>(bundleDir);
    QVERIFY2(book->open(), qPrintable(book->errorString()));
    QVERIFY(book->isDirectoryPackage());
    QCOMPARE(book->metadata().title, QStringLiteral("Apple Books Bundle"));
    QCOMPARE(book->metadata().authors, QStringList{QStringLiteral("Clara Vance")});
    QCOMPARE(book->metadata().language, QStringLiteral("en"));
    QCOMPARE(book->metadata().isbn, QStringLiteral("9780000000019"));
    QCOMPARE(book->spine().size(), 1);
    QCOMPARE(book->toc().size(), 1);
    QCOMPARE(book->toc().first().title, QStringLiteral("Opening"));
    QVERIFY(book->chapterText(0).contains(QStringLiteral("Unzipped Apple Books chapter content.")));
    QCOMPARE(book->coverImage(), coverPng);
    QVERIFY(book->chapterUncompressedBytes(0) > 0);
    QVERIFY(!book->estimatedChapterLengths().isEmpty());
    QVERIFY(book->estimatedChapterLengths().first() > 0);

    // Case-insensitive resource lookup works identically to ZipArchive mode.
    QString cssType, cssErr;
    const QByteArray cssBytes = book->resource(QStringLiteral("oebps/STYLE.CSS"), &cssType, &cssErr);
    QVERIFY(cssErr.isEmpty());
    QCOMPARE(cssType, QStringLiteral("text/css"));
    QVERIFY(cssBytes.contains("line-height"));

    // Strict path-traversal & escaping-symlink containment rejection.
    QString err;
    QVERIFY(book->resource(QStringLiteral("../secret.txt"), nullptr, &err).isNull());
    QVERIFY(!err.isEmpty());

    err.clear();
    QVERIFY(book->resource(QStringLiteral("OEBPS/../../secret.txt"), nullptr, &err).isNull());
    QVERIFY(!err.isEmpty());

    err.clear();
    QVERIFY(book->resource(QStringLiteral("/etc/passwd"), nullptr, &err).isNull());
    QVERIFY(!err.isEmpty());

    err.clear();
    QVERIFY(book->resource(QStringLiteral("OEBPS\\..\\..\\secret.txt"), nullptr, &err).isNull());
    QVERIFY(!err.isEmpty());

    err.clear();
    QVERIFY2(book->resource(QStringLiteral("OEBPS/escape.xhtml"), nullptr, &err).isNull(),
             "Symlink escaping unzipped EPUB bundle root must be rejected");
    QVERIFY(!err.isEmpty());

    // BookFiles::read() works end-to-end on the unzipped `.epub/` directory.
    QList<int> chapterLengths;
    const BookInfo info = BookFiles::read(bundleDir, 200, &chapterLengths);
    QVERIFY2(info.ok(), qPrintable(info.problem));
    QCOMPARE(info.format, QStringLiteral("epub"));
    QCOMPARE(info.identity.title, QStringLiteral("Apple Books Bundle"));
    QCOMPARE(info.chapters, 1);
    QVERIFY(!info.cover.isNull());
    QCOMPARE(chapterLengths.size(), 1);

    // BookResourceStore serves unzipped `.epub/` packages and blocks traversal/escaping symlinks.
    BookResourceStore store;
    const QString token = store.registerBook(bundleDir, book);
    QVERIFY(!token.isEmpty());
    const auto chRes = store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/text/chapter%201.xhtml").arg(token)));
    QVERIFY(chRes.ok());
    QCOMPARE(chRes.contentType, QByteArray("application/xhtml+xml"));
    QVERIFY(chRes.body.contains("Unzipped Apple Books chapter content."));
    QVERIFY(!store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/escape.xhtml").arg(token))).ok());
    QVERIFY(!store.resolve(QUrl(QStringLiteral("clothbound://local/book/%1/../secret.txt").arg(token))).ok());
}

void TestP0HeadlessCore::scannerTreatsUnzippedEpubBundleAsSingleLeaf() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString root = dir.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(root));

    // 1. Unzipped Apple Books `.EPUB/` bundle (mixed-case extension!) with a decoy PDF
    // inside it to prove Scanner treats the bundle as a single leaf and NEVER recurses inside.
    const QString bundleDir = root + QStringLiteral("/Shelf/AppleBook.EPUB");
    QVERIFY(createUnzippedEpubBundle(bundleDir, tinyPng(Qt::darkCyan), QStringLiteral("Leaf Bundle")));
    QVERIFY(TestBooks::write(bundleDir + QStringLiteral("/OEBPS/decoy_inside_bundle.pdf"),
                             TestBooks::pdf("Decoy", "Nobody")));

    // 2. Normal zipped EPUB alongside it.
    const QString zippedPath = root + QStringLiteral("/Shelf/Zipped.epub");
    QVERIFY(TestBooks::write(zippedPath, TestBooks::epub2(tinyPng(Qt::darkBlue))));

    // 3. Regular directory ending in `.epub` WITHOUT META-INF/container.xml is NOT a package
    // and should still be traversed normally.
    const QString notBundlePdf = root + QStringLiteral("/NotABundle.epub/real.pdf");
    QVERIFY(writeTextFile(notBundlePdf, TestBooks::pdf("Real PDF", "Author")));

    // 4. Hidden directory remains pruned (v0.5.2 regression check).
    QVERIFY(writeTextFile(root + QStringLiteral("/.hidden/skip.pdf"), TestBooks::pdf("Hidden", "Nobody")));

    Scanner scanner(dir.filePath(QStringLiteral("covers")));
    QSignalSpy listedSpy(&scanner, &Scanner::listed);
    QSignalSpy bookReadSpy(&scanner, &Scanner::bookRead);
    QSignalSpy finishedSpy(&scanner, &Scanner::finished);

    scanner.scan({root}, {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

    QCOMPARE(listedSpy.count(), 1);
    const QStringList listedPaths = listedSpy.at(0).at(0).toStringList();
    const QSet<QString> actual(listedPaths.cbegin(), listedPaths.cend());
    const QSet<QString> expected{
        QFileInfo(bundleDir).canonicalFilePath(),
        QFileInfo(zippedPath).canonicalFilePath(),
        QFileInfo(notBundlePdf).canonicalFilePath(),
    };
    QCOMPARE(listedPaths.size(), 3);
    QCOMPARE(actual, expected);
    QCOMPARE(bookReadSpy.count(), 3);

    bool sawBundleRead = false;
    for (int i = 0; i < bookReadSpy.count(); ++i) {
        const ScannedBook scanned = bookReadSpy.at(i).at(0).value<ScannedBook>();
        if (scanned.info.path == QFileInfo(bundleDir).canonicalFilePath()) {
            sawBundleRead = true;
            QVERIFY2(scanned.info.ok(), qPrintable(scanned.info.problem));
            QCOMPARE(scanned.info.identity.title, QStringLiteral("Leaf Bundle"));
            QVERIFY(!scanned.fingerprint.isEmpty());
            QVERIFY(!scanned.coverFile.isEmpty());
            QVERIFY(scanned.length > 0);
        }
    }
    QVERIFY(sawBundleRead);

    // Rescanning with `known` skips re-reading the unchanged unzipped `.epub/` bundle.
    QHash<QString, QPair<qint64, qint64>> known;
    for (const QString &p : listedPaths) {
        const QFileInfo fi(p);
        known.insert(p, {fi.size(), fi.lastModified().toSecsSinceEpoch()});
    }
    listedSpy.clear();
    bookReadSpy.clear();
    finishedSpy.clear();
    scanner.scan({root}, {}, known);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(listedSpy.at(0).at(0).toStringList().size(), 3);
    QCOMPARE(bookReadSpy.count(), 0);

    // Explicit loose-file addition also accepts an unzipped `.epub/` bundle.
    listedSpy.clear();
    bookReadSpy.clear();
    finishedSpy.clear();
    scanner.scan({}, {bundleDir}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(listedSpy.at(0).at(0).toStringList(), QStringList{QFileInfo(bundleDir).canonicalFilePath()});
    QCOMPARE(bookReadSpy.count(), 1);
}

void TestP0HeadlessCore::speechPlayerCustomAudioOutputBackend() {
    auto telemetry = std::make_shared<FakeAudioOutputBackend::Telemetry>();
    SpeechPlayer player(24000);
    player.setOutputBackendFactory([telemetry]() -> std::unique_ptr<AudioOutputBackend> {
        return std::make_unique<FakeAudioOutputBackend>(telemetry);
    });
    QVERIFY(player.hasCustomOutputBackend());
    QVERIFY(player.hasAudioOutputDevice());

    QSignalSpy posSpy(&player, &SpeechPlayer::position);
    QSignalSpy drainedSpy(&player, &SpeechPlayer::drained);
    QSignalSpy errorSpy(&player, &SpeechPlayer::sinkError);

    player.setVolume(0.75);
    player.addTrack(1, 0);
    QByteArray pcm(4800 * 2, '\0');  // 200 ms of 24 kHz mono Int16 silence
    player.appendAudio(1, pcm);
    player.finishTrack(1);
    player.play();

    QTRY_VERIFY_WITH_TIMEOUT(drainedSpy.count() >= 1, 5000);
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(posSpy.count() >= 1);

    player.pause();
    player.setMuted(true);
    player.clear();
    QCoreApplication::processEvents();

    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            std::lock_guard<std::mutex> lock(telemetry->mutex);
            return telemetry->openCalls >= 1 && telemetry->receivedPcm.size() == pcm.size() &&
                   telemetry->suspendCalls >= 1 && telemetry->stopCalls >= 1 && telemetry->lastVolume == 0.0;
        })(),
        3000);
}

QTEST_GUILESS_MAIN(TestP0HeadlessCore)
#include "tst_p0_headless_core.moc"
