#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlScheme>
#include <QtTest>

#include <atomic>
#include <memory>

#include "fakeserver.h"
#include "formats/bookscheme.h"
#include "formats/epubbook.h"
#include "testbooks.h"

namespace {

QByteArray tinyPngBytes() {
    QImage img(16, 16, QImage::Format_ARGB32);
    img.fill(QColor(0x3e, 0x5f, 0x99));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

QByteArray buildAdversarialEpub(const QString &localBeaconUrl, const QByteArray &coverPng) {
    QByteArray ch1Body;
    ch1Body += "<h1 id=\"ch1-title\">Chapter One: Adversarial Security &amp; Pagination</h1>";
    // Attempt XSS via inline <script>
    ch1Body += "<script>window.__xss = 1; try { window.parent.__xss = 1; } catch (e) {}</script>";
    // Attempt XSS via inline event handler on a valid data: URI image
    ch1Body += "<img id=\"onload-xss\" src=\"data:image/gif;base64,"
               "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\" "
               "onload=\"window.__xss_onload = 1\" alt=\"px\"/>";
    // External tracking beacons that MUST be blocked (CSP + OfflineInterceptor)
    ch1Body += "<img id=\"ext-beacon\" src=\"https://example.invalid/beacon.png\" alt=\"external beacon\"/>";
    ch1Body += QByteArrayLiteral("<img id=\"local-beacon\" src=\"") + localBeaconUrl.toUtf8() +
               QByteArrayLiteral("\" alt=\"local beacon\"/>");
    // DOM clobbering attempt against reader.js style injection IDs
    ch1Body += "<div id=\"cb-typography\" data-clobber=\"yes\">Clobbering typography element must stay intact.</div>";
    ch1Body += "<div id=\"cb-layout\" data-clobber=\"yes\">Clobbering layout element must stay intact.</div>";
    // Cross-chapter footnote reference for showNote() (about:srcdoc popover)
    ch1Body += "<p id=\"note-para\">The observatory kept careful records of every tide and lantern signal "
               "<a id=\"fn-ref-1\" epub:type=\"noteref\" href=\"ch2.xhtml#note1\">1</a> "
               "along the northern breakwater.</p>";
    // Enough literary paragraphs to reliably span multiple pages in CSS columns
    for (int i = 1; i <= 26; ++i) {
        ch1Body += QByteArrayLiteral("<p id=\"p") + QByteArray::number(i) + QByteArrayLiteral("\">Paragraph ") +
                   QByteArray::number(i) +
                   QByteArrayLiteral(
                       ": Along the stone quay the harbor fog drifted past the brass barometer while the watchman "
                       "trimmed the wick, checked the tide tables against the almanac, and listened to the steady "
                       "bell buoy ringing across the cold strait under a pale lantern moon.</p>\n");
    }

    const QByteArray ch2Body =
        "<h1 id=\"ch2-head\">Chapter Two: The Sounding Line</h1>\n"
        "<p id=\"ch2-intro\">By dawn the gale had shifted eastward, uncovering the shoals beneath the lighthouse headland.</p>\n"
        "<p id=\"target-anchor\">Anchor paragraph in chapter two reached via fragment navigation.</p>\n"
        "<aside epub:type=\"footnote\" id=\"note1\">"
        "<p>Cross-chapter footnote content rendered inside sandboxed srcdoc.</p>"
        "<script>window.__note_xss = 1;</script></aside>";

    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:identifier id=\"uid\">urn:uuid:e2e-webengine-security-test</dc:identifier>\n"
        "    <dc:title>Adversarial Harbor</dc:title>\n"
        "    <dc:creator>Ada Marsh</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        "  </metadata>\n"
        "  <manifest>\n"
        "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
        "    <item id=\"cover\" href=\"images/cover.png\" media-type=\"image/png\" properties=\"cover-image\"/>\n"
        "    <item id=\"ch1\" href=\"text/ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "    <item id=\"ch2\" href=\"text/ch2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "  </manifest>\n"
        "  <spine page-progression-direction=\"ltr\">\n"
        "    <itemref idref=\"ch1\"/>\n"
        "    <itemref idref=\"ch2\"/>\n"
        "  </spine>\n"
        "</package>";

    const QByteArray nav =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
        "<body><nav epub:type=\"toc\"><ol>\n"
        "  <li><a href=\"text/ch1.xhtml\">Chapter One</a></li>\n"
        "  <li><a href=\"text/ch2.xhtml\">Chapter Two</a></li>\n"
        "</ol></nav></body></html>";

    return TestBooks::epub({
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/nav.xhtml"), nav},
        {QStringLiteral("OEBPS/images/cover.png"), coverPng},
        {QStringLiteral("OEBPS/text/ch1.xhtml"), TestBooks::chapter("Chapter One", ch1Body)},
        {QStringLiteral("OEBPS/text/ch2.xhtml"), TestBooks::chapter("Chapter Two", ch2Body)},
    });
}

QVariantMap makeReaderStyle() {
    const QVariantMap theme{
        {QStringLiteral("bg"), QStringLiteral("#F6F1E7")},
        {QStringLiteral("fg"), QStringLiteral("#26221D")},
        {QStringLiteral("inkSoft"), QStringLiteral("#4D463D")},
        {QStringLiteral("link"), QStringLiteral("#3E5F99")},
        {QStringLiteral("accentInk"), QStringLiteral("#3E5F99")},
        {QStringLiteral("muted"), QStringLiteral("#6E665A")},
        {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
        {QStringLiteral("border"), QStringLiteral("rgba(38, 34, 29, 0.12)")},
        {QStringLiteral("selection"), QStringLiteral("rgba(62, 95, 153, 0.22)")},
        {QStringLiteral("markYellow"), QStringLiteral("#F7DE8A")},
        {QStringLiteral("markGreen"), QStringLiteral("#BFE5C8")},
        {QStringLiteral("markBlue"), QStringLiteral("#C2DDF7")},
        {QStringLiteral("markPink"), QStringLiteral("#F5C6D6")},
        {QStringLiteral("speaking"), QStringLiteral("rgba(62, 95, 153, 0.14)")},
        {QStringLiteral("dark"), false},
    };
    return QVariantMap{
        {QStringLiteral("theme"), theme},
        {QStringLiteral("font"), QStringLiteral("Literata")},
        {QStringLiteral("fontScale"), 1.0},
        {QStringLiteral("fontWeight"), 400},
        {QStringLiteral("lineHeight"), 1.55},
        {QStringLiteral("paragraphSpacing"), 0.4},
        {QStringLiteral("margin"), 0.10},
        {QStringLiteral("measure"), 30.0},
        {QStringLiteral("justify"), false},
        {QStringLiteral("hyphenate"), false},
        {QStringLiteral("dropCap"), false},
        {QStringLiteral("layout"), QStringLiteral("single")},
        {QStringLiteral("singleColumn"), true},
        {QStringLiteral("animate"), false},
        {QStringLiteral("reducedMotion"), true},
        {QStringLiteral("publisher"), false},
        {QStringLiteral("marginTop"), 64},
        {QStringLiteral("marginBottom"), 80},
        {QStringLiteral("radius"), 8},
        {QStringLiteral("listening"), false},
        {QStringLiteral("strings"),
         QVariantMap{
             {QStringLiteral("goToNote"), QStringLiteral("Go to note")},
             {QStringLiteral("listenFromHere"), QStringLiteral("Listen from here")},
         }},
    };
}

}  // namespace

// Mirrors Reader's QWebChannel bridge interface plus an async promise completion hook.
class TestBridge : public QObject {
    Q_OBJECT

public:
    explicit TestBridge(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void surfaceLoaded() {
        m_surfaceLoaded = true;
        emit surfaceLoadedChanged();
    }

    Q_INVOKABLE void relocated(const QVariantMap &location) {
        m_lastLocation = location;
        m_relocations.append(location);
        emit relocatedReceived(location);
    }

    Q_INVOKABLE void log(const QString &message) {
        m_logs.append(message);
        emit logReceived(message);
    }

    Q_INVOKABLE void tapped(const QString &zone) { m_taps.append(zone); }
    Q_INVOKABLE void openExternal(const QString &url) { m_externalLinks.append(url); }
    Q_INVOKABLE void keyPressed(const QString &key, int modifiers) {
        m_keys.append(QStringLiteral("%1:%2").arg(key).arg(modifiers));
    }
    Q_INVOKABLE void notifyPageTurn(int direction) { m_pageTurns.append(direction); }
    Q_INVOKABLE void selectionChanged(const QVariantMap &selection) { Q_UNUSED(selection); }
    Q_INVOKABLE void markClicked(int id, const QVariantMap &rect) {
        Q_UNUSED(id);
        Q_UNUSED(rect);
    }
    Q_INVOKABLE void imageClicked(const QVariantMap &info) { Q_UNUSED(info); }
    Q_INVOKABLE void speakableReady(int chapter, const QString &blocks) {
        Q_UNUSED(chapter);
        Q_UNUSED(blocks);
    }
    Q_INVOKABLE void speakableReadyFor(qint64 session, int chapter, const QString &blocks) {
        Q_UNUSED(session);
        Q_UNUSED(chapter);
        Q_UNUSED(blocks);
    }
    Q_INVOKABLE void listenFrom(int chapter, qint64 offset) {
        Q_UNUSED(chapter);
        Q_UNUSED(offset);
    }
    Q_INVOKABLE void listenFromSession(qint64 session, int chapter, qint64 offset) {
        Q_UNUSED(session);
        Q_UNUSED(chapter);
        Q_UNUSED(offset);
    }

    Q_INVOKABLE void asyncComplete(int token, const QVariant &value) {
        m_asyncResults.insert(token, value);
        emit asyncCompleted(token);
    }

    bool isSurfaceLoaded() const { return m_surfaceLoaded; }
    const QVariantMap &lastLocation() const { return m_lastLocation; }
    const QList<QVariantMap> &relocations() const { return m_relocations; }
    const QStringList &logs() const { return m_logs; }
    const QList<int> &pageTurns() const { return m_pageTurns; }

    bool takeAsyncResult(int token, QVariant *out) {
        const auto it = m_asyncResults.find(token);
        if (it == m_asyncResults.end())
            return false;
        if (out)
            *out = it.value();
        m_asyncResults.erase(it);
        return true;
    }

signals:
    void surfaceLoadedChanged();
    void relocatedReceived(const QVariantMap &location);
    void logReceived(const QString &message);
    void asyncCompleted(int token);

private:
    bool m_surfaceLoaded = false;
    QVariantMap m_lastLocation;
    QList<QVariantMap> m_relocations;
    QStringList m_logs;
    QStringList m_taps;
    QStringList m_externalLinks;
    QStringList m_keys;
    QList<int> m_pageTurns;
    QHash<int, QVariant> m_asyncResults;
};

// Wraps the real production OfflineInterceptor so we run its exact blocking logic
// while thread-safely recording every intercepted request from Chromium's IO thread.
class SpyOfflineInterceptor : public OfflineInterceptor {
    Q_OBJECT

public:
    struct Record {
        QUrl url;
        QWebEngineUrlRequestInfo::ResourceType resourceType;
        bool blocked = false;
    };

    using OfflineInterceptor::OfflineInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override {
        OfflineInterceptor::interceptRequest(info);
        QMutexLocker lock(&m_mutex);
        m_records.append({info.requestUrl(), info.resourceType(), info.changed()});
    }

    QList<Record> snapshot() const {
        QMutexLocker lock(&m_mutex);
        return m_records;
    }

    bool sawBlockedHost(const QString &host) const {
        QMutexLocker lock(&m_mutex);
        for (const Record &r : m_records) {
            if (r.blocked && r.url.host().compare(host, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    }

    bool sawBlockedUrlPrefix(const QString &prefix) const {
        QMutexLocker lock(&m_mutex);
        for (const Record &r : m_records) {
            if (r.blocked && r.url.toString().startsWith(prefix))
                return true;
        }
        return false;
    }

private:
    mutable QMutex m_mutex;
    QList<Record> m_records;
};

class HeadlessReaderPage : public QWebEnginePage {
    Q_OBJECT

public:
    HeadlessReaderPage(QWebEngineProfile *profile, QObject *parent = nullptr) : QWebEnginePage(profile, parent) {}

    QStringList consoleMessages() const { return m_consoleMessages; }

    QVariant evalJs(const QString &script, int timeoutMs = 10000, bool *okOut = nullptr) {
        QEventLoop loop;
        QVariant result;
        bool done = false;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);

        runJavaScript(script, [&](const QVariant &v) {
            result = v;
            done = true;
            loop.quit();
        });
        if (!done)
            loop.exec();
        if (okOut)
            *okOut = done;
        return result;
    }

    // Evaluates an expression returning a Promise and awaits settlement via QWebChannel.
    QVariant awaitPromise(TestBridge *bridge, const QString &jsExpr, int timeoutMs = 15000, bool *okOut = nullptr) {
        static int nextToken = 1;
        const int token = nextToken++;

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QMetaObject::Connection conn =
            connect(bridge, &TestBridge::asyncCompleted, &loop, [&](int completed) {
                if (completed == token)
                    loop.quit();
            });
        timer.start(timeoutMs);

        const QString wrapped = QStringLiteral(
            "(function() {"
            "  var b = window.__e2eBridge;"
            "  Promise.resolve().then(function() { return (%1); }).then("
            "    function(v) { b.asyncComplete(%2, v === undefined ? null : v); },"
            "    function(e) { b.asyncComplete(%2, { __error: String(e && e.stack ? e.stack : e) }); }"
            "  );"
            "})()")
                                    .arg(jsExpr)
                                    .arg(token);
        runJavaScript(wrapped);

        QVariant out;
        if (!bridge->takeAsyncResult(token, &out)) {
            loop.exec();
            bridge->takeAsyncResult(token, &out);
        }
        disconnect(conn);
        const bool ok = timer.isActive();
        if (okOut)
            *okOut = ok;
        return out;
    }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int lineNumber,
                                  const QString &sourceID) override {
        m_consoleMessages.append(
            QStringLiteral("[%1] %2:%3: %4").arg(int(level)).arg(sourceID).arg(lineNumber).arg(message));
    }

    // Mirrors EpubReader.qml's onNavigationRequested policy verbatim.
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
        Q_UNUSED(type);
        const QString u = url.toString();
        if (u.startsWith(QLatin1String("clothbound://")))
            return true;
        if (!isMainFrame && (u == QLatin1String("about:blank") || u == QLatin1String("about:srcdoc")))
            return true;
        return false;
    }

private:
    QStringList m_consoleMessages;
};

class WebEngineE2ETests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());

        m_fakeServer = std::make_unique<FakeServer>();
        const QByteArray coverPng = tinyPngBytes();
        m_fakeServer->routes.insert("/beacon.png", {200, "image/png", coverPng});
        m_fakeServer->routes.insert("/direct-beacon.png", {200, "image/png", coverPng});

        const QString localBeaconUrl = m_fakeServer->origin() + QStringLiteral("/beacon.png");
        const QString epubPath = m_tempDir.filePath(QStringLiteral("adversarial.epub"));
        QVERIFY(TestBooks::write(epubPath, buildAdversarialEpub(localBeaconUrl, coverPng)));

        m_book = std::make_shared<EpubBook>(epubPath);
        QVERIFY2(m_book->open(), qPrintable(m_book->errorString()));
        QCOMPARE(m_book->spine().size(), 2);

        m_scheme = std::make_unique<BookScheme>();
        m_interceptor = std::make_unique<SpyOfflineInterceptor>();
        m_token = m_scheme->add(m_book);
        QVERIFY(!m_token.isEmpty());

        // Off-the-record QWebEngineProfile mirroring Clothbound's reader profile.
        m_profile = std::make_unique<QWebEngineProfile>();
        QVERIFY(m_profile->isOffTheRecord());
        m_profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        m_profile->installUrlSchemeHandler("clothbound", m_scheme.get());
        m_profile->setUrlRequestInterceptor(m_interceptor.get());

        m_bridge = std::make_unique<TestBridge>();
        m_channel = std::make_unique<QWebChannel>();
        m_channel->registerObject(QStringLiteral("bridge"), m_bridge.get());

        m_page = std::make_unique<HeadlessReaderPage>(m_profile.get());
        m_page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
        m_page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        m_page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        m_page->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        m_page->settings()->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
        m_page->settings()->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        m_page->setWebChannel(m_channel.get());
        m_page->setVisible(true);

        // Load real clothbound://local/app/reader.html from Qt resources via BookScheme.
        QSignalSpy loadFinishedSpy(m_page.get(), &QWebEnginePage::loadFinished);
        m_page->load(QUrl(QStringLiteral("clothbound://local/app/reader.html")));
        QTRY_VERIFY_WITH_TIMEOUT(loadFinishedSpy.count() >= 1, 15000);
        QVERIFY2(loadFinishedSpy.first().first().toBool(), "reader.html failed to load in Chromium");
        QTRY_VERIFY_WITH_TIMEOUT(m_bridge->isSurfaceLoaded(), 15000);

        // Capture bridge handle in main world for awaiting Promises cleanly, and ensure
        // a realistic reading viewport size when running without an attached QWidget/QWindow.
        bool jsOk = false;
        const QVariant setupRes = m_page->evalJs(
            QStringLiteral(
                "(function() {"
                "  var s = document.getElementById('stage');"
                "  if (!s) return { ok: false, reason: 'no-stage' };"
                "  if (s.clientWidth < 600 || s.clientHeight < 500) {"
                "    document.documentElement.style.width = '900px';"
                "    document.documentElement.style.height = '700px';"
                "    document.body.style.width = '900px';"
                "    document.body.style.height = '700px';"
                "    s.style.width = '900px';"
                "    s.style.height = '700px';"
                "  }"
                "  return { ok: true, w: s.clientWidth, h: s.clientHeight,"
                "           hasClothbound: typeof clothbound === 'object' && typeof clothbound.open === 'function' };"
                "})()"),
            10000, &jsOk);
        QVERIFY(jsOk);
        const QVariantMap setupMap = setupRes.toMap();
        QVERIFY2(setupMap.value(QStringLiteral("ok")).toBool(), qPrintable(QJsonDocument::fromVariant(setupMap).toJson()));
        QVERIFY(setupMap.value(QStringLiteral("hasClothbound")).toBool());
        QVERIFY(setupMap.value(QStringLiteral("w")).toInt() >= 600);
        QVERIFY(setupMap.value(QStringLiteral("h")).toInt() >= 500);

        // Attach a lightweight handle to the same QWebChannel bridge for awaitPromise().
        bool bridgeReady = false;
        QEventLoop bridgeLoop;
        QTimer bridgeTimer;
        bridgeTimer.setSingleShot(true);
        connect(&bridgeTimer, &QTimer::timeout, &bridgeLoop, &QEventLoop::quit);
        connect(m_bridge.get(), &TestBridge::asyncCompleted, &bridgeLoop, [&](int tok) {
            if (tok == 0) {
                bridgeReady = true;
                bridgeLoop.quit();
            }
        });
        bridgeTimer.start(10000);
        m_page->runJavaScript(QStringLiteral(
            "new QWebChannel(qt.webChannelTransport, function(ch) {"
            "  window.__e2eBridge = ch.objects.bridge;"
            "  ch.objects.bridge.asyncComplete(0, true);"
            "});"));
        if (!bridgeReady)
            bridgeLoop.exec();
        QVERIFY2(bridgeReady, "Failed to bind QWebChannel bridge handle");
        m_bridge->takeAsyncResult(0, nullptr);
    }

    void cleanupTestCase() {
        // Destroy page BEFORE profile, and detach scheme/interceptor BEFORE destroying them.
        m_page.reset();
        m_channel.reset();
        m_bridge.reset();
        if (m_profile) {
            m_profile->setUrlRequestInterceptor(nullptr);
            if (m_scheme)
                m_profile->removeUrlSchemeHandler(m_scheme.get());
        }
        m_profile.reset();
        if (m_scheme && !m_token.isEmpty())
            m_scheme->remove(m_token);
        m_scheme.reset();
        m_interceptor.reset();
        m_book.reset();
        m_fakeServer.reset();
    }

    void testOpenPaginationAndSecurity() {
        // Build manifest matching Reader::openBook(), plus a 3rd spine entry pointing to
        // a missing chapter URL for the error-recovery test below.
        const QVariantList spine{
            QVariantMap{{QStringLiteral("href"), QStringLiteral("OEBPS/text/ch1.xhtml")},
                        {QStringLiteral("linear"), true}},
            QVariantMap{{QStringLiteral("href"), QStringLiteral("OEBPS/text/ch2.xhtml")},
                        {QStringLiteral("linear"), true}},
            QVariantMap{{QStringLiteral("href"), QStringLiteral("OEBPS/text/missing.xhtml")},
                        {QStringLiteral("linear"), true}},
        };
        const QVariantMap manifest{
            {QStringLiteral("base"), BookScheme::baseUrl(m_token)},
            {QStringLiteral("spine"), spine},
            {QStringLiteral("pageTargets"), QVariantList{}},
            {QStringLiteral("language"), QStringLiteral("en")},
            {QStringLiteral("rtl"), false},
            {QStringLiteral("title"), QStringLiteral("Adversarial Harbor")},
            {QStringLiteral("session"), 1.0},
        };
        const QVariantMap position{{QStringLiteral("chapter"), 0}, {QStringLiteral("offset"), 0}};
        const QVariantMap style = makeReaderStyle();

        const QString openExpr =
            QStringLiteral("clothbound.open(%1, %2, %3).then(() => JSON.parse(clothbound.debug()))")
                .arg(QString::fromUtf8(QJsonDocument::fromVariant(manifest).toJson(QJsonDocument::Compact)),
                     QString::fromUtf8(QJsonDocument::fromVariant(position).toJson(QJsonDocument::Compact)),
                     QString::fromUtf8(QJsonDocument::fromVariant(style).toJson(QJsonDocument::Compact)));

        bool ok = false;
        const QVariant openRes = m_page->awaitPromise(m_bridge.get(), openExpr, 15000, &ok);
        QVERIFY2(ok, "clothbound.open() timed out");
        const QVariantMap dbg = openRes.toMap();
        QVERIFY2(!dbg.contains(QStringLiteral("__error")), qPrintable(dbg.value(QStringLiteral("__error")).toString()));
        QCOMPARE(dbg.value(QStringLiteral("chapter")).toInt(), 0);
        QCOMPARE(dbg.value(QStringLiteral("page")).toInt(), 0);
        QVERIFY2(dbg.value(QStringLiteral("pages")).toInt() > 1,
                 qPrintable(QStringLiteral("Expected multi-page chapter 0, got pages=%1")
                                .arg(dbg.value(QStringLiteral("pages")).toInt())));
        QVERIFY(dbg.value(QStringLiteral("shown")).toBool());
        QVERIFY(dbg.value(QStringLiteral("indexed")).toInt() > 1000);
        QVERIFY(!m_bridge->relocations().isEmpty());

        // 1. Verify real Chromium enforced sandbox + CSP script-src 'none' (no XSS execution)
        //    AND verify cb-typography / cb-layout DOM clobbering did NOT break CSS injection!
        bool secOk = false;
        const QVariant secVar = m_page->evalJs(
            QStringLiteral(
                "(function() {"
                "  var active = document.querySelector('iframe.book-frame.shown') || document.getElementById('book');"
                "  var doc = active.contentDocument;"
                "  var win = active.contentWindow;"
                "  var clobberTypo = doc.getElementById('cb-typography');"
                "  var clobberLayout = doc.getElementById('cb-layout');"
                "  var styles = Array.from(doc.querySelectorAll('head style'));"
                "  var hasTypoStyle = styles.some(function(s) { return s.textContent.indexOf('--cb-') >= 0 || s.textContent.indexOf('Literata') >= 0; });"
                "  var hasLayoutStyle = styles.some(function(s) { return s.textContent.indexOf('column-count') >= 0; });"
                "  var htmlCs = win.getComputedStyle(doc.documentElement);"
                "  var extImg = doc.getElementById('ext-beacon');"
                "  var localImg = doc.getElementById('local-beacon');"
                "  return {"
                "    topXss: typeof window.__xss,"
                "    frameXss: typeof win.__xss,"
                "    frameOnloadXss: typeof win.__xss_onload,"
                "    clobberTypoTag: clobberTypo ? clobberTypo.localName : '',"
                "    clobberTypoText: clobberTypo ? clobberTypo.textContent : '',"
                "    clobberLayoutTag: clobberLayout ? clobberLayout.localName : '',"
                "    styleCount: styles.length,"
                "    hasTypoStyle: hasTypoStyle,"
                "    hasLayoutStyle: hasLayoutStyle,"
                "    columnCount: htmlCs.columnCount,"
                "    bgColor: htmlCs.backgroundColor,"
                "    extImgNaturalW: extImg ? extImg.naturalWidth : -1,"
                "    localImgNaturalW: localImg ? localImg.naturalWidth : -1"
                "  };"
                "})()"),
            10000, &secOk);
        QVERIFY(secOk);
        const QVariantMap sec = secVar.toMap();
        QCOMPARE(sec.value(QStringLiteral("topXss")).toString(), QStringLiteral("undefined"));
        QCOMPARE(sec.value(QStringLiteral("frameXss")).toString(), QStringLiteral("undefined"));
        QCOMPARE(sec.value(QStringLiteral("frameOnloadXss")).toString(), QStringLiteral("undefined"));

        // DOM-clobbering elements must remain intact <div>s while real <style> elements are injected.
        QCOMPARE(sec.value(QStringLiteral("clobberTypoTag")).toString(), QStringLiteral("div"));
        QVERIFY(sec.value(QStringLiteral("clobberTypoText")).toString().contains(QStringLiteral("Clobbering typography")));
        QCOMPARE(sec.value(QStringLiteral("clobberLayoutTag")).toString(), QStringLiteral("div"));
        QVERIFY(sec.value(QStringLiteral("styleCount")).toInt() >= 2);
        QVERIFY(sec.value(QStringLiteral("hasTypoStyle")).toBool());
        QVERIFY(sec.value(QStringLiteral("hasLayoutStyle")).toBool());
        QCOMPARE(sec.value(QStringLiteral("columnCount")).toString(), QStringLiteral("1"));
        // #F6F1E7 == rgb(246, 241, 231)
        QCOMPARE(sec.value(QStringLiteral("bgColor")).toString(), QStringLiteral("rgb(246, 241, 231)"));

        // External and local HTTP beacons inside the chapter MUST NOT load.
        QCOMPARE(sec.value(QStringLiteral("extImgNaturalW")).toInt(), 0);
        QCOMPARE(sec.value(QStringLiteral("localImgNaturalW")).toInt(), 0);
        QCOMPARE(m_fakeServer->requests.size(), 0);

        // 2. Verify pagination: next(), prev(), and goTo() across chapters.
        const qint64 offsetPage0 = m_bridge->lastLocation().value(QStringLiteral("offset")).toLongLong();

        const QVariant nextRes = m_page->awaitPromise(
            m_bridge.get(), QStringLiteral("clothbound.next().then(() => JSON.parse(clothbound.debug()))"), 10000, &ok);
        QVERIFY(ok);
        const QVariantMap nextDbg = nextRes.toMap();
        QCOMPARE(nextDbg.value(QStringLiteral("chapter")).toInt(), 0);
        QCOMPARE(nextDbg.value(QStringLiteral("page")).toInt(), 1);
        const qint64 offsetPage1 = m_bridge->lastLocation().value(QStringLiteral("offset")).toLongLong();
        QVERIFY2(offsetPage1 > offsetPage0,
                 qPrintable(QStringLiteral("Expected offset to advance on next(): page0=%1 page1=%2")
                                .arg(offsetPage0)
                                .arg(offsetPage1)));

        const QVariant prevRes = m_page->awaitPromise(
            m_bridge.get(), QStringLiteral("clothbound.prev().then(() => JSON.parse(clothbound.debug()))"), 10000, &ok);
        QVERIFY(ok);
        QCOMPARE(prevRes.toMap().value(QStringLiteral("page")).toInt(), 0);

        const QVariant goToRes = m_page->awaitPromise(
            m_bridge.get(),
            QStringLiteral(
                "clothbound.goTo({chapter: 1, fragment: 'target-anchor', instant: true}).then(() => JSON.parse(clothbound.debug()))"),
            10000, &ok);
        QVERIFY(ok);
        const QVariantMap ch2Dbg = goToRes.toMap();
        QCOMPARE(ch2Dbg.value(QStringLiteral("chapter")).toInt(), 1);
        QVERIFY(ch2Dbg.value(QStringLiteral("shown")).toBool());

        // Navigate back to chapter 0 page 0 for subsequent tests.
        const QVariant backCh0 = m_page->awaitPromise(
            m_bridge.get(),
            QStringLiteral("clothbound.goTo({chapter: 0, offset: 0, instant: true}).then(() => JSON.parse(clothbound.debug()))"),
            10000, &ok);
        QVERIFY(ok);
        QCOMPARE(backCh0.toMap().value(QStringLiteral("chapter")).toInt(), 0);
        QCOMPARE(backCh0.toMap().value(QStringLiteral("page")).toInt(), 0);
    }

    void testOfflineInterceptorBlocksExternalRequestsDirectly() {
        // Inside the EPUB iframe, BookScheme's strict CSP (`img-src 'self' data: blob:`)
        // already blocks external images at the Blink layer as defense-in-depth.
        // Now ALSO verify OfflineInterceptor directly in Chromium on the same QWebEngineProfile
        // when CSP is NOT blocking ahead of it: both https://example.invalid/beacon.png and
        // our live local FakeServer URL MUST be intercepted and blocked by OfflineInterceptor!
        auto probePage = std::make_unique<QWebEnginePage>(m_profile.get());
        QSignalSpy loadFinished(probePage.get(), &QWebEnginePage::loadFinished);

        probePage->load(QUrl(QStringLiteral("https://example.invalid/beacon.png")));
        QTRY_VERIFY_WITH_TIMEOUT(loadFinished.count() >= 1, 10000);
        QVERIFY(!loadFinished.takeFirst().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(m_interceptor->sawBlockedHost(QStringLiteral("example.invalid")), 5000);

        const QString directLocalUrl = m_fakeServer->origin() + QStringLiteral("/direct-beacon.png");
        probePage->load(QUrl(directLocalUrl));
        QTRY_VERIFY_WITH_TIMEOUT(loadFinished.count() >= 1, 10000);
        QVERIFY(!loadFinished.takeFirst().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(m_interceptor->sawBlockedUrlPrefix(m_fakeServer->origin()), 5000);
        QCOMPARE(m_fakeServer->requests.size(), 0);
    }

    void testShowNoteSrcdocPopover() {
        // Click the cross-chapter footnote reference in chapter 0 to trigger showNote()
        // which fetches ch2.xhtml from BookScheme and renders an about:srcdoc sandboxed iframe.
        bool clickOk = false;
        const QVariant clicked = m_page->evalJs(
            QStringLiteral(
                "(function() {"
                "  var active = document.querySelector('iframe.book-frame.shown') || document.getElementById('book');"
                "  var doc = active.contentDocument;"
                "  var a = doc.getElementById('fn-ref-1');"
                "  if (!a) return false;"
                "  a.dispatchEvent(new active.contentWindow.MouseEvent('click', { bubbles: true, cancelable: true }));"
                "  return true;"
                "})()"),
            10000, &clickOk);
        QVERIFY(clickOk);
        QVERIFY(clicked.toBool());

        // Wait for .note > iframe (about:srcdoc) to load its content in real Chromium.
        QVariantMap noteState;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                bool ok = false;
                const QVariant v = m_page->evalJs(
                    QStringLiteral(
                        "(function() {"
                        "  var note = document.querySelector('#overlay .note');"
                        "  if (!note) return { present: false };"
                        "  var ifr = note.querySelector('iframe');"
                        "  if (!ifr || !ifr.contentDocument || !ifr.contentDocument.body) return { present: true, ready: false };"
                        "  return {"
                        "    present: true,"
                        "    ready: ifr.contentDocument.readyState === 'complete' && ifr.contentDocument.body.textContent.trim().length > 0,"
                        "    sandbox: ifr.getAttribute('sandbox') || '',"
                        "    url: ifr.contentDocument.URL || '',"
                        "    text: ifr.contentDocument.body.textContent.trim(),"
                        "    noteXss: typeof ifr.contentWindow.__note_xss,"
                        "    scriptCount: ifr.contentDocument.querySelectorAll('script').length"
                        "  };"
                        "})()"),
                    5000, &ok);
                if (!ok)
                    return false;
                noteState = v.toMap();
                return noteState.value(QStringLiteral("ready")).toBool();
            }(),
            10000);

        QCOMPARE(noteState.value(QStringLiteral("sandbox")).toString(), QStringLiteral("allow-same-origin"));
        QCOMPARE(noteState.value(QStringLiteral("url")).toString(), QStringLiteral("about:srcdoc"));
        QVERIFY2(noteState.value(QStringLiteral("text")).toString().contains(QStringLiteral("Cross-chapter footnote content")),
                 qPrintable(noteState.value(QStringLiteral("text")).toString()));
        QCOMPARE(noteState.value(QStringLiteral("noteXss")).toString(), QStringLiteral("undefined"));
        QCOMPARE(noteState.value(QStringLiteral("scriptCount")).toInt(), 0);

        // clothbound.escape() must dismiss the note cleanly and return true.
        bool escOk = false;
        const QVariant esc = m_page->evalJs(
            QStringLiteral("({ closed: clothbound.escape(), remaining: document.querySelectorAll('#overlay .note').length })"),
            5000, &escOk);
        QVERIFY(escOk);
        QVERIFY(esc.toMap().value(QStringLiteral("closed")).toBool());
        QCOMPARE(esc.toMap().value(QStringLiteral("remaining")).toInt(), 0);
    }

    void testPrepareIntoFrameMissingChapterDoesNotDeadlockQueue() {
        const int logsBefore = m_bridge->logs().size();

        // Chapter 2 in the manifest points to OEBPS/text/missing.xhtml which is NOT in the ZIP;
        // BookScheme fails it with UrlNotFound. prepareIntoFrame() MUST reject and run() MUST
        // catch & recover so state.queue does NOT deadlock!
        bool missingOk = false;
        const QVariant missingRes = m_page->awaitPromise(
            m_bridge.get(),
            QStringLiteral("clothbound.goTo({chapter: 2, instant: true}).then(() => 'settled')"),
            15000, &missingOk);
        QVERIFY2(missingOk, "state.queue deadlocked on missing chapter URL");
        QCOMPARE(missingRes.toString(), QStringLiteral("settled"));
        QVERIFY2(m_bridge->logs().size() > logsBefore, "Expected reader.js to log the missing chapter failure");

        // Immediately issue a real navigation on the same state.queue to prove zero deadlock.
        bool recoverOk = false;
        const QVariant recoverRes = m_page->awaitPromise(
            m_bridge.get(),
            QStringLiteral("clothbound.goTo({chapter: 1, instant: true}).then(() => JSON.parse(clothbound.debug()))"),
            10000, &recoverOk);
        QVERIFY2(recoverOk, "Navigation after missing-chapter error failed/timed out");
        const QVariantMap recoverDbg = recoverRes.toMap();
        QCOMPARE(recoverDbg.value(QStringLiteral("chapter")).toInt(), 1);
        QVERIFY(recoverDbg.value(QStringLiteral("shown")).toBool());
        QVERIFY(recoverDbg.value(QStringLiteral("indexed")).toInt() > 0);
    }

    void testConcurrentBookSchemeRequestsFromChromium() {
        // Fire a burst of concurrent fetch() requests from live Chromium into BookScheme::requestStarted()
        // across valid chapters, image resources, and a missing entry to verify async reply/fail paths.
        const QString base = BookScheme::baseUrl(m_token);
        const QString expr = QStringLiteral(
            "(async function() {"
            "  var base = %1;"
            "  var urls = ["
            "    base + 'OEBPS/text/ch1.xhtml',"
            "    base + 'OEBPS/text/ch2.xhtml',"
            "    base + 'OEBPS/images/cover.png',"
            "    base + 'OEBPS/text/ch1.xhtml',"
            "    base + 'OEBPS/text/ch2.xhtml',"
            "    base + 'OEBPS/images/cover.png'"
            "  ];"
            "  var responses = await Promise.all(urls.map(function(u) { return fetch(u); }));"
            "  var statuses = responses.map(function(r) { return r.status; });"
            "  var csps = responses.map(function(r) { return r.headers.get('content-security-policy') || ''; });"
            "  var nosniffs = responses.map(function(r) { return r.headers.get('x-content-type-options') || ''; });"
            "  var bodies = await Promise.all(responses.map(function(r) { return r.arrayBuffer(); }));"
            "  var missingFailed = false;"
            "  try { var mr = await fetch(base + 'OEBPS/does-not-exist.xhtml'); missingFailed = !mr.ok; } catch (e) { missingFailed = true; }"
            "  return {"
            "    allOk: statuses.every(function(s) { return s === 200; }),"
            "    allHaveCsp: csps.every(function(c) { return c.indexOf(\"script-src 'none'\") >= 0; }),"
            "    allNoSniff: nosniffs.every(function(n) { return n.toLowerCase() === 'nosniff'; }),"
            "    sizes: bodies.map(function(b) { return b.byteLength; }),"
            "    missingFailed: missingFailed"
            "  };"
            "})()")
                                 .arg(QString::fromUtf8(QJsonDocument(QJsonArray{base}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));

        bool ok = false;
        const QVariant resVar = m_page->awaitPromise(m_bridge.get(), expr, 10000, &ok);
        QVERIFY2(ok, "Concurrent BookScheme fetch burst timed out");
        const QVariantMap res = resVar.toMap();
        QVERIFY2(!res.contains(QStringLiteral("__error")), qPrintable(res.value(QStringLiteral("__error")).toString()));
        QVERIFY(res.value(QStringLiteral("allOk")).toBool());
        QVERIFY(res.value(QStringLiteral("allHaveCsp")).toBool());
        QVERIFY(res.value(QStringLiteral("allNoSniff")).toBool());
        QVERIFY(res.value(QStringLiteral("missingFailed")).toBool());
        const QVariantList sizes = res.value(QStringLiteral("sizes")).toList();
        QCOMPARE(sizes.size(), 6);
        for (const QVariant &sz : sizes)
            QVERIFY(sz.toInt() > 0);
    }

    void testCleanCloseTeardown() {
        bool ok = false;
        const QVariant closeRes = m_page->awaitPromise(
            m_bridge.get(),
            QStringLiteral(
                "clothbound.close().then(function() {"
                "  var d = JSON.parse(clothbound.debug());"
                "  var b1 = document.getElementById('book');"
                "  var b2 = document.getElementById('book-back');"
                "  return {"
                "    book: d.book,"
                "    chapter: d.chapter,"
                "    indexed: d.indexed,"
                "    shown1: b1.classList.contains('shown'),"
                "    shown2: b2.classList.contains('shown'),"
                "    src1: b1.src,"
                "    src2: b2.src"
                "  };"
                "})"),
            10000, &ok);
        QVERIFY2(ok, "clothbound.close() hung or timed out");
        const QVariantMap closed = closeRes.toMap();
        QVERIFY2(!closed.contains(QStringLiteral("__error")),
                 qPrintable(closed.value(QStringLiteral("__error")).toString()));
        QCOMPARE(closed.value(QStringLiteral("book")).toBool(), false);
        QCOMPARE(closed.value(QStringLiteral("chapter")).toInt(), -1);
        QCOMPARE(closed.value(QStringLiteral("indexed")).toInt(), -1);
        QCOMPARE(closed.value(QStringLiteral("shown1")).toBool(), false);
        QCOMPARE(closed.value(QStringLiteral("shown2")).toBool(), false);
        QCOMPARE(closed.value(QStringLiteral("src1")).toString(), QStringLiteral("about:blank"));
        QCOMPARE(closed.value(QStringLiteral("src2")).toString(), QStringLiteral("about:blank"));
    }

private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<FakeServer> m_fakeServer;
    std::shared_ptr<EpubBook> m_book;
    std::unique_ptr<BookScheme> m_scheme;
    std::unique_ptr<SpyOfflineInterceptor> m_interceptor;
    QString m_token;
    std::unique_ptr<QWebEngineProfile> m_profile;
    std::unique_ptr<TestBridge> m_bridge;
    std::unique_ptr<QWebChannel> m_channel;
    std::unique_ptr<HeadlessReaderPage> m_page;
};

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox --disable-gpu --disable-dev-shm-usage");

    // Must register custom scheme BEFORE QGuiApplication is constructed.
    QWebEngineUrlScheme scheme("clothbound");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled |
                    QWebEngineUrlScheme::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clothbound_webengine_e2e"));

    WebEngineE2ETests tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_webengine_e2e.moc"
