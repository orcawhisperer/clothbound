#include "formats/epubbook.h"
#include "formats/ziparchive.h"
#include "library/library.h"
#include "online/articleextractor.h"
#include "online/epubwriter.h"
#include "online/urlguard.h"
#include "online/webimporter.h"
#include "testbooks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QImage>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>
#include <memory>

namespace {
// Minimal valid 1x1 PNG image (67 bytes).
QByteArray samplePng() {
    static const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    return png;
}

// Local HTTP test server supporting custom status codes, extra headers (e.g. Location),
// and chunked/streaming oversized payloads without Content-Length.
class LocalHttpServer {
public:
    struct Reply {
        int status = 200;
        QByteArray contentType = "text/html; charset=utf-8";
        QList<QPair<QByteArray, QByteArray>> headers;
        QByteArray body;
        bool omitContentLength = false;
    };

    LocalHttpServer() {
        m_server.listen(QHostAddress::LocalHost);
        QPointer<QTcpServer> guard(&m_server);
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this, guard] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, guard, socket, buf] {
                    if (!guard)
                        return;
                    *buf += socket->readAll();
                    const qsizetype end = buf->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QList<QByteArray> lines = buf->left(end).split('\n');
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    const QByteArray path = first.value(1);
                    requestedPaths.append(path);

                    Reply rep{404, "text/plain", {}, "Not Found", false};
                    if (routes.contains(path))
                        rep = routes.value(path);

                    QByteArray raw = "HTTP/1.1 " + QByteArray::number(rep.status) + " OK\r\n";
                    if (!rep.contentType.isEmpty())
                        raw += "Content-Type: " + rep.contentType + "\r\n";
                    for (const auto &[k, v] : rep.headers)
                        raw += k + ": " + v + "\r\n";
                    if (!rep.omitContentLength)
                        raw += "Content-Length: " + QByteArray::number(rep.body.size()) + "\r\n";
                    raw += "Connection: close\r\n\r\n";
                    raw += rep.body;
                    socket->write(raw);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    ~LocalHttpServer() { m_server.close(); }

    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    QMap<QByteArray, Reply> routes;
    QList<QByteArray> requestedPaths;

private:
    QTcpServer m_server;
};

bool isWellFormedXml(const QByteArray &xmlBytes, QString *errOut = nullptr) {
    QXmlStreamReader reader(xmlBytes);
    while (!reader.atEnd())
        reader.readNext();
    if (reader.hasError()) {
        if (errOut)
            *errOut = reader.errorString();
        return false;
    }
    return true;
}
}  // namespace

class TestOnlineIngest : public QObject {
    Q_OBJECT

private slots:
    void urlGuardRejectsUnsafeSchemes();
    void urlGuardBlocksPrivateAddresses();
    void urlGuardAllowsLocalWhenEnabled();
    void epubWriterRoundTrip();
    void articleExtractorCleanAndSanitize();
    void articleExtractorScoresProseContainer();
    void webImporterParsingHelpers();
    void webImporterEndToEndArticleWithImage();
    void webImporterEndToEndDirectEpub();
    void webImporterEndToEndPdfWithRedirect();
    void webImporterEndToEndArxiv();
    void webImporterRejectsForbiddenRedirect();
    void webImporterRejectsExcessiveRedirects();
    void webImporterRejectsOversizedPayload();
};

void TestOnlineIngest::urlGuardRejectsUnsafeSchemes() {
    QString reason;
    const QStringList blocked = {
        QStringLiteral("file:///etc/passwd"),
        QStringLiteral("ftp://example.com/book.epub"),
        QStringLiteral("javascript:alert(1)"),
        QStringLiteral("data:text/html,<h1>hi</h1>"),
        QStringLiteral("clothbound://book/chapter.xhtml"),
        QStringLiteral("gopher://example.com/"),
        QStringLiteral(""),
        QStringLiteral("not-a-url"),
        QStringLiteral("http:///missing-host"),
    };

    for (const QString &raw : blocked) {
        QVERIFY2(!UrlGuard::isAllowedUrl(QUrl(raw), false, &reason), qPrintable(raw));
        QVERIFY2(!reason.isEmpty(), qPrintable(raw));
        QVERIFY2(!UrlGuard::isAllowedUrl(QUrl(raw), true, &reason), qPrintable(raw));
    }
}

void TestOnlineIngest::urlGuardBlocksPrivateAddresses() {
    // Direct QHostAddress checks
    const QStringList privateIps = {
        QStringLiteral("0.0.0.0"),
        QStringLiteral("10.0.0.1"),
        QStringLiteral("100.64.0.1"),
        QStringLiteral("100.127.255.254"),
        QStringLiteral("127.0.0.1"),
        QStringLiteral("127.255.255.254"),
        QStringLiteral("169.254.169.254"),
        QStringLiteral("172.16.0.1"),
        QStringLiteral("172.31.255.254"),
        QStringLiteral("192.168.1.1"),
        QStringLiteral("224.0.0.1"),
        QStringLiteral("255.255.255.255"),
        QStringLiteral("::"),
        QStringLiteral("::1"),
        QStringLiteral("fc00::1"),
        QStringLiteral("fd12:3456:789a::1"),
        QStringLiteral("fe80::1"),
        QStringLiteral("ff02::1"),
        QStringLiteral("::ffff:127.0.0.1"),
        QStringLiteral("::ffff:169.254.169.254"),
        QStringLiteral("::ffff:10.0.0.5"),
        QStringLiteral("::ffff:192.168.0.10"),
        QStringLiteral("::ffff:100.64.1.1"),
        QStringLiteral("64:ff9b::127.0.0.1"),
        QStringLiteral("2002:7f00:0001::1"),
    };
    for (const QString &ip : privateIps) {
        const QHostAddress addr(ip);
        QVERIFY2(!addr.isNull(), qPrintable(ip));
        QVERIFY2(UrlGuard::isPrivateHostAddress(addr), qPrintable(ip));
    }

    // Public IPs must NOT be classified as private
    const QStringList publicIps = {
        QStringLiteral("1.1.1.1"),
        QStringLiteral("8.8.8.8"),
        QStringLiteral("93.184.216.34"),
        QStringLiteral("2606:4700:4700::1111"),
        QStringLiteral("2001:4860:4860::8888"),
        QStringLiteral("::ffff:93.184.216.34"),
    };
    for (const QString &ip : publicIps) {
        const QHostAddress addr(ip);
        QVERIFY2(!addr.isNull(), qPrintable(ip));
        QVERIFY2(!UrlGuard::isPrivateHostAddress(addr), qPrintable(ip));
    }

    // URL-level blocks when allowLocalNetwork == false
    QString reason;
    const QStringList blockedUrls = {
        QStringLiteral("http://127.0.0.1/secret"),
        QStringLiteral("http://localhost/secret"),
        QStringLiteral("http://localhost./secret"),
        QStringLiteral("http://api.localhost/secret"),
        QStringLiteral("http://printer.local/status"),
        QStringLiteral("http://169.254.169.254/latest/meta-data/"),
        QStringLiteral("http://10.0.0.1/admin"),
        QStringLiteral("http://172.16.5.4/internal"),
        QStringLiteral("http://192.168.1.50/router"),
        QStringLiteral("http://100.64.0.10/cgnat"),
        QStringLiteral("http://0.0.0.0/"),
        QStringLiteral("http://[::1]/"),
        QStringLiteral("http://[fc00::1]/"),
        QStringLiteral("http://[fe80::1]/"),
        QStringLiteral("http://[::ffff:127.0.0.1]/"),
        QStringLiteral("http://[::ffff:169.254.169.254]/"),
        QStringLiteral("http://2130706433/"),      // decimal 127.0.0.1
        QStringLiteral("http://0x7f000001/"),      // hex 127.0.0.1
        QStringLiteral("http://0177.0.0.1/"),      // octal 127.0.0.1
        QStringLiteral("https://user:pass@example.com/book.epub"),
    };
    for (const QString &raw : blockedUrls) {
        QVERIFY2(!UrlGuard::isAllowedUrl(QUrl(raw), false, &reason), qPrintable(raw));
        QVERIFY2(!reason.isEmpty(), qPrintable(raw));
    }

    // Public URLs allowed
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("https://example.com/essays/craft")), false, &reason));
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("https://93.184.216.34/doc.pdf")), false, &reason));
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("https://[2606:4700:4700::1111]/")), false, &reason));

    // validateResolvedHost rejects localhost even when resolving
    QVERIFY(!UrlGuard::validateResolvedHost(QUrl(QStringLiteral("http://localhost/")), false, &reason));
}

void TestOnlineIngest::urlGuardAllowsLocalWhenEnabled() {
    QString reason;
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("http://127.0.0.1:8080/article")), true, &reason));
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("http://localhost:9000/book.epub")), true, &reason));
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("http://[::1]:8080/test")), true, &reason));
    QVERIFY(UrlGuard::isAllowedUrl(QUrl(QStringLiteral("http://user:pass@127.0.0.1:8080/opds")), true, &reason));
    QVERIFY(UrlGuard::validateResolvedHost(QUrl(QStringLiteral("http://127.0.0.1:8080/test")), true, &reason));
}

void TestOnlineIngest::epubWriterRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString outPath = dir.filePath(QStringLiteral("essay.epub"));

    EpubArticle article;
    article.title = QStringLiteral("On Calm Typography & Paper");
    article.author = QStringLiteral("Ada Lovelace");
    article.publisher = QStringLiteral("Analytical Journal");
    article.publishedDate = QStringLiteral("2026-04-12");
    article.language = QStringLiteral("en-GB");
    article.sourceUrl = QStringLiteral("https://example.com/essays/calm-typography");
    article.description = QStringLiteral("Why quiet margins make long-form reading enduring.");
    article.subjects = {QStringLiteral("Typography"), QStringLiteral("Design")};
    article.readingMinutes = 4;
    article.headings = {
        {QStringLiteral("sec-1"), QStringLiteral("Margins & Rhythm"), 2},
        {QStringLiteral("sec-2"), QStringLiteral("Optical Scale"), 2},
    };
    article.bodyXhtml = QStringLiteral(
        "<h2 id=\"sec-1\">Margins &amp; Rhythm</h2>\n"
        "<p>Reading online should feel like opening a clothbound volume.</p>\n"
        "<figure><img src=\"images/img-001.png\" alt=\"Specimen\"/><figcaption>Type specimen</figcaption></figure>\n"
        "<h2 id=\"sec-2\">Optical Scale</h2>\n"
        "<blockquote><p>Good typography is invisible.</p></blockquote>\n");
    article.images = {{QStringLiteral("images/img-001.png"), QStringLiteral("image/png"), samplePng()}};

    QString err;
    QVERIFY2(EpubWriter::writeArticle(outPath, article, &err), qPrintable(err));

    // Verify EPUB OCF 3.3 byte-level ZIP layout: mimetype first, stored (method 0), 0 extra bytes.
    QFile rawFile(outPath);
    QVERIFY(rawFile.open(QIODevice::ReadOnly));
    const QByteArray raw = rawFile.readAll();
    QVERIFY(raw.size() > 60);
    QCOMPARE(raw.left(4), QByteArray("PK\x03\x04", 4));
    const quint16 method = quint16(quint8(raw[8]) | (quint8(raw[9]) << 8));
    const quint16 nameLen = quint16(quint8(raw[26]) | (quint8(raw[27]) << 8));
    const quint16 extraLen = quint16(quint8(raw[28]) | (quint8(raw[29]) << 8));
    QCOMPARE(method, quint16(0));
    QCOMPARE(nameLen, quint16(8));
    QCOMPARE(extraLen, quint16(0));
    QCOMPARE(raw.mid(30, 8), QByteArrayLiteral("mimetype"));
    QCOMPARE(raw.mid(38, 20), QByteArrayLiteral("application/epub+zip"));

    // Verify ZipArchive parsing
    ZipArchive zip(outPath);
    QVERIFY2(zip.open(), qPrintable(zip.errorString()));
    QCOMPARE(zip.names().first(), QStringLiteral("mimetype"));
    QCOMPARE(zip.read(QStringLiteral("mimetype")), QByteArrayLiteral("application/epub+zip"));

    const QByteArray opfBytes = zip.read(QStringLiteral("OEBPS/content.opf"));
    QVERIFY(opfBytes.contains("<dc:source>https://example.com/essays/calm-typography</dc:source>"));
    QVERIFY(opfBytes.contains("dcterms:modified"));
    QString xmlErr;
    QVERIFY2(isWellFormedXml(opfBytes, &xmlErr), qPrintable(xmlErr));
    QVERIFY2(isWellFormedXml(zip.read(QStringLiteral("OEBPS/nav.xhtml")), &xmlErr), qPrintable(xmlErr));
    QVERIFY2(isWellFormedXml(zip.read(QStringLiteral("OEBPS/toc.ncx")), &xmlErr), qPrintable(xmlErr));
    QVERIFY2(isWellFormedXml(zip.read(QStringLiteral("OEBPS/article.xhtml")), &xmlErr), qPrintable(xmlErr));
    QVERIFY2(isWellFormedXml(zip.read(QStringLiteral("OEBPS/cover.svg")), &xmlErr), qPrintable(xmlErr));

    // Verify full round-trip through Clothbound's EpubBook
    EpubBook book(outPath);
    QVERIFY2(book.open(), qPrintable(book.errorString()));
    QVERIFY(book.errorString().isEmpty());
    QCOMPARE(book.version(), QStringLiteral("3.0"));
    QCOMPARE(book.metadata().title, QStringLiteral("On Calm Typography & Paper"));
    QCOMPARE(book.metadata().authors, QStringList{QStringLiteral("Ada Lovelace")});
    QCOMPARE(book.metadata().publisher, QStringLiteral("Analytical Journal"));
    QCOMPARE(book.metadata().date, QStringLiteral("2026-04-12"));
    QCOMPARE(book.metadata().language, QStringLiteral("en-GB"));
    QCOMPARE(book.metadata().description, QStringLiteral("Why quiet margins make long-form reading enduring."));
    QVERIFY(book.metadata().subjects.contains(QStringLiteral("Clothbound Article")));
    QVERIFY(book.metadata().subjects.contains(QStringLiteral("Typography")));
    QVERIFY(book.metadata().subjects.contains(QStringLiteral("Design")));

    QCOMPARE(book.spine().size(), 1);
    QCOMPARE(book.spine().first().path, QStringLiteral("OEBPS/article.xhtml"));
    QCOMPARE(book.toc().size(), 3);
    QCOMPARE(book.toc()[0].title, QStringLiteral("On Calm Typography & Paper"));
    QCOMPARE(book.toc()[1].title, QStringLiteral("Margins & Rhythm"));
    QCOMPARE(book.toc()[1].fragment, QStringLiteral("sec-1"));
    QCOMPARE(book.toc()[2].title, QStringLiteral("Optical Scale"));
    QCOMPARE(book.toc()[2].fragment, QStringLiteral("sec-2"));

    QCOMPARE(book.coverPath(), QStringLiteral("OEBPS/cover.svg"));
    QString coverType;
    const QByteArray coverBytes = book.coverImage(&coverType);
    QCOMPARE(coverType, QStringLiteral("image/svg+xml"));
    QVERIFY(coverBytes.contains("<svg"));

    QString imgType;
    const QByteArray imgBytes = book.resource(QStringLiteral("OEBPS/images/img-001.png"), &imgType);
    QCOMPARE(imgType, QStringLiteral("image/png"));
    QCOMPARE(imgBytes, samplePng());

    const QString text = book.chapterText(0);
    QVERIFY(text.contains(QStringLiteral("clothbound volume")));
    QVERIFY(text.contains(QStringLiteral("Good typography is invisible.")));
}

void TestOnlineIngest::articleExtractorCleanAndSanitize() {
    const QByteArray html = R"HTML(<!doctype html>
<html lang="en-US">
<head>
  <meta charset="utf-8">
  <title>The Architecture of Calm Reading | Quiet Press</title>
  <meta property="og:title" content="The Architecture of Calm Reading | Quiet Press">
  <meta property="og:site_name" content="Quiet Press">
  <meta property="article:published_time" content="2026-02-19T09:30:00Z">
  <meta name="description" content="Designing distraction-free reading tools.">
  <meta property="og:image" content="/assets/lead.jpg">
  <link rel="canonical" href="https://quietpress.example/essays/calm-reading">
  <script type="application/ld+json">
  {
    "@context": "https://schema.org",
    "@type": "Article",
    "headline": "The Architecture of Calm Reading",
    "author": { "@type": "Person", "name": "Grace Hopper" }
  }
  </script>
  <style>.hero { color: red; }</style>
</head>
<body>
  <nav class="top-nav"><h1>Quiet Press Logo</h1><a href="/home">Home</a><a href="/about">About</a></nav>
  <div class="cookie-banner">We use cookies to track everything. <button>Accept</button></div>
  <div class="layout">
    <aside class="sidebar"><h3>Trending</h3><p>Click here for 10 tricks.</p></aside>
    <article class="essay-body">
      <h1>The Architecture of Calm Reading</h1>
      <p onclick="alert('xss')" onmouseover="evil()">Reading deeply requires silence, steady margins, and typography that gets out of the way of thought. Every ornament removed returns attention to the sentence.</p>
      <h2 id="orig-sec">Why Offline Matters</h2>
      <p>Links rot and pages shift, yet an <a href="/guides/epub" onclick="track()">EPUB archive</a> preserves the essay intact, whereas <a href="javascript:alert(1)">unsafe scripts</a> must never survive sanitization.</p>
      <figure>
        <img src="../media/diagram.png" alt="Reading flow &amp; margins" onerror="steal()"/>
        <figcaption>Figure 1: Reading flow.</figcaption>
      </figure>
      <img src="https://tracker.example/1x1.gif" width="1" height="1" alt=""/>
      <img src="data:image/svg+xml,<svg onload=alert(1)></svg>" alt="bad"/>
      <blockquote><p>Simplicity is prerequisite for reliability.</p></blockquote>
      <pre><code>if (a &lt; b &amp;&amp; b &gt; 0) { return a; }</code></pre>
      <div class="share-bar"><a href="https://social.example/share">Share on Social</a></div>
      <div id="comments" class="comment-section"><p>First! Great post!</p></div>
    </article>
  </div>
  <footer>Copyright 2026 Quiet Press</footer>
  <script>window.pwned = true;</script>
</body>
</html>)HTML";

    const QUrl pageUrl(QStringLiteral("https://quietpress.example/essays/calm-reading?ref=hn"));
    const ExtractedArticle art = ArticleExtractor::extract(html, pageUrl);

    QCOMPARE(art.title, QStringLiteral("The Architecture of Calm Reading"));
    QCOMPARE(art.author, QStringLiteral("Grace Hopper"));
    QCOMPARE(art.siteName, QStringLiteral("Quiet Press"));
    QCOMPARE(art.publishedDate, QStringLiteral("2026-02-19T09:30:00Z"));
    QCOMPARE(art.description, QStringLiteral("Designing distraction-free reading tools."));
    QCOMPARE(art.canonicalUrl, QUrl(QStringLiteral("https://quietpress.example/essays/calm-reading")));
    QCOMPARE(art.language, QStringLiteral("en-US"));
    QCOMPARE(art.leadImageUrl, QUrl(QStringLiteral("https://quietpress.example/assets/lead.jpg")));
    QVERIFY(art.wordCount >= 40);
    QCOMPARE(art.readingMinutes, 1);

    // Non-content stripped
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("We use cookies")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("Trending")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("Share on Social")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("First! Great post!")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("Copyright 2026")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("window.pwned")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("onclick")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("onmouseover")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("onerror")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("javascript:")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("data:image")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("1x1.gif")));

    // Real article content preserved & links/images resolved
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("https://quietpress.example/guides/epub")));
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("unsafe scripts")));
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("<blockquote>")));
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("<pre><code>")));
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("images/img-001.png")));

    QCOMPARE(art.images.size(), 1);
    QCOMPARE(art.images.first().sourceUrl, QUrl(QStringLiteral("https://quietpress.example/media/diagram.png")));
    QCOMPARE(art.images.first().localPath, QStringLiteral("images/img-001.png"));

    QCOMPARE(art.headings.size(), 1);
    QCOMPARE(art.headings.first().title, QStringLiteral("Why Offline Matters"));
    QCOMPARE(art.headings.first().id, QStringLiteral("sec-1"));

    // Strictly well-formed XML
    QString xmlErr;
    const QByteArray wrapped = ("<root>" + art.bodyXhtml + "</root>").toUtf8();
    QVERIFY2(isWellFormedXml(wrapped, &xmlErr), qPrintable(xmlErr + "\n" + art.bodyXhtml));

    // finalizeImages rewrites bundled images and drops missing ones cleanly
    const QString rewritten = ArticleExtractor::finalizeImages(
        art.bodyXhtml, {{QStringLiteral("images/img-001.png"), QStringLiteral("images/img-001.webp")}});
    QVERIFY(rewritten.contains(QStringLiteral("images/img-001.webp")));
    const QString dropped = ArticleExtractor::finalizeImages(art.bodyXhtml, {});
    QVERIFY(!dropped.contains(QStringLiteral("<img")));
    QVERIFY(!dropped.contains(QStringLiteral("<figure>")));
    QVERIFY2(isWellFormedXml(("<root>" + dropped + "</root>").toUtf8(), &xmlErr), qPrintable(xmlErr));
}

void TestOnlineIngest::articleExtractorScoresProseContainer() {
    // No <article> or <main> tag: scoring must select the dense prose div over a link-heavy menu div.
    const QByteArray html = R"HTML(<!doctype html>
<html><head><title>Long-Form Craft - Field Notes</title></head>
<body>
  <div class="links-directory">
    <p><a href="/1">Link One</a> <a href="/2">Link Two</a> <a href="/3">Link Three</a> <a href="/4">Link Four</a></p>
    <p><a href="/5">Link Five</a> <a href="/6">Link Six</a> <a href="/7">Link Seven</a> <a href="/8">Link Eight</a></p>
    <p><a href="/9">Link Nine</a> <a href="/10">Link Ten</a> <a href="/11">Link Eleven</a> <a href="/12">Link Twelve</a></p>
  </div>
  <div class="post-content">
    <p>Good tools reward quiet persistence, careful measurement, and honest materials that age gracefully with daily use.</p>
    <p>When a reader opens a long essay, the page should settle immediately into an unhurried rhythm of line length, spacing, and contrast.</p>
    <p>By stripping away banners, popups, and link farms, the original voice of the writer returns to center stage.</p>
  </div>
</body></html>)HTML";

    const ExtractedArticle art = ArticleExtractor::extract(html, QUrl(QStringLiteral("https://fieldnotes.example/craft")));
    QCOMPARE(art.title, QStringLiteral("Long-Form Craft"));
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("quiet persistence")));
    QVERIFY(art.bodyXhtml.contains(QStringLiteral("unhurried rhythm")));
    QVERIFY(!art.bodyXhtml.contains(QStringLiteral("Link Eleven")));
}

void TestOnlineIngest::webImporterParsingHelpers() {
    // arXiv ID parsing
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("https://arxiv.org/abs/2301.07041v2")),
             QStringLiteral("2301.07041v2"));
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("https://arxiv.org/pdf/2301.07041.pdf")),
             QStringLiteral("2301.07041"));
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("https://arxiv.org/html/2301.07041")),
             QStringLiteral("2301.07041"));
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("arxiv:2301.07041")), QStringLiteral("2301.07041"));
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("arXiv:hep-th/9901001")), QStringLiteral("hep-th/9901001"));
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("https://arxiv.org/abs/hep-th/9901001")),
             QStringLiteral("hep-th/9901001"));
    QCOMPARE(WebImporter::parseArxivId(QStringLiteral("https://arxiv.org/abs/math.GT/0309136v1")),
             QStringLiteral("math.GT/0309136v1"));
    QVERIFY(WebImporter::parseArxivId(QStringLiteral("https://example.com/abs/2301.07041")).isEmpty());
    QVERIFY(WebImporter::parseArxivId(QStringLiteral("not an arxiv id")).isEmpty());

    // URL kind detection
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("https://arxiv.org/abs/2301.07041")), QStringLiteral("arxiv"));
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("arxiv:2301.07041v2")), QStringLiteral("arxiv"));
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("https://example.org/books/odyssey.epub")),
             QStringLiteral("epub"));
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("https://example.org/papers/report.PDF?dl=1")),
             QStringLiteral("pdf"));
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("https://example.org/essays/reading")),
             QStringLiteral("article"));
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("file:///tmp/a.epub")), QString());
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("ftp://example.org/a.pdf")), QString());
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("javascript:alert(1)")), QString());
    QCOMPARE(WebImporter::detectUrlKind(QStringLiteral("plain text")), QString());

    // arXiv Atom XML parsing
    const QByteArray atomXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <id>http://arxiv.org/abs/2301.07041v2</id>
    <updated>2023-02-01T12:00:00Z</updated>
    <published>2023-01-17T18:00:00Z</published>
    <title>Calm Typography
      in Digital Systems</title>
    <summary>  We investigate long-form digital reading comfort
      across high-DPI displays.  </summary>
    <author><name>Ada Lovelace</name></author>
    <author><name>Charles Babbage</name></author>
    <link href="http://arxiv.org/abs/2301.07041v2" rel="alternate" type="text/html"/>
    <link title="pdf" href="http://arxiv.org/pdf/2301.07041v2" rel="related" type="application/pdf"/>
    <category term="cs.HC" scheme="http://arxiv.org/schemas/atom"/>
  </entry>
</feed>)XML";

    const QVariantMap meta = WebImporter::parseArxivAtom(atomXml);
    QCOMPARE(meta.value(QStringLiteral("title")).toString(), QStringLiteral("Calm Typography in Digital Systems"));
    QCOMPARE(meta.value(QStringLiteral("firstAuthor")).toString(), QStringLiteral("Ada Lovelace"));
    QCOMPARE(meta.value(QStringLiteral("authors")).toStringList(),
             (QStringList{QStringLiteral("Ada Lovelace"), QStringLiteral("Charles Babbage")}));
    QCOMPARE(meta.value(QStringLiteral("abstract")).toString(),
             QStringLiteral("We investigate long-form digital reading comfort across high-DPI displays."));
    QCOMPARE(meta.value(QStringLiteral("year")).toInt(), 2023);
    QCOMPARE(meta.value(QStringLiteral("categories")).toStringList(), QStringList{QStringLiteral("cs.HC")});
    QVERIFY(WebImporter::parseArxivAtom(QByteArrayLiteral("<not-xml")).isEmpty());
}

void TestOnlineIngest::webImporterEndToEndArticleWithImage() {
    LocalHttpServer server;
    const QString origin = server.origin();
    const QByteArray articleHtml =
        QByteArrayLiteral(
            "<!doctype html><html lang=\"en\"><head>"
            "<meta property=\"og:title\" content=\"Notes on Bookbinding\">"
            "<meta name=\"author\" content=\"William Morris\">"
            "<meta property=\"og:site_name\" content=\"Kelmscott Notes\">"
            "<link rel=\"canonical\" href=\"") +
        origin.toUtf8() +
        QByteArrayLiteral(
            "/essays/bookbinding\">"
            "</head><body>"
            "<nav><a href=\"/\">Home</a></nav>"
            "<article>"
            "<h1>Notes on Bookbinding</h1>"
            "<p>A well-bound book opens flat in the hand and invites slow, uninterrupted attention across decades of reading.</p>"
            "<h2>Thread and Linen</h2>"
            "<p>Smyth-sewn signatures stay supple while cheap adhesive spines crack after a single winter.</p>"
            "<figure><img src=\"/images/binding.png\" alt=\"Stitched spine\"/></figure>"
            "<figure><img src=\"/images/missing.png\" alt=\"Broken image\"/></figure>"
            "</article></body></html>");

    server.routes.insert("/essays/bookbinding", {200, "text/html; charset=utf-8", {}, articleHtml, false});
    server.routes.insert("/images/binding.png", {200, "image/png", {}, samplePng(), false});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY2(library.open(), qPrintable(library.errorString()));
    const int colId = library.createCollection(QStringLiteral("Essays"));
    QVERIFY(colId > 0);

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);

    int bookId = 0;
    QString kind, error;
    const QString url = origin + QStringLiteral("/essays/bookbinding");
    QVERIFY2(importer.importUrlSync(url, &bookId, &kind, &error, colId), qPrintable(error));

    QVERIFY(bookId > 0);
    QCOMPARE(kind, QStringLiteral("article"));
    QVERIFY(library.isInCollection(bookId, colId));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("content_kind")), QStringLiteral("article"));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("source_url")), url);

    const BookRecord rec = library.record(bookId);
    QCOMPARE(rec.format, QStringLiteral("epub"));
    QCOMPARE(rec.title, QStringLiteral("Notes on Bookbinding"));
    QCOMPARE(rec.author, QStringLiteral("William Morris"));
    QVERIFY(rec.path.contains(QStringLiteral("/articles/")));
    QVERIFY(QFileInfo::exists(rec.path));
    QVERIFY(!rec.cover.isEmpty());
    QVERIFY(QFileInfo(QDir(library.coverDirectory()).filePath(rec.cover)).size() > 0);

    // Verify generated offline EPUB opens cleanly in EpubBook with bundled image & missing image dropped.
    EpubBook epub(rec.path);
    QVERIFY2(epub.open(), qPrintable(epub.errorString()));
    QCOMPARE(epub.metadata().title, QStringLiteral("Notes on Bookbinding"));
    QVERIFY(epub.metadata().subjects.contains(QStringLiteral("Clothbound Article")));
    QCOMPARE(epub.resource(QStringLiteral("OEBPS/images/img-001.png")), samplePng());
    const QByteArray xhtml = epub.resource(QStringLiteral("OEBPS/article.xhtml"));
    QVERIFY(xhtml.contains("images/img-001.png"));
    QVERIFY(!xhtml.contains("missing.png"));
    QVERIFY(epub.chapterText(0).contains(QStringLiteral("Smyth-sewn signatures")));
}

void TestOnlineIngest::webImporterEndToEndDirectEpub() {
    LocalHttpServer server;
    const QString origin = server.origin();
    const QByteArray epubBytes = TestBooks::epub2(samplePng());
    server.routes.insert("/books/lantern.epub", {200, "application/epub+zip", {}, epubBytes, false});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);

    int bookId = 0;
    QString kind, error;
    const QString url = origin + QStringLiteral("/books/lantern.epub");
    QVERIFY2(importer.importUrlSync(url, &bookId, &kind, &error), qPrintable(error));

    QVERIFY(bookId > 0);
    QCOMPARE(kind, QStringLiteral("epub"));
    const BookRecord rec = library.record(bookId);
    QCOMPARE(rec.format, QStringLiteral("epub"));
    QCOMPARE(rec.title, QStringLiteral("The Lantern Keeper"));
    QVERIFY(rec.path.contains(QStringLiteral("/downloads/")));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("source_url")), url);
}

void TestOnlineIngest::webImporterEndToEndPdfWithRedirect() {
    LocalHttpServer server;
    const QString origin = server.origin();
    const QByteArray pdfBytes = TestBooks::pdf("On Signal Processing", "Claude Shannon");

    server.routes.insert(
        "/download/latest",
        {302, "text/plain", {{"Location", "/files/signal-processing.pdf"}}, "Redirecting", false});
    server.routes.insert(
        "/files/signal-processing.pdf",
        {200, "application/pdf", {}, pdfBytes, false});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);

    int bookId = 0;
    QString kind, error;
    QVERIFY2(importer.importUrlSync(origin + QStringLiteral("/download/latest"), &bookId, &kind, &error),
             qPrintable(error));

    QVERIFY(bookId > 0);
    QCOMPARE(kind, QStringLiteral("pdf"));
    const BookRecord rec = library.record(bookId);
    QCOMPARE(rec.format, QStringLiteral("pdf"));
    QVERIFY(rec.path.contains(QStringLiteral("/downloads/")));
    QVERIFY(rec.path.endsWith(QStringLiteral(".pdf")));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("source_url")),
             origin + QStringLiteral("/files/signal-processing.pdf"));
}

void TestOnlineIngest::webImporterEndToEndArxiv() {
    LocalHttpServer server;
    const QString origin = server.origin();
    const QByteArray atomXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <id>http://arxiv.org/abs/2301.07041</id>
    <published>2023-01-17T18:00:00Z</published>
    <title>Structured Attention Paper</title>
    <summary>A concise abstract about structured attention.</summary>
    <author><name>Alan Turing</name></author>
  </entry>
</feed>)XML";
    const QByteArray pdfBytes = TestBooks::pdf("Structured Attention Paper", "Alan Turing");

    server.routes.insert("/api/query?id_list=2301.07041", {200, "application/atom+xml", {}, atomXml, false});
    server.routes.insert("/pdf/2301.07041.pdf", {200, "application/pdf", {}, pdfBytes, false});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);
    importer.setArxivEndpointsForTests(origin + QStringLiteral("/api/query?id_list="),
                                       origin + QStringLiteral("/pdf/"));

    int bookId = 0;
    QString kind, error;
    QVERIFY2(importer.importUrlSync(QStringLiteral("https://arxiv.org/abs/2301.07041"), &bookId, &kind, &error),
             qPrintable(error));

    QVERIFY(bookId > 0);
    QCOMPARE(kind, QStringLiteral("arxiv"));
    const BookRecord rec = library.record(bookId);
    QCOMPARE(rec.format, QStringLiteral("pdf"));
    QCOMPARE(QFileInfo(rec.path).fileName(), QStringLiteral("Structured Attention Paper - Alan Turing (2023).pdf"));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("arxiv_id")), QStringLiteral("2301.07041"));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("source_url")),
             QStringLiteral("https://arxiv.org/abs/2301.07041"));
    QCOMPARE(library.bookSetting(bookId, QStringLiteral("arxiv_abstract")),
             QStringLiteral("A concise abstract about structured attention."));
}

void TestOnlineIngest::webImporterRejectsForbiddenRedirect() {
    LocalHttpServer server;
    const QString origin = server.origin();
    server.routes.insert("/to-file",
                         {302, "text/plain", {{"Location", "file:///etc/passwd"}}, "nope", false});
    server.routes.insert("/to-js",
                         {301, "text/plain", {{"Location", "javascript:alert(1)"}}, "nope", false});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);

    int bookId = 0;
    QString kind, error;
    QVERIFY(!importer.importUrlSync(origin + QStringLiteral("/to-file"), &bookId, &kind, &error));
    QCOMPARE(bookId, 0);
    QVERIFY(!error.isEmpty());
    QCOMPARE(library.bookCount(), 0);

    QVERIFY(!importer.importUrlSync(origin + QStringLiteral("/to-js"), &bookId, &kind, &error));
    QCOMPARE(bookId, 0);
    QVERIFY(!error.isEmpty());
    QCOMPARE(library.bookCount(), 0);
}

void TestOnlineIngest::webImporterRejectsExcessiveRedirects() {
    LocalHttpServer server;
    const QString origin = server.origin();
    for (int i = 0; i <= UrlGuard::maxRedirects; ++i) {
        const QByteArray from = "/hop-" + QByteArray::number(i);
        const QByteArray to = "/hop-" + QByteArray::number(i + 1);
        server.routes.insert(from, {302, "text/plain", {{"Location", to}}, "next", false});
    }
    server.routes.insert("/hop-" + QByteArray::number(UrlGuard::maxRedirects + 1),
                         {200, "text/html", {}, "<html><body><p>Too late</p></body></html>", false});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);

    int bookId = 0;
    QString kind, error;
    QVERIFY(!importer.importUrlSync(origin + QStringLiteral("/hop-0"), &bookId, &kind, &error));
    QCOMPARE(bookId, 0);
    QVERIFY(error.contains(QStringLiteral("redirect"), Qt::CaseInsensitive));
    QCOMPARE(library.bookCount(), 0);
}

void TestOnlineIngest::webImporterRejectsOversizedPayload() {
    LocalHttpServer server;
    const QString origin = server.origin();
    // Stream > maxHtmlBytes (10 MiB + 128 KiB) WITHOUT Content-Length so incremental
    // readyRead byte-cap enforcement aborts the transfer.
    const QByteArray hugeHtml = QByteArrayLiteral("<html><body><article><p>") +
                                QByteArray(int(UrlGuard::maxHtmlBytes + 128 * 1024), 'a') +
                                QByteArrayLiteral("</p></article></body></html>");
    server.routes.insert("/huge", {200, "text/html; charset=utf-8", {}, hugeHtml, true});

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Library library(tempDir.filePath(QStringLiteral("data")), tempDir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    WebImporter importer(&library, tempDir.filePath(QStringLiteral("storage")));
    importer.setAllowLocalNetworkForTests(true);

    int bookId = 0;
    QString kind, error;
    QVERIFY(!importer.importUrlSync(origin + QStringLiteral("/huge"), &bookId, &kind, &error));
    QCOMPARE(bookId, 0);
    QVERIFY(error.contains(QStringLiteral("size"), Qt::CaseInsensitive));
    QCOMPARE(library.bookCount(), 0);
}

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QTemporaryDir testXdg;
    if (testXdg.isValid()) {
        QDir(testXdg.path()).mkpath(QStringLiteral("config"));
        QDir(testXdg.path()).mkpath(QStringLiteral("data"));
        QDir(testXdg.path()).mkpath(QStringLiteral("cache"));
        qputenv("XDG_CONFIG_HOME", testXdg.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", testXdg.filePath(QStringLiteral("data")).toUtf8());
        qputenv("XDG_CACHE_HOME", testXdg.filePath(QStringLiteral("cache")).toUtf8());
    }
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clothbound"));
    TestOnlineIngest tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_online_ingest.moc"
