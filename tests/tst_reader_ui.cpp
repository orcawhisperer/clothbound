#include "clothbound_tests.h"

#include "ai/aiservices.h"
#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/gemini.h"
#include "ai/illustrator.h"
#include "ai/keystore.h"
#include "ai/localmodels.h"
#include "ai/localsetup.h"
#include "app/portalfilepicker.h"
#include "app/surfacewatcher.h"
#include "app/systemcheck.h"
#include "app/theme.h"
#include "app/translator.h"
#include "audio/audiobook.h"
#include "audio/audiocache.h"
#include "audio/narration.h"
#include "audio/speechplayer.h"
#include "audio/voicecatalog.h"
#include "audio/voicepacks.h"
#include "fakeserver.h"
#include "formats/bookscheme.h"
#include "formats/coverprovider.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "library/library.h"
#include "library/libraryfinder.h"
#include "library/libraryindex.h"
#include "library/marks.h"
#include "online/opdsclient.h"
#include "online/webimporter.h"
#include "reader/reader.h"
#include "reader/readerimageprovider.h"
#include "testbooks.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <QtConcurrent>
#include <QtTest>
#include <cmath>
#include <limits>

void ClothboundTests::readerMapsPositionsOntoTheBook() {
    QTemporaryDir dir;
    const QString path = dir.filePath("book.epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int id = library.addBookFile(path);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY2(reader.openBook(id), qPrintable(reader.errorString()));
    QCOMPARE(reader.chapterCount(), 2);
    const QVariantList spine = reader.manifest().value("spine").toList();
    QCOMPARE(spine[0].toMap().value("href").toString(), QString("OEBPS/text/chapter%201.xhtml"));
    QVERIFY(reader.manifest().value("base").toString().startsWith("clothbound://local/book/"));
    QCOMPARE(reader.titleOfChapter(1), QString("Notes"));

    const QList<int> lengths = library.chapterLengths(id);
    const double total = lengths[0] + lengths[1];
    QCOMPARE(reader.fractionAt(1, 0), lengths[0] / total);
    const QVariantMap middle = reader.targetForFraction(lengths[0] / total + 0.001);
    QCOMPARE(middle.value("chapter").toInt(), 1);
    QCOMPARE(reader.targetForPageLabel("2").value("chapter").toInt(), 1);
    QVERIFY(reader.targetForPageLabel("99").value("chapter").isValid());
    QVERIFY(!reader.targetForPageLabel("xx").contains("chapter"));

    reader.relocated({{"chapter", 1}, {"offset", 3}, {"snippet", "note"}, {"page", 0}, {"pages", 1}});
    QCOMPARE(reader.chapter(), 1);
    QCOMPARE(reader.chapterTitle(), QString("Notes"));
    reader.close();
    QCOMPARE(library.record(id).position, QString(R"({"chapter":1,"offset":3,"snippet":"note","title":"Notes"})"));
}

void ClothboundTests::readerPdfSpeakableIsAsyncAndDropsStaleSession() {
    QTemporaryDir dir;
    const QString pathA = dir.filePath("a.pdf");
    const QString pathB = dir.filePath("b.pdf");
    QVERIFY(TestBooks::write(pathA, TestBooks::pdf("Pdf A", "Author A")));
    QVERIFY(TestBooks::write(pathB, TestBooks::pdf("Pdf B", "Author B")));

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int idA = library.addBookFile(pathA);
    const int idB = library.addBookFile(pathB);
    QVERIFY(idA > 0 && idB > 0);

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(idA));
    QVERIFY(reader.hasPageList());
    QCOMPARE(reader.targetForPageLabel("1").value("chapter").toInt(), 0);
    const qint64 sessionA = reader.manifest().value("session").toLongLong();

    QSignalSpy spoke(&reader, &Reader::speakable);
    reader.pdfSpeakable(0);
    // Must be queued asynchronously, never emitted synchronously inside pdfSpeakable().
    QCOMPARE(spoke.count(), 0);
    // Switch books before the queued callback executes: stale session must be dropped.
    QVERIFY(reader.openBook(idB));
    QCoreApplication::processEvents();
    QTest::qWait(30);
    QCOMPARE(spoke.count(), 0);

    // Stale session token from JS surface is also rejected; current session passes.
    const qint64 sessionB = reader.manifest().value("session").toLongLong();
    QVERIFY(sessionB > sessionA);
    reader.speakableReadyFor(sessionA, 0, QStringLiteral("[]"));
    QCOMPARE(spoke.count(), 0);

    reader.pdfSpeakable(0);
    QCOMPARE(spoke.count(), 0);
    QTRY_COMPARE(spoke.count(), 1);
    QCOMPARE(spoke[0][0].toInt(), 0);
    QVERIFY(spoke[0][1].toString().contains(QStringLiteral("Hello reader")));
    reader.close();
}

void ClothboundTests::readerPerBookAppearanceRoundTripAndIsolation() {
    QTemporaryDir dir;
    const QString epubA = dir.filePath("bookA.epub");
    const QString epubB = dir.filePath("bookB.epub");
    QVERIFY(TestBooks::write(epubA, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epubB, TestBooks::epub3(adobeObfuscate(fontPayload()))));

    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    auto restoreTypography = qScopeGuard([&] {
        QSettings s(settingsPath, QSettings::IniFormat);
        s.remove(QStringLiteral("typography"));
    });

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int idA = library.addBookFile(epubA);
    const int idB = library.addBookFile(epubB);
    QVERIFY(idA > 0 && idB > 0 && idA != idB);

    BookScheme scheme;
    Reader reader(&library, &scheme);

    // Global typography controls clamp cleanly.
    reader.setFontWeight(900);
    QCOMPARE(reader.fontWeight(), 700);
    reader.setFontWeight(100);
    QCOMPARE(reader.fontWeight(), 300);
    reader.setFontWeight(450);
    QCOMPARE(reader.fontWeight(), 450);

    reader.setParagraphSpacing(2.5);
    QCOMPARE(reader.paragraphSpacing(), 1.2);
    reader.setParagraphSpacing(-0.5);
    QCOMPARE(reader.paragraphSpacing(), 0.0);
    reader.setParagraphSpacing(0.65);
    QCOMPARE(reader.paragraphSpacing(), 0.65);

    // Book A starts without per-book override.
    QVERIFY(reader.openBook(idA));
    QVERIFY(!reader.bookAppearanceOverride());
    QVERIFY(reader.bookAppearance().isEmpty());

    // Saving per-book appearance clamps typography bounds and persists in book_settings.
    QSignalSpy appearanceSpy(&reader, &Reader::appearanceChanged);
    reader.saveBookAppearance({{QStringLiteral("fontWeight"), 950},
                               {QStringLiteral("paragraphSpacing"), 3.0},
                               {QStringLiteral("font"), QStringLiteral("Literata")},
                               {QStringLiteral("fontScale"), 1.25}});
    QVERIFY(reader.bookAppearanceOverride());
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 700);
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 1.2);
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("font")).toString(), QStringLiteral("Literata"));
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontScale")).toDouble(), 1.25);
    QVERIFY(!library.bookSetting(idA, QStringLiteral("appearance")).isEmpty());
    QVERIFY(appearanceSpy.count() >= 1);

    // Lower-bound clamping on saveBookAppearance.
    reader.saveBookAppearance({{QStringLiteral("fontWeight"), 150},
                               {QStringLiteral("paragraphSpacing"), -0.4},
                               {QStringLiteral("font"), QStringLiteral("Literata")}});
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 300);
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 0.0);

    // Switching to Book B isolates per-book appearance completely.
    QVERIFY(reader.openBook(idB));
    QVERIFY(!reader.bookAppearanceOverride());
    QVERIFY(reader.bookAppearance().isEmpty());
    QVERIFY(library.bookSetting(idB, QStringLiteral("appearance")).isEmpty());

    // setBookAppearanceOverride(true) seeds from global defaults for Book B.
    reader.setBookAppearanceOverride(true);
    QVERIFY(reader.bookAppearanceOverride());
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 450);
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 0.65);

    // Cold reload into a fresh Reader restores Book A's saved appearance.
    reader.close();
    Reader reloaded(&library, &scheme);
    QVERIFY(reloaded.openBook(idA));
    QVERIFY(reloaded.bookAppearanceOverride());
    QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 300);
    QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("paragraphSpacing")).toDouble(), 0.0);
    QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("font")).toString(), QStringLiteral("Literata"));

    // Clearing Book A removes its override without affecting Book B.
    reloaded.clearBookAppearance();
    QVERIFY(!reloaded.bookAppearanceOverride());
    QVERIFY(reloaded.bookAppearance().isEmpty());
    QVERIFY(library.bookSetting(idA, QStringLiteral("appearance")).isEmpty());

    QVERIFY(reloaded.openBook(idB));
    QVERIFY(reloaded.bookAppearanceOverride());
    QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("fontWeight")).toInt(), 450);
    reloaded.setBookAppearanceOverride(false);
    QVERIFY(!reloaded.bookAppearanceOverride());
    QVERIFY(library.bookSetting(idB, QStringLiteral("appearance")).isEmpty());
}

void ClothboundTests::readerRtlDirectionJumpBackAndWpmAutomatedExclusion() {
    QTemporaryDir dir;
    // Build an EPUB 3 book declaring page-progression-direction="rtl".
    const QByteArray rtlOpf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:identifier id=\"uid\">urn:uuid:rtl-test-001</dc:identifier>\n"
        "    <dc:title>Vertical Tales</dc:title>\n"
        "    <dc:creator>A. Author</dc:creator>\n"
        "    <dc:language>ja</dc:language>\n"
        "  </metadata>\n"
        "  <manifest>\n"
        "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
        "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "  </manifest>\n"
        "  <spine page-progression-direction=\"rtl\"><itemref idref=\"c1\"/></spine>\n"
        "</package>";
    const QByteArray rtlNav =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>\n"
        "<nav epub:type=\"toc\"><ol><li><a href=\"c1.xhtml\">Chapter One</a></li></ol></nav>\n"
        "</body></html>";
    const QByteArray longBody = "<p>" + QByteArray(1200, 'a') + "</p>";
    const QString rtlPath = dir.filePath("rtl.epub");
    QVERIFY(TestBooks::write(rtlPath, TestBooks::epub({
                                          {"OEBPS/content.opf", rtlOpf},
                                          {"OEBPS/nav.xhtml", rtlNav},
                                          {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", longBody)},
                                      })));
    const QString ltrPath = dir.filePath("ltr.epub");
    QVERIFY(TestBooks::write(ltrPath, TestBooks::epub2(png(20, 30))));

    EpubBook rtlBook(rtlPath);
    QVERIFY(rtlBook.open());
    QVERIFY(rtlBook.isRightToLeft());

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int rtlId = library.addBookFile(rtlPath);
    const int ltrId = library.addBookFile(ltrPath);
    QVERIFY(rtlId > 0 && ltrId > 0);
    // Pre-seed chapter lengths so globalOffset() is deterministic without waiting on background measurement.
    library.setChapterLengths(rtlId, {4000});
    library.setChapterLengths(ltrId, {4000, 1000});

    BookScheme scheme;
    Reader readerAuto(&library, &scheme);
    QVERIFY(readerAuto.openBook(rtlId));
    QVERIFY(readerAuto.rtl());
    QVERIFY(readerAuto.manifest().value(QStringLiteral("rtl")).toBool());

    // Dynamic CSS writing-mode (vertical-lr -> rtl=false, vertical-rl -> rtl=true) updates Reader::rtl.
    QSignalSpy jumpSpy(&readerAuto, &Reader::jumpRecorded);
    readerAuto.relocated({{"chapter", 0}, {"offset", 0}, {"end", 200}, {"page", 0}, {"pages", 4}, {"rtl", false}});
    QVERIFY(!readerAuto.rtl());
    readerAuto.relocated({{"chapter", 0}, {"offset", 100}, {"end", 300}, {"page", 1}, {"pages", 4},
                          {"rtl", true}, {"canGoBack", true}, {"canGoForward", false},
                          {"backChapter", 0}, {"backPageLabel", "12"}, {"jumped", true}});
    QVERIFY(readerAuto.rtl());
    QVERIFY(readerAuto.canGoBack());
    QVERIFY(!readerAuto.canGoForward());
    QCOMPARE(readerAuto.backLabel(), QStringLiteral("p. 12"));
    QCOMPARE(jumpSpy.count(), 1);

    // Start manual reader in parallel so one 2.1s wait verifies BOTH automated exclusion and manual inclusion.
    Reader readerManual(&library, &scheme);
    QVERIFY(readerManual.openBook(ltrId));
    QVERIFY(!readerManual.rtl());
    readerManual.relocated({{"chapter", 0}, {"offset", 0}, {"end", 200}, {"page", 0}, {"pages", 4}});

    QTest::qWait(2100);

    // Automated narration page turn must NOT accumulate visual reading time or WPM chars.
    readerAuto.relocated({{"chapter", 0}, {"offset", 900}, {"end", 1100}, {"page", 2}, {"pages", 4},
                          {"automated", true}});
    // atEnd reports 0 minutes remaining in chapter and book.
    readerAuto.relocated({{"chapter", 0}, {"offset", 3900}, {"end", 4000}, {"page", 3}, {"pages", 4},
                          {"atEnd", true}, {"automated", true}});
    QCOMPARE(readerAuto.minutesLeftInChapter(), 0);
    QCOMPARE(readerAuto.minutesLeftInBook(), 0);
    readerAuto.close();

    // Non-automated manual page turn DOES accumulate reading time.
    readerManual.relocated({{"chapter", 0}, {"offset", 900}, {"end", 1100}, {"page", 1}, {"pages", 4},
                            {"automated", false}});
    readerManual.close();

    QCOMPARE(library.record(rtlId).secondsRead, qint64(0));
    QVERIFY(library.record(ltrId).secondsRead >= 2);
}

void ClothboundTests::surfaceWatcherMergesChromiumFlagsCleanly() {
    QCOMPARE(SurfaceWatcher::mergeChromiumFlags({}),
             QByteArray("--disable-backgrounding-occluded-windows"));
    QCOMPARE(SurfaceWatcher::mergeChromiumFlags("  --enable-features=Foo\t--ozone-platform=wayland  "),
             QByteArray("--enable-features=Foo --ozone-platform=wayland --disable-backgrounding-occluded-windows"));
    // Idempotent when already present.
    const QByteArray once = SurfaceWatcher::mergeChromiumFlags("--foo --disable-backgrounding-occluded-windows --bar");
    QCOMPARE(once, QByteArray("--foo --disable-backgrounding-occluded-windows --bar"));
    QCOMPARE(SurfaceWatcher::mergeChromiumFlags(once), once);
    // Token-accurate: a flag merely sharing the prefix must not suppress adding the real flag.
    QCOMPARE(SurfaceWatcher::mergeChromiumFlags("--disable-backgrounding-occluded-windows-custom"),
             QByteArray("--disable-backgrounding-occluded-windows-custom --disable-backgrounding-occluded-windows"));

    // applyChromiumFlags() updates QTWEBENGINE_CHROMIUM_FLAGS cleanly.
    const bool hadOrig = qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS");
    const QByteArray orig = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--use-gl=egl");
    SurfaceWatcher::applyChromiumFlags();
    QCOMPARE(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"),
             QByteArray("--use-gl=egl --disable-backgrounding-occluded-windows"));
    if (hadOrig)
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", orig);
    else
        qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void ClothboundTests::surfaceWatcherHandlesExposeHyprlandAndSceneGraph() {
    // Hyprland .socket2.sock workspace event filter.
    QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("workspace>>2"));
    QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("workspacev2>>2,reading\n"));
    QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("focusedmon>>DP-1,2"));
    QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("focusedmonv2>>DP-1,2"));
    QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("activespecial>>scratch,DP-1"));
    QVERIFY(SurfaceWatcher::isHyprWorkspaceEvent("activespecialv2>>-98,special:scratch,DP-1"));
    QVERIFY(!SurfaceWatcher::isHyprWorkspaceEvent("activewindow>>kitty,zsh"));
    QVERIFY(!SurfaceWatcher::isHyprWorkspaceEvent("openwindow>>0x1,1,clothbound,Clothbound"));
    QVERIFY(!SurfaceWatcher::isHyprWorkspaceEvent(""));

    QTemporaryDir dir;
    const QString sockPath = dir.filePath(".socket2.sock");
    QLocalServer server;
    QVERIFY(server.listen(sockPath));
    QPointer<QLocalSocket> serverPeer;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        serverPeer = server.nextPendingConnection();
    });

    QQuickWindow win;
    win.resize(640, 480);
    win.show();
    QVERIFY(QTest::qWaitFor([&] { return win.isExposed(); }, 2000));

    SurfaceWatcher watcher;
    watcher.setHyprEventSocketForTesting(sockPath);
    watcher.attach(&win);
    QVERIFY(watcher.isExposed());
    QTRY_VERIFY(serverPeer != nullptr);

    QSignalSpy repaintSpy(&watcher, &SurfaceWatcher::repaintRequested);
    QSignalSpy exposedSpy(&watcher, &SurfaceWatcher::exposedChanged);

    // Unrelated Hyprland events must not trigger unnecessary repaints.
    serverPeer->write("activewindow>>kitty,term\n");
    serverPeer->flush();
    QTest::qWait(40);
    QCOMPARE(repaintSpy.count(), 0);

    // Returning to a workspace while tiled and unfocused must still request a repaint.
    serverPeer->write("workspacev2>>1,1\n");
    serverPeer->flush();
    QTRY_COMPARE(repaintSpy.count(), 1);
    QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("hyprWorkspace"));

    // Fragmented socket reads are buffered until newline.
    serverPeer->write("workspacev2>>");
    serverPeer->flush();
    QTest::qWait(30);
    QCOMPARE(repaintSpy.count(), 1);
    serverPeer->write("2,2\n");
    serverPeer->flush();
    QTRY_COMPARE(repaintSpy.count(), 2);
    QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("hyprWorkspace"));

    // Hide -> Show (unexposed -> exposed) emits exposedChanged and requests repaint on return.
    const int beforeHide = repaintSpy.count();
    win.hide();
    QTRY_VERIFY(!win.isExposed());
    QVERIFY(!watcher.isExposed());
    QVERIFY(exposedSpy.count() >= 1);

    // Workspace events while unexposed must not fire prematurely while hidden.
    serverPeer->write("workspacev2>>3,3\n");
    serverPeer->flush();
    QTest::qWait(40);
    QCOMPARE(repaintSpy.count(), beforeHide);

    win.show();
    QTRY_VERIFY(win.isExposed());
    QVERIFY(watcher.isExposed());
    QTRY_VERIFY(repaintSpy.count() > beforeHide);

    // Queued sceneGraphInitialized triggers a sceneGraph repaint when exposed.
    const int beforeSg = repaintSpy.count();
    emit win.sceneGraphInitialized();
    QTRY_VERIFY(repaintSpy.count() > beforeSg);
    QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("sceneGraph"));

    // forceItemUpdate safely marks parent, nested child items (like WebEngine's delegate), and window.
    QQuickItem parentItem(win.contentItem());
    parentItem.setSize(QSizeF(400, 300));
    QQuickItem childItem(&parentItem);
    childItem.setSize(QSizeF(400, 300));
    watcher.forceItemUpdate(&parentItem);
    watcher.forceItemUpdate(nullptr);

    // Detaching cleanly clears state without crashing on subsequent window events.
    watcher.attach(nullptr);
    QVERIFY(!watcher.isExposed());
}

void ClothboundTests::surfaceWatcherCoalescesReturnNudgesAndHyprBursts() {
    QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("expose")));
    QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("sceneGraph")));
    QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("visibility")));
    QVERIFY(SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("hyprWorkspace")));
    QVERIFY(!SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("active")));
    QVERIFY(!SurfaceWatcher::isSurfaceNudgeReason(QStringLiteral("appActive")));
    QVERIFY(!SurfaceWatcher::isSurfaceNudgeReason(QString()));

    QTemporaryDir dir;
    const QString sockPath = dir.filePath(".socket2.sock");
    QLocalServer server;
    QVERIFY(server.listen(sockPath));
    QPointer<QLocalSocket> serverPeer;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        serverPeer = server.nextPendingConnection();
    });

    QQuickWindow win;
    win.resize(640, 480);
    win.show();
    QVERIFY(QTest::qWaitFor([&] { return win.isExposed(); }, 2000));

    SurfaceWatcher watcher;
    watcher.setHyprEventSocketForTesting(sockPath);
    watcher.attach(&win);
    QTRY_VERIFY(serverPeer != nullptr);

    QSignalSpy repaintSpy(&watcher, &SurfaceWatcher::repaintRequested);

    // Rapid multi-line Hyprland burst (workspace>> + workspacev2>> + focusedmon>>)
    // across separate flushes must coalesce into a single hyprWorkspace signal.
    serverPeer->write("workspace>>1\n");
    serverPeer->flush();
    QTest::qWait(5);
    serverPeer->write("workspacev2>>1,1\n");
    serverPeer->flush();
    QTest::qWait(5);
    serverPeer->write("focusedmon>>DP-1,1\n");
    serverPeer->flush();
    QTRY_COMPARE(repaintSpy.count(), 1);
    QCOMPARE(repaintSpy.last().first().toString(), QStringLiteral("hyprWorkspace"));
    QTest::qWait(50);
    QCOMPARE(repaintSpy.count(), 1);

    // Rapid back-to-back workspace return triggers must coalesce into ONE
    // surface nudge and never re-nudge mid-debounce (no strongTrigger bypass),
    // and ordinary focus switches ("active" / "appActive") never nudge.
    QVERIFY(watcher.takeSurfaceNudge(QStringLiteral("hyprWorkspace"), 400));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("visibility"), 400));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("sceneGraph"), 400));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("active"), 0));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("appActive"), 0));

    // Leaving a workspace (workspace event immediately followed by Expose(false))
    // must cancel the pending hyprWorkspace timer and reset cooldown for return.
    const int beforeLeave = repaintSpy.count();
    serverPeer->write("workspacev2>>2,2\n");
    serverPeer->flush();
    QTest::qWait(5);
    win.hide();
    QTRY_VERIFY(!win.isExposed());
    QTest::qWait(50);
    QCOMPARE(repaintSpy.count(), beforeLeave);
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));

    // Returning from another workspace (Expose false->true) immediately permits
    // a single fresh nudge even within the old cooldown window.
    win.show();
    QTRY_VERIFY(win.isExposed());
    QVERIFY(watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("hyprWorkspace"), 400));

    // Scene-graph invalidation resets the cooldown so rebuild gets a fresh nudge.
    emit win.sceneGraphInvalidated();
    QCoreApplication::processEvents();
    QVERIFY(watcher.takeSurfaceNudge(QStringLiteral("sceneGraph"), 400));
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 400));

    // Detached watcher never grants nudges.
    watcher.attach(nullptr);
    QVERIFY(!watcher.takeSurfaceNudge(QStringLiteral("expose"), 0));
}

void ClothboundTests::readerSurfaceRefreshAvoidsElidedOutlineAndNoOpRelayout() {
    QFile jsFile(QFINDTESTDATA("../src/reader/reader.js"));
    QVERIFY(jsFile.open(QIODevice::ReadOnly));
    const QString js = QString::fromUtf8(jsFile.readAll());
    QVERIFY(!js.contains(QStringLiteral("1px solid transparent")));
    QVERIFY(!js.contains(QStringLiteral("root.style.opacity = '0.999'")));
    QVERIFY(!js.contains(QStringLiteral("root.style.transform = 'translateZ(0)'")));
    QVERIFY(js.contains(QStringLiteral("cb-paint-probe")));
    QVERIFY(js.contains(QStringLiteral("beginSurfaceNudge")));
    QVERIFY(js.contains(QStringLiteral("endSurfaceNudge")));
    QVERIFY(js.contains(QStringLiteral("isNudgeGuarded")));
    QVERIFY(js.contains(QStringLiteral("isNudgeDelta")));

    QFile qmlFile(QFINDTESTDATA("../src/ui/reader/EpubReader.qml"));
    QVERIFY(qmlFile.open(QIODevice::ReadOnly));
    const QString qml = QString::fromUtf8(qmlFile.readAll());
    QVERIFY(qml.contains(QStringLiteral("backgroundColor: view.look.bg")));
    QVERIFY(qml.contains(QStringLiteral("target: surfaceWatcher")));
    QVERIFY(qml.contains(QStringLiteral("surfaceWatcher.forceItemUpdate")));
    QVERIFY(qml.contains(QStringLiteral("surfaceWatcher.takeSurfaceNudge")));
    QVERIFY(qml.contains(QStringLiteral("clothbound.beginSurfaceNudge")));
    QVERIFY(qml.contains(QStringLiteral("clothbound.endSurfaceNudge")));
    QVERIFY(qml.contains(QStringLiteral("anchors.bottomMargin: view.surfaceNudge")));
    QVERIFY(qml.contains(QStringLiteral("visible: win.visible && win.visibility !== Window.Minimized")));
    QVERIFY(qml.contains(QStringLiteral("WebEngineView.LifecycleState.Frozen")));
    QVERIFY(!qml.contains(QStringLiteral("strongTrigger")));
}

void ClothboundTests::dropCapOpensTheFirstRealParagraph() {
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node executable not available");
    const QString script = QStringLiteral(R"JS(
const assert = require('assert');
const DropCap = require(process.argv[1]);
const long = 'The Time Traveller (for so it will be convenient to speak of him) was expounding a recondite matter to us. His grey eyes shone.';
// The first long paragraph of running text, past an epigraph and a short line.
assert.strictEqual(DropCap.choose([
    { text: 'To my friends.', excluded: false },
    { text: long, excluded: true },
    { text: 'Short.', excluded: false },
    { text: long, excluded: false },
]), 3);
// Opening quotes are skipped over; a digit or a CJK character means none.
assert.strictEqual(DropCap.choose([{ text: '“' + long, excluded: false }]), 0);
assert.strictEqual(DropCap.choose([{ text: '1854. ' + long, excluded: false }]), -1);
assert.strictEqual(DropCap.choose([{ text: '吾輩は猫である。'.repeat(20), excluded: false }]), -1);
// Cyrillic and Greek have capitals.
assert.strictEqual(DropCap.choose([{ text: 'Все счастливые семьи похожи друг на друга. '.repeat(4), excluded: false }]), 0);
// Nothing long enough near the top: no drop cap.
assert.strictEqual(DropCap.choose(Array(10).fill({ text: 'A line of verse.', excluded: false }).concat([{ text: long }])), -1);
assert.strictEqual(DropCap.choose([]), -1);
console.log('ok');
)JS");
    QProcess process;
    process.start(node, {"-e", script, QFINDTESTDATA("../src/reader/dropcap.js")});
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
    QCOMPARE(process.readAllStandardOutput().trimmed(), QByteArray("ok"));
}

void ClothboundTests::readerJsSuppressesNudgeRelayoutAndAllowsRealResize() {
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node executable not available");

    const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
    QVERIFY(QFileInfo::exists(readerJsPath));

    const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let resizeCallback = null;
let domReady = null;
const rafQueue = [];

function makeStyle() {
    return {
        cssText: '', opacity: '', transform: '', backgroundColor: '',
        left: '', top: '', width: '', height: '',
        setProperty(k, v) { this[k] = v; },
    };
}

const stageEl = {
    clientWidth: 1200,
    clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: this.clientWidth, height: this.clientHeight }; },
};

const styleNodes = new Map();
const textNode = { nodeType: 3, data: 'Hello world from Clothbound chapter text.', parentElement: null };
const bodyEl = {
    childNodes: [textNode],
    lastElementChild: {
        getClientRects() { return [{ left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }]; },
        getBoundingClientRect() { return { left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }; },
    },
};
textNode.parentElement = bodyEl;

const chapterDocEl = {
    style: makeStyle(),
    scrollWidth: 1200,
    scrollHeight: 692,
    scrollLeft: 0,
    scrollTop: 0,
    scrollTo(opts) {
        if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
        if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
    },
    appendChild() {},
};

const chapterDoc = {
    URL: 'clothbound://book/1/ch1.xhtml',
    documentElement: chapterDocEl,
    head: { appendChild(el) { styleNodes.set(el.id, el); } },
    body: bodyEl,
    scrollingElement: chapterDocEl,
    fonts: { ready: Promise.resolve() },
    images: [],
    defaultView: {
        getComputedStyle() { return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start' }; },
        CSS: { highlights: new Map() },
        Highlight: class {},
    },
    getElementById(id) { return styleNodes.get(id) || null; },
    createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    addEventListener() {},
    removeEventListener() {},
    createTreeWalker() {
        let done = false;
        return { nextNode() { if (done) return null; done = true; return textNode; } };
    },
    createRange() {
        return {
            selectNodeContents() {},
            setStart() {},
            setEnd() {},
            getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
            getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
        };
    },
    getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
};

const classes = new Set();
const frameEl = {
    style: makeStyle(),
    classList: {
        add(c) { classes.add(c); },
        remove(c) { classes.delete(c); },
        contains(c) { return classes.has(c); },
    },
    contentDocument: chapterDoc,
    contentWindow: chapterDoc.defaultView,
    onload: null,
    _src: '',
    set src(v) {
        this._src = v;
        if (typeof this.onload === 'function')
            queueMicrotask(() => this.onload());
    },
    get src() { return this._src; },
    getBoundingClientRect() { return { left: 0, top: 54, width: 800, height: 692 }; },
};

const bodyChildren = [];
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: {
        children: bodyChildren,
        appendChild(el) { bodyChildren.push(el); return el; },
    },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'book') return frameEl;
        if (id === 'overlay') return overlayEl;
        if (id === 'cb-paint-probe') return bodyChildren.find(c => c.id === 'cb-paint-probe') || null;
        return null;
    },
    createElement(tag) {
        return {
            tagName: tag.toUpperCase(),
            id: '',
            attrs: {},
            style: makeStyle(),
            setAttribute(k, v) { this.attrs[k] = v; },
        };
    },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

let relocatedCount = 0;
const bridge = {
    surfaceLoaded() {},
    relocated() { ++relocatedCount; },
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console,
    setTimeout,
    clearTimeout,
    performance,
    Promise,
    URL,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1200, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class {
        constructor(cb) { resizeCallback = cb; }
        observe() {}
    },
    matchMedia: () => ({ matches: false }),
    requestAnimationFrame: fn => { rafQueue.push(fn); return rafQueue.length; },
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

const wait = ms => new Promise(r => setTimeout(r, ms));
const flushRaf = () => {
    while (rafQueue.length) {
        const batch = rafQueue.splice(0, rafQueue.length);
        for (const fn of batch) fn();
    }
};

(async () => {
    const book = { session: 1, base: 'clothbound://book/1/', language: 'en', rtl: false,
                   spine: [{ href: 'ch1.xhtml', linear: true }], pageTargets: [] };
    const style = {
        theme: { bg: '#fbfaf6', fg: '#23221f', link: '#8a6a2a', muted: '#6f6b63', popover: '#fff',
                 border: 'rgba(0,0,0,0.12)', selection: '#cde', search: '#ff0', found: '#fa0',
                 markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f', markPink: '#f0f',
                 speaking: '#eef', dark: false },
        font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.4,
        margin: 0.06, measure: 34, justify: false, hyphenate: false, layout: 'single',
        animate: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
        listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
    };

    await clothbound.open(book, { chapter: 0, offset: 0 }, style);
    assert.strictEqual(relocatedCount, 1, 'initial open relocates once');
    assert.strictEqual(frameEl.style.height, '692px');

    // 1. Transient 1 px surface nudge (beginSurfaceNudge -> shrink -> restore -> endSurfaceNudge):
    // must NOT trigger relayout even if held longer than the 120 ms resize debounce.
    clothbound.beginSurfaceNudge(600);
    stageEl.clientHeight = 799;
    resizeCallback();
    await wait(180);
    assert.strictEqual(relocatedCount, 1, 'guarded 1px nudge must not trigger relayout');
    assert.strictEqual(frameEl.style.height, '692px', '#book height must remain locked during nudge');
    stageEl.clientHeight = 800;
    clothbound.endSurfaceNudge(80);
    resizeCallback();
    await wait(160);
    assert.strictEqual(relocatedCount, 1, 'restoring 1px nudge must not trigger relayout');

    // 2. IPC race: ResizeObserver fires BEFORE beginSurfaceNudge arrives.
    stageEl.clientHeight = 799;
    resizeCallback();
    await wait(10);
    clothbound.beginSurfaceNudge(600);
    await wait(170);
    assert.strictEqual(relocatedCount, 1, 'late beginSurfaceNudge must still suppress relayout');
    stageEl.clientHeight = 800;
    clothbound.endSurfaceNudge(60);
    resizeCallback();
    await wait(120);
    assert.strictEqual(relocatedCount, 1, 'restoring after late guard must not trigger relayout');

    // 3. refresh() must NOT toggle opacity/transform on document or chapter iframe,
    // and must use the 1x1 corner paint probe and clean it up cleanly.
    clothbound.refresh();
    assert.strictEqual(docEl.style.opacity, '', 'outer html opacity untouched');
    assert.strictEqual(docEl.style.transform, '', 'outer html transform untouched');
    assert.strictEqual(chapterDocEl.style.opacity, '', 'chapter html opacity untouched');
    assert.strictEqual(chapterDocEl.style.transform, '', 'chapter html transform untouched');
    const probe = documentMock.getElementById('cb-paint-probe');
    assert.ok(probe, 'paint probe created');
    assert.ok(probe.style.backgroundColor.startsWith('rgba('), 'paint probe painted with non-zero alpha');
    flushRaf();
    assert.strictEqual(probe.style.backgroundColor, '', 'paint probe cleaned up after double-rAF');

    // 4. Genuine user window resize STILL relayouts cleanly (even during a nudge guard window).
    clothbound.beginSurfaceNudge(600);
    stageEl.clientHeight = 650;
    resizeCallback();
    await wait(180);
    assert.strictEqual(relocatedCount, 2, 'genuine resize (>2px) must still relayout');
    assert.strictEqual(frameEl.style.height, '542px', '#book height updated for real resize');

    // 5. After guard expires, even a genuine 1 px user resize relayouts cleanly.
    clothbound.endSurfaceNudge(0);
    stageEl.clientHeight = 649;
    resizeCallback();
    await wait(180);
    assert.strictEqual(relocatedCount, 3, 'unguarded 1px user resize must still relayout');
    assert.strictEqual(frameEl.style.height, '541px');

    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");

    QProcess proc;
    proc.start(node, {QStringLiteral("-e"), harness, readerJsPath});
    QVERIFY2(proc.waitForFinished(10000), "node reader.js harness timed out");
    const QByteArray stderrOut = proc.readAllStandardError();
    QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
             stderrOut.isEmpty() ? "node reader.js harness failed" : stderrOut.constData());
}

void ClothboundTests::wave2QmlComponentsCompileCleanlyAndListColumnsAligned() {
    QQmlEngine engine;
    const QString srcRoot = QFINDTESTDATA("../src");
    QVERIFY(QFileInfo::exists(srcRoot));
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));

    QStringList qmlFiles;
    QDirIterator it(srcRoot + QStringLiteral("/ui"), {QStringLiteral("*.qml")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        qmlFiles.append(it.next());
    qmlFiles.sort();

    QVERIFY2(qmlFiles.size() >= 65, qPrintable(QString::number(qmlFiles.size())));
    QStringList failures;
    for (const QString &file : qmlFiles) {
        QQmlComponent component(&engine, QUrl::fromLocalFile(file));
        if (component.isError() || component.status() != QQmlComponent::Ready)
            failures.append(QFileInfo(file).fileName() + QStringLiteral(": ") + component.errorString().trimmed());
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));

    // Regression check for All Books list mode column alignment in LibraryView.qml.
    QFile listFile(srcRoot + QStringLiteral("/ui/library/LibraryView.qml"));
    QVERIFY(listFile.open(QIODevice::ReadOnly));
    const QString qml = QString::fromUtf8(listFile.readAll());
    QVERIFY(qml.contains(QStringLiteral("Layout.preferredWidth: 48")));
    QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 48")));
    QVERIFY(qml.contains(QStringLiteral("Layout.maximumWidth: 48")));
    QVERIFY(qml.contains(QStringLiteral("Layout.preferredWidth: 112")));
    QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 112")));
    QVERIFY(qml.contains(QStringLiteral("Layout.maximumWidth: 112")));
    QVERIFY(qml.contains(QStringLiteral("Layout.preferredWidth: 0")));
    QVERIFY(qml.contains(QStringLiteral("Layout.minimumWidth: 0")));
}

void ClothboundTests::smoothWheelScrollPhysicsAndLibraryWiring() {
    Theme theme;
    QQmlEngine engine;
    const QString srcRoot = QFINDTESTDATA("../src");
    QVERIFY(QFileInfo::exists(srcRoot));
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

    // 1. Standalone SmoothWheel.qml compiles via QQmlComponent and computeWheelTarget() math is exact.
    QQmlComponent swComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/components/SmoothWheel.qml")));
    QVERIFY2(swComp.status() == QQmlComponent::Ready, qPrintable(swComp.errorString()));
    std::unique_ptr<QObject> swObj(swComp.create());
    QVERIFY2(swObj != nullptr, qPrintable(swComp.errorString()));
    QCOMPARE(swObj->property("stepSize").toDouble(), 135.0);
    QCOMPARE(swObj->property("pixelMultiplier").toDouble(), 1.25);

    auto compute = [&](double curY, double activeY, bool gliding, double minY, double maxY,
                       double angleY, double pixelY) -> double {
        QVariant ret;
        const bool ok = QMetaObject::invokeMethod(
            swObj.get(), "computeWheelTarget",
            Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, curY), Q_ARG(QVariant, activeY), Q_ARG(QVariant, gliding),
            Q_ARG(QVariant, minY), Q_ARG(QVariant, maxY),
            Q_ARG(QVariant, angleY), Q_ARG(QVariant, pixelY));
        Q_ASSERT(ok);
        return ret.toDouble();
    };

    // Single discrete wheel notch down (-120) advances by +stepSize (135 px).
    QCOMPARE(compute(0.0, 0.0, false, 0.0, 1000.0, -120.0, 0.0), 135.0);
    // Consecutive notch mid-glide accumulates onto activeTargetY (135 -> 270), even while curY is still at 40.
    QCOMPARE(compute(40.0, 135.0, true, 0.0, 1000.0, -120.0, 0.0), 270.0);
    // Reversing direction mid-glide pivots immediately from currentY instead of overshooting forward.
    QCOMPARE(compute(100.0, 400.0, true, 0.0, 1000.0, 120.0, 0.0), 0.0);
    // Trackpad pixelDelta moves directly by -pixelDeltaY * pixelMultiplier, ignoring stale activeTargetY.
    QCOMPARE(compute(200.0, 600.0, true, 0.0, 1000.0, -60.0, -40.0), 250.0);
    // Top & bottom clamping, including negative minY (GridView/ListView header originY).
    QCOMPARE(compute(950.0, 950.0, false, 0.0, 1000.0, -120.0, 0.0), 1000.0);
    QCOMPARE(compute(30.0, 30.0, false, 0.0, 1000.0, 120.0, 0.0), 0.0);
    QCOMPARE(compute(-260.0, -260.0, false, -260.0, 540.0, -120.0, 0.0), -125.0);
    QCOMPARE(compute(-200.0, -200.0, false, -260.0, 540.0, 120.0, 0.0), -260.0);
    // Zero vertical scroll room stays pinned at minY.
    QCOMPARE(compute(0.0, 0.0, false, 0.0, 0.0, -120.0, 0.0), 0.0);

    // 2. Live QQuickWindow + Flickable + GridView with REAL QWheelEvent delivery:
    //    proves reparenting off contentItem onto viewport (even when contentWidth == 0),
    //    discrete glide accumulation, direct pixelDelta, negative originY header bounds,
    //    Ctrl+wheel passthrough, external reset cancellation, and reducedMotion.
    const QString harnessQml = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "ui/components"
Item {
    id: harnessRoot
    width: 600
    height: 400
    property alias flick: flick
    property alias wheel: sw
    property alias grid: grid
    property alias gridWheel: gridSw
    property int ctrlWheelCount: 0

    Flickable {
        id: flick
        anchors.fill: parent
        // Intentionally omit contentWidth (like LibrarySidebar / BookDetails / CompanionPanel)
        // to verify SmoothWheel reparents onto the Flickable viewport instead of 0-wide contentItem.
        contentHeight: 2000
        boundsBehavior: Flickable.StopAtBounds
        pixelAligned: true
        ScrollBar.vertical: ScrollBar {}
        Item {
            parent: flick
            anchors.fill: parent
            WheelHandler {
                acceptedModifiers: Qt.ControlModifier
                onWheel: harnessRoot.ctrlWheelCount++
            }
        }
        SmoothWheel {
            id: sw
            stepSize: 165
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        visible: false
        cellWidth: 150
        cellHeight: 200
        model: 40
        boundsBehavior: Flickable.StopAtBounds
        pixelAligned: true
        header: Item { width: grid.width; height: 220 }
        delegate: Item { width: 150; height: 200 }
        ScrollBar.vertical: ScrollBar {}
        SmoothWheel {
            id: gridSw
            stepSize: 165
        }
    }
}
)QML");
    QQmlComponent harnessComp(&engine);
    harnessComp.setData(harnessQml.toUtf8(), QUrl::fromLocalFile(srcRoot + QStringLiteral("/SmoothWheelHarness.qml")));
    QVERIFY2(harnessComp.status() == QQmlComponent::Ready, qPrintable(harnessComp.errorString()));
    std::unique_ptr<QQuickWindow> win(new QQuickWindow);
    win->resize(600, 400);
    std::unique_ptr<QObject> rootObj(harnessComp.create());
    QVERIFY2(rootObj != nullptr, qPrintable(harnessComp.errorString()));
    QQuickItem *rootItem = qobject_cast<QQuickItem *>(rootObj.get());
    QVERIFY(rootItem != nullptr);
    rootItem->setParentItem(win->contentItem());
    win->show();
    QVERIFY(QTest::qWaitForWindowExposed(win.get()));

    QQuickItem *flickItem = rootObj->property("flick").value<QQuickItem *>();
    QQuickItem *wheelItem = rootObj->property("wheel").value<QQuickItem *>();
    QQuickItem *gridItem = rootObj->property("grid").value<QQuickItem *>();
    QQuickItem *gridWheelItem = rootObj->property("gridWheel").value<QQuickItem *>();
    QVERIFY(flickItem && wheelItem && gridItem && gridWheelItem);

    // SmoothWheel automatically resolved Flickable & GridView and reparented onto viewport.
    QCOMPARE(wheelItem->property("target").value<QObject *>(), flickItem);
    QCOMPARE(wheelItem->parentItem(), flickItem);
    QCOMPARE(wheelItem->width(), 600.0);
    QCOMPARE(wheelItem->height(), 400.0);
    QCOMPARE(gridWheelItem->property("target").value<QObject *>(), gridItem);
    QCOMPARE(gridWheelItem->parentItem(), gridItem);

    auto sendWheel = [&](QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QPointF pos(300.0, 200.0);
        QWheelEvent ev(pos, pos, pixelDelta, angleDelta, Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(win.get(), &ev);
    };

    // Two rapid REAL mouse-wheel notches accumulate to 330px (never double-scrolled by Flickable).
    sendWheel(QPoint(0, 0), QPoint(0, -120));
    QCOMPARE(wheelItem->property("gliding").toBool(), true);
    QCOMPARE(wheelItem->property("activeTargetY").toDouble(), 165.0);
    sendWheel(QPoint(0, 0), QPoint(0, -120));
    QCOMPARE(wheelItem->property("activeTargetY").toDouble(), 330.0);
    QTRY_VERIFY_WITH_TIMEOUT(!wheelItem->property("gliding").toBool(), 1000);
    QVERIFY2(qAbs(flickItem->property("contentY").toDouble() - 330.0) <= 1.0,
             qPrintable(QString::number(flickItem->property("contentY").toDouble())));

    // Real trackpad pixelDelta cancels any running glide immediately and moves directly by -pixelDelta.y * 1.25.
    sendWheel(QPoint(0, 0), QPoint(0, -120));
    QCOMPARE(wheelItem->property("gliding").toBool(), true);
    const double beforePixel = flickItem->property("contentY").toDouble();
    sendWheel(QPoint(0, -40), QPoint(0, 0));
    QCOMPARE(wheelItem->property("gliding").toBool(), false);
    QVERIFY2(qAbs(flickItem->property("contentY").toDouble() - (beforePixel + 50.0)) <= 1.0,
             qPrintable(QString::number(flickItem->property("contentY").toDouble())));

    // Ctrl+Wheel passes through untouched (leaves contentY stable, reaches outer Ctrl WheelHandler).
    const double beforeCtrl = flickItem->property("contentY").toDouble();
    sendWheel(QPoint(0, 0), QPoint(0, -120), Qt::ControlModifier);
    QCOMPARE(flickItem->property("contentY").toDouble(), beforeCtrl);
    QCOMPARE(rootObj->property("ctrlWheelCount").toInt(), 1);

    // External contentY reset (like positionViewAtBeginning) cancels an in-flight glide cleanly.
    sendWheel(QPoint(0, 0), QPoint(0, -120));
    QCOMPARE(wheelItem->property("gliding").toBool(), true);
    flickItem->setProperty("contentY", 0.0);
    QCOMPARE(wheelItem->property("gliding").toBool(), false);
    QCOMPARE(flickItem->property("contentY").toDouble(), 0.0);

    // reducedMotion jumps instantaneously and clamps cleanly at top (0) and bottom (1600).
    theme.setReducedMotion(true);
    sendWheel(QPoint(0, 0), QPoint(0, -120));
    QCOMPARE(wheelItem->property("gliding").toBool(), false);
    QCOMPARE(flickItem->property("contentY").toDouble(), 165.0);
    sendWheel(QPoint(0, 0), QPoint(0, 2400));
    QCOMPARE(flickItem->property("contentY").toDouble(), 0.0);
    sendWheel(QPoint(0, 0), QPoint(0, -24000));
    QCOMPARE(flickItem->property("contentY").toDouble(), 1600.0);

    // GridView with a 220px header (negative originY): scrolls down by exact stepSize and back to header top.
    flickItem->setVisible(false);
    gridItem->setVisible(true);
    QMetaObject::invokeMethod(gridItem, "positionViewAtBeginning");
    const double gridOriginY = gridItem->property("originY").toDouble();
    QVERIFY2(gridOriginY <= -200.0, qPrintable(QString::number(gridOriginY)));
    QCOMPARE(gridItem->property("contentY").toDouble(), gridOriginY);
    sendWheel(QPoint(0, 0), QPoint(0, -120));
    QVERIFY2(qAbs(gridItem->property("contentY").toDouble() - (gridOriginY + 165.0)) <= 1.0,
             qPrintable(QStringLiteral("contentY=%1 expected=%2").arg(gridItem->property("contentY").toDouble()).arg(gridOriginY + 165.0)));
    sendWheel(QPoint(0, 0), QPoint(0, 240));
    QVERIFY2(qAbs(gridItem->property("contentY").toDouble() - gridOriginY) <= 1.0,
             qPrintable(QStringLiteral("contentY=%1 expected=%2").arg(gridItem->property("contentY").toDouble()).arg(gridOriginY)));

    // When no vertical scroll room exists (contentHeight <= height), handleWheelDelta returns false.
    gridItem->setVisible(false);
    flickItem->setVisible(true);
    flickItem->setProperty("contentHeight", 300.0);
    flickItem->setProperty("contentY", 0.0);
    QVariant handled;
    QVERIFY(QMetaObject::invokeMethod(wheelItem, "handleWheelDelta", Q_RETURN_ARG(QVariant, handled),
                                      Q_ARG(QVariant, -120.0), Q_ARG(QVariant, 0.0)));
    QCOMPARE(handled.toBool(), false);
    rootItem->setParentItem(nullptr);

    // 3. CoverImage.qml compiles cleanly with async decoding and rounded-mask guards.
    QQmlComponent coverComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CoverImage.qml")));
    QVERIFY2(coverComp.status() == QQmlComponent::Ready, qPrintable(coverComp.errorString()));

    // 4. LibraryView.qml wires SmoothWheel + flick tuning on both grid and listView,
    //    and all app scroll views wire SmoothWheel consistently.
    auto readQml = [&](const QString &rel) -> QString {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    };
    const QString libQml = readQml(QStringLiteral("ui/library/LibraryView.qml"));
    const qsizetype gridPos = libQml.indexOf(QStringLiteral("id: grid"));
    const qsizetype listPos = libQml.indexOf(QStringLiteral("id: listView"));
    QVERIFY(gridPos > 0 && listPos > gridPos);
    const QString gridSection = libQml.mid(gridPos, listPos - gridPos);
    const QString listSection = libQml.mid(listPos, 900);
    QVERIFY(gridSection.contains(QStringLiteral("SmoothWheel { stepSize: 165 }")));
    QVERIFY(gridSection.contains(QStringLiteral("maximumFlickVelocity: 4500")));
    QVERIFY(gridSection.contains(QStringLiteral("flickDeceleration: 2200")));
    QVERIFY(gridSection.contains(QStringLiteral("pixelAligned: true")));
    QVERIFY(listSection.contains(QStringLiteral("SmoothWheel { stepSize: 115 }")));
    QVERIFY(listSection.contains(QStringLiteral("maximumFlickVelocity: 4500")));
    QVERIFY(listSection.contains(QStringLiteral("flickDeceleration: 2200")));
    QVERIFY(listSection.contains(QStringLiteral("pixelAligned: true")));

    QVERIFY(readQml(QStringLiteral("ui/library/LibrarySidebar.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 105 }")));
    QVERIFY(readQml(QStringLiteral("ui/settings/SettingsView.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 120 }")));
    QVERIFY(readQml(QStringLiteral("ui/audio/VoicePanel.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 110 }")));
    QVERIFY(readQml(QStringLiteral("ui/reader/ContentsPanel.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 110 }")));
    QVERIFY(readQml(QStringLiteral("ui/reader/MarksPanel.qml")).contains(QStringLiteral("SmoothWheel { stepSize: 110 }")));
    const QString compQml = readQml(QStringLiteral("ui/companion/CompanionPanel.qml"));
    QVERIFY(compQml.count(QStringLiteral("SmoothWheel { stepSize: 115 }")) >= 4);
}

void ClothboundTests::pdfPresetViewModes() {
    QTemporaryDir dir;
    const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
    QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Preset Modes", "Ada Marsh")));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(pdfPath);
    QVERIFY(bookId > 0);

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(bookId));
    QCOMPARE(reader.format(), QStringLiteral("pdf"));

    Marks marks(&library);
    BookSearch bookSearch(&reader);
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    Narration narration(&services, &cache, &reader, &library);
    narration.m_player->setHeadless(true);
    Theme theme;

    const QVariantMap mockReadingSettings{
        {QStringLiteral("pageTurnSound"), true},
        {QStringLiteral("pdfFit"), QStringLiteral("page")},
    };

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
    const QString srcRoot = QFINDTESTDATA("../src");
    QVERIFY(QFileInfo::exists(srcRoot));
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
    engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
    engine.rootContext()->setContextProperty(QStringLiteral("readingSettings"), mockReadingSettings);

    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &list) { warnings.append(list); });

    // 1. Live PdfSurface.qml fit modes: Fit page, Fit width (+ live resize/Companion dock), Actual, and Custom zoom.
    QQmlComponent surfComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml")));
    QVERIFY2(surfComp.status() == QQmlComponent::Ready, qPrintable(surfComp.errorString()));
    const QVariantMap look{
        {QStringLiteral("bg"), QStringLiteral("#FAF6EE")},
        {QStringLiteral("fg"), QStringLiteral("#26221D")},
        {QStringLiteral("muted"), QStringLiteral("#787066")},
        {QStringLiteral("line"), QStringLiteral("#E2DCD0")},
        {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
        {QStringLiteral("link"), QStringLiteral("#8A5A12")},
        {QStringLiteral("dark"), false},
    };
    std::unique_ptr<QObject> surfObj(surfComp.createWithInitialProperties({
        {QStringLiteral("width"), 1000.0},
        {QStringLiteral("height"), 760.0},
        {QStringLiteral("look"), look},
    }));
    QVERIFY2(surfObj != nullptr, qPrintable(surfComp.errorString()));

    QVariantMap styleMap{
        {QStringLiteral("layout"), QStringLiteral("single")},
        {QStringLiteral("pdfCrop"), false},
        {QStringLiteral("pdfOriginalColors"), false},
        {QStringLiteral("pdfFit"), QStringLiteral("page")},
        {QStringLiteral("marginTop"), 60.0},
        {QStringLiteral("marginBottom"), 76.0},
        {QStringLiteral("animate"), false},
    };
    const QVariantMap posMap{{QStringLiteral("chapter"), 0}};
    QMetaObject::invokeMethod(surfObj.get(), "open",
                              Q_ARG(QVariant, reader.manifest()),
                              Q_ARG(QVariant, posMap),
                              Q_ARG(QVariant, styleMap));
    QCoreApplication::processEvents();

    // Default Fit page: fits inside 1000x760 with zero panning and 1.0 effectiveZoom.
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("page"));
    QVERIFY(qAbs(surfObj->property("effectiveZoom").toDouble() - 1.0) < 0.02);
    QCOMPARE(surfObj->property("canPanX").toBool(), false);
    QCOMPARE(surfObj->property("canPanY").toBool(), false);
    const double fitPageW = surfObj->property("activeDisplayWidth").toDouble();
    QVERIFY2(fitPageW > 200.0 && fitPageW < 700.0, qPrintable(QString::number(fitPageW)));

    // Fit width: fills roomWidth (1000 - 48 = 952), zero horizontal overflow, vertical scroll active.
    QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("width")));
    QCoreApplication::processEvents();
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("width"));
    QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - 952.0) < 1.5,
             qPrintable(QString::number(surfObj->property("activeDisplayWidth").toDouble())));
    QCOMPARE(surfObj->property("canPanX").toBool(), false);
    QCOMPARE(surfObj->property("canPanY").toBool(), true);
    QCOMPARE(surfObj->property("zoomedIn").toBool(), true);
    const double widthScale = surfObj->property("effectiveZoom").toDouble();
    QVERIFY2(widthScale > 1.3, qPrintable(QString::number(widthScale)));

    // Live auto-refit when window tiles or Companion drawer docks (1000 -> 680 px).
    surfObj->setProperty("width", 680.0);
    QCoreApplication::processEvents();
    QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - 632.0) < 1.5,
             qPrintable(QString::number(surfObj->property("activeDisplayWidth").toDouble())));
    QCOMPARE(surfObj->property("canPanX").toBool(), false);
    surfObj->setProperty("width", 1000.0);
    QCoreApplication::processEvents();

    // Actual (1:1 print scale): 96/72 logical pixels per PDF point.
    QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("actual")));
    QCoreApplication::processEvents();
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("actual"));
    const QVariantMap pts = reader.pdfPageSize(0);
    const double expectedActualW = pts.value(QStringLiteral("width")).toDouble() * (96.0 / 72.0);
    QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - expectedActualW) < 1.5,
             qPrintable(QStringLiteral("got=%1 expected=%2")
                            .arg(surfObj->property("activeDisplayWidth").toDouble())
                            .arg(expectedActualW)));

    // Nudging zoom from Fit width transitions smoothly to custom without jumping back to 1.0x.
    QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("width")));
    QCoreApplication::processEvents();
    const double beforeNudge = surfObj->property("effectiveZoom").toDouble();
    QMetaObject::invokeMethod(surfObj.get(), "nudgeZoom", Q_ARG(QVariant, 1.15));
    QCoreApplication::processEvents();
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("custom"));
    QVERIFY2(surfObj->property("effectiveZoom").toDouble() > beforeNudge * 1.10,
             qPrintable(QStringLiteral("before=%1 after=%2")
                            .arg(beforeNudge)
                            .arg(surfObj->property("effectiveZoom").toDouble())));

    // Unrelated setStyle() update (same pdfFit) preserves active custom zoom.
    styleMap[QStringLiteral("pdfFit")] = QStringLiteral("width");
    styleMap[QStringLiteral("pdfOriginalColors")] = true;
    QMetaObject::invokeMethod(surfObj.get(), "setStyle", Q_ARG(QVariant, styleMap));
    QCoreApplication::processEvents();
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("custom"));

    // Explicit pdfFit change in setStyle() snaps to the new preset.
    styleMap[QStringLiteral("pdfFit")] = QStringLiteral("page");
    QMetaObject::invokeMethod(surfObj.get(), "setStyle", Q_ARG(QVariant, styleMap));
    QCoreApplication::processEvents();
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("page"));

    // Scroll layout also honors Fit width with zero horizontal overflow.
    styleMap[QStringLiteral("layout")] = QStringLiteral("scroll");
    styleMap[QStringLiteral("pdfFit")] = QStringLiteral("width");
    QMetaObject::invokeMethod(surfObj.get(), "setStyle", Q_ARG(QVariant, styleMap));
    QCoreApplication::processEvents();
    QCOMPARE(surfObj->property("fitMode").toString(), QStringLiteral("width"));
    QVERIFY2(qAbs(surfObj->property("activeDisplayWidth").toDouble() - 952.0) < 1.5,
             qPrintable(QString::number(surfObj->property("activeDisplayWidth").toDouble())));
    QCOMPARE(surfObj->property("canPanX").toBool(), false);

    // 1b. Two-page spread Fit width: next()/prev() (Left/Right arrows) turn immediately on a single press
    //     without vertically shifting first, while scrollVertical() scrolls tall pages cleanly.
    {
        const QString multiPath = dir.filePath(QStringLiteral("multi.pdf"));
        {
            QPdfWriter writer(multiPath);
            writer.setPageSize(QPageSize(QPageSize::Letter));
            writer.setResolution(72);
            QPainter p(&writer);
            for (int i = 0; i < 5; ++i) {
                if (i > 0)
                    writer.newPage();
                p.drawText(QPointF(72, 100), QStringLiteral("Page %1").arg(i + 1));
            }
        }
        const int multiId = library.addBookFile(multiPath);
        QVERIFY(multiId > 0);
        QVERIFY(reader.openBook(multiId));
        QCOMPARE(reader.chapterCount(), 5);

        surfObj->setProperty("width", 1280.0);
        surfObj->setProperty("height", 800.0);
        QVariantMap spreadStyle{
            {QStringLiteral("layout"), QStringLiteral("double")},
            {QStringLiteral("pdfCrop"), false},
            {QStringLiteral("pdfOriginalColors"), false},
            {QStringLiteral("pdfFit"), QStringLiteral("width")},
            {QStringLiteral("marginTop"), 60.0},
            {QStringLiteral("marginBottom"), 76.0},
            {QStringLiteral("animate"), false},
        };
        const QVariantMap startPos{{QStringLiteral("chapter"), 0}};
        QMetaObject::invokeMethod(surfObj.get(), "open",
                                  Q_ARG(QVariant, reader.manifest()),
                                  Q_ARG(QVariant, startPos),
                                  Q_ARG(QVariant, spreadStyle));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("spread").toBool(), true);
        QCOMPARE(surfObj->property("current").toInt(), 0);
        QCOMPARE(surfObj->property("canPanY").toBool(), true);

        // Single press of next() turns from cover [0] -> spread [1, 2] -> spread [3, 4].
        QMetaObject::invokeMethod(surfObj.get(), "next");
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("current").toInt(), 1);
        QCOMPARE(surfObj->property("canPanY").toBool(), true);
        QMetaObject::invokeMethod(surfObj.get(), "next");
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("current").toInt(), 3);

        // Single press of prev() turns straight back [3, 4] -> [1, 2] -> [0].
        QMetaObject::invokeMethod(surfObj.get(), "prev");
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("current").toInt(), 1);
        QMetaObject::invokeMethod(surfObj.get(), "prev");
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("current").toInt(), 0);

        // scrollVertical(1) scrolls vertically down the tall page without turning pages.
        QVariant scrolled;
        QMetaObject::invokeMethod(surfObj.get(), "scrollVertical",
                                  Q_RETURN_ARG(QVariant, scrolled), Q_ARG(QVariant, 1));
        QCOMPARE(scrolled.toBool(), true);
        QCOMPARE(surfObj->property("current").toInt(), 0);

        // Switch back to original single-page PDF for remaining per-book checks.
        QVERIFY(reader.openBook(bookId));
    }

    // 2. Per-book appearance override persists pdfFit cleanly across Reader reload.
    reader.saveBookAppearance(QVariantMap{
        {QStringLiteral("pdfFit"), QStringLiteral("width")},
        {QStringLiteral("pdfCrop"), true},
    });
    QVERIFY(reader.bookAppearanceOverride());
    QCOMPARE(reader.bookAppearance().value(QStringLiteral("pdfFit")).toString(), QStringLiteral("width"));
    Reader reloaded(&library, &scheme);
    QVERIFY(reloaded.openBook(bookId));
    QVERIFY(reloaded.bookAppearanceOverride());
    QCOMPARE(reloaded.bookAppearance().value(QStringLiteral("pdfFit")).toString(), QStringLiteral("width"));

    // 3. AppearancePanel.qml instantiates cleanly for PDFs with Fit presets and emits pdfFitChosen.
    QQmlComponent apComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/AppearancePanel.qml")));
    QVERIFY2(apComp.status() == QQmlComponent::Ready, qPrintable(apComp.errorString()));
    const QVariantMap apSettings{
        {QStringLiteral("theme"), QStringLiteral("paper")},
        {QStringLiteral("font"), QStringLiteral("Literata")},
        {QStringLiteral("fontScale"), 1.15},
        {QStringLiteral("fontWeight"), 400},
        {QStringLiteral("lineHeight"), 1.62},
        {QStringLiteral("paragraphSpacing"), 0.0},
        {QStringLiteral("margins"), QStringLiteral("normal")},
        {QStringLiteral("justify"), true},
        {QStringLiteral("hyphenate"), true},
        {QStringLiteral("dropCap"), true},
        {QStringLiteral("layout"), QStringLiteral("auto")},
        {QStringLiteral("animate"), true},
        {QStringLiteral("publisher"), false},
        {QStringLiteral("pdfCrop"), true},
        {QStringLiteral("pdfOriginalColors"), false},
        {QStringLiteral("pdfFit"), QStringLiteral("width")},
    };
    std::unique_ptr<QObject> apObj(apComp.createWithInitialProperties({
        {QStringLiteral("height"), 760.0},
        {QStringLiteral("look"), look},
        {QStringLiteral("settings"), apSettings},
        {QStringLiteral("pdf"), true},
        {QStringLiteral("pdfSurface"), QVariant::fromValue(surfObj.get())},
        {QStringLiteral("bookOverride"), false},
    }));
    QVERIFY2(apObj != nullptr, qPrintable(apComp.errorString()));
    QCoreApplication::processEvents();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));

    // 4. Bookmark dog-ear stays clear of Companion button (52 px tool reserve) and Companion C toggles cleanly.
    {
        QFile erFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
        QVERIFY(erFile.open(QIODevice::ReadOnly));
        const QString erQml = QString::fromUtf8(erFile.readAll());
        QVERIFY(erQml.contains(QStringLiteral("anchors.rightMargin: 52")));
        QVERIFY(erQml.contains(QStringLiteral("companionPanel.open ? companionPanel.close() : showCompanion(\"ask\")")));

        QFile cpFile(srcRoot + QStringLiteral("/ui/companion/CompanionPanel.qml"));
        QVERIFY(cpFile.open(QIODevice::ReadOnly));
        const QString cpQml = QString::fromUtf8(cpFile.readAll());
        QVERIFY(cpQml.contains(QStringLiteral("function show(which, focusInput)")));
        QVERIFY(cpQml.contains(QStringLiteral("if (focusInput && !choosing && tab === \"ask\")")));
    }
}

void ClothboundTests::smoothWheelTrackpadAccelerationAndKineticCoast() {
    Theme theme;
    QQmlEngine engine;
    const QString srcRoot = QFINDTESTDATA("../src");
    QVERIFY(QFileInfo::exists(srcRoot));
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

    // 1. Pure velocity-gain and kinetic coast math.
    QQmlComponent swComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/components/SmoothWheel.qml")));
    QVERIFY2(swComp.status() == QQmlComponent::Ready, qPrintable(swComp.errorString()));
    std::unique_ptr<QObject> swObj(swComp.create());
    QVERIFY2(swObj != nullptr, qPrintable(swComp.errorString()));

    auto gainFor = [&](double speed) -> double {
        QVariant ret;
        QMetaObject::invokeMethod(swObj.get(), "trackpadGainForSpeed", Q_RETURN_ARG(QVariant, ret), Q_ARG(QVariant, speed));
        return ret.toDouble();
    };
    QCOMPARE(gainFor(150.0), 1.0);
    QVERIFY(gainFor(1200.0) > 1.8);
    QVERIFY(gainFor(2500.0) >= 2.75);

    auto coastPlan = [&](double curY, double vel, double minY, double maxY) -> QVariantMap {
        QVariant ret;
        QMetaObject::invokeMethod(swObj.get(), "computeCoastPlan", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, curY), Q_ARG(QVariant, vel),
                                  Q_ARG(QVariant, minY), Q_ARG(QVariant, maxY));
        return ret.toMap();
    };
    // Below threshold -> zero coast.
    QCOMPARE(coastPlan(100.0, 150.0, 0.0, 6000.0).value(QStringLiteral("duration")).toInt(), 0);
    // Brisk downward flick -> substantial smooth coast distance (reaches across long views effortlessly).
    const QVariantMap brisk = coastPlan(100.0, 3600.0, 0.0, 6000.0);
    QVERIFY2(brisk.value(QStringLiteral("distance")).toDouble() > 1500.0,
             qPrintable(QString::number(brisk.value(QStringLiteral("distance")).toDouble())));
    QVERIFY(brisk.value(QStringLiteral("duration")).toInt() >= 220);
    // Clamping near bottom shortens duration proportionally rather than crawling.
    const QVariantMap nearBottom = coastPlan(5800.0, 4500.0, 0.0, 6000.0);
    QCOMPARE(nearBottom.value(QStringLiteral("targetY")).toDouble(), 6000.0);
    QVERIFY(nearBottom.value(QStringLiteral("duration")).toInt() < 350);

    // 2. Live QQuickWindow trackpad flick -> accelerated tracking + kinetic coast on Qt::ScrollEnd,
    //    immediate stop when fingers touch back down, and zero double-coast on native ScrollMomentum.
    const QString harnessQml = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "ui/components"
Item {
    width: 600
    height: 400
    property alias flick: flick
    property alias wheel: sw
    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: 8000
        boundsBehavior: Flickable.StopAtBounds
        pixelAligned: true
        ScrollBar.vertical: ScrollBar {}
        SmoothWheel {
            id: sw
            stepSize: 165
        }
    }
}
)QML");
    QQmlComponent harnessComp(&engine);
    harnessComp.setData(harnessQml.toUtf8(), QUrl::fromLocalFile(srcRoot + QStringLiteral("/SmoothWheelTrackpadHarness.qml")));
    QVERIFY2(harnessComp.status() == QQmlComponent::Ready, qPrintable(harnessComp.errorString()));
    std::unique_ptr<QQuickWindow> win(new QQuickWindow);
    win->resize(600, 400);
    std::unique_ptr<QObject> rootObj(harnessComp.create());
    QVERIFY2(rootObj != nullptr, qPrintable(harnessComp.errorString()));
    QQuickItem *rootItem = qobject_cast<QQuickItem *>(rootObj.get());
    QVERIFY(rootItem != nullptr);
    rootItem->setParentItem(win->contentItem());
    win->show();
    QVERIFY(QTest::qWaitForWindowExposed(win.get()));

    QQuickItem *flickItem = rootObj->property("flick").value<QQuickItem *>();
    QQuickItem *wheelItem = rootObj->property("wheel").value<QQuickItem *>();
    QVERIFY(flickItem && wheelItem);

    auto invokeWheel = [&](double ay, double py, int phase, bool isTouchPad) -> bool {
        QVariant ret;
        QMetaObject::invokeMethod(wheelItem, "handleWheelEvent", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, ay), Q_ARG(QVariant, py),
                                  Q_ARG(QVariant, phase), Q_ARG(QVariant, isTouchPad));
        return ret.toBool();
    };

    // Simulate a brisk 6-frame two-finger flick (14 px per ~10 ms = ~1750 px/s raw).
    QVERIFY(invokeWheel(0.0, -14.0, int(Qt::ScrollBegin), true));
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(10);
        QVERIFY(invokeWheel(0.0, -14.0, int(Qt::ScrollUpdate), true));
    }
    const double atLiftY = flickItem->property("contentY").toDouble();
    // Adaptive velocity gain should already exceed flat 1.25x (6 * 14 * 1.25 = 105 px).
    QVERIFY2(atLiftY > 140.0, qPrintable(QStringLiteral("atLiftY=%1").arg(atLiftY)));
    // Lift fingers -> ScrollEnd immediately kicks off kinetic coasting.
    QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollEnd), true));
    QCOMPARE(wheelItem->property("coasting").toBool(), true);
    const double coastTarget = wheelItem->property("activeTargetY").toDouble();
    QVERIFY2(coastTarget > atLiftY + 800.0,
             qPrintable(QStringLiteral("coastTarget=%1 atLiftY=%2").arg(coastTarget).arg(atLiftY)));

    // Putting fingers back down (ScrollBegin) catches and stops the coast instantly.
    QTest::qWait(40);
    QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollBegin), true));
    QCOMPARE(wheelItem->property("coasting").toBool(), false);
    QCOMPARE(wheelItem->property("gliding").toBool(), false);

    // Native compositor ScrollMomentum passes through directly without launching synthetic coasting.
    const double beforeMom = flickItem->property("contentY").toDouble();
    QVERIFY(invokeWheel(0.0, -20.0, int(Qt::ScrollMomentum), true));
    QCOMPARE(wheelItem->property("coasting").toBool(), false);
    QVERIFY2(qAbs(flickItem->property("contentY").toDouble() - (beforeMom + 25.0)) <= 1.0,
             qPrintable(QString::number(flickItem->property("contentY").toDouble())));
    QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollEnd), true));
    QCOMPARE(wheelItem->property("coasting").toBool(), false);

    // Reduced motion disables kinetic coasting.
    theme.setReducedMotion(true);
    QVERIFY(invokeWheel(0.0, -16.0, int(Qt::ScrollBegin), true));
    for (int i = 0; i < 4; ++i) {
        QTest::qWait(10);
        QVERIFY(invokeWheel(0.0, -16.0, int(Qt::ScrollUpdate), true));
    }
    QVERIFY(invokeWheel(0.0, 0.0, int(Qt::ScrollEnd), true));
    QCOMPARE(wheelItem->property("coasting").toBool(), false);
    rootItem->setParentItem(nullptr);

    // 3. PdfSurface.qml attaches SmoothWheel to both continuous scroll pageList and zoomed pan view.
    QFile pdfFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml"));
    QVERIFY(pdfFile.open(QIODevice::ReadOnly));
    const QString pdfQml = QString::fromUtf8(pdfFile.readAll());
    QVERIFY(pdfQml.contains(QStringLiteral("SmoothWheel { stepSize: 165 }")));
    QVERIFY(pdfQml.contains(QStringLiteral("enabled: surface.canPanY")));
}

void ClothboundTests::fixReaderUiReaderJsRecoveryClobberAndPerf() {
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node executable not available");

    const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
    QVERIFY(QFileInfo::exists(readerJsPath));

    const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;
let fakeNow = 1000;
let nextTimerId = 1;
const timers = new Map();

function fakeSetTimeout(fn, delay) {
    const id = nextTimerId++;
    timers.set(id, { fn, at: fakeNow + Math.max(0, Number(delay) || 0) });
    return id;
}
function fakeClearTimeout(id) { timers.delete(id); }
function advanceTimers(ms) {
    fakeNow += ms;
    while (true) {
        let earliestId = null, earliest = null;
        for (const [id, entry] of timers.entries()) {
            if (entry.at <= fakeNow && (!earliest || entry.at < earliest.at)) {
                earliestId = id;
                earliest = entry;
            }
        }
        if (!earliest) break;
        timers.delete(earliestId);
        earliest.fn();
    }
}

let autoRaf = true;
let nextRafId = 1;
const rafQueue = new Map();
function fakeRaf(cb) {
    const id = nextRafId++;
    rafQueue.set(id, cb);
    if (autoRaf) {
        queueMicrotask(() => {
            const fn = rafQueue.get(id);
            if (!fn) return;
            rafQueue.delete(id);
            fakeNow += 120;
            fn(fakeNow);
        });
    }
    return id;
}
function fakeCancelRaf(id) { rafQueue.delete(id); }
function stepRaf(deltaMs) {
    fakeNow += deltaMs;
    const batch = [...rafQueue.entries()];
    rafQueue.clear();
    for (const [, fn] of batch) fn(fakeNow);
}

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        willChange: '', colorScheme: '', left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}
function makeClassList() {
    const s = new Set();
    return { add(c) { s.add(c); }, remove(c) { s.delete(c); }, contains(c) { return s.has(c); } };
}

let getImageDataCalls = 0;
let badgeCreations = 0;
const frameModes = new Map(); // url -> 'ok' | 'error' | 'hang' | 'nullDoc' | 'clobber' | 'image' | 'multipage'
const createdDocs = new Map();

function makeDoc(ownerFrame, url, mode) {
    const listeners = new Map();
    const headChildren = [];
    const byId = new Map();
    if (mode === 'clobber') {
        byId.set('cb-typography', { localName: 'div', id: 'cb-typography', textContent: 'PRESERVE_TYPO' });
        byId.set('cb-layout', { localName: 'p', id: 'cb-layout', textContent: 'PRESERVE_LAYOUT' });
    }
    const textNode = { nodeType: 3, data: 'Clothbound chapter text for testing recovery and perf.', parentElement: null };
    const bodyEl = {
        childNodes: [textNode],
        lastElementChild: {
            getClientRects() { return [{ left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }]; },
            getBoundingClientRect() { return { left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }; },
        },
    };
    textNode.parentElement = bodyEl;
    const pages = mode === 'multipage' ? 2 : 1;
    const docEl = {
        style: makeStyle(),
        get scrollWidth() { return pages * (parseFloat(ownerFrame.style.width) || 680); },
        scrollHeight: 692,
        scrollLeft: 0,
        scrollTop: 0,
        scrollTo(opts) {
            if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
            if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
            for (const fn of (listeners.get('scroll') || [])) fn();
        },
        appendChild() {},
    };
    const imgAttrs = new Map();
    const mockImg = {
        complete: true, naturalWidth: 16, naturalHeight: 16,
        src: 'clothbound://book/1/fig.png', currentSrc: 'clothbound://book/1/fig.png',
        hasAttribute(k) { return imgAttrs.has(k); },
        setAttribute(k, v) { imgAttrs.set(k, v); },
        getAttribute(k) { return imgAttrs.get(k); },
    };
    const doc = {
        URL: url,
        _headChildren: headChildren,
        _byId: byId,
        _mockImg: mockImg,
        documentElement: docEl,
        head: {
            appendChild(el) {
                headChildren.push(el);
                if (el.id && !byId.has(el.id)) byId.set(el.id, el);
            },
        },
        body: bodyEl,
        scrollingElement: docEl,
        fonts: { ready: Promise.resolve() },
        images: mode === 'image' ? [mockImg] : [],
        defaultView: {
            getComputedStyle() { return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start', whiteSpace: 'normal' }; },
            CSS: { highlights: new Map() },
            Highlight: class { constructor(...r) { this.ranges = r; this.priority = 0; } },
        },
        getElementById(id) { return byId.get(id) || null; },
        createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
        querySelector() { return null; },
        querySelectorAll() { return []; },
        addEventListener(type, fn) {
            if (!listeners.has(type)) listeners.set(type, []);
            listeners.get(type).push(fn);
        },
        removeEventListener() {},
        createTreeWalker() {
            let done = false;
            return { nextNode() { if (done) return null; done = true; return textNode; } };
        },
        createRange() {
            return {
                selectNodeContents() {},
                setStart() {},
                setEnd() {},
                getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
                getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
            };
        },
        getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
    };
    createdDocs.set(url, doc);
    return doc;
}

function makeFrame(id) {
    const f = {
        id,
        style: makeStyle(),
        classList: makeClassList(),
        contentDocument: null,
        contentWindow: null,
        onload: null,
        onerror: null,
        _src: 'about:blank',
        setAttribute() {},
        removeAttribute() {},
        getBoundingClientRect() { return { left: 160, top: 54, width: parseFloat(this.style.width) || 680, height: 692 }; },
    };
    Object.defineProperty(f, 'src', {
        get() { return this._src; },
        set(v) {
            this._src = v;
            if (!v || v === 'about:blank') {
                this.contentDocument = null;
                this.contentWindow = null;
                return;
            }
            const mode = frameModes.get(v) || 'ok';
            if (mode === 'hang') {
                this.contentDocument = null;
                this.contentWindow = null;
                return;
            }
            if (mode === 'error') {
                this.contentDocument = null;
                this.contentWindow = null;
                const cb = this.onerror;
                if (typeof cb === 'function') queueMicrotask(() => cb());
                return;
            }
            if (mode === 'nullDoc') {
                this.contentDocument = null;
                this.contentWindow = null;
                const cb = this.onload;
                if (typeof cb === 'function') queueMicrotask(() => cb());
                return;
            }
            const doc = makeDoc(this, v, mode);
            this.contentDocument = doc;
            this.contentWindow = doc.defaultView;
            const cb = this.onload;
            if (typeof cb === 'function') queueMicrotask(() => cb());
        },
    });
    return f;
}

const stageEl = {
    clientWidth: 1200, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 1200, height: 800 }; },
};
const viewportEl = { style: makeStyle(), classList: makeClassList() };
const bookFrame = makeFrame('book');
const backFrame = makeFrame('book-back');
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'viewport') return viewportEl;
        if (id === 'book') return bookFrame;
        if (id === 'book-back') return backFrame;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement(tag) {
        if (tag === 'canvas') {
            return {
                width: 32, height: 32,
                getContext() {
                    return {
                        clearRect() {},
                        drawImage() {},
                        getImageData(_x, _y, w, h) {
                            ++getImageDataCalls;
                            const data = new Uint8ClampedArray(w * h * 4);
                            // Half transparent, half dark monochrome pixels -> data-cb-lineart.
                            for (let i = 0; i < w * h; ++i) {
                                data[i * 4 + 0] = 20;
                                data[i * 4 + 1] = 20;
                                data[i * 4 + 2] = 20;
                                data[i * 4 + 3] = (i % 2 === 0) ? 0 : 255;
                            }
                            return { data };
                        },
                    };
                },
            };
        }
        if (tag === 'button') {
            ++badgeCreations;
            return {
                tagName: 'BUTTON', type: '', className: '', title: '', innerHTML: '',
                style: makeStyle(),
                setAttribute() {}, addEventListener() {}, remove() {},
            };
        }
        return { tagName: tag.toUpperCase(), id: '', style: makeStyle(), setAttribute() {}, remove() {} };
    },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

const logs = [];
let lastLoc = null;
const bridge = {
    surfaceLoaded() {},
    relocated(loc) { lastLoc = loc; },
    notifyPageTurn() {},
    log(msg) { logs.push(String(msg)); },
};

const sandbox = {
    console, Promise, URL, Uint8ClampedArray,
    setTimeout: fakeSetTimeout, clearTimeout: fakeClearTimeout,
    performance: { now: () => fakeNow },
    requestAnimationFrame: fakeRaf, cancelAnimationFrame: fakeCancelRaf,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1200, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class { observe() {} },
    matchMedia: () => ({ matches: false }),
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;
const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

const makeStyleCfg = dark => ({
    theme: { bg: dark ? '#1e1d1b' : '#fbfaf6', fg: dark ? '#dcd5c8' : '#23221f', link: '#8a6a2a',
             muted: '#6f6b63', popover: '#fff', border: 'rgba(0,0,0,0.12)', selection: '#cde',
             search: '#ff0', found: '#fa0', markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f',
             markPink: '#f0f', speaking: '#eef', dark },
    font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.0,
    margin: 0.1, measure: 30, justify: false, hyphenate: false, layout: 'single', singleColumn: false,
    animate: true, reducedMotion: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
    listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
});
const makeBook = (session, href) => ({
    session, base: `clothbound://book/${session}/`, language: 'en', rtl: false,
    spine: [{ href, linear: true }], pageTargets: [],
});

(async () => {
    // 1. onerror recovery: state.queue does NOT deadlock; next open() succeeds.
    frameModes.set('clothbound://book/1/bad.xhtml', 'error');
    frameModes.set('clothbound://book/2/good.xhtml', 'ok');
    await clothbound.open(makeBook(1, 'bad.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.ok(logs.some(m => m.includes('Failed to load chapter')), 'onerror should log error');
    await clothbound.open(makeBook(2, 'good.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(lastLoc.session, 2, 'queue recovered after onerror');

    // 2. 10s safety timeout recovery: hanging iframe times out and unblocks state.queue.
    logs.length = 0;
    frameModes.set('clothbound://book/3/hang.xhtml', 'hang');
    frameModes.set('clothbound://book/4/after-timeout.xhtml', 'ok');
    const pHang = clothbound.open(makeBook(3, 'hang.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    await Promise.resolve();
    advanceTimers(10001);
    await pHang;
    assert.ok(logs.some(m => m.includes('Timed out loading chapter')), 'timeout should log error');
    await clothbound.open(makeBook(4, 'after-timeout.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(lastLoc.session, 4, 'queue recovered after timeout');

    // 3. contentDocument null check recovery.
    logs.length = 0;
    frameModes.set('clothbound://book/5/null.xhtml', 'nullDoc');
    frameModes.set('clothbound://book/6/after-null.xhtml', 'ok');
    await clothbound.open(makeBook(5, 'null.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.ok(logs.some(m => m.includes('has no document')), 'null contentDocument should log error');
    await clothbound.open(makeBook(6, 'after-null.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(lastLoc.session, 6, 'queue recovered after null contentDocument');

    // 4. Generation/session cancellation: opening Book B cancels hanging Book A immediately with ZERO error log.
    logs.length = 0;
    frameModes.set('clothbound://book/7/hang-cancel.xhtml', 'hang');
    frameModes.set('clothbound://book/8/fresh.xhtml', 'ok');
    const pCancelled = clothbound.open(makeBook(7, 'hang-cancel.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    await Promise.resolve();
    const pFresh = clothbound.open(makeBook(8, 'fresh.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    await Promise.all([pCancelled, pFresh]);
    assert.strictEqual(lastLoc.session, 8, 'book 8 opened cleanly without waiting for timeout');
    assert.strictEqual(logs.length, 0, 'cancelled superseded load must not log an error');

    // 5. styleElement() DOM clobbering protection: non-<style> elements with cb-typography/cb-layout stay untouched.
    frameModes.set('clothbound://book/9/clobber.xhtml', 'clobber');
    await clothbound.open(makeBook(9, 'clobber.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    const clobDoc = createdDocs.get('clothbound://book/9/clobber.xhtml');
    assert.strictEqual(clobDoc.getElementById('cb-typography').textContent, 'PRESERVE_TYPO');
    assert.strictEqual(clobDoc.getElementById('cb-layout').textContent, 'PRESERVE_LAYOUT');
    const stylesAfterOpen = clobDoc._headChildren.filter(e => e.localName === 'style');
    assert.strictEqual(stylesAfterOpen.length, 2, 'two real <style> elements injected into head');
    await clothbound.setStyle(makeStyleCfg(true));
    assert.strictEqual(clobDoc.getElementById('cb-typography').textContent, 'PRESERVE_TYPO');
    assert.strictEqual(clobDoc._headChildren.filter(e => e.localName === 'style').length, 2, 'style updates reuse elements');

    // 6. classifyImages() skipped in light theme and runs when switching to dark theme.
    getImageDataCalls = 0;
    frameModes.set('clothbound://book/10/img.xhtml', 'image');
    await clothbound.open(makeBook(10, 'img.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    assert.strictEqual(getImageDataCalls, 0, 'light mode must skip synchronous getImageData()');
    await clothbound.setStyle(makeStyleCfg(true));
    assert.strictEqual(getImageDataCalls, 1, 'dark mode switch classifies raster images once');
    const imgDoc = createdDocs.get('clothbound://book/10/img.xhtml');
    assert.strictEqual(imgDoc._mockImg.hasAttribute('data-cb-lineart'), true);

    // 7. layoutMarginNotes() paused during turnTo() RAF animation, runs once on settle.
    frameModes.set('clothbound://book/11/notes.xhtml', 'multipage');
    await clothbound.open(makeBook(11, 'notes.xhtml'), { chapter: 0, offset: 0 }, makeStyleCfg(false));
    clothbound.marks([{ id: 1, start: 0, end: 10, color: 'yellow', noted: true, note: 'Margin note' }]);
    await Promise.resolve();
    badgeCreations = 0;
    autoRaf = false;
    const pTurn = clothbound.next();
    await Promise.resolve();
    // Step 3 intermediate RAF frames mid-turn: scroll fires every frame, but 0 badges created mid-turn!
    stepRaf(40);
    stepRaf(40);
    stepRaf(40);
    assert.strictEqual(badgeCreations, 0, 'layoutMarginNotes must pause during turnTo RAF animation');
    // Finish turn and flush the settled scheduleMarginNotes RAF once.
    stepRaf(140);
    await pTurn;
    stepRaf(16);
    assert.strictEqual(badgeCreations, 1, 'layoutMarginNotes runs once after turnTo settles');

    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");

    QProcess proc;
    proc.start(node, {QStringLiteral("-e"), harness, readerJsPath});
    QVERIFY2(proc.waitForFinished(10000), "node fix-reader-ui reader.js harness timed out");
    const QByteArray stderrOut = proc.readAllStandardError();
    QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
             stderrOut.isEmpty() ? "node fix-reader-ui reader.js harness failed" : stderrOut.constData());
}

void ClothboundTests::fixReaderUiQmlSafetyLifecyclePanningAndPerf() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    const QString pdfPath = dir.filePath(QStringLiteral("pan.pdf"));
    QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("<b>Injected Title</b>", "Ada Marsh")));

    Library library(dataDir, cacheDir);
    QVERIFY(library.open());
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(pdfId > 0);

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(pdfId));
    Marks marks(&library);
    BookSearch bookSearch(&reader);
    LibraryIndex libraryIndex(cacheDir + QStringLiteral("/index.sqlite"));
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices ai(&gemini, &keys, library.database().connectionName());
    AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    Narration narration(&ai, &audioCache, &reader, &library);
    narration.m_player->setHeadless(true);
    Companion companion(&ai, &reader, library.database().connectionName());
    Illustrator illustrator(&ai, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("pictures")));
    LocalSetup localSetup(&ai);
    Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
    Translator translator;

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
    const QString srcRoot = QFINDTESTDATA("../src");
    QVERIFY(QFileInfo::exists(srcRoot));
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
    engine.rootContext()->setContextProperty(QStringLiteral("libraryIndex"), &libraryIndex);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
    engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
    engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
    engine.rootContext()->setContextProperty(QStringLiteral("companion"), &companion);
    engine.rootContext()->setContextProperty(QStringLiteral("illustrator"), &illustrator);
    engine.rootContext()->setContextProperty(QStringLiteral("localSetup"), &localSetup);
    engine.rootContext()->setContextProperty(QStringLiteral("settingsUrl"),
                                             QUrl::fromLocalFile(dir.filePath(QStringLiteral("settings.ini"))).toString());

    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errs) { warnings.append(errs); });

    // 1. Safe-by-default Text.PlainText on Label, ToolTip, Toast.
    {
        QQmlComponent styleComp(&engine);
        styleComp.setData(QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "ui/components"
Item {
    property alias lbl: lbl
    property alias tip: tip
    property alias toast: toast
    Label { id: lbl; text: "<img src=x>hello" }
    ToolTip { id: tip; text: "<img src=x>tip" }
    Toast { id: toast }
}
)QML"),
                          QUrl::fromLocalFile(srcRoot + QStringLiteral("/PlainTextCheck.qml")));
        QVERIFY2(styleComp.status() == QQmlComponent::Ready, qPrintable(styleComp.errorString()));
        std::unique_ptr<QObject> obj(styleComp.create());
        QVERIFY(obj != nullptr);
        QObject *lbl = obj->property("lbl").value<QObject *>();
        QObject *tip = obj->property("tip").value<QObject *>();
        QVERIFY(lbl && tip);
        QCOMPARE(lbl->property("textFormat").toInt(), int(Qt::PlainText));
        QObject *tipContent = tip->property("contentItem").value<QObject *>();
        QVERIFY(tipContent != nullptr);
        QCOMPARE(tipContent->property("textFormat").toInt(), int(Qt::PlainText));
    }

    // 2. SmoothWheel coast distance clamped to slope-matched cap (zero 3.67x velocity surge).
    {
        QQmlComponent swComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/components/SmoothWheel.qml")));
        QVERIFY2(swComp.status() == QQmlComponent::Ready, qPrintable(swComp.errorString()));
        std::unique_ptr<QObject> swObj(swComp.create());
        QVERIFY(swObj != nullptr);
        QVariant ret;
        QVERIFY(QMetaObject::invokeMethod(swObj.get(), "computeCoastPlan", Q_RETURN_ARG(QVariant, ret),
                                          Q_ARG(QVariant, 0.0), Q_ARG(QVariant, 9000.0),
                                          Q_ARG(QVariant, 0.0), Q_ARG(QVariant, 50000.0)));
        const QVariantMap maxPlan = ret.toMap();
        const double dist = maxPlan.value(QStringLiteral("distance")).toDouble();
        const double durSec = maxPlan.value(QStringLiteral("duration")).toDouble() / 1000.0;
        QVERIFY(durSec > 0.0);
        const double initVel = (3.0 * dist) / durSec;
        QVERIFY2(initVel <= 9050.0, qPrintable(QStringLiteral("initial velocity surged to %1 px/s").arg(initVel)));
    }

    // 3. CoverImage gates effectiveSource on visible (real QQuickWindow visibility).
    {
        QQmlComponent covComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CoverImage.qml")));
        QVERIFY2(covComp.status() == QQmlComponent::Ready, qPrintable(covComp.errorString()));
        std::unique_ptr<QQuickWindow> covWin(new QQuickWindow);
        covWin->resize(200, 240);
        std::unique_ptr<QObject> covObj(covComp.createWithInitialProperties({
            {QStringLiteral("visible"), false},
            {QStringLiteral("width"), 120.0},
            {QStringLiteral("height"), 180.0},
            {QStringLiteral("source"), QStringLiteral("file:///tmp/nonexistent-cover.png")},
        }));
        QVERIFY(covObj != nullptr);
        auto *covItem = qobject_cast<QQuickItem *>(covObj.get());
        QVERIFY(covItem != nullptr);
        covItem->setParentItem(covWin->contentItem());
        covWin->show();
        QVERIFY(QTest::qWaitForWindowExposed(covWin.get()));
        QCOMPARE(covObj->property("effectiveSource").toString(), QString());
        covItem->setVisible(true);
        QVERIFY(!covObj->property("effectiveSource").toString().isEmpty());
        covItem->setParentItem(nullptr);
    }

    // 4. BookDetails does not flash visible when window widens while closed.
    {
        QQmlComponent detComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/BookDetails.qml")));
        QVERIFY2(detComp.status() == QQmlComponent::Ready, qPrintable(detComp.errorString()));
        std::unique_ptr<QObject> detObj(detComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 760.0},
        }));
        QVERIFY(detObj != nullptr);
        QCOMPARE(detObj->property("visible").toBool(), false);
        detObj->setProperty("width", 1360.0);
        QCoreApplication::processEvents();
        QCOMPARE(detObj->property("visible").toBool(), false);
        QCOMPARE(detObj->property("slideProgress").toDouble(), 0.0);
    }

    // 5. NoteEditor bounds long notes inside Flickable so buttons stay on-screen.
    {
        const int hlId = marks.addHighlight(0, 0, 12, QStringLiteral("Short quote"), QStringLiteral("yellow"), QStringLiteral("P1"));
        QString longNote;
        for (int i = 0; i < 60; ++i)
            longNote += QStringLiteral("Line %1 of a very long margin note that must scroll.\n").arg(i);
        marks.setNote(hlId, longNote);

        QQmlComponent noteHostComp(&engine);
        noteHostComp.setData(QByteArrayLiteral(R"QML(
import QtQuick
import "ui/reader"
Item {
    width: 720; height: 520
    property alias editor: ed
    NoteEditor {
        id: ed
        look: ({ dark: false, bg: "#fff", fg: "#111", muted: "#666", popover: "#fff", line: "#ddd", link: "#00f" })
    }
}
)QML"),
                             QUrl::fromLocalFile(srcRoot + QStringLiteral("/NoteEditorHost.qml")));
        QVERIFY2(noteHostComp.status() == QQmlComponent::Ready, qPrintable(noteHostComp.errorString()));
        std::unique_ptr<QObject> hostObj(noteHostComp.create());
        QVERIFY(hostObj != nullptr);
        QObject *ed = hostObj->property("editor").value<QObject *>();
        QVERIFY(ed != nullptr);
        QMetaObject::invokeMethod(ed, "show", Q_ARG(QVariant, hlId));
        QCoreApplication::processEvents();
        QVERIFY2(ed->property("height").toDouble() <= 472.5,
                 qPrintable(QString::number(ed->property("height").toDouble())));
    }

    // 6. PdfSurface proportional Ctrl+Wheel zoom & Fit-to-Width canPan.
    {
        const QVariantMap look{
            {QStringLiteral("bg"), QStringLiteral("#FAF6EE")},
            {QStringLiteral("fg"), QStringLiteral("#26221D")},
            {QStringLiteral("muted"), QStringLiteral("#787066")},
            {QStringLiteral("line"), QStringLiteral("#E2DCD0")},
            {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
            {QStringLiteral("link"), QStringLiteral("#8A5A12")},
            {QStringLiteral("dark"), false},
        };
        QQmlComponent surfComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml")));
        QVERIFY2(surfComp.status() == QQmlComponent::Ready, qPrintable(surfComp.errorString()));
        std::unique_ptr<QObject> surfObj(surfComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 760.0},
            {QStringLiteral("look"), look},
        }));
        QVERIFY(surfObj != nullptr);
        QVariantMap styleMap{
            {QStringLiteral("layout"), QStringLiteral("single")},
            {QStringLiteral("pdfCrop"), false},
            {QStringLiteral("pdfOriginalColors"), false},
            {QStringLiteral("pdfFit"), QStringLiteral("width")},
            {QStringLiteral("marginTop"), 60.0},
            {QStringLiteral("marginBottom"), 76.0},
            {QStringLiteral("animate"), false},
        };
        const QVariantMap posMap{{QStringLiteral("chapter"), 0}};
        QMetaObject::invokeMethod(surfObj.get(), "open",
                                  Q_ARG(QVariant, reader.manifest()),
                                  Q_ARG(QVariant, posMap),
                                  Q_ARG(QVariant, styleMap));
        QCoreApplication::processEvents();
        QCOMPARE(surfObj->property("canPanX").toBool(), false);
        QCOMPARE(surfObj->property("canPanY").toBool(), true);
        QCOMPARE(surfObj->property("canPan").toBool(), true);
        const double z0 = surfObj->property("effectiveZoom").toDouble();
        QMetaObject::invokeMethod(surfObj.get(), "zoomByWheel", Q_ARG(QVariant, 15.0));
        const double zSmall = surfObj->property("effectiveZoom").toDouble();
        QVERIFY2(zSmall > z0 && zSmall < z0 * 1.03,
                 qPrintable(QStringLiteral("z0=%1 zSmall=%2").arg(z0).arg(zSmall)));
    }

    // 7. CommandPalette classifies builtin:/studio: offline voices & clears cachedBooks on close().
    {
        QQmlComponent cpComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/CommandPalette.qml")));
        QVERIFY2(cpComp.status() == QQmlComponent::Ready, qPrintable(cpComp.errorString()));
        std::unique_ptr<QObject> cpObj(cpComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 760.0},
        }));
        QVERIFY(cpObj != nullptr);
        QVariant sub;
        narration.setVoice(QStringLiteral("builtin:kokoro:af_heart"), QStringLiteral("Heart"));
        QVERIFY(QMetaObject::invokeMethod(cpObj.get(), "voiceSubtitle", Q_RETURN_ARG(QVariant, sub)));
        QVERIFY2(sub.toString().contains(QStringLiteral("on this computer")), qPrintable(sub.toString()));
        narration.setVoice(QStringLiteral("studio:kokoro"), QStringLiteral("Studio Voice"));
        QVERIFY(QMetaObject::invokeMethod(cpObj.get(), "voiceSubtitle", Q_RETURN_ARG(QVariant, sub)));
        QVERIFY2(sub.toString().contains(QStringLiteral("on this computer")), qPrintable(sub.toString()));
        QMetaObject::invokeMethod(cpObj.get(), "show");
        QVERIFY(!cpObj->property("cachedBooks").toList().isEmpty());
        QMetaObject::invokeMethod(cpObj.get(), "close");
        QVERIFY(cpObj->property("cachedBooks").toList().isEmpty());
    }

    // 8. CompanionPanel instantiates cleanly with virtualized ListView, throttled stream, and zero QML warnings.
    {
        QQmlComponent compComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/companion/CompanionPanel.qml")));
        QVERIFY2(compComp.status() == QQmlComponent::Ready, qPrintable(compComp.errorString()));
        std::unique_ptr<QObject> compObj(compComp.createWithInitialProperties({
            {QStringLiteral("height"), 760.0},
            {QStringLiteral("look"), QVariantMap{{QStringLiteral("dark"), false}}},
        }));
        QVERIFY(compObj != nullptr);
        QCoreApplication::processEvents();
    }

    // 9. Wiring checks for Main.qml defaultSleep, EpubReader.qml teardown/subframes/lifecycle,
    //    VoicePanel.qml lazy Loader, PdfSurface.qml mipmap/canPan, LibraryView.qml dialog MouseArea.
    auto readSrc = [&](const QString &rel) -> QString {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    const QString mainQml = readSrc(QStringLiteral("ui/Main.qml"));
    QVERIFY(mainQml.contains(QStringLiteral("id: listenSettings")));
    QVERIFY(mainQml.contains(QStringLiteral("property string defaultSleep: \"off\"")));
    QVERIFY(mainQml.contains(QStringLiteral("win.applyDefaultSleep()")));

    const QString erQml = readSrc(QStringLiteral("ui/reader/EpubReader.qml"));
    QVERIFY(erQml.contains(QStringLiteral("!request.isMainFrame && (u === \"about:blank\" || u === \"about:srcdoc\")")));
    QVERIFY(erQml.contains(QStringLiteral("(reader.open && !view.pdf)")));
    QVERIFY(erQml.contains(QStringLiteral("closeWebSurface()")));

    const QString pdfQml = readSrc(QStringLiteral("ui/reader/PdfSurface.qml"));
    QVERIFY(pdfQml.contains(QStringLiteral("mipmap: true")));
    QVERIFY(pdfQml.contains(QStringLiteral("panning = surface.canPan &&")));

    const QString vpQml = readSrc(QStringLiteral("ui/audio/VoicePanel.qml"));
    QVERIFY(vpQml.contains(QStringLiteral("id: designLoader")));
    QVERIFY(vpQml.contains(QStringLiteral("active: delWrap.isDesign")));

    const QString cpQml = readSrc(QStringLiteral("ui/companion/CompanionPanel.qml"));
    QVERIFY(cpQml.contains(QStringLiteral("id: talkModel")));
    QVERIFY(cpQml.contains(QStringLiteral("id: streamThrottle")));
    QVERIFY(cpQml.contains(QStringLiteral("onClosed: item = null")));

    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
}

void ClothboundTests::aPlusCraftBridgeAccessibilityAndReducedMotion() {
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());
    auto readSrc = [&](const QString &rel) -> QString {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };

    // 1. EpubReader.qml: zero new Function / eval / brittle string replace; typed dispatch + keyboard reachability.
    const QString erQml = readSrc(QStringLiteral("ui/reader/EpubReader.qml"));
    QVERIFY(!erQml.isEmpty());
    QVERIFY2(!erQml.contains(QStringLiteral("new Function")), "EpubReader.qml must not use new Function");
    QVERIFY2(!erQml.contains(QStringLiteral("eval(")), "EpubReader.qml must not use eval()");
    QVERIFY2(!erQml.contains(QStringLiteral("code.replace(\"clothbound.marks(\"")),
             "EpubReader.qml must not use brittle clothbound.marks string replace");
    QVERIFY(erQml.contains(QStringLiteral("function surfaceOpen(")));
    QVERIFY(erQml.contains(QStringLiteral("function surfaceFound(")));
    QVERIFY(erQml.contains(QStringLiteral("function surfaceMarks(")));
    QVERIFY(erQml.contains(QStringLiteral("function surfaceGoTo(")));
    QVERIFY(erQml.contains(QStringLiteral("readonly property bool chromeFocused:")));
    QVERIFY(erQml.contains(QStringLiteral("readonly property bool readingViewportFocused:")));
    QVERIFY(erQml.contains(QStringLiteral("tabFocusable: view.chromeShown")));
    QVERIFY(erQml.contains(QStringLiteral("enabled: view.keys && view.readingViewportFocused")));

    // 2. Accessibility roles & descriptive names across primary interactive surfaces.
    const QString bookCardQml = readSrc(QStringLiteral("ui/library/BookCard.qml"));
    QVERIFY(bookCardQml.contains(QStringLiteral("Accessible.role: Accessible.ListItem")));
    QVERIFY(bookCardQml.contains(QStringLiteral("Accessible.name:")));

    const QString libraryViewQml = readSrc(QStringLiteral("ui/library/LibraryView.qml"));
    QVERIFY(libraryViewQml.contains(QStringLiteral("Accessible.role: Accessible.ListItem")));
    QVERIFY(libraryViewQml.contains(QStringLiteral("Accessible.name:")));

    const QString continueShelfQml = readSrc(QStringLiteral("ui/library/ContinueShelf.qml"));
    QVERIFY(continueShelfQml.contains(QStringLiteral("Accessible.role: Accessible.Button")));
    QVERIFY(continueShelfQml.count(QStringLiteral("Accessible.name:")) >= 3);

    const QString cmdPaletteQml = readSrc(QStringLiteral("ui/CommandPalette.qml"));
    QVERIFY(cmdPaletteQml.contains(QStringLiteral("Accessible.role: Accessible.ListItem")));
    QVERIFY(cmdPaletteQml.contains(QStringLiteral("Accessible.name:")));

    const QString companionQml = readSrc(QStringLiteral("ui/companion/CompanionPanel.qml"));
    QVERIFY(companionQml.contains(QStringLiteral("Accessible.role: Accessible.EditableText")));
    QVERIFY(companionQml.contains(QStringLiteral("Accessible.name: questionPlaceholder.text")));

    const QString listenOfferQml = readSrc(QStringLiteral("ui/audio/ListenOffer.qml"));
    QVERIFY(listenOfferQml.contains(QStringLiteral("FocusScope {")));
    QVERIFY(listenOfferQml.contains(QStringLiteral("Keys.onEscapePressed:")));
    QVERIFY(listenOfferQml.contains(QStringLiteral("Accessible.role: Accessible.Dialog")));

    // 3. Reduced motion across views, ListenBar sleepMoon gating, and Clothbound style controls.
    const QString skeletonQml = readSrc(QStringLiteral("ui/components/Skeleton.qml"));
    QVERIFY(skeletonQml.contains(QStringLiteral("!theme.reducedMotion")));

    const QString welcomeQml = readSrc(QStringLiteral("ui/Welcome.qml"));
    QVERIFY(welcomeQml.contains(QStringLiteral("theme.reducedMotion ? 0 : 150")));

    QVERIFY(cmdPaletteQml.contains(QStringLiteral("theme.reducedMotion ? 0 : 140")));

    const QString bookDetailsQml = readSrc(QStringLiteral("ui/library/BookDetails.qml"));
    QVERIFY(bookDetailsQml.contains(QStringLiteral("theme.reducedMotion")));

    const QString listenBarQml = readSrc(QStringLiteral("ui/audio/ListenBar.qml"));
    QVERIFY(listenBarQml.contains(QStringLiteral("running: narration.sleepFading && expandedCol.visible && !theme.reducedMotion")));
    QVERIFY(!listenBarQml.contains(QStringLiteral("bar.visible && !theme.reducedMotion")));
    QVERIFY(listenBarQml.contains(QStringLiteral("enabled: !theme.reducedMotion")));

    for (const QString &ctrl : {QStringLiteral("Button.qml"), QStringLiteral("Popup.qml"),
                                QStringLiteral("Menu.qml"), QStringLiteral("ToolTip.qml"),
                                QStringLiteral("Slider.qml"), QStringLiteral("ProgressBar.qml"),
                                QStringLiteral("ScrollBar.qml")}) {
        const QString qml = readSrc(QStringLiteral("ui/style/Clothbound/") + ctrl);
        QVERIFY2(qml.contains(QStringLiteral("theme.reducedMotion")), qPrintable(ctrl));
    }

    // 4. Live ListenOffer FocusScope instantiation + Esc dismissal (zero QML warnings).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    BookScheme scheme;
    Reader reader(&library, &scheme);
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices ai(&gemini, &keys, library.database().connectionName());
    AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    Narration narration(&ai, &audioCache, &reader, &library);
    narration.m_player->setHeadless(true);
    VoicePacks voicePacks(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                          packsManifest(QStringLiteral("http://127.0.0.1/")));
    LocalSpeech localSpeech(&voicePacks);
    VoiceCatalog voices(&ai, &audioCache, &reader, library.database().connectionName(),
                        dir.filePath(QStringLiteral("voices-cache.json")));
    Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
    Translator translator;

    QQmlEngine engine;
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
    engine.rootContext()->setContextProperty(QStringLiteral("voicePacks"), &voicePacks);
    engine.rootContext()->setContextProperty(QStringLiteral("localSpeech"), &localSpeech);
    engine.rootContext()->setContextProperty(QStringLiteral("voices"), &voices);
    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errs) { warnings.append(errs); });

    QQmlComponent offerComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/audio/ListenOffer.qml")));
    QVERIFY2(offerComp.status() == QQmlComponent::Ready, qPrintable(offerComp.errorString()));
    QQuickWindow win;
    win.resize(900, 640);
    std::unique_ptr<QObject> offerObj(offerComp.create());
    QVERIFY(offerObj != nullptr);
    auto *offerItem = qobject_cast<QQuickItem *>(offerObj.get());
    QVERIFY(offerItem != nullptr);
    QVERIFY(offerItem->isFocusScope());
    offerItem->setParentItem(win.contentItem());
    QSignalSpy closedSpy(offerItem, SIGNAL(closed()));
    QVERIFY(QMetaObject::invokeMethod(offerItem, "show"));
    QCOMPARE(offerItem->property("open").toBool(), true);
    QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(offerItem, &escEvent);
    QCOMPARE(offerItem->property("open").toBool(), false);
    QCOMPARE(closedSpy.count(), 1);
    QVERIFY2(warnings.isEmpty(),
             qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
}

void ClothboundTests::aPlusCraftHotPathsCommandPaletteCatalogsPdfAndReader() {
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());
    auto readSrc = [&](const QString &rel) -> QString {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };

    // 1. CatalogsSheet scroll preservation + PdfSurface O(1) indexAt & drag-select coalescer + single-open Reader::openPdf.
    const QString catalogsQml = readSrc(QStringLiteral("ui/library/CatalogsSheet.qml"));
    QVERIFY(catalogsQml.contains(QStringLiteral("function syncFeed()")));
    QVERIFY(catalogsQml.contains(QStringLiteral("model: sheet.feedEntries")));
    QVERIFY(catalogsQml.contains(QStringLiteral("Qt.callLater(restoreScroll)")));

    const QString pdfSurfaceQml = readSrc(QStringLiteral("ui/reader/PdfSurface.qml"));
    QVERIFY(pdfSurfaceQml.contains(QStringLiteral("pageList.indexAt(")));
    QVERIFY2(!pdfSurfaceQml.contains(QStringLiteral("for (let i = 0; i < pageList.count")),
             "PdfSurface.qml must not scan pageList linearly");
    QVERIFY(pdfSurfaceQml.contains(QStringLiteral("id: dragSelectTimer")));
    QVERIFY(pdfSurfaceQml.contains(QStringLiteral("function flushPendingSelect()")));
    QVERIFY(pdfSurfaceQml.contains(QStringLiteral("function applySelection(")));

    const QString readerCpp = readSrc(QStringLiteral("reader/reader.cpp"));
    QVERIFY2(!readerCpp.contains(QStringLiteral("PdfBook own(path)")),
             "Reader::openPdf() must reuse m_pdf instead of opening redundant PdfBook instances");

    // 2. Live CommandPalette pre-normalized search cache + diacritic-insensitive matching.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    auto makeSimpleEpub = [](const QByteArray &title, const QByteArray &author, const QByteArray &body) {
        const QByteArray opf =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
            "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "    <dc:title>" + title + "</dc:title>\n"
            "    <dc:creator>" + author + "</dc:creator>\n"
            "    <dc:language>en</dc:language>\n"
            "  </metadata>\n"
            "  <manifest>\n"
            "    <item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "  </manifest>\n"
            "  <spine><itemref idref=\"ch1\"/></spine>\n"
            "</package>";
        return TestBooks::epub({
            {QStringLiteral("OEBPS/content.opf"), opf},
            {QStringLiteral("OEBPS/ch1.xhtml"), TestBooks::chapter(title, body)},
        });
    };
    const QString epub1 = dir.filePath(QStringLiteral("miserables.epub"));
    const QString epub2 = dir.filePath(QStringLiteral("uber.epub"));
    QVERIFY(TestBooks::write(epub1, makeSimpleEpub("Les Misérables", "Victor Hugo",
                                                   "<p>Jean Valjean walked on.</p>")));
    QVERIFY(TestBooks::write(epub2, makeSimpleEpub("Über die Liebe", "Rainer Maria Rilke",
                                                   "<p>Ein ruhiger Abend.</p>")));

    // Build a minimal 2-page PDF embedding a DOI so we also test m_pdf citation reuse & unified init.
    auto makeDoiPdf = []() {
        QByteArray out = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        auto object = [&](const QByteArray &body) {
            offsets.append(out.size());
            out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
        };
        object("<< /Type /Catalog /Pages 2 0 R >>");
        object("<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>");
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> >>");
        const QByteArray s1 = "BT /F1 11 Tf 36 340 Td (Attention Is All You Need DOI: 10.1145/9876543.2109876) Tj ET";
        object("<< /Length " + QByteArray::number(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");
        object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 7 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> >>");
        const QByteArray s2 = "BT /F1 11 Tf 36 340 Td (Second page of the paper with body text.) Tj ET";
        object("<< /Length " + QByteArray::number(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");
        const qsizetype xref = out.size();
        out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
        for (const qsizetype off : offsets)
            out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
        out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
               QByteArray::number(xref) + "\n%%EOF\n";
        return out;
    };
    const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
    QVERIFY(TestBooks::write(pdfPath, makeDoiPdf()));

    Library library(dataDir, cacheDir);
    QVERIFY(library.open());
    QVERIFY(library.addBookFile(epub1) > 0);
    QVERIFY(library.addBookFile(epub2) > 0);
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(pdfId > 0);

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QSignalSpy lengthsSpy(&reader, &Reader::lengthsChanged);
    QVERIFY(reader.openBook(pdfId));
    QTRY_VERIFY_WITH_TIMEOUT(lengthsSpy.count() >= 1, 5000);
    QCOMPARE(library.chapterLengths(pdfId).size(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!library.bookSetting(pdfId, QStringLiteral("pdf_crop")).isEmpty(), 5000);

    // Remove the PDF file on disk while it remains open in Reader: formatBookCitation()
    // called with only `path` (and id=0) must still enrich the DOI from the already-open m_pdf.
    QVERIFY(QFile::remove(pdfPath));
    const QString bibtex = reader.formatBookCitation(
        QVariantMap{
            {QStringLiteral("id"), 0},
            {QStringLiteral("title"), QStringLiteral("Attention Is All You Need")},
            {QStringLiteral("author"), QStringLiteral("Ashish Vaswani")},
            {QStringLiteral("format"), QStringLiteral("pdf")},
            {QStringLiteral("path"), pdfPath},
        },
        QStringLiteral("bibtex"));
    QVERIFY2(bibtex.contains(QStringLiteral("10.1145/9876543.2109876")), qPrintable(bibtex));

    // CommandPalette live pre-normalized search test.
    Marks marks(&library);
    BookSearch bookSearch(&reader);
    LibraryIndex libraryIndex(cacheDir + QStringLiteral("/index.sqlite"));
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices ai(&gemini, &keys, library.database().connectionName());
    AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    Narration narration(&ai, &audioCache, &reader, &library);
    narration.m_player->setHeadless(true);
    Companion companion(&ai, &reader, library.database().connectionName());
    Illustrator illustrator(&ai, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("pictures")));
    LocalSetup localSetup(&ai);
    Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
    Translator translator;

    QQmlEngine engine;
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
    engine.rootContext()->setContextProperty(QStringLiteral("libraryIndex"), &libraryIndex);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
    engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
    engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
    engine.rootContext()->setContextProperty(QStringLiteral("companion"), &companion);
    engine.rootContext()->setContextProperty(QStringLiteral("illustrator"), &illustrator);
    engine.rootContext()->setContextProperty(QStringLiteral("localSetup"), &localSetup);

    QQmlComponent cpComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/CommandPalette.qml")));
    QVERIFY2(cpComp.status() == QQmlComponent::Ready, qPrintable(cpComp.errorString()));
    std::unique_ptr<QObject> cpObj(cpComp.createWithInitialProperties({
        {QStringLiteral("width"), 1000.0},
        {QStringLiteral("height"), 760.0},
    }));
    QVERIFY(cpObj != nullptr);
    QMetaObject::invokeMethod(cpObj.get(), "show");
    const QVariantList cached = cpObj->property("cachedBooks").toList();
    QVERIFY(cached.size() >= 2);
    bool sawMiserables = false;
    bool sawUber = false;
    for (const QVariant &v : cached) {
        const QVariantMap entry = v.toMap();
        const QString normTitle = entry.value(QStringLiteral("normTitle")).toString();
        QVERIFY(!entry.value(QStringLiteral("book")).toMap().isEmpty());
        QVERIFY(!entry.value(QStringLiteral("normFull")).toString().isEmpty());
        if (normTitle == QStringLiteral("les miserables"))
            sawMiserables = true;
        if (normTitle == QStringLiteral("uber die liebe"))
            sawUber = true;
    }
    QVERIFY(sawMiserables);
    QVERIFY(sawUber);

    QMetaObject::invokeMethod(cpObj.get(), "search", Q_ARG(QVariant, QStringLiteral("miserables")));
    const QVariantList results = cpObj->property("results").toList();
    bool matchedBook = false;
    for (const QVariant &r : results) {
        const QVariantMap row = r.toMap();
        if (row.value(QStringLiteral("kind")).toString() == QStringLiteral("book") &&
            row.value(QStringLiteral("title")).toString() == QStringLiteral("Les Misérables")) {
            matchedBook = true;
            break;
        }
    }
    QVERIFY(matchedBook);
    QMetaObject::invokeMethod(cpObj.get(), "close");
    QVERIFY(cpObj->property("cachedBooks").toList().isEmpty());

    const QString cpQml = readSrc(QStringLiteral("ui/CommandPalette.qml"));
    QCOMPARE(cpQml.count(QStringLiteral(".normalize(\"NFD\")")), 1);
    QVERIFY(cpQml.contains(QStringLiteral("function buildBookCache()")));
    QVERIFY(cpQml.contains(QStringLiteral("function fuzzyScoreNormalized(")));
}

void ClothboundTests::aPlusCraftReaderJsSwipeQueueAndScrollQuiet() {
    const QString nodeBin = QStandardPaths::findExecutable(QStringLiteral("node"));
    QVERIFY2(!nodeBin.isEmpty(), "node executable required for reader.js verification");

    const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
    QVERIFY(QFileInfo::exists(readerJsPath));

    const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;
let wheelHandler = null;
let fakeNow = 1000;
let nextTimerId = 1;
const timers = new Map();

function fakeSetTimeout(fn, delay) {
    const id = nextTimerId++;
    timers.set(id, { fn, at: fakeNow + Math.max(0, Number(delay) || 0) });
    return id;
}
function fakeClearTimeout(id) { timers.delete(id); }
function advanceTimers(ms) {
    fakeNow += ms;
    while (true) {
        let earliestId = null, earliest = null;
        for (const [id, entry] of timers.entries()) {
            if (entry.at <= fakeNow && (!earliest || entry.at < earliest.at)) {
                earliestId = id;
                earliest = entry;
            }
        }
        if (!earliest) break;
        timers.delete(earliestId);
        earliest.fn();
    }
}

let nextRafId = 1;
const rafQueue = new Map();
function fakeRaf(cb) {
    const id = nextRafId++;
    rafQueue.set(id, cb);
    queueMicrotask(() => {
        const fn = rafQueue.get(id);
        if (!fn) return;
        rafQueue.delete(id);
        fakeNow += 120;
        fn(fakeNow);
    });
    return id;
}
function fakeCancelRaf(id) { rafQueue.delete(id); }

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        willChange: '', colorScheme: '', left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}
function makeClassList() {
    const s = new Set();
    return { add(c) { s.add(c); }, remove(c) { s.delete(c); }, contains(c) { return s.has(c); } };
}

function makeDoc(ownerFrame, url, pages) {
    const headChildren = [];
    const byId = new Map();
    const textNode = { nodeType: 3, data: 'Chapter text for swipe serialization and scroll quiet tests.', parentElement: null };
    const bodyEl = {
        childNodes: [textNode],
        lastElementChild: {
            getClientRects() { return [{ left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }]; },
            getBoundingClientRect() { return { left: 0, right: 200, top: 0, bottom: 24, width: 200, height: 24 }; },
        },
    };
    textNode.parentElement = bodyEl;
    const docEl = {
        style: makeStyle(),
        get scrollWidth() { return pages * (parseFloat(ownerFrame.style.width) || 680); },
        scrollHeight: 600,
        clientHeight: 600,
        scrollLeft: 0,
        scrollTop: 0,
        scrollTo(opts) {
            if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
            if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
        },
        appendChild() {},
    };
    return {
        URL: url,
        documentElement: docEl,
        head: { appendChild(el) { headChildren.push(el); if (el.id) byId.set(el.id, el); } },
        body: bodyEl,
        scrollingElement: docEl,
        fonts: { ready: Promise.resolve() },
        images: [],
        defaultView: {
            getComputedStyle() { return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start', whiteSpace: 'normal' }; },
            CSS: { highlights: new Map() },
            Highlight: class { constructor(...r) { this.ranges = r; this.priority = 0; } },
        },
        getElementById(id) { return byId.get(id) || null; },
        createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
        querySelector() { return null; },
        querySelectorAll() { return []; },
        addEventListener() {},
        removeEventListener() {},
        createTreeWalker() {
            let done = false;
            return { nextNode() { if (done) return null; done = true; return textNode; } };
        },
        createRange() {
            return {
                selectNodeContents() {}, setStart() {}, setEnd() {},
                getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
                getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
            };
        },
        getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
    };
}

let deferredResolve = null;
const pageCounts = new Map([
    ['clothbound://book/1/ch0.xhtml', 2],
    ['clothbound://book/1/ch1.xhtml', 1],
    ['clothbound://book/1/ch2.xhtml', 1],
]);

function makeFrame(id) {
    const f = {
        id, style: makeStyle(), classList: makeClassList(),
        contentDocument: null, contentWindow: null, onload: null, onerror: null, _src: 'about:blank',
        setAttribute() {}, removeAttribute() {},
        getBoundingClientRect() { return { left: 160, top: 54, width: parseFloat(this.style.width) || 680, height: 692 }; },
    };
    Object.defineProperty(f, 'src', {
        get() { return this._src; },
        set(v) {
            this._src = v;
            if (!v || v === 'about:blank') { this.contentDocument = null; this.contentWindow = null; return; }
            const finish = () => {
                const doc = makeDoc(this, v, pageCounts.get(v) || 1);
                this.contentDocument = doc;
                this.contentWindow = doc.defaultView;
                if (typeof this.onload === 'function') this.onload();
            };
            if (deferredResolve === 'armed') {
                deferredResolve = finish;
            } else {
                queueMicrotask(finish);
            }
        },
    });
    return f;
}

const stageEl = {
    clientWidth: 1200, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 1200, height: 800 }; },
};
const viewportEl = { style: makeStyle(), classList: makeClassList() };
const bookFrame = makeFrame('book');
const backFrame = makeFrame('book-back');
const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'viewport') return viewportEl;
        if (id === 'book') return bookFrame;
        if (id === 'book-back') return backFrame;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement() { return { style: makeStyle(), setAttribute() {}, remove() {}, addEventListener() {} }; },
    addEventListener(type, fn) {
        if (type === 'DOMContentLoaded') domReady = fn;
        if (type === 'wheel') wheelHandler = fn;
    },
};

const bridge = {
    surfaceLoaded() {},
    relocated() {},
    tapped() {},
    notifyPageTurn() {},
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console, URL, Map, Set, WeakMap, Promise, Math, Number, String, Array, Object, JSON, Error, RegExp,
    document: documentMock,
    window: { innerWidth: 1200, innerHeight: 800 },
    performance: { now() { return fakeNow; } },
    setTimeout: fakeSetTimeout,
    clearTimeout: fakeClearTimeout,
    requestAnimationFrame: fakeRaf,
    cancelAnimationFrame: fakeCancelRaf,
    matchMedia() { return { matches: false }; },
    ResizeObserver: class { observe() {} },
    QWebChannel: class { constructor(_t, cb) { cb({ objects: { bridge } }); } },
    qt: { webChannelTransport: {} },
    NodeFilter: { SHOW_TEXT: 4 },
    Hyphenation: { load: async () => false },
    fetch() { return Promise.resolve({ ok: true, text: async () => '' }); },
    getComputedStyle() { return { fontSize: '18px' }; },
};
sandbox.globalThis = sandbox;
sandbox.window.document = documentMock;
vm.createContext(sandbox);
vm.runInContext(source + '\nthis.__cb = clothbound;', sandbox);
domReady();
const cb = sandbox.__cb;

const book = {
    session: 1,
    base: 'clothbound://book/1/',
    language: 'en',
    spine: [
        { href: 'ch0.xhtml', linear: true },
        { href: 'ch1.xhtml', linear: true },
        { href: 'ch2.xhtml', linear: true },
    ],
};
const pagedStyle = {
    theme: { bg: '#fff', fg: '#111', link: '#00f', muted: '#666', popover: '#fff', border: '#ccc', dark: false },
    font: 'Literata', fontScale: 1, lineHeight: 1.5, margin: 0.08, measure: 38, marginTop: 54, marginBottom: 54,
    justify: false, hyphenate: false, layout: 'single', singleColumn: true, animate: true, reducedMotion: false,
};

async function flushMicrotasks(n = 8) {
    for (let i = 0; i < n; ++i) await Promise.resolve();
}

(async () => {
    await cb.open(book, { chapter: 0, offset: 0 }, pagedStyle);
    let dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 0);
    assert.strictEqual(dbg.page, 0);
    assert.strictEqual(dbg.pages, 2);

    // 1. Mid-swipe goTo() cancels pending swipeEnd so stale swipe never snaps back to ch0.
    wheelHandler({ deltaMode: 0, deltaX: 240, deltaY: 0, preventDefault() {} });
    await cb.goTo({ chapter: 2, offset: 0, instant: true });
    advanceTimers(150);
    await flushMicrotasks();
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 2, 'stale swipe timer must not override goTo(2)');

    // 2. Horizontal swipe past threshold settles via state.queue and serializes cleanly with queued next().
    await cb.goTo({ chapter: 0, offset: 0, instant: true });
    fakeNow += 500;
    wheelHandler({ deltaMode: 0, deltaX: 260, deltaY: 0, preventDefault() {} });
    advanceTimers(100);
    const queuedNext = cb.next();
    await flushMicrotasks(16);
    await queuedNext;
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 1, 'swipe -> page 1 followed by queued next() must reach chapter 1');
    assert.strictEqual(dbg.page, 0);

    // 3. Swipe start is ignored while a queued chapter navigation is actively in flight (state.navBusy > 0).
    await cb.goTo({ chapter: 0, offset: 0, instant: true });
    fakeNow += 500;
    deferredResolve = 'armed';
    const pendingNav = cb.goTo({ chapter: 2, offset: 0, instant: true });
    await flushMicrotasks(4);
    assert.strictEqual(typeof deferredResolve, 'function');
    wheelHandler({ deltaMode: 0, deltaX: 300, deltaY: 0, preventDefault() {} });
    const resolveLoad = deferredResolve;
    deferredResolve = null;
    resolveLoad();
    await pendingNav;
    advanceTimers(150);
    await flushMicrotasks();
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 2, 'wheel swipe during active navigation must be ignored');

    // 4. Scroll-mode wheelQuietUntil >= 550ms blocks accidental momentum multi-chapter skip.
    const scrollStyle = Object.assign({}, pagedStyle, { layout: 'scroll', animate: false });
    await cb.setStyle(scrollStyle);
    await cb.goTo({ chapter: 0, offset: 0, instant: true });
    fakeNow += 700;
    wheelHandler({ deltaMode: 0, deltaX: 0, deltaY: 80, preventDefault() {} });
    await flushMicrotasks(12);
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 1, 'bottom-edge scroll should advance to chapter 1');

    // Momentum wheel 400ms later is inside 550ms cooldown -> must stay on chapter 1.
    fakeNow += 400;
    wheelHandler({ deltaMode: 0, deltaX: 0, deltaY: 80, preventDefault() {} });
    await flushMicrotasks(12);
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 1, 'scroll momentum within 550ms must not double-skip to chapter 2');

    // Intentional wheel after 550ms cooldown -> advances to chapter 2.
    fakeNow += 200;
    wheelHandler({ deltaMode: 0, deltaX: 0, deltaY: 80, preventDefault() {} });
    await flushMicrotasks(12);
    dbg = JSON.parse(cb.debug());
    assert.strictEqual(dbg.chapter, 2, 'scroll wheel after 550ms cooldown should advance to chapter 2');

    process.stdout.write('OK\n');
})().catch(err => {
    console.error(err && err.stack || err);
    process.exit(1);
});
)JS");

    QProcess proc;
    proc.start(nodeBin, {QStringLiteral("-e"), harness, readerJsPath});
    QVERIFY2(proc.waitForFinished(10000), "node a-plus-craft reader.js harness timed out");
    const QByteArray stderrOut = proc.readAllStandardError();
    QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
             stderrOut.isEmpty() ? "node a-plus-craft reader.js harness failed" : stderrOut.constData());
    QCOMPARE(proc.readAllStandardOutput().trimmed(), QByteArray("OK"));
}

void ClothboundTests::w5ReaderCraftAsyncLightboxSnipAndPeek() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 1. ReaderImageProvider async EPUB PNG + SVG decode & cache lifecycle.
    const QByteArray samplePng = png(48, 32);
    const QByteArray sampleSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"60\">"
        "<rect width=\"80\" height=\"60\" fill=\"#336699\"/></svg>";
    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>Images Book</dc:title><dc:creator>Ada</dc:creator><dc:language>en</dc:language></metadata>\n"
        "  <manifest>\n"
        "    <item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "    <item id=\"i1\" href=\"images/plate.png\" media-type=\"image/png\"/>\n"
        "    <item id=\"i2\" href=\"images/diagram.svg\" media-type=\"image/svg+xml\"/>\n"
        "  </manifest>\n"
        "  <spine><itemref idref=\"c1\"/></spine>\n"
        "</package>";
    const QString epubPath = dir.filePath(QStringLiteral("images.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub({
                                           {QStringLiteral("OEBPS/content.opf"), opf},
                                           {QStringLiteral("OEBPS/c1.xhtml"),
                                            TestBooks::chapter("Plate", "<p><img src=\"images/plate.png\"/></p>")},
                                           {QStringLiteral("OEBPS/images/plate.png"), samplePng},
                                           {QStringLiteral("OEBPS/images/diagram.svg"), sampleSvg},
                                       })));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int epubId = library.addBookFile(epubPath);
    QVERIFY(epubId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(epubId));
    const QString base = reader.manifest().value(QStringLiteral("base")).toString();
    QVERIFY(!base.isEmpty());

    // 1a. Fallback when ReaderImageProvider is NOT registered: async local temp file (no base64 on GUI thread).
    QVERIFY(!ReaderImageProvider::isRegistered());
    {
        QSignalSpy chosenSpy(&reader, &Reader::imageChosen);
        reader.imageClicked(QVariantMap{
            {QStringLiteral("url"), base + QStringLiteral("OEBPS/images/plate.png")},
            {QStringLiteral("caption"), QStringLiteral("Plate I")},
        });
        QTRY_COMPARE_WITH_TIMEOUT(chosenSpy.count(), 1, 5000);
        const QString emittedUrl = chosenSpy.first().at(0).toString();
        QCOMPARE(chosenSpy.first().at(1).toString(), QStringLiteral("Plate I"));
        QVERIFY2(emittedUrl.startsWith(QStringLiteral("file://")), qPrintable(emittedUrl));
        const QString localFile = QUrl(emittedUrl).toLocalFile();
        QVERIFY(QFileInfo::exists(localFile));
        const QImage loaded(localFile);
        QCOMPARE(loaded.size(), QSize(48, 32));
    }

    // 1b. Registered ReaderImageProvider path: immediate image://bookimg/... URL + async off-thread decode.
    {
        ReaderImageProvider provider;
        QVERIFY(ReaderImageProvider::isRegistered());
        QSignalSpy chosenSpy(&reader, &Reader::imageChosen);
        reader.imageClicked(QVariantMap{
            {QStringLiteral("url"), base + QStringLiteral("OEBPS/images/plate.png")},
            {QStringLiteral("caption"), QStringLiteral("Plate Async")},
        });
        QCOMPARE(chosenSpy.count(), 1);
        const QString providerUrl = chosenSpy.first().at(0).toString();
        QVERIFY2(providerUrl.startsWith(QStringLiteral("image://bookimg/")), qPrintable(providerUrl));
        const QString id = providerUrl.mid(QStringLiteral("image://bookimg/").size());

        std::unique_ptr<QQuickImageResponse> resp(provider.requestImageResponse(id, QSize()));
        int done = 0;
        connect(resp.get(), &QQuickImageResponse::finished, this, [&done] { ++done; });
        QTRY_COMPARE_WITH_TIMEOUT(done, 1, 5000);
        std::unique_ptr<QQuickTextureFactory> factory(resp->textureFactory());
        QVERIFY(factory != nullptr);
        QCOMPARE(factory->image().size(), QSize(48, 32));

        // SVG decode also succeeds off-thread.
        const QString svgId = id.section(QLatin1Char('/'), 0, 0) + QStringLiteral("/OEBPS/images/diagram.svg");
        std::unique_ptr<QQuickImageResponse> svgResp(provider.requestImageResponse(svgId, QSize()));
        int svgDone = 0;
        connect(svgResp.get(), &QQuickImageResponse::finished, this, [&svgDone] { ++svgDone; });
        QTRY_COMPARE_WITH_TIMEOUT(svgDone, 1, 5000);
        std::unique_ptr<QQuickTextureFactory> svgFactory(svgResp->textureFactory());
        QVERIFY(svgFactory != nullptr && !svgFactory->image().isNull());

        // Closing the book cleans ReaderImageProvider cache & mapping.
        reader.close();
        std::unique_ptr<QQuickImageResponse> afterClose(provider.requestImageResponse(id, QSize()));
        int closedDone = 0;
        connect(afterClose.get(), &QQuickImageResponse::finished, this, [&closedDone] { ++closedDone; });
        QTRY_COMPARE_WITH_TIMEOUT(closedDone, 1, 3000);
        QVERIFY(afterClose->textureFactory() == nullptr);
    }
    QVERIFY(!ReaderImageProvider::isRegistered());

    // 2. Async PDF snip & PDF peek offload off GUI thread and drop stale responses cleanly.
    const QString pdfPath = dir.filePath(QStringLiteral("async_snip_peek.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QSizeF(300, 400), QPageSize::Point));
        writer.setResolution(72);
        QPainter p(&writer);
        p.drawText(QRect(24, 30, 240, 60), QStringLiteral("Page One Equation E = mc^2"));
        writer.newPage();
        p.drawText(QRect(24, 30, 240, 60), QStringLiteral("Reference [42] Attention Architecture"));
        p.end();
    }
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(pdfId > 0);
    QVERIFY(reader.openBook(pdfId));

    {
        QSignalSpy snipSpy(&reader, &Reader::pdfSnipReady);
        const int reqId = reader.requestPdfSnip(0, 0.05, 0.05, 0.8, 0.25, QStringLiteral("explain"), QString());
        QVERIFY(reqId > 0);
        QTRY_COMPARE_WITH_TIMEOUT(snipSpy.count(), 1, 8000);
        QCOMPARE(snipSpy.first().at(0).toInt(), reqId);
        QCOMPARE(snipSpy.first().at(1).toString(), QStringLiteral("explain"));
        const QString b64 = snipSpy.first().at(2).toString();
        QVERIFY(!b64.isEmpty());
        QVERIFY(!QImage::fromData(QByteArray::fromBase64(b64.toLatin1()), "PNG").isNull());
        QCOMPARE(snipSpy.first().at(3).toInt(), 0);
    }

    {
        QSignalSpy peekSpy(&reader, &Reader::pdfPeekReady);
        const int staleId = reader.requestPdfPeek(0, 0.1);
        const int latestId = reader.requestPdfPeek(1, 0.1);
        QVERIFY(staleId > 0 && latestId > staleId);
        QTRY_VERIFY_WITH_TIMEOUT(peekSpy.count() >= 1, 8000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        QCOMPARE(peekSpy.count(), 1);
        QCOMPARE(peekSpy.first().at(0).toInt(), latestId);
        const QVariantMap info = peekSpy.first().at(1).toMap();
        QCOMPARE(info.value(QStringLiteral("page")).toInt(), 1);
        QVERIFY(info.value(QStringLiteral("imageDataUrl")).toString().startsWith(QStringLiteral("data:image/png;base64,")));
        QVERIFY(info.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Reference")));
    }

    // In-flight snip cancelled cleanly across reader.close() without crash or stale signal.
    {
        QSignalSpy snipSpy(&reader, &Reader::pdfSnipReady);
        QVERIFY(reader.requestPdfSnip(0, 0.1, 0.1, 0.5, 0.3, QStringLiteral("latex"), QString()) > 0);
        reader.close();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        QCOMPARE(snipSpy.count(), 0);
    }
}

void ClothboundTests::w5ReaderCraftPdfSurfaceFocalZoomAndBaseCap() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdfPath = dir.filePath(QStringLiteral("focal.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QSizeF(612, 792), QPageSize::Point));
        writer.setResolution(72);
        QPainter p(&writer);
        for (int i = 0; i < 3; ++i) {
            if (i > 0)
                writer.newPage();
            p.drawText(QRect(40, 60, 480, 100), QStringLiteral("Focal zoom page %1").arg(i + 1));
        }
        p.end();
    }

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(pdfPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(bookId));
    Marks marks(&library);
    marks.setBook(bookId);
    BookSearch bookSearch(&reader);
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices ai(&gemini, &keys, library.database().connectionName());
    AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    Narration narration(&ai, &audioCache, &reader, &library);
    narration.m_player->setHeadless(true);
    Theme theme(dir.filePath(QStringLiteral("no-omarchy")));

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
    engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errs) { warnings.append(errs); });

    QQmlComponent surfComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml")));
    QVERIFY2(surfComp.status() == QQmlComponent::Ready, qPrintable(surfComp.errorString()));
    const QVariantMap look{
        {QStringLiteral("bg"), QStringLiteral("#FAF6EE")},
        {QStringLiteral("fg"), QStringLiteral("#26221D")},
        {QStringLiteral("muted"), QStringLiteral("#787066")},
        {QStringLiteral("line"), QStringLiteral("#E2DCD0")},
        {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
        {QStringLiteral("link"), QStringLiteral("#8A5A12")},
        {QStringLiteral("dark"), false},
    };
    std::unique_ptr<QObject> surfObj(surfComp.createWithInitialProperties({
        {QStringLiteral("width"), 1000.0},
        {QStringLiteral("height"), 760.0},
        {QStringLiteral("look"), look},
    }));
    QVERIFY(surfObj != nullptr);
    QCOMPARE(surfObj->property("maxBaseTextureWidth").toInt(), 2048);

    QVariantMap styleMap{
        {QStringLiteral("layout"), QStringLiteral("single")},
        {QStringLiteral("pdfCrop"), false},
        {QStringLiteral("pdfOriginalColors"), false},
        {QStringLiteral("pdfFit"), QStringLiteral("page")},
        {QStringLiteral("marginTop"), 60.0},
        {QStringLiteral("marginBottom"), 76.0},
        {QStringLiteral("animate"), false},
    };
    const QVariantMap openLoc{{QStringLiteral("chapter"), 0}};
    QMetaObject::invokeMethod(surfObj.get(), "open", Q_ARG(QVariant, reader.manifest()),
                              Q_ARG(QVariant, openLoc), Q_ARG(QVariant, styleMap));
    QCoreApplication::processEvents();

    // 1. Pure focal-scroll math locks document point under cursor (vx, vy).
    {
        QVariant ret;
        const bool ok = QMetaObject::invokeMethod(
            surfObj.get(), "computeFocalScroll", Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, 0.0), Q_ARG(QVariant, 0.0),         // oldContentX, oldContentY
            Q_ARG(QVariant, 960.0), Q_ARG(QVariant, 1248.0),    // oldW, oldH
            Q_ARG(QVariant, 1920.0), Q_ARG(QVariant, 2496.0),   // newW, newH (2x zoom)
            Q_ARG(QVariant, 1000.0), Q_ARG(QVariant, 760.0),    // viewW, viewH
            Q_ARG(QVariant, 60.0), Q_ARG(QVariant, 76.0),       // topPad, bottomPad
            Q_ARG(QVariant, 620.0), Q_ARG(QVariant, 440.0));    // cursorX, cursorY
        QVERIFY(ok);
        const QVariantMap next = ret.toMap();
        const double nx = next.value(QStringLiteral("x")).toDouble();
        const double ny = next.value(QStringLiteral("y")).toDouble();
        // Verify exact cursor lock: doc point under (620, 440) before zoom stays at (620, 440) after zoom!
        const double relX = (620.0 - 24.0) / 960.0;
        const double relY = (440.0 - 60.0) / 1248.0;
        const double screenXAfter = 24.0 + relX * 1920.0 - nx;
        const double screenYAfter = 60.0 + relY * 2496.0 - ny;
        QVERIFY2(qAbs(screenXAfter - 620.0) < 0.5, qPrintable(QString::number(screenXAfter)));
        QVERIFY2(qAbs(screenYAfter - 440.0) < 0.5, qPrintable(QString::number(screenYAfter)));
    }

    // 2. Pure scroll-mode focal horizontal pan math locks cursor X.
    {
        QVariant ret;
        const bool ok = QMetaObject::invokeMethod(
            surfObj.get(), "computeFocalPanX", Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, 0.0), Q_ARG(QVariant, 1000.0), Q_ARG(QVariant, 2000.0),
            Q_ARG(QVariant, 1000.0), Q_ARG(QVariant, 952.0), Q_ARG(QVariant, 750.0));
        QVERIFY(ok);
        // Cursor is +250 px right of center; at 2x zoom that point moves to +500 px, requiring panX == +250.
        QVERIFY2(qAbs(ret.toDouble() - 250.0) < 0.5, qPrintable(QString::number(ret.toDouble())));
    }

    // 3. Tile URL builder handles both themed (?bg=...&fg=...&tx=...) and originalColors (?tx=...).
    {
        QVariant themedUrl, rawUrl;
        QVERIFY(QMetaObject::invokeMethod(
            surfObj.get(), "buildTileUrl", Q_RETURN_ARG(QVariant, themedUrl),
            Q_ARG(QVariant, 0), Q_ARG(QVariant, QStringLiteral("?bg=%231e1f24&fg=%23d6d2c9")),
            Q_ARG(QVariant, QRectF(0.125, 0.25, 0.5, 0.4)), Q_ARG(QVariant, 3584)));
        const QString u1 = themedUrl.toString();
        QVERIFY2(u1.contains(QStringLiteral("&tx=0.1250&ty=0.2500&tw=0.5000&th=0.4000&fullw=3584")), qPrintable(u1));

        QVERIFY(QMetaObject::invokeMethod(
            surfObj.get(), "buildTileUrl", Q_RETURN_ARG(QVariant, rawUrl),
            Q_ARG(QVariant, 0), Q_ARG(QVariant, QString()),
            Q_ARG(QVariant, QRectF(0.0, 0.5, 0.5, 0.5)), Q_ARG(QVariant, 3072)));
        const QString u2 = rawUrl.toString();
        QVERIFY2(u2.contains(QStringLiteral("?tx=0.0000&ty=0.5000&tw=0.5000&th=0.5000&fullw=3072")), qPrintable(u2));
    }

    // 4. High zoom (4.0x on 1000x760 -> displayWidth > 2048) keeps basePixelWidth <= 2048 and enables tile overlay.
    QMetaObject::invokeMethod(surfObj.get(), "setCustomZoom", Q_ARG(QVariant, 4.0));
    QCoreApplication::processEvents();
    const QString pdfQml = [&] {
        QFile f(srcRoot + QStringLiteral("/ui/reader/PdfSurface.qml"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }();
    QVERIFY(pdfQml.contains(QStringLiteral("readonly property int maxBaseTextureWidth: 2048")));
    QVERIFY(pdfQml.contains(QStringLiteral("sourceSize.width: pageItem.basePixelWidth")));
    QVERIFY(pdfQml.contains(QStringLiteral("id: tileImage")));
    QVERIFY(pdfQml.contains(QStringLiteral("reader.requestPdfPeek(")));
    QVERIFY(pdfQml.contains(QStringLiteral("reader.requestPdfSnip(")));

    // Live cursor-focal zoomByWheelAt around bottom-right cursor moves zoomed Flickable away from (0, 0).
    QMetaObject::invokeMethod(surfObj.get(), "setFitMode", Q_ARG(QVariant, QStringLiteral("width")));
    QCoreApplication::processEvents();
    QVERIFY(QMetaObject::invokeMethod(surfObj.get(), "zoomByWheelAt",
                                      Q_ARG(QVariant, 240.0), Q_ARG(QVariant, 800.0), Q_ARG(QVariant, 620.0)));
    QCoreApplication::processEvents();
    QVERIFY(surfObj->property("effectiveZoom").toDouble() > 1.5);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
}

void ClothboundTests::w5ReaderCraftDecomposedReaderGpuFoldAndKeyboardA11y() {
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());
    auto readSrc = [&](const QString &rel) -> QString {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };

    // 1. EpubReader.qml LOC reduction (< 2100 LOC), zero software Canvas dog-ear, and resources.qrc registration.
    const QString erQml = readSrc(QStringLiteral("ui/reader/EpubReader.qml"));
    const int erLines = erQml.count(QLatin1Char('\n')) + 1;
    QVERIFY2(erLines < 2100, qPrintable(QStringLiteral("EpubReader.qml has %1 lines (target < 2100)").arg(erLines)));
    QVERIFY2(!erQml.contains(QStringLiteral("Canvas {")), "EpubReader.qml must not use software Canvas dog-ear");
    QVERIFY(!erQml.contains(QStringLiteral("shadowBlur")));

    const QString foldQml = readSrc(QStringLiteral("ui/reader/ReaderBookmarkFold.qml"));
    QVERIFY(foldQml.contains(QStringLiteral("import QtQuick.Shapes")));
    QVERIFY(foldQml.contains(QStringLiteral("Shape {")));
    QVERIFY(!foldQml.contains(QStringLiteral("Canvas {")));

    const QString qrc = readSrc(QStringLiteral("resources.qrc"));
    const QString cmakeLists = readSrc(QStringLiteral("../CMakeLists.txt"));
    for (const QString &sub : {QStringLiteral("ui/reader/ReaderBookmarkFold.qml"),
                               QStringLiteral("ui/reader/ReaderChrome.qml"),
                               QStringLiteral("ui/reader/ReaderLightbox.qml"),
                               QStringLiteral("ui/reader/ReaderMenus.qml"),
                               QStringLiteral("ui/reader/ReaderPanels.qml")}) {
        QVERIFY2(qrc.contains(sub) || cmakeLists.contains(sub), qPrintable(sub));
    }

    // 2. Live QML compilation & zero warnings for all decomposed reader components + keyboard-accessible views.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int colId = library.createCollection(QStringLiteral("Essays"));
    QVERIFY(colId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    Marks marks(&library);
    BookSearch bookSearch(&reader);
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices ai(&gemini, &keys, library.database().connectionName());
    AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    VoicePacks voicePacks(dir.filePath(QStringLiteral("voices")), nullptr, QStringLiteral("aarch64"),
                          packsManifest(QStringLiteral("http://127.0.0.1/")));
    LocalSpeech localSpeech(&voicePacks);
    VoiceCatalog voices(&ai, &audioCache, &reader, library.database().connectionName(),
                        dir.filePath(QStringLiteral("voices.json")));
    Narration narration(&ai, &audioCache, &reader, &library);
    narration.m_player->setHeadless(true);
    Companion companion(&ai, &reader, library.database().connectionName());
    Illustrator illustrator(&ai, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("pics")));
    OpdsClient opds(&library, &keys, dir.filePath(QStringLiteral("online")));
    Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
    Translator translator;

    QQmlEngine engine;
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("marks"), &marks);
    engine.rootContext()->setContextProperty(QStringLiteral("bookSearch"), &bookSearch);
    engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
    engine.rootContext()->setContextProperty(QStringLiteral("voicePacks"), &voicePacks);
    engine.rootContext()->setContextProperty(QStringLiteral("localSpeech"), &localSpeech);
    engine.rootContext()->setContextProperty(QStringLiteral("voices"), &voices);
    engine.rootContext()->setContextProperty(QStringLiteral("companion"), &companion);
    engine.rootContext()->setContextProperty(QStringLiteral("illustrator"), &illustrator);
    engine.rootContext()->setContextProperty(QStringLiteral("opds"), &opds);
    engine.rootContext()->setContextProperty(
        QStringLiteral("settingsUrl"), QUrl::fromLocalFile(dir.filePath(QStringLiteral("settings.ini"))));
    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errs) { warnings.append(errs); });

    const QVariantMap look{
        {QStringLiteral("bg"), QStringLiteral("#FAF6EE")},
        {QStringLiteral("fg"), QStringLiteral("#26221D")},
        {QStringLiteral("inkSoft"), QStringLiteral("#3B352D")},
        {QStringLiteral("muted"), QStringLiteral("#787066")},
        {QStringLiteral("line"), QStringLiteral("#E2DCD0")},
        {QStringLiteral("popover"), QStringLiteral("#FDFAF3")},
        {QStringLiteral("link"), QStringLiteral("#8A5A12")},
        {QStringLiteral("fold"), QStringLiteral("#DDD5C5")},
        {QStringLiteral("dark"), false},
    };

    // 2a. GPU Shape ReaderBookmarkFold live behavior.
    {
        QQmlComponent comp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/ReaderBookmarkFold.qml")));
        QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
        theme.setReducedMotion(true);
        std::unique_ptr<QObject> obj(comp.createWithInitialProperties({{QStringLiteral("look"), look}}));
        QVERIFY(obj != nullptr);
        QCOMPARE(obj->property("fold").toDouble(), 0.0);
        obj->setProperty("bookmarked", true);
        QCOMPARE(obj->property("fold").toDouble(), 1.0);
    }

    // 2b. ReaderLightbox live open/zoom/Esc behavior.
    {
        QQmlComponent comp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/ReaderLightbox.qml")));
        QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
        QQuickWindow win;
        win.resize(800, 600);
        std::unique_ptr<QObject> obj(comp.createWithInitialProperties({
            {QStringLiteral("width"), 800.0},
            {QStringLiteral("height"), 600.0},
        }));
        QVERIFY(obj != nullptr);
        auto *item = qobject_cast<QQuickItem *>(obj.get());
        QVERIFY(item != nullptr);
        item->setParentItem(win.contentItem());
        QSignalSpy closedSpy(item, SIGNAL(closed()));
        QVERIFY(QMetaObject::invokeMethod(item, "openImage", Q_ARG(QVariant, QStringLiteral("data:image/png;base64,")),
                                          Q_ARG(QVariant, QStringLiteral("Figure 1"))));
        QCOMPARE(item->property("open").toBool(), true);
        QVERIFY(QMetaObject::invokeMethod(item, "setZoom", Q_ARG(QVariant, 2.5)));
        QCOMPARE(item->property("zoom").toDouble(), 2.5);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(item, &esc);
        QCOMPARE(item->property("open").toBool(), false);
        QCOMPARE(closedSpy.count(), 1);
        item->setParentItem(nullptr);
    }

    // 2c. ReaderMenus, ReaderChrome, ReaderPanels instantiate cleanly with zero warnings.
    {
        QQmlComponent mComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/ReaderMenus.qml")));
        QVERIFY2(mComp.status() == QQmlComponent::Ready, qPrintable(mComp.errorString()));
        std::unique_ptr<QObject> mObj(mComp.createWithInitialProperties({{QStringLiteral("look"), look}}));
        QVERIFY(mObj != nullptr);

        QQmlComponent cComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/ReaderChrome.qml")));
        QVERIFY2(cComp.status() == QQmlComponent::Ready, qPrintable(cComp.errorString()));
        std::unique_ptr<QObject> cObj(cComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 700.0},
            {QStringLiteral("look"), look},
        }));
        QVERIFY(cObj != nullptr);

        QQmlComponent pComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/reader/ReaderPanels.qml")));
        QVERIFY2(pComp.status() == QQmlComponent::Ready, qPrintable(pComp.errorString()));
        std::unique_ptr<QObject> pObj(pComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 700.0},
            {QStringLiteral("look"), look},
        }));
        QVERIFY(pObj != nullptr);
    }

    // 3. Keyboard accessibility for CatalogsSheet custom server delete, VoicePanel designed-voice delete,
    //    and LibrarySidebar collection Rename (F2) / Delete (Del) / Context Menu.
    {
        const QString catalogsQml = readSrc(QStringLiteral("ui/library/CatalogsSheet.qml"));
        QVERIFY(catalogsQml.contains(QStringLiteral("customRow.hovered || customRow.activeFocus || removeBtn.activeFocus || customWrap.confirming")));
        QVERIFY(catalogsQml.contains(QStringLiteral("event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace")));
        QVERIFY(catalogsQml.contains(QStringLiteral("id: confirmRemoveBtn")));

        const QString vpQml = readSrc(QStringLiteral("ui/audio/VoicePanel.qml"));
        QVERIFY(vpQml.contains(QStringLiteral("function deleteHighlightedDesignedVoice()")));
        QVERIFY(vpQml.contains(QStringLiteral("function triggerForget()")));
        QVERIFY(vpQml.contains(QStringLiteral("row.hovered || row.activeFocus || row.keyboardHighlighted || forgetButton.activeFocus || row.confirming")));

        const QString sbQml = readSrc(QStringLiteral("ui/library/LibrarySidebar.qml"));
        QVERIFY(sbQml.contains(QStringLiteral("event.key === Qt.Key_F2")));
        QVERIFY(sbQml.contains(QStringLiteral("event.key === Qt.Key_Delete")));
        QVERIFY(sbQml.contains(QStringLiteral("id: colMoreBtn")));

        // Live LibrarySidebar F2 rename & Delete removal on a real collection row.
        QQmlComponent sbComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/LibrarySidebar.qml")));
        QVERIFY2(sbComp.status() == QQmlComponent::Ready, qPrintable(sbComp.errorString()));
        QQuickWindow win;
        win.resize(400, 640);
        std::unique_ptr<QObject> sbObj(sbComp.createWithInitialProperties({{QStringLiteral("height"), 640.0}}));
        QVERIFY(sbObj != nullptr);
        auto *sbItem = qobject_cast<QQuickItem *>(sbObj.get());
        QVERIFY(sbItem != nullptr);
        sbItem->setParentItem(win.contentItem());
        QCoreApplication::processEvents();

        QQuickItem *essaysRow = nullptr;
        std::function<void(QQuickItem *)> findEssays = [&](QQuickItem *node) {
            if (!node || essaysRow)
                return;
            if (node->property("text").toString() == QStringLiteral("Essays")) {
                essaysRow = node;
                return;
            }
            for (QQuickItem *child : node->childItems())
                findEssays(child);
        };
        findEssays(sbItem);
        QVERIFY2(essaysRow != nullptr, "Collection row 'Essays' not found in LibrarySidebar");

        QSignalSpy renameSpy(sbItem, SIGNAL(renameCollection(int, QString)));
        QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
        QCoreApplication::sendEvent(essaysRow, &f2);
        QCOMPARE(renameSpy.count(), 1);
        QCOMPARE(renameSpy.first().at(0).toInt(), colId);
        QCOMPARE(renameSpy.first().at(1).toString(), QStringLiteral("Essays"));

        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QCoreApplication::sendEvent(essaysRow, &del);
        QCOMPARE(library.collections().size(), 0);
        sbItem->setParentItem(nullptr);

        // Live CatalogsSheet & VoicePanel instantiate cleanly with zero warnings.
        QQmlComponent catComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CatalogsSheet.qml")));
        QVERIFY2(catComp.status() == QQmlComponent::Ready, qPrintable(catComp.errorString()));
        std::unique_ptr<QObject> catObj(catComp.createWithInitialProperties({
            {QStringLiteral("width"), 900.0},
            {QStringLiteral("height"), 640.0},
        }));
        QVERIFY(catObj != nullptr);

        QQmlComponent vpComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/audio/VoicePanel.qml")));
        QVERIFY2(vpComp.status() == QQmlComponent::Ready, qPrintable(vpComp.errorString()));
        std::unique_ptr<QObject> vpObj(vpComp.createWithInitialProperties({
            {QStringLiteral("width"), 900.0},
            {QStringLiteral("height"), 640.0},
            {QStringLiteral("look"), look},
        }));
        QVERIFY(vpObj != nullptr);
    }

    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
}

void ClothboundTests::uxPolishSleepIconSmoothDockTopEdgeAndLibraryMiniPlayer() {
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());
    auto readSrc = [&](const QString &rel) -> QString {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    };

    // 1. Sleep timer button wraps its Row inside an Item so AbstractButton's
    //    contentItem resizing cannot pin the moon glyph to the left edge.
    {
        const QString lbQml = readSrc(QStringLiteral("ui/audio/ListenBar.qml"));
        QVERIFY(lbQml.contains(QStringLiteral("signal openBookRequested()")));
        QVERIFY(lbQml.contains(QStringLiteral("id: restCoverBtn")));
        QVERIFY(lbQml.contains(QStringLiteral("id: bookTitleBtn")));
        const qsizetype sleepIdx = lbQml.indexOf(QStringLiteral("id: sleepButton"));
        QVERIFY(sleepIdx > 0);
        const QString sleepChunk = lbQml.mid(sleepIdx, 4000);
        QVERIFY2(sleepChunk.contains(QStringLiteral("contentItem: Item {")),
                 "sleepButton contentItem must wrap sleepRow in an Item for exact centering");
        QVERIFY(!sleepChunk.contains(QStringLiteral("contentItem: Row {")));
    }

    // 2. Smooth Companion docking: PdfSurface tracks liveResizing to GPU-scale smoothly
    //    and defers crisp raster commit until settled; EPUB glides without mid-slide reflow.
    {
        const QString compQml = readSrc(QStringLiteral("ui/companion/CompanionPanel.qml"));
        QVERIFY(compQml.contains(QStringLiteral("readonly property bool sliding: slideAnim.running")));

        const QString panelsQml = readSrc(QStringLiteral("ui/reader/ReaderPanels.qml"));
        QVERIFY(panelsQml.contains(QStringLiteral("readonly property bool sliding:")));
        QVERIFY(panelsQml.contains(QStringLiteral("readonly property real slideProgress:")));

        const QString pdfQml = readSrc(QStringLiteral("ui/reader/PdfSurface.qml"));
        QVERIFY(pdfQml.contains(QStringLiteral("property bool liveResizing: false")));
        QVERIFY(pdfQml.contains(QStringLiteral("property int committedPixelWidth: 64")));
        QVERIFY(pdfQml.contains(QStringLiteral("restoreMode: Binding.RestoreNone")));
        QVERIFY(pdfQml.contains(QStringLiteral("if (surface.liveResizing)")));

        const QString erQml = readSrc(QStringLiteral("ui/reader/EpubReader.qml"));
        QVERIFY(erQml.contains(QStringLiteral("readonly property real companionLiveDockWidth:")));
        QVERIFY(erQml.contains(QStringLiteral("liveResizing: view.companionSliding || view.companionResizing")));
        QVERIFY(erQml.contains(QStringLiteral("readonly property real epubDockGlide:")));
        QVERIFY(erQml.contains(QStringLiteral("function leaveForLibrary()")));
        // Top-edge hover reveal works in all modes (including while listening or zoomed PDF).
        QVERIFY(erQml.contains(QStringLiteral("const nearTopEdge = py <= 56")));
        QVERIFY(erQml.contains(QStringLiteral("onHoveredChanged:")));
    }

    // 3. Continuous background audio mini-player in the Library.
    {
        const QString mainQml = readSrc(QStringLiteral("ui/Main.qml"));
        QVERIFY(mainQml.contains(QStringLiteral("property bool browsingLibrary: false")));
        QVERIFY(mainQml.contains(QStringLiteral("readonly property bool readerOpen: reader.open && !browsingLibrary")));
        QVERIFY(mainQml.contains(QStringLiteral("function returnToPlayingBook()")));
        QVERIFY(mainQml.contains(QStringLiteral("id: libraryListenBar")));
        QVERIFY(mainQml.contains(QStringLiteral("showBookInfo: true")));
        QVERIFY(mainQml.contains(QStringLiteral("onOpenBookRequested: win.returnToPlayingBook()")));

        const QString libQml = readSrc(QStringLiteral("ui/library/LibraryView.qml"));
        QVERIFY(libQml.contains(QStringLiteral("property real bottomClearance: 0")));
    }

    // 4. Live QML instantiation of ListenBar (showBookInfo=true) verifying zero warnings
    //    and exact horizontal centering of sleepButton's moon glyph inside its 28 px box.
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices ai(&gemini, &keys, library.database().connectionName());
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&ai, &cache, &reader, &library);
        narration.m_player->setHeadless(true);
        VoiceCatalog voices(&ai, &cache, &reader, library.database().connectionName(),
                            dir.filePath(QStringLiteral("voices.json")));
        Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
        Translator translator;

        QQmlEngine engine;
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &w) { warnings.append(w); });
        engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
        engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);
        engine.rootContext()->setContextProperty(QStringLiteral("voices"), &voices);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(
            QStringLiteral("settingsUrl"), QUrl::fromLocalFile(dir.filePath(QStringLiteral("settings.ini"))));

        QQmlComponent comp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/audio/ListenBar.qml")));
        QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
        const QVariantMap look{{QStringLiteral("dark"), true}};
        std::unique_ptr<QObject> obj(comp.createWithInitialProperties({
            {QStringLiteral("showBookInfo"), true},
            {QStringLiteral("availableWidth"), 960.0},
            {QStringLiteral("look"), look},
            {QStringLiteral("hoverExpanded"), true},
        }));
        QVERIFY(obj != nullptr);
        QQuickWindow win;
        win.resize(960, 240);
        auto *barItem = qobject_cast<QQuickItem *>(obj.get());
        QVERIFY(barItem != nullptr);
        barItem->setParentItem(win.contentItem());
        QCoreApplication::processEvents();

        // Find sleepButton's inner Row and verify it sits centered inside its 28 px button.
        QQuickItem *sleepBtn = nullptr;
        std::function<void(QQuickItem *)> findSleep = [&](QQuickItem *node) {
            if (!node || sleepBtn)
                return;
            if (node->property("Accessible.name").toString().contains(QStringLiteral("Sleep timer")) &&
                qAbs(node->width() - 28.0) < 1.0 && qAbs(node->height() - 28.0) < 1.0) {
                sleepBtn = node;
                return;
            }
            for (QQuickItem *child : node->childItems())
                findSleep(child);
        };
        findSleep(barItem);
        if (sleepBtn) {
            QQuickItem *contentWrap = sleepBtn->property("contentItem").value<QQuickItem *>();
            QVERIFY(contentWrap != nullptr);
            QVERIFY(!contentWrap->childItems().isEmpty());
            QQuickItem *row = contentWrap->childItems().first();
            const qreal leftGap = row->x();
            const qreal rightGap = contentWrap->width() - (row->x() + row->width());
            QVERIFY2(qAbs(leftGap - rightGap) <= 1.0,
                     qPrintable(QStringLiteral("leftGap=%1 rightGap=%2").arg(leftGap).arg(rightGap)));
        }
        barItem->setParentItem(nullptr);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
    }

    // 5. Right-click / cite / overflow menus explicitly bind menus.look palette
    //    so Sepia/Paper popups stay high-contrast even on a dark desktop theme.
    {
        const QString menusQml = readSrc(QStringLiteral("ui/reader/ReaderMenus.qml"));
        QCOMPARE(menusQml.count(QStringLiteral("palette.windowText: menus.menuFg")), 3);
        QCOMPARE(menusQml.count(QStringLiteral("palette.light: menus.menuBg")), 3);
        QVERIFY(menusQml.contains(QStringLiteral("text: qsTr(\"Snip region…\") + \"\\tX\"")));

        // Verify PdfPages::recolor keeps dark colored PDF text legible on dark themes.
        QImage darkPdfTest(8, 2, QImage::Format_RGB32);
        darkPdfTest.fill(qRgb(255, 255, 255));
        darkPdfTest.setPixel(0, 0, qRgb(10, 30, 100));   // dark navy hyperlink / heading
        darkPdfTest.setPixel(1, 0, qRgb(110, 15, 20));   // dark burgundy heading
        darkPdfTest.setPixel(2, 0, qRgb(15, 90, 25));    // dark green text
        darkPdfTest.setPixel(3, 0, qRgb(0, 0, 255));     // pure saturated blue link
        const QColor darkBg(QStringLiteral("#1E1D1B"));
        const QColor lightFg(QStringLiteral("#DCD5C8"));
        PdfPages::recolor(darkPdfTest, darkBg, lightFg);
        auto lumaOf = [](QRgb px) {
            return (qRed(px) * 299 + qGreen(px) * 587 + qBlue(px) * 114) / 1000;
        };
        const int bgL = lumaOf(darkBg.rgb());
        for (int x = 0; x < 4; ++x) {
            const int outL = lumaOf(darkPdfTest.pixel(x, 0));
            QVERIFY2(outL - bgL >= 85,
                     qPrintable(QStringLiteral("pixel %1 too dark on dark theme: luma=%2 bg=%3").arg(x).arg(outL).arg(bgL)));
        }

        // Verify reader.js normalizes inline author colors & detects opaque line-art diagrams.
        const QString rjs = readSrc(QStringLiteral("reader/reader.js"));
        QVERIFY(rjs.contains(QStringLiteral("function normalizeInlineContrast(doc)")));
        QVERIFY(rjs.contains(QStringLiteral("st.removeProperty('color')")));

        // Verify OpdsClient has in-memory LRU feed cache and UrlGuard prioritizes IPv4 + per-IP failover.
        const QString opdsCpp = readSrc(QStringLiteral("online/opdsclient.cpp"));
        QVERIFY(opdsCpp.contains(QStringLiteral("tryServeCachedFeed(")));
        QVERIFY(opdsCpp.contains(QStringLiteral("storeCachedFeed(")));
        const QString ugCpp = readSrc(QStringLiteral("online/urlguard.cpp"));
        QVERIFY(ugCpp.contains(QStringLiteral("QAbstractSocket::IPv4Protocol")));

        // Verify PDF Fit-to-Width keeps IBeamCursor / left-drag text selection by default
        // (middle-drag or Alt+drag handles 2D hand-panning when zoomed).
        const QString pdfQml = readSrc(QStringLiteral("ui/reader/PdfSurface.qml"));
        QVERIFY(!pdfQml.contains(QStringLiteral("Qt.OpenHandCursor")));
        QVERIFY(pdfQml.contains(QStringLiteral("Qt.MiddleButton")));
        QVERIFY(pdfQml.contains(QStringLiteral("Qt.AltModifier")));
    }
}
