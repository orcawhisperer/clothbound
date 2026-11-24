#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QImage>
#include <QMap>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>

#include "ai/keystore.h"
#include "app/theme.h"
#include "app/translator.h"
#include "library/library.h"
#include "online/opdsclient.h"
#include "online/opdsparser.h"
#include "testbooks.h"

namespace {

// Flexible local HTTP server for end-to-end OPDS tests (supports custom headers,
// redirects, HTTP 401 challenges, and exact request header recording).
class OpdsTestServer {
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QMap<QByteArray, QByteArray> headers;
    };

    struct Response {
        int status = 200;
        QByteArray contentType = "application/atom+xml; charset=utf-8";
        QList<QPair<QByteArray, QByteArray>> extraHeaders;
        QByteArray body;
    };

    using Handler = std::function<Response(const Request &)>;

    OpdsTestServer() {
        m_server.listen(QHostAddress::LocalHost);
        QPointer<QTcpServer> guard(&m_server);
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this, guard] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, guard, socket, buffer] {
                    if (!guard)
                        return;
                    *buffer += socket->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;

                    Request req;
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    req.method = first.value(0);
                    req.path = first.value(1);
                    for (const QByteArray &line : lines.mid(1)) {
                        const qsizetype colon = line.indexOf(':');
                        if (colon > 0)
                            req.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                    requests.append(req);

                    Response resp{404, "text/plain", {}, "Not found"};
                    if (handler)
                        resp = handler(req);

                    QByteArray out = "HTTP/1.1 " + QByteArray::number(resp.status) + " Status\r\n";
                    out += "Content-Type: " + resp.contentType + "\r\n";
                    bool hasLength = false;
                    for (const auto &[k, v] : resp.extraHeaders) {
                        if (k.compare("Content-Length", Qt::CaseInsensitive) == 0)
                            hasLength = true;
                        out += k + ": " + v + "\r\n";
                    }
                    if (!hasLength)
                        out += "Content-Length: " + QByteArray::number(resp.body.size()) + "\r\n";
                    out += "Connection: close\r\n\r\n";
                    out += resp.body;
                    socket->write(out);
                    socket->flush();
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    ~OpdsTestServer() { m_server.close(); }

    quint16 port() const { return m_server.serverPort(); }
    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    QString url(const QString &path) const { return origin() + path; }

    Handler handler;
    QList<Request> requests;

private:
    QTcpServer m_server;
};

QByteArray samplePng() {
    QImage img(16, 24, QImage::Format_RGB32);
    img.fill(QColor("#2c4c6e"));
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

}  // namespace

class OpdsTests : public QObject {
    Q_OBJECT

private slots:
    void opds1NavigationAndAcquisitionParsing();
    void openSearchTemplateParsingAndExpansion();
    void opds2JsonFeedParsingAndAutoDetection();
    void clientBuiltInsCustomServersAndKeyringSecurity();
    void endToEndBrowseSearchAuthCrossOriginRedirectAndDownload();
    void catalogsSheetQmlSmokeTest();
};

void OpdsTests::opds1NavigationAndAcquisitionParsing() {
        const QUrl base(QStringLiteral("https://catalog.example.org/opds/root.xml"));
        const QByteArray navXml = R"OPDS(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <id>urn:catalog:root</id>
  <title>Example Public Catalog</title>
  <subtitle>&lt;p&gt;Free &amp;amp; open books.&lt;/p&gt;</subtitle>
  <link rel="self" type="application/atom+xml;profile=opds-catalog;kind=navigation" href="root.xml"/>
  <link rel="start" type="application/atom+xml;profile=opds-catalog;kind=navigation" href="/opds/root.xml"/>
  <link rel="search" type="application/opensearchdescription+xml" href="../opensearch.xml"/>
  <entry>
    <id>urn:nav:popular</id>
    <title>Popular Books</title>
    <content type="html">&lt;p&gt;Most downloaded &lt;b&gt;classics&lt;/b&gt;.&lt;/p&gt;</content>
    <link rel="subsection" type="application/atom+xml;profile=opds-catalog;kind=acquisition" href="popular.xml"/>
  </entry>
  <entry>
    <id>urn:nav:evil</id>
    <title>Evil Link</title>
    <link rel="subsection" type="application/atom+xml" href="javascript:alert(1)"/>
  </entry>
</feed>)OPDS";

        const OpdsFeed navFeed = OpdsParser::parse(navXml, base, QStringLiteral("application/atom+xml"));
        QVERIFY(navFeed.isValid());
        QCOMPARE(navFeed.format, OpdsFormat::Opds1Atom);
        QCOMPARE(navFeed.id, QStringLiteral("urn:catalog:root"));
        QCOMPARE(navFeed.title, QStringLiteral("Example Public Catalog"));
        QCOMPARE(navFeed.subtitle, QStringLiteral("Free & open books."));
        QCOMPARE(navFeed.selfUrl, QStringLiteral("https://catalog.example.org/opds/root.xml"));
        QCOMPARE(navFeed.startUrl, QStringLiteral("https://catalog.example.org/opds/root.xml"));
        QCOMPARE(navFeed.searchUrl, QStringLiteral("https://catalog.example.org/opensearch.xml"));
        QVERIFY(!navFeed.searchTemplated);

        QCOMPARE(navFeed.entries.size(), 2);
        const OpdsEntry &popular = navFeed.entries[0];
        QVERIFY(popular.isNavigation);
        QCOMPARE(popular.title, QStringLiteral("Popular Books"));
        QCOMPARE(popular.summary, QStringLiteral("Most downloaded classics."));
        QCOMPARE(popular.navigationUrl, QStringLiteral("https://catalog.example.org/opds/popular.xml"));

        // Malicious javascript: link must be rejected.
        QVERIFY(navFeed.entries[1].navigationUrl.isEmpty());
        QVERIFY(!navFeed.entries[1].isNavigation);

        // Acquisition feed with multiple formats, covers, pricing, next pagination, and direct templated search.
        const QByteArray acqXml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"
      xmlns:dcterms="http://purl.org/dc/terms/"
      xmlns:opds="http://opds-spec.org/2010/catalog">
  <id>urn:catalog:popular</id>
  <title>Popular Books</title>
  <link rel="self" href="/opds/popular.xml" type="application/atom+xml"/>
  <link rel="up" href="/opds/root.xml" type="application/atom+xml"/>
  <link rel="next" href="popular.xml?page=2" type="application/atom+xml"/>
  <link rel="search" href="/opds/search?q={searchTerms}" type="application/atom+xml"/>
  <entry>
    <id>urn:book:1</id>
    <title>The Lantern Keeper</title>
    <author><name>Ada Marsh</name></author>
    <author><name>Tom Reed</name></author>
    <dcterms:issued>1911-04-02</dcterms:issued>
    <category term="fic" label="Fiction"/>
    <category term="Maritime"/>
    <summary type="html">&lt;p&gt;A keeper &amp;amp; her &lt;em&gt;beacon&lt;/em&gt;.&lt;/p&gt;&lt;p&gt;Second line.&lt;/p&gt;</summary>
    <link rel="http://opds-spec.org/image" type="image/jpeg" href="/covers/lantern.jpg"/>
    <link rel="http://opds-spec.org/image/thumbnail" type="image/jpeg" href="/covers/lantern-sm.jpg"/>
    <link rel="http://opds-spec.org/acquisition" type="application/x-mobipocket-ebook" href="/dl/lantern.mobi"/>
    <link rel="http://opds-spec.org/acquisition/buy" type="application/vnd.adobe.adept+xml" href="/dl/lantern.acsm"/>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/pdf" href="/dl/lantern.pdf"/>
    <link rel="http://opds-spec.org/acquisition/buy" type="application/epub+zip" href="/dl/lantern-paid.epub">
      <opds:price currencycode="USD">4.99</opds:price>
    </link>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/epub+zip" href="/dl/lantern.epub"/>
  </entry>
  <entry>
    <id>urn:book:2</id>
    <title>Cover Rel Fallback</title>
    <published>2023-06-15T00:00:00Z</published>
    <link rel="cover" type="image/png" href="covers/fallback.png"/>
    <link rel="http://opds-spec.org/acquisition" type="application/pdf" href="dl/doc.pdf"/>
  </entry>
</feed>)";

        const OpdsFeed acqFeed = OpdsParser::parse(acqXml, QUrl(QStringLiteral("https://catalog.example.org/opds/popular.xml")));
        QVERIFY(acqFeed.isValid());
        QCOMPARE(acqFeed.upUrl, QStringLiteral("https://catalog.example.org/opds/root.xml"));
        QCOMPARE(acqFeed.nextUrl, QStringLiteral("https://catalog.example.org/opds/popular.xml?page=2"));
        QCOMPARE(acqFeed.searchUrl, QStringLiteral("https://catalog.example.org/opds/search?q={searchTerms}"));
        QVERIFY(acqFeed.searchTemplated);

        QCOMPARE(acqFeed.entries.size(), 2);
        const OpdsEntry &b1 = acqFeed.entries[0];
        QVERIFY(!b1.isNavigation);
        QCOMPARE(b1.author, QStringLiteral("Ada Marsh, Tom Reed"));
        QCOMPARE(b1.year, 1911);
        QCOMPARE(b1.categories, (QStringList{QStringLiteral("Fiction"), QStringLiteral("Maritime")}));
        QCOMPARE(b1.summary, QStringLiteral("A keeper & her beacon. Second line."));
        QCOMPARE(b1.coverUrl, QStringLiteral("https://catalog.example.org/covers/lantern.jpg"));
        QCOMPARE(b1.thumbnailUrl, QStringLiteral("https://catalog.example.org/covers/lantern-sm.jpg"));

        // MOBI and ACSM DRM filtered out; open-access EPUB ranked first, paid EPUB second, open-access PDF third.
        QCOMPARE(b1.acquisitions.size(), 3);
        QCOMPARE(b1.acquisitions[0].format, QStringLiteral("epub"));
        QCOMPARE(b1.acquisitions[0].href, QStringLiteral("https://catalog.example.org/dl/lantern.epub"));
        QVERIFY(b1.acquisitions[0].openAccess);
        QCOMPARE(b1.acquisitions[1].format, QStringLiteral("epub"));
        QCOMPARE(b1.acquisitions[1].price, QStringLiteral("USD 4.99"));
        QVERIFY(!b1.acquisitions[1].openAccess);
        QCOMPARE(b1.acquisitions[2].format, QStringLiteral("pdf"));
        QVERIFY(b1.acquisitions[2].openAccess);

        const QVariantMap vm = b1.toVariantMap();
        QCOMPARE(vm.value(QStringLiteral("primaryFormat")).toString(), QStringLiteral("epub"));
        QCOMPARE(vm.value(QStringLiteral("primaryAcquisitionUrl")).toString(),
                 QStringLiteral("https://catalog.example.org/dl/lantern.epub"));
        QCOMPARE(vm.value(QStringLiteral("openAccess")).toBool(), true);

        const OpdsEntry &b2 = acqFeed.entries[1];
        QCOMPARE(b2.year, 2023);
        QCOMPARE(b2.coverUrl, QStringLiteral("https://catalog.example.org/opds/covers/fallback.png"));
        QCOMPARE(b2.acquisitions.size(), 1);
        QCOMPARE(b2.acquisitions[0].format, QStringLiteral("pdf"));
    }

void OpdsTests::openSearchTemplateParsingAndExpansion() {
        const QByteArray osXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <ShortName>Catalog Search</ShortName>
  <Url type="text/html" template="/search?q={searchTerms}"/>
  <Url type="application/x-suggestions+json" rel="suggestions" template="/suggest?q={searchTerms}"/>
  <Url type="application/atom+xml;profile=opds-catalog" template="/opds/search?query={searchTerms}&amp;page={startPage?}"/>
</OpenSearchDescription>)";

        const QUrl osUrl(QStringLiteral("https://books.example.org/meta/opensearch.xml"));
        const QString tmpl = OpdsParser::parseOpenSearchTemplate(osXml, osUrl);
        QCOMPARE(tmpl, QStringLiteral("https://books.example.org/opds/search?query={searchTerms}&page={startPage?}"));

        const QString expanded = OpdsParser::expandSearchTemplate(tmpl, QStringLiteral("Moby Dick & Co"));
        QCOMPARE(expanded, QStringLiteral("https://books.example.org/opds/search?query=Moby%20Dick%20%26%20Co&page="));
    }

void OpdsTests::opds2JsonFeedParsingAndAutoDetection() {
        const QByteArray json = R"({
  "metadata": {
    "title": {"en": "Kavita OPDS 2.0"},
    "identifier": "urn:kavita:root"
  },
  "links": [
    {"rel": "self", "href": "/opds/v2", "type": "application/opds+json"},
    {"rel": "next", "href": "/opds/v2?page=2", "type": "application/opds+json"},
    {"rel": "search", "href": "/opds/v2/search{?query}", "templated": false},
    {"rel": "search", "href": "/opds/v2/search?q={searchTerms}", "type": "application/opds+json", "templated": true}
  ],
  "navigation": [
    {"title": "Recently Added", "href": "/opds/v2/recent", "type": "application/opds+json", "properties": {"numberOfItems": 12}}
  ],
  "publications": [
    {
      "metadata": {
        "title": "Salt Roads",
        "identifier": "urn:salt-roads",
        "author": [{"name": "Wen Lo"}, "Ira Bell"],
        "description": "<p>Across the salt flats.</p>",
        "published": "2021-09-10",
        "subject": [{"name": "Travel"}, "Essays"]
      },
      "images": [
        {"href": "/img/salt-full.jpg", "type": "image/jpeg", "width": 1200},
        {"href": "/img/salt-thumb.jpg", "type": "image/jpeg", "width": 240}
      ],
      "links": [
        {"rel": "http://opds-spec.org/acquisition", "href": "/dl/salt.pdf", "type": "application/pdf"},
        {"rel": ["http://opds-spec.org/acquisition/open-access"], "href": "/dl/salt.epub", "type": "application/epub+zip"}
      ]
    }
  ],
  "groups": [
    {
      "metadata": {"title": "Featured Classics"},
      "links": [{"rel": "self", "href": "/opds/v2/featured", "type": "application/opds+json"}],
      "publications": [
        {
          "metadata": {"title": "Meditations", "author": "Marcus Aurelius", "published": "0180"},
          "images": [{"href": "/img/meditations.png", "type": "image/png"}],
          "links": [{"rel": "http://opds-spec.org/acquisition", "href": "/dl/meditations.epub", "type": "application/epub+zip"}]
        }
      ]
    },
    {
      "metadata": {"title": "All Shelves", "numberOfItems": 4},
      "links": [{"rel": "self", "href": "/opds/v2/shelves", "type": "application/opds+json"}]
    }
  ]
})";

        // Auto-detect from payload prefix with empty Content-Type.
        QCOMPARE(OpdsParser::detectFormat(json), OpdsFormat::Opds2Json);
        const OpdsFeed feed = OpdsParser::parse(json, QUrl(QStringLiteral("https://kavita.lan:5000/opds/v2")));
        QVERIFY(feed.isValid());
        QCOMPARE(feed.format, OpdsFormat::Opds2Json);
        QCOMPARE(feed.title, QStringLiteral("Kavita OPDS 2.0"));
        QCOMPARE(feed.nextUrl, QStringLiteral("https://kavita.lan:5000/opds/v2?page=2"));
        QCOMPARE(feed.searchUrl, QStringLiteral("https://kavita.lan:5000/opds/v2/search?q={searchTerms}"));
        QVERIFY(feed.searchTemplated);

        // 1 nav + 1 top-level pub + 1 grouped pub + 1 link-only group nav = 4 entries.
        QCOMPARE(feed.entries.size(), 4);
        QVERIFY(feed.entries[0].isNavigation);
        QCOMPARE(feed.entries[0].title, QStringLiteral("Recently Added"));
        QCOMPARE(feed.entries[0].subtitle, QStringLiteral("12 items"));
        QCOMPARE(feed.entries[0].navigationUrl, QStringLiteral("https://kavita.lan:5000/opds/v2/recent"));

        const OpdsEntry &salt = feed.entries[1];
        QVERIFY(!salt.isNavigation);
        QCOMPARE(salt.title, QStringLiteral("Salt Roads"));
        QCOMPARE(salt.author, QStringLiteral("Wen Lo, Ira Bell"));
        QCOMPARE(salt.summary, QStringLiteral("Across the salt flats."));
        QCOMPARE(salt.year, 2021);
        QCOMPARE(salt.categories, (QStringList{QStringLiteral("Travel"), QStringLiteral("Essays")}));
        QCOMPARE(salt.coverUrl, QStringLiteral("https://kavita.lan:5000/img/salt-thumb.jpg"));
        QCOMPARE(salt.acquisitions.size(), 2);
        QCOMPARE(salt.acquisitions[0].format, QStringLiteral("epub"));
        QCOMPARE(salt.acquisitions[1].format, QStringLiteral("pdf"));

        const OpdsEntry &med = feed.entries[2];
        QCOMPARE(med.title, QStringLiteral("Meditations"));
        QCOMPARE(med.groupTitle, QStringLiteral("Featured Classics"));
        QCOMPARE(med.groupUrl, QStringLiteral("https://kavita.lan:5000/opds/v2/featured"));

        const OpdsEntry &shelves = feed.entries[3];
        QVERIFY(shelves.isNavigation);
        QCOMPARE(shelves.title, QStringLiteral("All Shelves"));
        QCOMPARE(shelves.navigationUrl, QStringLiteral("https://kavita.lan:5000/opds/v2/shelves"));

        // HTML payload returns calm error.
        const OpdsFeed htmlFeed = OpdsParser::parse("<!DOCTYPE html><html><body>Hi</body></html>",
                                                    QUrl(QStringLiteral("https://example.org/")));
        QVERIFY(!htmlFeed.isValid());
        QVERIFY(htmlFeed.error.contains(QStringLiteral("web page"), Qt::CaseInsensitive));
    }

void OpdsTests::clientBuiltInsCustomServersAndKeyringSecurity() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        Library lib(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(lib.open());
        MemoryKeyStore keys;
        const QString settingsPath = dir.filePath(QStringLiteral("settings.ini"));

        {
            OpdsClient client(&lib, &keys, dir.filePath(QStringLiteral("storage")), settingsPath);

            // 3 curated built-in catalogs seeded by default.
            const QVariantList cats = client.catalogs();
            QCOMPARE(cats.size(), 3);
            QCOMPARE(cats[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("gutenberg"));
            QCOMPARE(cats[0].toMap().value(QStringLiteral("url")).toString(),
                     QStringLiteral("https://www.gutenberg.org/ebooks/search.opds/"));
            QCOMPARE(cats[1].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("oapen"));
            QCOMPARE(cats[1].toMap().value(QStringLiteral("url")).toString(),
                     QStringLiteral("https://library.oapen.org/opds"));
            QCOMPARE(cats[2].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("standard-ebooks"));
            QCOMPARE(cats[2].toMap().value(QStringLiteral("url")).toString(),
                     QStringLiteral("https://standardebooks.org/feeds/opds"));
            QCOMPARE(cats[2].toMap().value(QStringLiteral("authOptional")).toBool(), true);
            QVERIFY(!cats[2].toMap().value(QStringLiteral("authHint")).toString().isEmpty());

            // Disallowed schemes rejected.
            QVERIFY(client.addCatalog(QStringLiteral("Bad File"), QStringLiteral("file:///etc/passwd")).isEmpty());
            QVERIFY(client.addCatalog(QStringLiteral("Bad FTP"), QStringLiteral("ftp://example.org/opds")).isEmpty());
            QVERIFY(client.addCatalog(QStringLiteral("Bad JS"), QStringLiteral("javascript:alert(1)")).isEmpty());
            QVERIFY(client.addCatalog(QStringLiteral("Bad Scheme"), QStringLiteral("clothbound://local/opds")).isEmpty());

            // LAN / localhost custom server allowed.
            const QString secretPass = QStringLiteral("UltraSecretOpdsPass!99");
            const QString customId = client.addCatalog(QStringLiteral("Home Calibre-Web"),
                                                       QStringLiteral("http://192.168.1.50:8083/opds"),
                                                       QStringLiteral("alice"),
                                                       secretPass);
            QVERIFY(!customId.isEmpty());
            QCOMPARE(client.catalogs().size(), 4);

            // Password MUST be stored in KeyStore under "opds:" + id, and NEVER in settings.ini!
            QCOMPARE(keys.load(QStringLiteral("opds:") + customId), secretPass);

            QFile iniFile(settingsPath);
            QVERIFY(iniFile.open(QIODevice::ReadOnly));
            const QByteArray rawIni = iniFile.readAll();
            QVERIFY(rawIni.contains("Home Calibre-Web"));
            QVERIFY(rawIni.contains("alice"));
            QVERIFY(!rawIni.contains(secretPass.toUtf8()));

            // Update credentials (change password, then clear password).
            QVERIFY(client.updateCatalogCredentials(customId, QStringLiteral("alice2"), QStringLiteral("NewPass42")));
            QCOMPARE(keys.load(QStringLiteral("opds:") + customId), QStringLiteral("NewPass42"));
            QVERIFY(client.updateCatalogCredentials(customId, QStringLiteral("alice2"), QString()));
            QVERIFY(keys.load(QStringLiteral("opds:") + customId).isEmpty());
            QVERIFY(client.updateCatalogCredentials(customId, QStringLiteral("alice2"), QStringLiteral("RestoredPass")));
            QCOMPARE(keys.load(QStringLiteral("opds:") + customId), QStringLiteral("RestoredPass"));

            // Hide built-in catalog without losing custom servers, then restore.
            QVERIFY(client.removeCatalog(QStringLiteral("oapen")));
            QCOMPARE(client.catalogs().size(), 3);
            QVERIFY(client.hasHiddenBuiltIns());
            client.restoreBuiltInCatalogs();
            QCOMPARE(client.catalogs().size(), 4);
            QVERIFY(!client.hasHiddenBuiltIns());
        }

        // Reload from settings.ini in a fresh OpdsClient instance and verify persistence + removal.
        {
            OpdsClient reloaded(&lib, &keys, dir.filePath(QStringLiteral("storage")), settingsPath);
            QCOMPARE(reloaded.catalogs().size(), 4);
            const QString persistedId = reloaded.catalogs()[3].toMap().value(QStringLiteral("id")).toString();
            QCOMPARE(reloaded.catalogs()[3].toMap().value(QStringLiteral("name")).toString(),
                     QStringLiteral("Home Calibre-Web"));
            QCOMPARE(reloaded.catalogs()[3].toMap().value(QStringLiteral("username")).toString(),
                     QStringLiteral("alice2"));

            // Removing custom server must also purge its secret from KeyStore.
            QVERIFY(reloaded.removeCatalog(persistedId));
            QCOMPARE(reloaded.catalogs().size(), 3);
            QVERIFY(keys.load(QStringLiteral("opds:") + persistedId).isEmpty());
        }
    }

void OpdsTests::endToEndBrowseSearchAuthCrossOriginRedirectAndDownload() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        Library lib(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(lib.open());
        const int colId = lib.createCollection(QStringLiteral("OPDS Picks"));
        QVERIFY(colId > 0);

        MemoryKeyStore keys;
        const QByteArray realEpub = TestBooks::epub2(samplePng());
        const QByteArray realPdf = TestBooks::pdf("Field Notes", "Clara Vance");

        OpdsTestServer primaryServer;
        OpdsTestServer cdnServer;

        const QByteArray expectedAuth = "Basic " + QByteArray("patron@example.com:topsecret").toBase64();
        int openSearchFetches = 0;

        cdnServer.handler = [&](const OpdsTestServer::Request &req) -> OpdsTestServer::Response {
            if (req.path == "/cdn/lantern.epub")
                return {200, "application/epub+zip", {}, realEpub};
            return {404, "text/plain", {}, "missing"};
        };

        primaryServer.handler = [&](const OpdsTestServer::Request &req) -> OpdsTestServer::Response {
            if (req.path == "/opds") {
                return {200, "application/atom+xml", {},
                        R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <id>urn:test:root</id>
  <title>Local OPDS Library</title>
  <link rel="search" type="application/opensearchdescription+xml" href="/opensearch.xml"/>
  <entry>
    <id>urn:sub:new</id>
    <title>New Arrivals</title>
    <content type="text">Fresh public domain books</content>
    <link rel="subsection" type="application/atom+xml" href="/opds/new?page=1"/>
  </entry>
  <entry>
    <id>urn:sub:members</id>
    <title>Members Only</title>
    <link rel="subsection" type="application/opds+json" href="/opds/protected"/>
  </entry>
</feed>)"};
            }
            if (req.path == "/opds/new?page=1") {
                return {200, "application/atom+xml", {},
                        R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <id>urn:test:new:1</id>
  <title>New Arrivals</title>
  <link rel="next" type="application/atom+xml" href="/opds/new?page=2"/>
  <entry>
    <id>urn:book:lantern</id>
    <title>The Lantern Keeper</title>
    <author><name>Ada Marsh</name></author>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/epub+zip" href="/dl/cross-origin-lantern"/>
  </entry>
</feed>)"};
            }
            if (req.path == "/opds/new?page=2") {
                return {200, "application/atom+xml", {},
                        R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <id>urn:test:new:2</id>
  <title>New Arrivals</title>
  <entry>
    <id>urn:book:notes</id>
    <title>Field Notes</title>
    <author><name>Clara Vance</name></author>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/pdf" href="/dl/same-origin-pdf"/>
  </entry>
</feed>)"};
            }
            if (req.path == "/opensearch.xml") {
                ++openSearchFetches;
                return {200, "application/opensearchdescription+xml", {},
                        R"(<?xml version="1.0" encoding="utf-8"?>
<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <Url type="application/atom+xml" template="/opds/search?q={searchTerms}"/>
</OpenSearchDescription>)"};
            }
            if (req.path.startsWith("/opds/search?q=")) {
                const QByteArray term = req.path.mid(15);
                return {200, "application/atom+xml", {},
                        QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <id>urn:search</id>
  <title>Search Results</title>
  <entry>
    <id>urn:search:hit</id>
    <title>Match for )") + term + R"(</title>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/dl/cross-origin-lantern"/>
  </entry>
</feed>)"};
            }
            if (req.path == "/opds/protected") {
                if (req.headers.value("authorization") != expectedAuth) {
                    return {401, "text/plain", {{"WWW-Authenticate", "Basic realm=\"Test\""}}, "Unauthorized"};
                }
                return {200, "application/opds+json", {},
                        R"({
  "metadata": {"title": "Members Shelf"},
  "publications": [
    {
      "metadata": {"title": "Patron Edition", "author": "Ada Marsh"},
      "links": [
        {"rel": "http://opds-spec.org/acquisition", "href": "/dl/cross-origin-lantern", "type": "application/epub+zip"}
      ]
    }
  ]
})"};
            }
            if (req.path == "/dl/cross-origin-lantern") {
                return {302, "text/html", {{"Location", cdnServer.url(QStringLiteral("/cdn/lantern.epub")).toUtf8()}},
                        "<html>Moved</html>"};
            }
            if (req.path == "/dl/same-origin-pdf") {
                return {302, "text/html", {{"Location", "/files/notes.pdf"}}, "<html>Moved</html>"};
            }
            if (req.path == "/files/notes.pdf") {
                return {200, "application/pdf", {}, realPdf};
            }
            if (req.path == "/dl/bad-magic.epub") {
                return {200, "application/epub+zip", {}, "<!DOCTYPE html><html><body>Login required</body></html>"};
            }
            if (req.path == "/dl/redirect-file") {
                return {302, "text/plain", {{"Location", "file:///etc/passwd"}}, ""};
            }
            if (req.path == "/dl/loop") {
                return {302, "text/plain", {{"Location", "/dl/loop"}}, ""};
            }
            return {404, "text/plain", {}, "Not found"};
        };

        OpdsClient client(&lib, &keys, dir.filePath(QStringLiteral("storage")),
                          dir.filePath(QStringLiteral("settings.ini")));

        const QString serverId = client.addCatalog(QStringLiteral("Test Local OPDS"),
                                                   primaryServer.url(QStringLiteral("/opds")));
        QVERIFY(!serverId.isEmpty());

        // 1. Open root catalog.
        client.openCatalog(serverId);
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QVERIFY(client.error().isEmpty());
        QCOMPARE(client.feedTitle(), QStringLiteral("Local OPDS Library"));
        QVERIFY(client.canSearch());
        QVERIFY(!client.canGoBack());
        QCOMPARE(client.entries().size(), 2);

        // 2. Navigate into subsection & test pagination (loadMore).
        const QString newUrl = client.entries()[0].toMap().value(QStringLiteral("navigationUrl")).toString();
        client.navigate(newUrl, QStringLiteral("New Arrivals"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QCOMPARE(client.breadcrumbs().size(), 2);
        QVERIFY(client.canGoBack());
        QVERIFY(client.canLoadMore());
        QCOMPARE(client.entries().size(), 1);

        client.loadMore();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMore(), 3000);
        QCOMPARE(client.entries().size(), 2);
        QVERIFY(!client.canLoadMore());

        // 3. Lazy OpenSearch discovery + search caching.
        client.search(QStringLiteral("lantern"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QCOMPARE(openSearchFetches, 1);
        QCOMPARE(client.entries().size(), 1);
        QCOMPARE(client.entries()[0].toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Match for lantern"));

        client.search(QStringLiteral("keeper"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QCOMPARE(openSearchFetches, 1);  // Cached template reused!
        QCOMPARE(client.entries()[0].toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Match for keeper"));

        // 4. Back / breadcrumb navigation.
        client.navigateBreadcrumb(0);
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QCOMPARE(client.breadcrumbs().size(), 1);
        QCOMPARE(client.feedTitle(), QStringLiteral("Local OPDS Library"));

        // 5. HTTP 401 clean auth prompt + authenticated retry.
        const QString protectedUrl = client.entries()[1].toMap().value(QStringLiteral("navigationUrl")).toString();
        client.navigate(protectedUrl, QStringLiteral("Members Only"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QVERIFY(client.authRequired());
        QVERIFY(client.error().isEmpty());

        QVERIFY(client.updateCatalogCredentials(serverId, QStringLiteral("patron@example.com"),
                                                QStringLiteral("topsecret")));
        client.retry();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QVERIFY(!client.authRequired());
        QVERIFY(client.error().isEmpty());
        QCOMPARE(client.feedTitle(), QStringLiteral("Members Shelf"));
        QCOMPARE(client.entries().size(), 1);

        // 6. Real EPUB download via CROSS-ORIGIN redirect -> Authorization header MUST be stripped on Server B!
        primaryServer.requests.clear();
        cdnServer.requests.clear();
        const QString epubAcqUrl = primaryServer.url(QStringLiteral("/dl/cross-origin-lantern"));
        client.downloadPublication(epubAcqUrl, QStringLiteral("The Lantern Keeper"),
                                   QStringLiteral("Ada Marsh"), QStringLiteral("epub"), colId);
        QTRY_COMPARE_WITH_TIMEOUT(
            client.downloadStates().value(epubAcqUrl).toMap().value(QStringLiteral("status")).toString(),
            QStringLiteral("done"), 3000);

        // Server A (same origin) got Authorization; Server B (cross origin) got ZERO Authorization header!
        QVERIFY(!primaryServer.requests.isEmpty());
        QCOMPARE(primaryServer.requests.last().headers.value("authorization"), expectedAuth);
        QCOMPARE(cdnServer.requests.size(), 1);
        QVERIFY(!cdnServer.requests.first().headers.contains("authorization"));

        const QVariantMap dlState = client.downloadStates().value(epubAcqUrl).toMap();
        const int epubBookId = dlState.value(QStringLiteral("bookId")).toInt();
        QVERIFY(epubBookId > 0);
        const BookRecord epubRec = lib.record(epubBookId);
        QCOMPARE(epubRec.format, QStringLiteral("epub"));
        QVERIFY(epubRec.path.contains(QStringLiteral("/downloads/")));
        QVERIFY(QFileInfo::exists(epubRec.path));
        QVERIFY(lib.isInCollection(epubBookId, colId));
        QCOMPARE(lib.bookSetting(epubBookId, QStringLiteral("source_url")), epubAcqUrl);
        QCOMPARE(lib.bookSetting(epubBookId, QStringLiteral("opds_catalog")), QStringLiteral("Test Local OPDS"));

        // 7. Same-origin redirect preserves Authorization & imports valid PDF (%PDF- magic verified).
        primaryServer.requests.clear();
        const QString pdfAcqUrl = primaryServer.url(QStringLiteral("/dl/same-origin-pdf"));
        client.downloadPublication(pdfAcqUrl, QStringLiteral("Field Notes"),
                                   QStringLiteral("Clara Vance"), QStringLiteral("pdf"));
        QTRY_COMPARE_WITH_TIMEOUT(
            client.downloadStates().value(pdfAcqUrl).toMap().value(QStringLiteral("status")).toString(),
            QStringLiteral("done"), 3000);
        QCOMPARE(primaryServer.requests.size(), 2);
        QCOMPARE(primaryServer.requests[0].headers.value("authorization"), expectedAuth);
        QCOMPARE(primaryServer.requests[1].headers.value("authorization"), expectedAuth);
        const int pdfBookId = client.downloadStates().value(pdfAcqUrl).toMap().value(QStringLiteral("bookId")).toInt();
        QVERIFY(pdfBookId > 0);
        QCOMPARE(lib.record(pdfBookId).format, QStringLiteral("pdf"));

        // 8. Bad magic bytes rejected (no HTML masquerading as EPUB written to disk).
        const QString badMagicUrl = primaryServer.url(QStringLiteral("/dl/bad-magic.epub"));
        client.downloadPublication(badMagicUrl, QStringLiteral("Fake Book"), {}, QStringLiteral("epub"));
        QTRY_COMPARE_WITH_TIMEOUT(
            client.downloadStates().value(badMagicUrl).toMap().value(QStringLiteral("status")).toString(),
            QStringLiteral("error"), 3000);
        QVERIFY(client.downloadStates().value(badMagicUrl).toMap().value(QStringLiteral("error")).toString()
                    .contains(QStringLiteral("EPUB or PDF")));

        // 9. Redirect to file:// scheme rejected.
        const QString fileRedirUrl = primaryServer.url(QStringLiteral("/dl/redirect-file"));
        client.downloadPublication(fileRedirUrl, QStringLiteral("Blocked"), {}, QStringLiteral("epub"));
        QTRY_COMPARE_WITH_TIMEOUT(
            client.downloadStates().value(fileRedirUrl).toMap().value(QStringLiteral("status")).toString(),
            QStringLiteral("error"), 3000);

        // 10. Redirect loop (> 5 hops) rejected.
        const QString loopUrl = primaryServer.url(QStringLiteral("/dl/loop"));
        client.downloadPublication(loopUrl, QStringLiteral("Loop"), {}, QStringLiteral("epub"));
        QTRY_COMPARE_WITH_TIMEOUT(
            client.downloadStates().value(loopUrl).toMap().value(QStringLiteral("status")).toString(),
            QStringLiteral("error"), 3000);
        QVERIFY(client.downloadStates().value(loopUrl).toMap().value(QStringLiteral("error")).toString()
                    .contains(QStringLiteral("redirects"), Qt::CaseInsensitive));

        // 11. Feed exceeding 10 MB size cap is rejected cleanly via Content-Length.
        primaryServer.handler = [](const OpdsTestServer::Request &) -> OpdsTestServer::Response {
            return {200, "application/atom+xml", {{"Content-Length", "15000000"}}, "<feed/>"};
        };
        client.navigate(primaryServer.url(QStringLiteral("/opds/huge")), QStringLiteral("Huge"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loading(), 3000);
        QVERIFY(client.error().contains(QStringLiteral("10 MB")));

        // 12. Disallowed direct download scheme rejected immediately.
        client.downloadPublication(QStringLiteral("file:///etc/passwd"), QStringLiteral("Passwd"), {}, QStringLiteral("epub"));
        QCOMPARE(client.downloadStates().value(QStringLiteral("file:///etc/passwd")).toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("error"));
    }

void OpdsTests::catalogsSheetQmlSmokeTest() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QQuickStyle::setStyle(QStringLiteral("Clothbound"));
        Library lib(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(lib.open());
        MemoryKeyStore keys;
        OpdsClient opds(&lib, &keys, dir.filePath(QStringLiteral("storage")),
                        dir.filePath(QStringLiteral("settings.ini")));
        Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
        Translator translator;

        OpdsTestServer server;
        server.handler = [](const OpdsTestServer::Request &req) -> OpdsTestServer::Response {
            if (req.path == "/opds2") {
                return {200, "application/opds+json", {},
                        R"({
  "metadata": {"title": "Smoke Catalog"},
  "links": [{"rel": "search", "href": "/search?q={searchTerms}", "templated": true}],
  "navigation": [{"title": "Classics", "href": "/opds2/classics", "type": "application/opds+json"}],
  "groups": [{
    "metadata": {"title": "Featured"},
    "links": [{"rel": "self", "href": "/opds2/featured"}],
    "publications": [{
      "metadata": {
        "title": "The Lantern Keeper",
        "author": "Ada Marsh",
        "published": "1911",
        "description": "A keeper and her beacon.",
        "subject": ["Fiction", "Maritime"]
      },
      "links": [
        {"rel": "http://opds-spec.org/acquisition/open-access", "href": "/dl/lantern.epub", "type": "application/epub+zip"},
        {"rel": "http://opds-spec.org/acquisition/open-access", "href": "/dl/lantern.pdf", "type": "application/pdf"}
      ]
    }]
  }]
})"};
            }
            return {401, "text/plain", {{"WWW-Authenticate", "Basic realm=\"Test\""}}, "Unauthorized"};
        };

        const QString cid = opds.addCatalog(QStringLiteral("Smoke Server"), server.url(QStringLiteral("/opds2")));
        QVERIFY(!cid.isEmpty());

        QQmlEngine engine;
        const QString repoSrc = QFileInfo(QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir()
                                              .filePath(QStringLiteral("../src"))).canonicalFilePath();
        engine.addImportPath(repoSrc + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(QStringLiteral("library"), &lib);
        engine.rootContext()->setContextProperty(QStringLiteral("opds"), &opds);

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent comp(&engine, QUrl::fromLocalFile(repoSrc + QStringLiteral("/ui/library/CatalogsSheet.qml")));
        QVERIFY2(!comp.isError(), qPrintable(comp.errorString()));
        std::unique_ptr<QObject> obj(comp.create());
        QVERIFY2(obj != nullptr, qPrintable(comp.errorString()));

        QCOMPARE(obj->property("open").toBool(), false);

        // Drive live feed, 401 sign-in, and add-server UI states and confirm zero QML warnings.
        opds.openCatalog(cid);
        QTRY_VERIFY_WITH_TIMEOUT(!opds.loading(), 3000);
        QCOMPARE(opds.entries().size(), 2);

        QMetaObject::invokeMethod(obj.get(), "show");
        QCOMPARE(obj->property("open").toBool(), true);
        QCoreApplication::processEvents();

        opds.navigate(server.url(QStringLiteral("/opds2/private")), QStringLiteral("Private"));
        QTRY_VERIFY_WITH_TIMEOUT(opds.authRequired(), 3000);
        QCoreApplication::processEvents();

        QMetaObject::invokeMethod(obj.get(), "openAddServer");
        QCOMPARE(obj->property("addingServer").toBool(), true);
        QCoreApplication::processEvents();

        QMetaObject::invokeMethod(obj.get(), "close");
        QCOMPARE(obj->property("open").toBool(), false);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
}

QTEST_MAIN(OpdsTests)
#include "tst_opds.moc"
