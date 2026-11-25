#include "clothbound_tests.h"

#include "ai/aiservices.h"
#include "ai/aiusage.h"
#include "ai/bookcontextreader.h"
#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/companionstore.h"
#include "ai/gemini.h"
#include "ai/illustrator.h"
#include "ai/keystore.h"
#include "ai/localdictionary.h"
#include "ai/localmodels.h"
#include "ai/localsetup.h"
#include "ai/passageindex.h"
#include "ai/sseparser.h"
#include "app/cli.h"
#include "app/migration.h"
#include "app/systemcheck.h"
#include "app/theme.h"
#include "app/translator.h"
#include "audio/audiocache.h"
#include "audio/localspeech.h"
#include "audio/narration.h"
#include "audio/speechplayer.h"
#include "audio/voicecatalog.h"
#include "audio/voicepacks.h"
#include "fakeserver.h"
#include "formats/audiopcm.h"
#include "formats/bookscheme.h"
#include "formats/epubbook.h"
#include "formats/htmltokenizer.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "formats/ziparchive.h"
#include "library/library.h"
#include "library/libraryfinder.h"
#include "library/libraryindex.h"
#include "library/marks.h"
#include "library/scanner.h"
#include "mcp/mcpserver.h"
#include "online/articleextractor.h"
#include "online/opdsclient.h"
#include "online/opdsparser.h"
#include "reader/reader.h"
#include "testbooks.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <QtTest>
#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <sys/stat.h>
#include <thread>

void ClothboundTests::cliCommandsHandleEdgeCases() {
    QVERIFY(isCliCommand("add"));
    QVERIFY(isCliCommand("ai"));
    QVERIFY(!isCliCommand("open"));

    QTemporaryDir dir;
    const QString epubPath = dir.filePath("Marsh, Ada - Lantern Keeper, The (Harbor, 1911).epub");
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

    // Single-pass info succeeds on a valid EPUB.
    QCOMPARE(runCli({"clothbound", "info", "--json", epubPath}), 0);
    QCOMPARE(runCli({"clothbound", "help", "open"}), 0);

    // Add book once, then re-add: idempotent, no crash/hang.
    QCOMPARE(runCli({"clothbound", "add", "--json", epubPath}), 0);
    QCOMPARE(runCli({"clothbound", "add", "--json", epubPath}), 0);

    // Add multiple folders in one call: batches into one scan, never hangs.
    const QString dir1 = dir.filePath("d1"), dir2 = dir.filePath("d2");
    QDir().mkpath(dir1);
    QDir().mkpath(dir2);
    QCOMPARE(runCli({"clothbound", "add", "--json", dir1, dir2}), 0);

    // notes: must not collide with a same-named directory in CWD, and 0 marks exits 0.
    const QString prevCwd = QDir::currentPath();
    QDir().mkpath(dir.filePath("lantern"));
    QVERIFY(QDir::setCurrent(dir.path()));
    const int notesPlain = runCli({"clothbound", "notes", "lantern"});
    const int notesJson = runCli({"clothbound", "notes", "--json", "lantern"});
    QVERIFY(QDir::setCurrent(prevCwd));
    QCOMPARE(notesPlain, 0);
    QCOMPARE(notesJson, 0);

    // ai local runs without opening SQLite or libsecret keyring.
    QCOMPARE(runCli({"clothbound", "ai", "local", "--json"}), 0);
}

void ClothboundTests::cliVersionFastPathExitsCleanlyWithoutDisplay() {
    const QString clothboundBin = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../clothbound"));
    if (!QFileInfo::exists(clothboundBin))
        QSKIP("build/clothbound binary not built yet");

    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("DISPLAY"));
    env.remove(QStringLiteral("WAYLAND_DISPLAY"));
    env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    proc.setProcessEnvironment(env);

    QElapsedTimer timer;
    timer.start();
    proc.start(clothboundBin, {QStringLiteral("--version")});
    QVERIFY(proc.waitForFinished(5000));
    const qint64 elapsedMs = timer.elapsed();

    QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
    QCOMPARE(proc.exitCode(), 0);
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    QVERIFY2(out.startsWith(QStringLiteral("clothbound ")), qPrintable(out));
    QVERIFY2(QRegularExpression(QStringLiteral("^clothbound \\d+\\.\\d+\\.\\d+")).match(out).hasMatch(), qPrintable(out));
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY2(elapsedMs < 2000, qPrintable(QStringLiteral("slow --version: %1 ms").arg(elapsedMs)));

    // --help also completes via the lightweight QCoreApplication fast path without display.
    QProcess helpProc;
    helpProc.setProcessEnvironment(env);
    helpProc.start(clothboundBin, {QStringLiteral("--help")});
    QVERIFY(helpProc.waitForFinished(5000));
    QCOMPARE(helpProc.exitStatus(), QProcess::NormalExit);
    QCOMPARE(helpProc.exitCode(), 0);
    const QString helpOut = QString::fromUtf8(helpProc.readAllStandardOutput());
    QVERIFY(helpOut.contains(QStringLiteral("Usage:")));
    QVERIFY(QString::fromUtf8(helpProc.readAllStandardError()).trimmed().isEmpty());
}

void ClothboundTests::wave2ReaderBookPagesPreviousOpenedAtAndSurfaceProtections() {
    QTemporaryDir dir;
    const QString epubPath = dir.filePath("pages.epub");
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

    // Build a 4-page PDF for PDF bookPageStart/bookPageEnd/previousOpenedAt tests.
    const QString pdfPath = dir.filePath("four.pdf");
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A5));
        writer.setResolution(72);
        QPainter painter(&writer);
        for (int p = 0; p < 4; ++p) {
            if (p > 0)
                writer.newPage();
            painter.drawText(QPointF(30, 60), QStringLiteral("PDF page %1 content.").arg(p + 1));
        }
    }

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int epubId = library.addBookFile(epubPath);
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(epubId > 0 && pdfId > 0);
    // 6600 total chars / 1650 chars-per-page = 4 estimated book pages.
    library.setChapterLengths(epubId, {3300, 3300});

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QCOMPARE(reader.paragraphSpacing(), 0.0);

    // Fresh EPUB open: previousOpenedAt == 0, then markOpened sets openedAt > 0.
    QVERIFY(reader.openBook(epubId));
    QCOMPARE(reader.previousOpenedAt(), qint64(0));
    QVERIFY(library.record(epubId).openedAt > 0);

    reader.relocated({{"chapter", 0}, {"offset", 0}, {"page", 0}, {"pages", 2}, {"columns", 1}});
    QCOMPARE(reader.visibleColumns(), 1);
    QCOMPARE(reader.bookPages(), 4);
    QCOMPARE(reader.bookPageStart(), 1);
    QCOMPARE(reader.bookPageEnd(), 1);

    reader.relocated({{"chapter", 0}, {"offset", 0}, {"page", 0}, {"pages", 1}, {"columns", 2}});
    QCOMPARE(reader.visibleColumns(), 2);
    QCOMPARE(reader.bookPageStart(), 1);
    QCOMPARE(reader.bookPageEnd(), 2);

    reader.relocated({{"chapter", 1}, {"offset", 1650}, {"page", 0}, {"pages", 1}, {"columns", 2}, {"atEnd", true}});
    QCOMPARE(reader.bookPageStart(), 3);
    QCOMPARE(reader.bookPageEnd(), 4);

    reader.close();
    QCOMPARE(reader.previousOpenedAt(), qint64(0));
    QCOMPARE(reader.bookPages(), 1);
    QCOMPARE(reader.visibleColumns(), 1);

    // Seed past opened_at timestamps and verify previousOpenedAt captures them BEFORE markOpened().
    {
        QSqlQuery q(library.database());
        QVERIFY(q.exec(QStringLiteral("UPDATE books SET opened_at = 1700000123 WHERE id = %1").arg(epubId)));
        QVERIFY(q.exec(QStringLiteral("UPDATE books SET opened_at = 1700000456 WHERE id = %1").arg(pdfId)));
    }
    Library reopened(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(reopened.open());
    Reader reader2(&reopened, &scheme);

    QVERIFY(reader2.openBook(epubId));
    QCOMPARE(reader2.previousOpenedAt(), qint64(1700000123));
    QVERIFY(reopened.record(epubId).openedAt > 1700000123);
    reader2.close();

    QVERIFY(reader2.openBook(pdfId));
    QCOMPARE(reader2.previousOpenedAt(), qint64(1700000456));
    QVERIFY(reopened.record(pdfId).openedAt > 1700000456);
    reader2.relocated({{"chapter", 1}, {"offset", 0}, {"page", 1}, {"pages", 4}, {"columns", 1}});
    QCOMPARE(reader2.bookPages(), 4);
    QCOMPARE(reader2.bookPageStart(), 2);
    QCOMPARE(reader2.bookPageEnd(), 2);
    reader2.relocated({{"chapter", 1}, {"offset", 0}, {"page", 1}, {"pages", 4}, {"columns", 2}, {"visiblePageEnd", 2}});
    QCOMPARE(reader2.visibleColumns(), 2);
    QCOMPARE(reader2.bookPageStart(), 2);
    QCOMPARE(reader2.bookPageEnd(), 3);
    reader2.close();

    // reader.js Node VM test: singleColumn docking, cb-spoken dimming/clearing, and 2-column bleed containment.
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty())
        QSKIP("node executable not available");
    const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
    const QString harness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}

const stageEl = {
    clientWidth: 1400, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: this.clientWidth, height: this.clientHeight }; },
};

const styleNodes = new Map();
const textNode = { nodeType: 3, data: 'Filmer worked in Shoreditch and Dymchurch on his machine.', parentElement: null };
const nowrapP = { localName: 'p', _ws: 'nowrap', style: makeStyle() };
const preDiv = { localName: 'div', _ws: 'pre', style: makeStyle() };
const tableCell = { localName: 'td', _ws: 'nowrap', style: makeStyle() };
const preEl = { localName: 'pre', _ws: 'pre', style: makeStyle() };
const bodyEl = {
    childNodes: [textNode],
    lastElementChild: {
        getClientRects() { return [{ left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }]; },
        getBoundingClientRect() { return { left: 0, right: 600, top: 0, bottom: 24, width: 600, height: 24 }; },
    },
};
textNode.parentElement = bodyEl;

const chapterDocEl = {
    style: makeStyle(), scrollWidth: 1400, scrollHeight: 692, scrollLeft: 0, scrollTop: 0,
    scrollTo(opts) {
        if (opts && typeof opts.left === 'number') this.scrollLeft = opts.left;
        if (opts && typeof opts.top === 'number') this.scrollTop = opts.top;
    },
    appendChild() {},
};

const highlights = new Map();
const chapterDoc = {
    URL: 'clothbound://book/1/ch1.xhtml',
    documentElement: chapterDocEl,
    head: { appendChild(el) { styleNodes.set(el.id, el); } },
    body: bodyEl,
    scrollingElement: chapterDocEl,
    fonts: { ready: Promise.resolve() },
    images: [],
    defaultView: {
        getComputedStyle(el) {
            return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start',
                     whiteSpace: (el && el._ws) || 'normal' };
        },
        CSS: { highlights },
        Highlight: class { constructor(...ranges) { this.ranges = ranges; this.priority = 0; } },
    },
    getElementById(id) { return styleNodes.get(id) || null; },
    createElementNS(_ns, tag) { return { localName: tag, id: '', textContent: '', style: makeStyle() }; },
    querySelector() { return null; },
    querySelectorAll(sel) {
        if (sel === 'body *') return [nowrapP, preDiv, tableCell, preEl];
        return [];
    },
    addEventListener() {},
    removeEventListener() {},
    createTreeWalker() {
        let done = false;
        return { nextNode() { if (done) return null; done = true; return textNode; } };
    },
    createRange() {
        return {
            start: 0, end: 0,
            selectNodeContents() {},
            setStart(_n, o) { this.start = o; },
            setEnd(_n, o) { this.end = o; },
            getClientRects() { return [{ left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }]; },
            getBoundingClientRect() { return { left: 10, right: 200, top: 10, bottom: 28, width: 190, height: 18 }; },
        };
    },
    getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
};

const classes = new Set();
const frameEl = {
    style: makeStyle(),
    classList: { add(c) { classes.add(c); }, remove(c) { classes.delete(c); }, contains(c) { return classes.has(c); } },
    contentDocument: chapterDoc,
    contentWindow: chapterDoc.defaultView,
    onload: null,
    _src: '',
    set src(v) { this._src = v; if (typeof this.onload === 'function') queueMicrotask(() => this.onload()); },
    get src() { return this._src; },
    getBoundingClientRect() { return { left: 0, top: 54, width: 1200, height: 692 }; },
};

const overlayEl = { appendChild() {} };
const docEl = { style: makeStyle() };
const documentMock = {
    documentElement: docEl,
    body: { appendChild(el) { return el; } },
    getElementById(id) {
        if (id === 'stage') return stageEl;
        if (id === 'book') return frameEl;
        if (id === 'overlay') return overlayEl;
        return null;
    },
    createElement(tag) { return { tagName: tag.toUpperCase(), id: '', style: makeStyle(), setAttribute() {} }; },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

let lastLoc = null;
const bridge = {
    surfaceLoaded() {},
    relocated(loc) { lastLoc = loc; },
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console, setTimeout, clearTimeout, performance, Promise, URL,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1400, innerHeight: 800 },
    qt: { webChannelTransport: {} },
    QWebChannel: function(_t, cb) { cb({ objects: { bridge } }); },
    ResizeObserver: class { observe() {} },
    matchMedia: () => ({ matches: false }),
    requestAnimationFrame: fn => { fn(performance.now()); return 1; },
    fetch: async () => ({ ok: true, text: async () => '', headers: { get: () => '' } }),
};
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
const clothbound = vm.runInContext(source + '\n;clothbound;', ctx);
domReady();

(async () => {
    const book = { session: 1, base: 'clothbound://book/1/', language: 'en', rtl: false,
                   spine: [{ href: 'ch1.xhtml', linear: true }], pageTargets: [] };
    const baseStyle = {
        theme: { bg: '#fbfaf6', fg: '#23221f', link: '#8a6a2a', accentInk: '#7a5a1a', muted: '#6f6b63',
                 popover: '#fff', border: 'rgba(0,0,0,0.12)', selection: '#cde', search: '#ff0', found: '#fa0',
                 markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f', markPink: '#f0f',
                 speaking: '#eef', dark: false },
        font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.0,
        margin: 0.06, measure: 34, justify: true, hyphenate: false, layout: 'double', singleColumn: false,
        animate: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
        listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
    };

    await clothbound.open(book, { chapter: 0, offset: 0 }, baseStyle);
    assert.strictEqual(lastLoc.columns, 2, 'double layout on 1400px uses 2 columns');

    // Verify 2-column horizontal bleed containment CSS & fitBlocks nowrap/pre neutralization.
    const typoCss = styleNodes.get('cb-typography').textContent;
    const layCss = styleNodes.get('cb-layout').textContent;
    assert.ok(typoCss.includes('overflow-wrap: break-word'), 'typographyCss enables break-word');
    assert.ok(typoCss.includes('text-wrap-mode: wrap !important'), 'typographyCss forces wrap mode');
    assert.ok(typoCss.includes('nobr { white-space: normal !important; }'), 'typographyCss neutralizes nobr');
    assert.ok(layCss.includes('column-span: none !important'), 'layoutCss blocks column-span');
    assert.ok(layCss.includes('max-width: 100% !important'), 'layoutCss caps child max-width');
    assert.ok(layCss.includes('overflow-x: clip'), 'layoutCss clips column horizontal overflow');
    assert.deepStrictEqual(nowrapP.style.props['white-space'], { value: 'normal', prio: 'important' });
    assert.deepStrictEqual(preDiv.style.props['white-space'], { value: 'pre-wrap', prio: 'important' });
    assert.strictEqual(tableCell.style.props['white-space'], undefined, 'table cells untouched');
    assert.strictEqual(preEl.style.props['white-space'], undefined, 'pre blocks handled by CSS');

    // Companion docked seam: singleColumn forces 1 column even in layout=double, then restores 2.
    await clothbound.setStyle({ ...baseStyle, singleColumn: true });
    assert.strictEqual(lastLoc.columns, 1, 'singleColumn forces 1 column');
    await clothbound.setStyle({ ...baseStyle, singleColumn: false });
    assert.strictEqual(lastLoc.columns, 2, 'clearing singleColumn restores 2 columns');

    // cb-spoken dims prior text [0, start) at priority 0 < marks (1) < cb-speaking (2) < cb-search (3) < cb-found (4).
    clothbound.highlight('cb-mark-yellow', [{ start: 0, end: 6 }]);
    clothbound.found([{ start: 7, end: 13 }], { start: 7, end: 13 });
    clothbound.speak(15, 28, false);
    assert.ok(highlights.has('cb-spoken'), 'cb-spoken added for start > 0');
    assert.ok(highlights.has('cb-speaking'), 'cb-speaking added');
    assert.strictEqual(highlights.get('cb-spoken').priority, 0);
    assert.strictEqual(highlights.get('cb-mark-yellow').priority, 1);
    assert.strictEqual(highlights.get('cb-speaking').priority, 2);
    assert.strictEqual(highlights.get('cb-search').priority, 3);
    assert.strictEqual(highlights.get('cb-found').priority, 4);
    assert.strictEqual(highlights.get('cb-spoken').ranges[0].start, 0);
    assert.strictEqual(highlights.get('cb-spoken').ranges[0].end, 15);

    // Clearing cb-speaking automatically clears cb-spoken as well.
    clothbound.highlight('cb-speaking', []);
    assert.ok(!highlights.has('cb-speaking'));
    assert.ok(!highlights.has('cb-spoken'), 'cb-spoken cleared when cb-speaking clears');

    // Speaking at offset 0 creates no cb-spoken range.
    clothbound.speak(0, 6, false);
    assert.ok(highlights.has('cb-speaking'));
    assert.ok(!highlights.has('cb-spoken'));
    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");
    QProcess proc;
    proc.start(node, {QStringLiteral("-e"), harness, readerJsPath});
    QVERIFY2(proc.waitForFinished(10000), "node wave2 reader.js harness timed out");
    const QByteArray stderrOut = proc.readAllStandardError();
    QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
             stderrOut.isEmpty() ? "node wave2 reader.js harness failed" : stderrOut.constData());
}

void ClothboundTests::wave2CompanionMigrationWhereLocalSuggestionsAiServicesAndSystemCheck() {
    QTemporaryDir dir;
    const QString epubPath = dir.filePath("comp.epub");
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(epubPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(bookId));
    reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});

    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    auto restoreAiSettings = qScopeGuard([&] {
        QSettings(settingsPath, QSettings::IniFormat).remove(QStringLiteral("ai"));
    });

    // 1. Seed a legacy companion_threads table WITHOUT provenance / is_local columns to verify additive migration.
    {
        QSqlQuery q(library.database());
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE companion_threads ("
            " id INTEGER PRIMARY KEY,"
            " book_id INTEGER NOT NULL,"
            " seq INTEGER NOT NULL,"
            " from_user INTEGER NOT NULL,"
            " text TEXT NOT NULL,"
            " sources TEXT NOT NULL DEFAULT '[]',"
            " created_at INTEGER NOT NULL)")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO companion_threads (book_id, seq, from_user, text, sources, created_at) "
            "VALUES (%1, 0, 1, 'Earlier question?', '[]', 100)").arg(bookId)));
    }

    FakeServer cloudServer;
    FakeServer localServer;
    GeminiProvider gemini(nullptr, cloudServer.endpoint());
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    services.setLocalModels(&local);

    // 2. AiServices: routeFor("illustrate"), whereFor("illustrate"), friendlyModelTitle, localModelsSummary.
    QCOMPARE(AiServices::friendlyModelTitle(QStringLiteral("qwen3.5:4b")), QStringLiteral("Qwen3.5 4B"));
    QCOMPARE(AiServices::friendlyModelTitle(QStringLiteral("embeddinggemma:latest")), QStringLiteral("EmbeddingGemma"));
    QCOMPARE(AiServices::friendlyModelTitle(QStringLiteral("custom:latest")), QStringLiteral("custom"));
    QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QString());

    gemini.setKey(QStringLiteral("cloud-key"));
    services.setPreset(QStringLiteral("cloud"));
    QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QStringLiteral("cloud"));
    QVERIFY(services.whereFor(QStringLiteral("illustrate")).contains(QStringLiteral("Gemini")));
    services.setOverride(QStringLiteral("illustrate"), QStringLiteral("local"));
    QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QString());
    services.clearOverrides();
    services.setPreset(QStringLiteral("local"));
    QCOMPARE(services.routeFor(QStringLiteral("illustrate")), QString());
    QVERIFY(services.whereFor(QStringLiteral("illustrate")).contains(QStringLiteral("Unavailable")));

    LocalModels::Server where;
    where.kind = LocalModels::Ollama;
    where.url = QUrl(localServer.origin());
    local.configure(where, QStringLiteral("qwen3.5:4b"), QStringLiteral("qwen3-embedding:0.6b"));
    QCOMPARE(services.localModelsSummary(), QStringLiteral("Qwen3.5 4B · Qwen3-Embedding 0.6B"));

    // 3. Companion additive migration check + local ask provenance + persistence + reload.
    Companion companion(&services, &reader, library.database().connectionName());
    {
        QSqlQuery info(library.database());
        QVERIFY(info.exec(QStringLiteral("PRAGMA table_info(companion_threads)")));
        QSet<QString> cols;
        while (info.next())
            cols.insert(info.value(1).toString());
        QVERIFY(cols.contains(QStringLiteral("provenance")));
        QVERIFY(cols.contains(QStringLiteral("is_local")));
    }
    QCOMPARE(companion.conversation().size(), 1);
    QCOMPARE(companion.conversation().first().toMap().value(QStringLiteral("provenance")).toString(), QString());
    QCOMPARE(companion.conversation().first().toMap().value(QStringLiteral("local")).toBool(), false);

    // Configure text-only local model for Ask so Companion uses word search (deterministic 1-request FakeServer).
    local.configure(where, QStringLiteral("qwen3.5:4b"));
    localServer.responses.append({200, "application/x-ndjson",
                                  "{\"message\":{\"content\":\"The lamp shines by the sea.\"},\"done\":false}\n"
                                  "{\"message\":{\"content\":\"\"},\"done\":true}\n"});
    companion.ask(QStringLiteral("What shines by the sea?"));
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
    QCOMPARE(companion.whereLocal(), true);
    QCOMPARE(companion.conversation().size(), 3);
    const QVariantMap localTurn = companion.conversation().last().toMap();
    QVERIFY(localTurn.value(QStringLiteral("provenance")).toString().contains(QStringLiteral("On this computer")));
    QCOMPARE(localTurn.value(QStringLiteral("local")).toBool(), true);
    {
        Companion reloaded(&services, &reader, library.database().connectionName());
        QCOMPARE(reloaded.conversation().size(), 3);
        QCOMPARE(reloaded.conversation().last().toMap().value(QStringLiteral("local")).toBool(), true);
        QVERIFY(reloaded.conversation().last().toMap().value(QStringLiteral("provenance")).toString()
                    .contains(QStringLiteral("On this computer")));
    }

    // 4. whereLocal == false on cloud Gemini reply, and whereLocal == true on offline sdcv Define even in Cloud mode.
    services.setPreset(QStringLiteral("cloud"));
    cloudServer.responses.append({200, "text/event-stream", fixture("text-stream.sse")});
    companion.ask(QStringLiteral("Tell me more in cloud mode."));
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
    QCOMPARE(companion.whereLocal(), false);
    QVERIFY(companion.conversation().last().toMap().value(QStringLiteral("provenance")).toString()
                .contains(QStringLiteral("Gemini")));
    QCOMPARE(companion.conversation().last().toMap().value(QStringLiteral("local")).toBool(), false);

    // Hermetic fake sdcv executable on PATH verifies offline dictionary sets whereLocal=true even in Cloud preset.
    const QString fakeBin = dir.filePath("fake-bin");
    QVERIFY(QDir().mkpath(fakeBin));
    const QString sdcvPath = fakeBin + QStringLiteral("/sdcv");
    write(sdcvPath, "#!/bin/sh\nprintf '[{\"word\":\"lamp\",\"dict\":\"LocalDict\",\"definition\":\"A vessel of light.\"}]'\n");
    QVERIFY(QFile::setPermissions(sdcvPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const QByteArray origPath = qgetenv("PATH");
    auto restorePath = qScopeGuard([&] { qputenv("PATH", origPath); });
    qputenv("PATH", fakeBin.toUtf8() + ":" + origPath);

    companion.define(QStringLiteral("lamp"), 0, 4);
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QCOMPARE(companion.whereLocal(), true);
    QCOMPARE(companion.where(), QStringLiteral("Dictionary on this computer"));
    QVERIFY(companion.answer().contains(QStringLiteral("A vessel of light.")));

    // 5. Spoiler-safe suggestions() uses loaded people + chapter title and skips already-asked prompts.
    services.setPreset(QStringLiteral("local"));
    companion.newConversation();
    localServer.responses.append({200, "application/x-ndjson",
                                  "{\"message\":{\"content\":\"{\\\"people\\\":[{\\\"name\\\":\\\"Ada Marsh\\\","
                                  "\\\"also\\\":\\\"\\\",\\\"who\\\":\\\"Keeper.\\\",\\\"chapter\\\":\\\"Chapter One\\\"}]}\"},\"done\":true}\n"});
    companion.whosWho();
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    const QStringList firstSuggestions = companion.suggestions();
    QCOMPARE(firstSuggestions.size(), 2);
    QCOMPARE(firstSuggestions[0], QStringLiteral("Who is Ada Marsh?"));
    QVERIFY(firstSuggestions[1].contains(QStringLiteral("Chapter One")));

    // After asking about Ada Marsh, suggestions() skips that character and rotates cleanly.
    localServer.responses.append({200, "application/x-ndjson",
                                  "{\"message\":{\"content\":\"She tends the lantern.\"},\"done\":true}\n"});
    companion.ask(QStringLiteral("Who is Ada Marsh?"));
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    const QStringList nextSuggestions = companion.suggestions();
    QCOMPARE(nextSuggestions.size(), 2);
    QVERIFY(!nextSuggestions.contains(QStringLiteral("Who is Ada Marsh?")));

    // 6. SystemCheck::summaryVerdicts populates 3 rows on refresh() and live-updates on routing changes.
    LocalModels freshLocal;
    AiServices checkAi(&gemini, &keys, library.database().connectionName());
    checkAi.setLocalModels(&freshLocal);
    SystemCheck check(&checkAi, nullptr);
    check.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(check.summaryVerdicts().size(), 3, 5000);
    QCOMPARE(check.summaryVerdicts()[0].toMap().value(QStringLiteral("feature")).toString(), QStringLiteral("Narration"));
    QCOMPARE(check.summaryVerdicts()[1].toMap().value(QStringLiteral("feature")).toString(), QStringLiteral("Define, translate"));
    QCOMPARE(check.summaryVerdicts()[2].toMap().value(QStringLiteral("feature")).toString(), QStringLiteral("Ask the whole book"));

    QSignalSpy checkSpy(&check, &SystemCheck::changed);
    freshLocal.configure(where, QStringLiteral("qwen3.5:4b"));
    emit checkAi.routingChanged();
    QTRY_VERIFY_WITH_TIMEOUT(checkSpy.count() >= 1, 2000);
    QCOMPARE(check.summaryVerdicts()[1].toMap().value(QStringLiteral("detail")).toString(), QStringLiteral("a few seconds"));
}

void ClothboundTests::wave2TransitionsDualFramePdfPrefetchAndSfx() {
    // 1. Dual-frame reader.js Node VM test:
    //    - zero .shown during src navigation
    //    - #book-back idle standby pre-warming & 0-load chapter swap reuse
    //    - backward chapter turn pre-scrolls to last page BEFORE .shown reveal
    //    - rapid in-chapter turn retargeting mid-flight without snapping/queuing
    //    - close() resets both frames and clears #viewport.turning
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (!node.isEmpty()) {
        const QString readerJsPath = QFINDTESTDATA("../src/reader/reader.js");
        const QString dualHarness = QStringLiteral(R"JS(
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[1], 'utf8');
let domReady = null;

function makeStyle() {
    const props = {};
    return {
        props, cssText: '', opacity: '', transform: '', backgroundColor: '',
        willChange: '', colorScheme: '', left: '', top: '', width: '', height: '',
        setProperty(k, v, prio) { this[k] = v; props[k] = { value: v, prio: prio || '' }; },
    };
}

function makeClassList(onAdd) {
    const set = new Set();
    return {
        add(c) { set.add(c); if (onAdd) onAdd(c); },
        remove(c) { set.delete(c); },
        contains(c) { return set.has(c); },
    };
}

const chapterSpecs = {
    'clothbound://book/1/ch0.xhtml': { pages: 1, text: 'Chapter zero opening words.' },
    'clothbound://book/1/ch1.xhtml': { pages: 3, text: 'Chapter one has three full pages of prose.' },
    'clothbound://book/1/ch2.xhtml': { pages: 2, text: 'Chapter two follows with two pages.' },
};

function makeChapterDoc(ownerFrame, url) {
    const spec = chapterSpecs[url] || { pages: 1, text: '' };
    const styleNodes = new Map();
    const textNode = { nodeType: 3, data: spec.text, parentElement: null };
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
        get scrollWidth() { return spec.pages * (parseFloat(ownerFrame.style.width) || 680); },
        scrollHeight: 692,
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
        head: { appendChild(el) { styleNodes.set(el.id, el); } },
        body: bodyEl,
        scrollingElement: docEl,
        fonts: { ready: Promise.resolve() },
        images: [],
        defaultView: {
            getComputedStyle() {
                return { writingMode: 'horizontal-tb', direction: 'ltr', textAlign: 'start', whiteSpace: 'normal' };
            },
            CSS: { highlights: new Map() },
            Highlight: class { constructor(...ranges) { this.ranges = ranges; this.priority = 0; } },
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
                getClientRects() { return [{ left: 10, right: 120, top: 10, bottom: 28, width: 110, height: 18 }]; },
                getBoundingClientRect() { return { left: 10, right: 120, top: 10, bottom: 28, width: 110, height: 18 }; },
            };
        },
        getSelection() { return { isCollapsed: true, removeAllRanges() {} }; },
    };
}

const srcLoads = [];
const shownEvents = [];

function makeFrame(id) {
    const f = {
        id,
        style: makeStyle(),
        contentDocument: null,
        contentWindow: null,
        onload: null,
        _src: 'about:blank',
        _attrs: {},
        setAttribute(k, v) { this._attrs[k] = v; },
        removeAttribute(k) { delete this._attrs[k]; },
        getBoundingClientRect() { return { left: 160, top: 54, width: parseFloat(this.style.width) || 680, height: 692 }; },
    };
    f.classList = makeClassList(cls => {
        if (cls === 'shown') {
            shownEvents.push({
                id: f.id,
                url: f._src,
                scrollLeft: f.contentDocument ? f.contentDocument.documentElement.scrollLeft : -1,
            });
        }
    });
    Object.defineProperty(f, 'src', {
        get() { return this._src; },
        set(v) {
            assert.strictEqual(this.classList.contains('shown'), false,
                `frame #${this.id} must NEVER carry .shown during src assignment (${v})`);
            this._src = v;
            if (v && v !== 'about:blank') {
                srcLoads.push({ id: this.id, url: v });
                const doc = makeChapterDoc(this, v);
                this.contentDocument = doc;
                this.contentWindow = doc.defaultView;
            } else {
                this.contentDocument = null;
                this.contentWindow = null;
            }
            const cb = this.onload;
            if (typeof cb === 'function') {
                queueMicrotask(() => {
                    assert.strictEqual(this.classList.contains('shown'), false,
                        `frame #${this.id} must still hide .shown when onload fires (${v})`);
                    cb();
                });
            }
        },
    });
    return f;
}

const stageEl = {
    clientWidth: 1000, clientHeight: 800,
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: this.clientWidth, height: this.clientHeight }; },
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
    createElement(tag) { return { tagName: tag.toUpperCase(), id: '', style: makeStyle(), setAttribute() {} }; },
    addEventListener(type, fn) { if (type === 'DOMContentLoaded') domReady = fn; },
};

let fakeNow = 1000;
let autoRaf = true;
let nextRafId = 1;
const rafQueue = new Map();

function requestAnimationFrame(cb) {
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

function cancelAnimationFrame(id) { rafQueue.delete(id); }

function stepRaf(deltaMs) {
    fakeNow += deltaMs;
    const batch = [...rafQueue.entries()];
    rafQueue.clear();
    for (const [, fn] of batch) fn(fakeNow);
}

let lastLoc = null;
const turnDirs = [];
const bridge = {
    surfaceLoaded() {},
    relocated(loc) { lastLoc = loc; },
    notifyPageTurn(dir) { turnDirs.push(dir); },
    log(msg) { throw new Error('reader.js log error: ' + msg); },
};

const sandbox = {
    console, setTimeout, clearTimeout, Promise, URL,
    performance: { now: () => fakeNow },
    requestAnimationFrame, cancelAnimationFrame,
    Node: { TEXT_NODE: 3 },
    NodeFilter: { SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, FILTER_ACCEPT: 1, FILTER_REJECT: 2 },
    CSS: { escape: s => s },
    Hyphenation: { load: async () => false, node() {} },
    document: documentMock,
    window: { innerWidth: 1000, innerHeight: 800 },
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

(async () => {
    const book = {
        session: 1, base: 'clothbound://book/1/', language: 'en', rtl: false,
        spine: [
            { href: 'ch0.xhtml', linear: true },
            { href: 'ch1.xhtml', linear: true },
            { href: 'ch2.xhtml', linear: true },
        ],
        pageTargets: [],
    };
    const style = {
        theme: { bg: '#1e1d1b', fg: '#dcd5c8', link: '#9db8f2', accentInk: '#9db8f2', muted: '#958d80',
                 popover: '#2a2926', border: 'rgba(255,255,255,0.12)', selection: '#345', search: '#ff0', found: '#fa0',
                 markYellow: '#ff0', markGreen: '#0f0', markBlue: '#00f', markPink: '#f0f',
                 speaking: '#345', dark: true },
        font: 'Literata', fontScale: 1.0, fontWeight: 400, lineHeight: 1.5, paragraphSpacing: 0.0,
        margin: 0.1, measure: 30, justify: true, hyphenate: false, layout: 'single', singleColumn: false,
        animate: true, reducedMotion: false, publisher: false, marginTop: 54, marginBottom: 54, radius: 0,
        listening: false, strings: { goToNote: 'Go', listenFromHere: 'Listen' },
    };

    await clothbound.open(book, { chapter: 0, offset: 0 }, style);
    assert.strictEqual(lastLoc.chapter, 0);
    assert.strictEqual(bookFrame.classList.contains('shown'), true);
    assert.strictEqual(backFrame.classList.contains('shown'), false);
    assert.strictEqual(srcLoads.length, 1);

    // Wait for 45ms scheduleWarm timer to pre-render ch1 into hidden #book-back.
    await new Promise(r => setTimeout(r, 90));
    assert.strictEqual(srcLoads.length, 2, 'standby #book-back should pre-warm ch1');
    assert.strictEqual(srcLoads[1].id, 'book-back');
    assert.strictEqual(srcLoads[1].url, 'clothbound://book/1/ch1.xhtml');
    assert.strictEqual(backFrame.classList.contains('shown'), false, 'pre-warmed standby stays hidden');

    // Cross chapter 0 -> chapter 1: reuses warm #book-back with ZERO extra src load.
    // First verify pressing prev() at the very start of the book emits ZERO page-turn sounds.
    await clothbound.prev();
    assert.deepStrictEqual(turnDirs, [], 'prev() at start of book must not emit page-turn sound');
    const loadsBeforeSwap = srcLoads.length;
    await clothbound.next();
    assert.strictEqual(srcLoads.length, loadsBeforeSwap, 'warm chapter swap must not re-navigate src');
    assert.strictEqual(lastLoc.chapter, 1);
    assert.strictEqual(lastLoc.page, 0);
    assert.strictEqual(lastLoc.pages, 3);
    assert.strictEqual(backFrame.classList.contains('shown'), true, 'swapped #book-back is now active');
    assert.strictEqual(bookFrame.classList.contains('shown'), false, 'old #book is hidden after swap');
    assert.strictEqual(viewportEl.classList.contains('turning'), false);
    assert.deepStrictEqual(turnDirs, [1]);

    // Rapid in-chapter turn retargeting mid-flight (ch1: page 0 -> 1 -> 2).
    const activeDocEl = backFrame.contentDocument.documentElement;
    const step = parseFloat(backFrame.style.width);
    assert.ok(step > 200);
    autoRaf = false;
    const p1 = clothbound.next();
    await new Promise(r => setTimeout(r, 10));
    assert.strictEqual(viewportEl.classList.contains('turning'), true, 'feather mask enabled while turning');
    stepRaf(65);
    const midScroll = activeDocEl.scrollLeft;
    assert.ok(midScroll > 0 && midScroll < step, `expected mid-flight scroll, got ${midScroll}/${step}`);

    // Fire second turn before first finishes: keeps live scroll and retargets cleanly to page 2.
    const p2 = clothbound.next();
    await new Promise(r => setTimeout(r, 10));
    assert.strictEqual(activeDocEl.scrollLeft, midScroll, 'retargeting must not snap scroll back before gliding');
    stepRaf(20);
    assert.ok(activeDocEl.scrollLeft > midScroll && activeDocEl.scrollLeft < 2 * step);
    stepRaf(160);
    await Promise.all([p1, p2]);
    assert.strictEqual(activeDocEl.scrollLeft, 2 * step);
    assert.strictEqual(lastLoc.chapter, 1);
    assert.strictEqual(lastLoc.page, 2);
    assert.strictEqual(viewportEl.classList.contains('turning'), false, 'feather mask cleared at rest');
    assert.deepStrictEqual(turnDirs, [1, 1, 1]);

    // Turn forward into ch2 (after standby warms), then backward into 3-page ch1 (end: true).
    autoRaf = true;
    await new Promise(r => setTimeout(r, 90));
    const loadsBeforeCh2 = srcLoads.length;
    await clothbound.next();
    assert.strictEqual(srcLoads.length, loadsBeforeCh2, 'ch2 should also reuse warm standby');
    assert.strictEqual(lastLoc.chapter, 2);
    assert.strictEqual(lastLoc.page, 0);

    // Advance to the final page of ch2 (page 1 of 2), then verify pressing next()
    // past the end of the book emits ZERO page-turn sounds, before returning to page 0.
    await clothbound.next();
    assert.strictEqual(lastLoc.chapter, 2);
    assert.strictEqual(lastLoc.page, 1);
    const turnsAtEnd = turnDirs.length;
    await clothbound.next();
    assert.strictEqual(turnDirs.length, turnsAtEnd, 'next() at end of book must not emit page-turn sound');
    assert.strictEqual(lastLoc.chapter, 2);
    assert.strictEqual(lastLoc.page, 1);
    await clothbound.prev();
    assert.strictEqual(lastLoc.chapter, 2);
    assert.strictEqual(lastLoc.page, 0);

    // Backward chapter turn: ch1 must be pre-scrolled to its LAST page (2 * step) BEFORE .shown is added.
    const shownCountBeforePrev = shownEvents.length;
    await clothbound.prev();
    assert.strictEqual(lastLoc.chapter, 1);
    assert.strictEqual(lastLoc.page, 2);
    const ch1Reveal = shownEvents.slice(shownCountBeforePrev).find(e => e.url === 'clothbound://book/1/ch1.xhtml');
    assert.ok(ch1Reveal, 'ch1 reveal should be recorded');
    assert.strictEqual(ch1Reveal.scrollLeft, 2 * step,
        `backward turn must pre-scroll to last page (${2 * step}) before adding .shown (saw ${ch1Reveal.scrollLeft})`);

    // close() hides both frames and resets them to about:blank.
    await clothbound.close();
    assert.strictEqual(bookFrame.classList.contains('shown'), false);
    assert.strictEqual(backFrame.classList.contains('shown'), false);
    assert.strictEqual(bookFrame.src, 'about:blank');
    assert.strictEqual(backFrame.src, 'about:blank');
    assert.strictEqual(viewportEl.classList.contains('turning'), false);
    process.exit(0);
})().catch(err => { console.error(err); process.exit(1); });
)JS");
        QProcess proc;
        proc.start(node, {QStringLiteral("-e"), dualHarness, readerJsPath});
        QVERIFY2(proc.waitForFinished(10000), "node dual-frame reader.js harness timed out");
        const QByteArray stderrOut = proc.readAllStandardError();
        QVERIFY2(proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                 stderrOut.isEmpty() ? "node dual-frame reader.js harness failed" : stderrOut.constData());
    }

    // 2. C++ PdfPages::prefetch() cache hit + cancelPrefetch + Reader::playPageTurn(+1/-1) headless & throttle.
    QTemporaryDir dir;
    const QString pdfPath = dir.filePath(QStringLiteral("multi.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QSizeF(200, 300), QPageSize::Point));
        QPainter painter(&writer);
        for (int p = 0; p < 3; ++p) {
            if (p > 0)
                writer.newPage();
            painter.fillRect(0, 0, writer.width(), writer.height(), Qt::white);
            painter.setPen(Qt::black);
            painter.drawText(80, 200, QStringLiteral("PDF Page %1").arg(p + 1));
        }
        painter.end();
    }

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(pdfId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(pdfId));
    const QString token = reader.pdfSource().mid(QStringLiteral("image://pdf/").size());
    QVERIFY(!token.isEmpty());

    const QString query = QStringLiteral("?bg=%231e1f24&fg=%23d6d2c9");
    const QString id1 = token + QStringLiteral("/1") + query;
    const QString id2 = token + QStringLiteral("/2") + query;
    const QString key1 = PdfPages::makeCacheKey(id1, QSize(220, 0));
    const QString key2 = PdfPages::makeCacheKey(id2, QSize(220, 0));

    std::vector<std::unique_ptr<QQuickImageResponse>> responses;
    {
        PdfPages pages;
        QVERIFY(PdfPages::cached(key1).isNull());
        QVERIFY(PdfPages::cached(key2).isNull());

        reader.pdfPrefetch({1, 2}, 220, query);
        QTRY_VERIFY_WITH_TIMEOUT(!PdfPages::cached(key1).isNull() && !PdfPages::cached(key2).isNull(), 8000);

        int done = 0;
        auto *resp = pages.requestImageResponse(id1, QSize(220, 0));
        connect(resp, &QQuickImageResponse::finished, this, [&done] { ++done; });
        responses.emplace_back(resp);
        QTRY_COMPARE_WITH_TIMEOUT(done, 1, 2000);
        std::unique_ptr<QQuickTextureFactory> factory(resp->textureFactory());
        QVERIFY(factory != nullptr);
        const QImage img = factory->image();
        QCOMPARE(img.width(), 220);
        QCOMPARE(QColor(img.pixel(2, 2)).name(), QStringLiteral("#1e1f24"));

        PdfPages::cancelPrefetch();
        reader.close();
        QVERIFY(PdfPages::cached(key1).isNull());
        QVERIFY(PdfPages::cached(key2).isNull());
    }

    // Reader::notifyPageTurn direction normalization + playPageTurn headless no-op + PageTurnSfx throttle.
    QSignalSpy turnSpy(&reader, &Reader::pageTurnRequested);
    reader.notifyPageTurn(3);
    reader.notifyPageTurn(0);
    reader.notifyPageTurn(-2);
    QCOMPARE(turnSpy.count(), 3);
    QCOMPARE(turnSpy[0][0].toInt(), 1);
    QCOMPARE(turnSpy[1][0].toInt(), 1);
    QCOMPARE(turnSpy[2][0].toInt(), -1);

    // Under offscreen test platform, playPageTurn() is a silent no-op (never opens an audio sink).
    reader.playPageTurn(1);
    reader.playPageTurn(-1);
    QVERIFY(reader.m_sfx == nullptr);

    // Dry-run synthesis + 85 ms rapid-repeat throttle verification (no audio hardware touched).
    QByteArray fwdPcm, throttledPcm, bwdPcm;
    QVERIFY(reader.playPageTurnDryRun(1, &fwdPcm));
    QVERIFY(fwdPcm.size() >= 9000);
    // Immediate back-to-back call is throttled (< 85ms).
    QVERIFY(!reader.playPageTurnDryRun(-1, &throttledPcm));
    QVERIFY(throttledPcm.isEmpty());
    // After the 85ms throttle window, backward flip plays a distinct synthesized clip.
    QTest::qWait(95);
    QVERIFY(reader.playPageTurnDryRun(-1, &bwdPcm));
    QVERIFY(bwdPcm.size() >= 9000);
    QVERIFY(bwdPcm != fwdPcm);
    const auto *samples = reinterpret_cast<const qint16 *>(fwdPcm.constData());
    const qsizetype count = fwdPcm.size() / 2;
    int peak = 0;
    for (qsizetype i = 0; i < count; ++i)
        peak = qMax(peak, std::abs(int(samples[i])));
    QVERIFY2(peak > 1000 && peak <= 7000, qPrintable(QString::number(peak)));
}

void ClothboundTests::wave2SettingsAiLocalSetupAudioCacheVoicePacksAndFolders() {
    QTemporaryDir dir;
    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
    auto restoreSettings = qScopeGuard([&] {
        if (origEnvKey.isEmpty())
            qunsetenv("GEMINI_API_KEY");
        else
            qputenv("GEMINI_API_KEY", origEnvKey);
        QSettings s(settingsPath, QSettings::IniFormat);
        s.remove(QStringLiteral("ai"));
        s.remove(QStringLiteral("local"));
    });
    qunsetenv("GEMINI_API_KEY");

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());

    // 1. AiServices::maskedKey() + resetUsageThisMonth().
    FakeServer cloudSrv;
    GeminiProvider gemini(nullptr, cloudSrv.endpoint());
    MemoryKeyStore keys;
    {
        AiServices noKey(&gemini, &keys, library.database().connectionName());
        QCOMPARE(noKey.maskedKey(), QString());
    }
    keys.save(QStringLiteral("gemini"), QStringLiteral("shortkey"));
    {
        AiServices shortKey(&gemini, &keys, library.database().connectionName());
        QCOMPARE(shortKey.maskedKey(), QStringLiteral("••••••••"));
    }
    keys.save(QStringLiteral("gemini"), QStringLiteral("AIzaSyDemo1234567890ABCD"));
    AiServices services(&gemini, &keys, library.database().connectionName());
    QCOMPARE(services.maskedKey(), QStringLiteral("AIza••••ABCD"));

    Ai::Usage usage;
    usage.inputTokens = 200000;
    usage.outputTokens = 50000;
    services.ledger().record(QStringLiteral("gemini"), QStringLiteral("gemini-3.5-flash-lite"),
                             QStringLiteral("ask"), usage, false);
    QVERIFY(services.spentThisMonth() > 0.0);
    QVERIFY(!services.usageThisMonth().isEmpty());
    {
        QSignalSpy usageSpy(&services, &AiServices::usageChanged);
        services.resetUsageThisMonth();
        QCOMPARE(usageSpy.count(), 1);
        QCOMPARE(services.spentThisMonth(), 0.0);
        QVERIFY(services.usageThisMonth().isEmpty());
    }

    // 2. AiServices::testLocal() + ServerFinder loopback alias dedup (localhost == 127.0.0.1).
    FakeServer ollamaSrv;
    ollamaSrv.routes.insert("/api/tags", {200, "application/json",
                                          R"({"models":[{"name":"qwen3.5:4b"},{"name":"qwen3-embedding:0.6b"}]})"});
    ollamaSrv.routes.insert("/api/show", {200, "application/json",
                                          R"({"model_info":{"qwen3.context_length":32768}})"});
    LocalModels local;
    services.setLocalModels(&local);
    services.useLocal(QStringLiteral("ollama"), ollamaSrv.origin(), QStringLiteral("qwen3.5:4b"), QString());

    services.testLocal();
    QVERIFY(services.testingLocal());
    QTRY_VERIFY_WITH_TIMEOUT(!services.testingLocal(), 5000);
    QVERIFY2(services.localError().isEmpty(), qPrintable(services.localError()));
    QVERIFY2(services.localTestResult().startsWith(QStringLiteral("Connected")), qPrintable(services.localTestResult()));

    // Invalid custom URL surfaces immediate validation error without probing.
    services.findLocal(QStringLiteral("ftp://example.com"));
    QVERIFY(!services.localError().isEmpty());

    // Loopback alias dedup: passing localhost:<port>/v1 and 127.0.0.1:<port>/api/tags finds it ONCE.
    const int port = QUrl(ollamaSrv.origin()).port();
    ServerFinder finder;
    QSignalSpy finderDone(&finder, &ServerFinder::finished);
    finder.find({QUrl(QStringLiteral("http://localhost:%1/v1").arg(port)),
                 QUrl(QStringLiteral("http://127.0.0.1:%1/api/tags").arg(port))});
    QTRY_COMPARE_WITH_TIMEOUT(finderDone.count(), 1, 5000);
    int matchedOnPort = 0;
    QUrl matchedUrl;
    for (const LocalModels::Server &srv : finder.servers()) {
        if (srv.url.port() == port) {
            ++matchedOnPort;
            matchedUrl = srv.url;
        }
    }
    QCOMPARE(matchedOnPort, 1);
    QCOMPARE(matchedUrl.host(), QStringLiteral("localhost"));

    // 3. LocalSetup::pullModel() progress & slot assignment + cancel() state reset.
    ollamaSrv.routes.insert("/api/pull",
                            {200, "application/x-ndjson",
                             "{\"status\":\"downloading\",\"digest\":\"sha256:aaa\",\"completed\":50,\"total\":100}\n"
                             "{\"status\":\"downloading\",\"digest\":\"sha256:aaa\",\"completed\":100,\"total\":100}\n"
                             "{\"status\":\"success\"}\n"});
    LocalSetup setup(&services);
    setup.pullModel(QStringLiteral("qwen3-embedding:0.6b"));
    QVERIFY(setup.pulling());
    QTRY_VERIFY_WITH_TIMEOUT(!setup.pulling(), 5000);
    QVERIFY2(setup.problem().isEmpty(), qPrintable(setup.problem()));
    QCOMPARE(setup.progress(), 1.0);
    QCOMPARE(services.localEmbeddingModel(), QStringLiteral("qwen3-embedding:0.6b"));
    QCOMPARE(services.localTextModel(), QStringLiteral("qwen3.5:4b"));

    // Hanging server tests LocalSetup::cancel() mid-pull resets pulling/progress/status cleanly.
    QTcpServer hangServer;
    QVERIFY(hangServer.listen(QHostAddress::LocalHost));
    QList<QPointer<QTcpSocket>> heldSockets;
    connect(&hangServer, &QTcpServer::newConnection, &hangServer, [&hangServer, &heldSockets] {
        while (QTcpSocket *s = hangServer.nextPendingConnection())
            heldSockets.append(s);
    });
    services.useLocal(QStringLiteral("ollama"),
                      QStringLiteral("http://127.0.0.1:%1").arg(hangServer.serverPort()),
                      QStringLiteral("qwen3.5:4b"), QString());
    QSignalSpy setupChanged(&setup, &LocalSetup::changed);
    setup.pullModel(QStringLiteral("qwen3.5:2b"));
    QVERIFY(setup.pulling());
    setup.cancel();
    QVERIFY(!setup.pulling());
    QVERIFY(!setup.waiting());
    QCOMPARE(setup.progress(), 0.0);
    QVERIFY(setup.status().isEmpty());
    QVERIFY(setupChanged.count() >= 2);
    for (const QPointer<QTcpSocket> &s : std::as_const(heldSockets))
        if (s)
            s->close();
    hangServer.close();
    services.forgetLocal();

    // 4. AudioCache async clear() + O(1) clipCount.
    {
        AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        QCOMPARE(cache.clipCount(), 0);
        QCOMPARE(cache.megabytes(), 0.0);
        QVERIFY(!cache.clearing());

        const QByteArray pcm(24000 * 2 / 5, '\x10');  // 0.2s mono 16-bit PCM
        QSignalSpy idleSpy(&cache, &AudioCache::storesIdle);
        cache.store(QStringLiteral("clip-1"), pcm, 24000);
        cache.store(QStringLiteral("clip-2"), pcm, 24000);
        QTRY_VERIFY_WITH_TIMEOUT(cache.clipCount() == 2 && !cache.hasPendingStores(), 10000);
        const QString file1 = cache.lookupPath(QStringLiteral("clip-1"));
        const QString file2 = cache.lookupPath(QStringLiteral("clip-2"));
        QVERIFY(QFileInfo::exists(file1));
        QVERIFY(QFileInfo::exists(file2));
        QVERIFY(cache.megabytes() > 0.0);

        QSignalSpy cacheChanged(&cache, &AudioCache::changed);
        cache.clear();
        QCOMPARE(cache.clipCount(), 0);
        QCOMPARE(cache.megabytes(), 0.0);
        QVERIFY(cache.clearing());
        QTRY_VERIFY_WITH_TIMEOUT(!cache.clearing(), 5000);
        QVERIFY(!QFileInfo::exists(file1));
        QVERIFY(!QFileInfo::exists(file2));
        QVERIFY(cacheChanged.count() >= 2);
    }

    // 5. VoicePacks::catalogPacks() + installPack().
    {
        auto makeArchive = [&dir](const QString &folder, const QString &file) {
            QDir().mkpath(dir.filePath(QStringLiteral("vp-src/") + folder + QStringLiteral("/lib")));
            write(dir.filePath(QStringLiteral("vp-src/") + folder + QStringLiteral("/lib/") + file), "pack bytes");
            QProcess tar;
            tar.start(QStringLiteral("tar"),
                      {QStringLiteral("-cjf"), dir.filePath(folder + QStringLiteral(".tar.bz2")),
                       QStringLiteral("-C"), dir.filePath(QStringLiteral("vp-src")), folder});
            tar.waitForFinished();
            QFile f(dir.filePath(folder + QStringLiteral(".tar.bz2")));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        const QByteArray engArc = makeArchive(QStringLiteral("engine-arm"), QStringLiteral("libsherpa-onnx-c-api.so"));
        const QByteArray kokArc = makeArchive(QStringLiteral("kokoro-fp32"), QStringLiteral("model.onnx"));
        auto sha = [](const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); };

        FakeServer vpSrv;
        vpSrv.routes.insert("/v1beta/engine-arm.tar.bz2", {200, "application/octet-stream", engArc});
        vpSrv.routes.insert("/v1beta/k-fp32.tar.bz2", {200, "application/octet-stream", kokArc});

        QJsonObject manifest = QJsonDocument::fromJson(
            packsManifest(vpSrv.endpoint(), sha(engArc).toLatin1(), "00", engArc.size(), 10)).object();
        QJsonObject kokoro = manifest.value(QStringLiteral("kokoro")).toObject();
        QJsonObject variants = kokoro.value(QStringLiteral("variants")).toObject();
        QJsonObject armVar = variants.value(QStringLiteral("aarch64")).toObject();
        armVar[QStringLiteral("sha256")] = sha(kokArc);
        armVar[QStringLiteral("size")] = kokArc.size();
        variants[QStringLiteral("aarch64")] = armVar;
        kokoro[QStringLiteral("variants")] = variants;
        manifest[QStringLiteral("kokoro")] = kokoro;
        manifest[QStringLiteral("pocket")] = QJsonObject{
            {QStringLiteral("id"), QStringLiteral("pocket-v1")},
            {QStringLiteral("title"), QStringLiteral("Pocket TTS")},
            {QStringLiteral("license"), QStringLiteral("MIT")},
            {QStringLiteral("url"), vpSrv.endpoint() + QStringLiteral("/pocket.tar.bz2")},
            {QStringLiteral("size"), 50},
            {QStringLiteral("sha256"), QStringLiteral("aa")},
            {QStringLiteral("dir"), QStringLiteral("pocket-v1")},
            {QStringLiteral("voices"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("bria")},
                                                              {QStringLiteral("name"), QStringLiteral("Bria")},
                                                              {QStringLiteral("language"), QStringLiteral("en-US")},
                                                              {QStringLiteral("reference"), QStringLiteral("bria.wav")}}}},
        };

        VoicePacks packs(dir.filePath(QStringLiteral("vp-root")), nullptr, QStringLiteral("aarch64"),
                         QJsonDocument(manifest).toJson());
        QVERIFY(!packs.engineVersion().isEmpty());
        const QVariantList beforeCatalog = packs.catalogPacks();
        QCOMPARE(beforeCatalog.size(), 2);
        QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("kokoro-v1.0"));
        QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("installed")).toBool(), false);
        QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("bytes")).toLongLong(), qint64(engArc.size() + kokArc.size()));
        QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("voiceCount")).toInt(), 3);
        QCOMPARE(beforeCatalog[0].toMap().value(QStringLiteral("languageCount")).toInt(), 1);
        QCOMPARE(beforeCatalog[1].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("pocket-v1"));

        packs.installPack(QStringLiteral("kokoro-v1.0"));
        QVERIFY(packs.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!packs.busy(), 10000);
        QVERIFY2(packs.problem().isEmpty(), qPrintable(packs.problem()));
        QVERIFY(packs.engineInstalled());
        QVERIFY(packs.isInstalled(QStringLiteral("kokoro-v1.0")));
        QCOMPARE(packs.installedVoiceCount(), 3);

        const QVariantList afterCatalog = packs.catalogPacks();
        QCOMPARE(afterCatalog[0].toMap().value(QStringLiteral("installed")).toBool(), true);
        QCOMPARE(afterCatalog[0].toMap().value(QStringLiteral("bytes")).toLongLong(), qint64(0));
        // Installing an already-installed pack is a clean no-op.
        packs.installPack(QStringLiteral("kokoro-v1.0"));
        QVERIFY(!packs.busy());
    }

    // 6. Library::folderStats() + empty-path guard + Scanner::cancel() sets m_running=false.
    QSignalSpy libProblem(&library, &Library::problem);
    library.addFolder(QString());
    library.addFolder(QStringLiteral("   "));
    library.removeFolder(QString());
    library.removeFolder(QStringLiteral("   "));
    QCOMPARE(libProblem.count(), 0);
    QVERIFY(library.folders().isEmpty());

    const QString booksDir = dir.filePath(QStringLiteral("shelf-a"));
    const QString goneDir = dir.filePath(QStringLiteral("shelf-gone"));
    QVERIFY(QDir().mkpath(booksDir));
    QVERIFY(QDir().mkpath(goneDir));
    QVERIFY(TestBooks::write(booksDir + QStringLiteral("/one.epub"), TestBooks::epub2(png(16, 20))));
    QVERIFY(TestBooks::write(booksDir + QStringLiteral("/two.pdf"), TestBooks::pdf("Two", "Auth")));

    QSignalSpy scanDone(&library, &Library::scanFinished);
    library.addFolder(booksDir);
    QTRY_COMPARE_WITH_TIMEOUT(scanDone.count(), 1, 10000);
    library.addFolder(goneDir);
    QTRY_COMPARE_WITH_TIMEOUT(scanDone.count(), 2, 10000);
    QVERIFY(QDir(goneDir).removeRecursively());

    const QVariantList stats = library.folderStats();
    QCOMPARE(stats.size(), 2);
    QCOMPARE(stats[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("shelf-a"));
    QCOMPARE(stats[0].toMap().value(QStringLiteral("exists")).toBool(), true);
    QCOMPARE(stats[0].toMap().value(QStringLiteral("books")).toInt(), 2);
    QCOMPARE(stats[1].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("shelf-gone"));
    QCOMPARE(stats[1].toMap().value(QStringLiteral("exists")).toBool(), false);
    QCOMPARE(stats[1].toMap().value(QStringLiteral("books")).toInt(), 0);

    Scanner scanner(dir.filePath(QStringLiteral("scan-covers")));
    scanner.scan({booksDir}, {}, {});
    QVERIFY(scanner.running());
    scanner.cancel();
    QVERIFY(!scanner.running());
}

void ClothboundTests::wave2CompanionRecapsFirstSeenResumeSdcvSseGeminiIllustratorAndTranslator() {
    QTemporaryDir dir;
    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
    const QByteArray origPath = qgetenv("PATH");
    const QLocale origLocale;
    const Qt::LayoutDirection origDir = QGuiApplication::layoutDirection();
    auto cleanup = qScopeGuard([&] {
        if (origEnvKey.isEmpty())
            qunsetenv("GEMINI_API_KEY");
        else
            qputenv("GEMINI_API_KEY", origEnvKey);
        qputenv("PATH", origPath);
        QLocale::setDefault(origLocale);
        QGuiApplication::setLayoutDirection(origDir);
        QSettings s(settingsPath, QSettings::IniFormat);
        s.remove(QStringLiteral("ai"));
        s.remove(QStringLiteral("local"));
        s.remove(QStringLiteral("appearance/language"));
    });
    qunsetenv("GEMINI_API_KEY");

    // Build a 3-chapter linear EPUB so resolveFirstSeen() and recap spoiler-guard span real chapters.
    const QByteArray epubBytes = TestBooks::epub({
        {"OEBPS/content.opf",
         "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<package version=\"2.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
         "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
         "    <dc:title>Fog Over Baker Street</dc:title>\n"
         "    <dc:creator>Arthur Doyle</dc:creator>\n"
         "    <dc:language>en</dc:language>\n"
         "  </metadata>\n"
         "  <manifest>\n"
         "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
         "    <item id=\"ch0\" href=\"ch0.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
         "    <item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
         "    <item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
         "  </manifest>\n"
         "  <spine toc=\"ncx\">\n"
         "    <itemref idref=\"ch0\"/>\n"
         "    <itemref idref=\"ch1\"/>\n"
         "    <itemref idref=\"ch2\"/>\n"
         "  </spine>\n"
         "</package>"},
        {"OEBPS/toc.ncx",
         "<?xml version=\"1.0\"?>\n"
         "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>\n"
         "  <navPoint id=\"n0\" playOrder=\"1\"><navLabel><text>Prologue</text></navLabel><content src=\"ch0.xhtml\"/></navPoint>\n"
         "  <navPoint id=\"n1\" playOrder=\"2\"><navLabel><text>Interlude</text></navLabel><content src=\"ch1.xhtml\"/></navPoint>\n"
         "  <navPoint id=\"n2\" playOrder=\"3\"><navLabel><text>Revelation</text></navLabel><content src=\"ch2.xhtml\"/></navPoint>\n"
         "</navMap></ncx>"},
        {"OEBPS/ch0.xhtml", TestBooks::chapter("Prologue",
            "<p>A quiet figure called The Detective watched the brass lantern while Lestrade took notes.</p>")},
        {"OEBPS/ch1.xhtml", TestBooks::chapter("Interlude",
            "<p>Rain drummed softly across the empty cobblestone square until dawn.</p>")},
        {"OEBPS/ch2.xhtml", TestBooks::chapter("Revelation",
            "<p>At last Sherlock Holmes greeted Inspector Lestrade beneath the archway.</p>")},
    });
    const QString epubPath = dir.filePath(QStringLiteral("fog.epub"));
    QVERIFY(TestBooks::write(epubPath, epubBytes));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(epubPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(bookId));
    reader.relocated({{"chapter", 2}, {"offset", 0}, {"end", 120}, {"page", 0}, {"pages", 1}});

    FakeServer cloudSrv;
    FakeServer localSrv;
    GeminiProvider gemini(nullptr, cloudSrv.endpoint());
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    services.setLocalModels(&local);

    // 1. Companion::resumePendingSetup() handoff when invoked unconfigured.
    Companion companion(&services, &reader, library.database().connectionName());
    companion.ask(QStringLiteral("Who watched the lantern?"));
    QVERIFY(!companion.busy());
    QVERIFY(companion.problemIsConfig());
    QVERIFY(companion.hasPendingSetup());
    QVERIFY(companion.pendingSetupLabel().contains(QStringLiteral("Who watched the lantern?")));

    LocalModels::Server srv;
    srv.kind = LocalModels::Ollama;
    srv.url = QUrl(localSrv.origin());
    local.configure(srv, QStringLiteral("qwen3.5:4b"));
    services.setPreset(QStringLiteral("local"));
    localSrv.responses.append({200, "application/x-ndjson",
                               "{\"message\":{\"content\":\"The Detective watched the lantern.\"},\"done\":true}\n"});
    companion.resumePendingSetup();
    QVERIFY(!companion.hasPendingSetup());
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
    QCOMPARE(companion.conversation().size(), 2);
    QVERIFY(companion.conversation().last().toMap().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("The Detective")));

    // 2. Companion::resolveFirstSeen() earliest-mention ordering across chapters (alias & surname in ch0 beat full name in ch2).
    localSrv.responses.append({200, "application/x-ndjson",
                               "{\"message\":{\"content\":\"{\\\"people\\\":["
                               "{\\\"name\\\":\\\"Sherlock Holmes\\\",\\\"also\\\":\\\"The Detective\\\","
                               "\\\"who\\\":\\\"Consulting detective.\\\",\\\"chapter\\\":\\\"Revelation\\\"},"
                               "{\\\"name\\\":\\\"Inspector Lestrade\\\",\\\"also\\\":\\\"\\\","
                               "\\\"who\\\":\\\"Scotland Yard inspector.\\\",\\\"chapter\\\":\\\"Revelation\\\"}"
                               "]}\"},\"done\":true}\n"});
    companion.whosWho();
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QCOMPARE(companion.people().size(), 2);
    const QVariantMap holmes = companion.people()[0].toMap();
    const QVariantMap lestrade = companion.people()[1].toMap();
    QCOMPARE(holmes.value(QStringLiteral("chapterIndex")).toInt(), 0);
    QCOMPARE(holmes.value(QStringLiteral("chapter")).toString(), QStringLiteral("Prologue"));
    QVERIFY(holmes.value(QStringLiteral("offset")).toLongLong() > 0);
    QCOMPARE(lestrade.value(QStringLiteral("chapterIndex")).toInt(), 0);
    QCOMPARE(lestrade.value(QStringLiteral("chapter")).toString(), QStringLiteral("Prologue"));
    QVERIFY(lestrade.value(QStringLiteral("offset")).toLongLong() > holmes.value(QStringLiteral("offset")).toLongLong());

    // 3. companion_recaps persistence, memory cache, reload, backward spoiler guard, forward auto-restore & AFTER DELETE trigger.
    localSrv.responses.append({200, "application/x-ndjson",
                               "{\"message\":{\"content\":\"Recap up to Revelation.\"},\"done\":true}\n"});
    const qsizetype reqBeforeRecap = localSrv.requests.size();
    companion.recap();
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QCOMPARE(localSrv.requests.size(), reqBeforeRecap + 1);
    QCOMPARE(companion.recapLoaded(), true);
    QCOMPARE(companion.recapChapter(), 2);
    QCOMPARE(companion.recapText(), QStringLiteral("Recap up to Revelation."));
    QCOMPARE(companion.recapWhereLocal(), true);

    // Second call at same chapter hits memory cache (0 new requests).
    companion.recap();
    QVERIFY(!companion.busy());
    QCOMPARE(localSrv.requests.size(), reqBeforeRecap + 1);

    // Fresh Companion restores persisted recap from SQLite with 0 network requests.
    {
        Companion fresh(&services, &reader, library.database().connectionName());
        QCOMPARE(fresh.recapLoaded(), true);
        QCOMPARE(fresh.recapChapter(), 2);
        QCOMPARE(fresh.recapText(), QStringLiteral("Recap up to Revelation."));
    }

    // Backward navigation to chapter 0 hides chapter 2 recap (strict spoiler guard).
    reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 80}, {"page", 0}, {"pages", 1}});
    QCOMPARE(companion.recapLoaded(), false);
    QVERIFY(companion.recapText().isEmpty());

    // Returning forward to chapter 2 automatically restores the cached chapter 2 recap.
    reader.relocated({{"chapter", 2}, {"offset", 0}, {"end", 120}, {"page", 0}, {"pages", 1}});
    QCOMPARE(companion.recapLoaded(), true);
    QCOMPARE(companion.recapText(), QStringLiteral("Recap up to Revelation."));

    // 4. sdcv punctuation trim & single fallback guarantee.
    const QString fakeBin = dir.filePath(QStringLiteral("sdcv-bin"));
    QVERIFY(QDir().mkpath(fakeBin));
    const QString sdcvLog = dir.filePath(QStringLiteral("sdcv.log"));
    const QString sdcvScript = fakeBin + QStringLiteral("/sdcv");
    write(sdcvScript,
          QStringLiteral("#!/bin/sh\n"
                         "last=\"\"\nfor a in \"$@\"; do last=\"$a\"; done\n"
                         "printf '%s\\n' \"$last\" >> \"%1\"\n"
                         "if [ \"$last\" = \"lantern\" ]; then\n"
                         "  printf '[{\"word\":\"lantern\",\"dict\":\"WordNet\",\"definition\":\"A portable lamp.\"}]'\n"
                         "  exit 0\n"
                         "fi\n"
                         "exit 1\n")
              .arg(sdcvLog)
              .toUtf8());
    QVERIFY(QFile::setPermissions(sdcvScript, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    qputenv("PATH", fakeBin.toUtf8() + ":" + origPath);

    companion.define(QStringLiteral("“lantern,”"), 0, 10);
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QCOMPARE(companion.where(), QStringLiteral("Dictionary on this computer"));
    QVERIFY(companion.answer().contains(QStringLiteral("A portable lamp.")));
    {
        QFile logFile(sdcvLog);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(logFile.readAll()).trimmed(), QStringLiteral("lantern"));
    }

    // Missing dictionary word falls back to the AI model exactly ONCE.
    const qsizetype reqBeforeMiss = localSrv.requests.size();
    localSrv.responses.append({200, "application/x-ndjson",
                               "{\"message\":{\"content\":\"*noun* — A rare fogbound word.\"},\"done\":true}\n"});
    companion.define(QStringLiteral("unlistedword"), 0, 10);
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QTest::qWait(50);
    QCOMPARE(localSrv.requests.size(), reqBeforeMiss + 1);
    QVERIFY(companion.answer().contains(QStringLiteral("rare fogbound word")));

    // Broken non-executable sdcv (FailedToStart -> errorOccurred) also triggers a single fallback.
    QVERIFY(QFile::setPermissions(sdcvScript, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const qsizetype reqBeforeBroken = localSrv.requests.size();
    localSrv.responses.append({200, "application/x-ndjson",
                               "{\"message\":{\"content\":\"*noun* — Fallback after FailedToStart.\"},\"done\":true}\n"});
    companion.define(QStringLiteral("anotherword"), 0, 10);
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QTest::qWait(50);
    QCOMPARE(localSrv.requests.size(), reqBeforeBroken + 1);
    QVERIFY(companion.answer().contains(QStringLiteral("Fallback after FailedToStart")));
    qputenv("PATH", origPath);

    // Verify AFTER DELETE ON books trigger cascades companion_recaps, companion_threads, companion_whoswho.
    {
        QSqlQuery countBefore(library.database());
        QVERIFY(countBefore.exec(QStringLiteral("SELECT COUNT(*) FROM companion_recaps WHERE book_id = %1").arg(bookId)));
        QVERIFY(countBefore.next());
        QCOMPARE(countBefore.value(0).toInt(), 1);
        QSqlQuery delBook(library.database());
        QVERIFY(delBook.exec(QStringLiteral("DELETE FROM books WHERE id = %1").arg(bookId)));
        for (const char *tbl : {"companion_recaps", "companion_threads", "companion_whoswho"}) {
            QSqlQuery check(library.database());
            QVERIFY(check.exec(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE book_id = %2")
                                   .arg(QString::fromLatin1(tbl))
                                   .arg(bookId)));
            QVERIFY(check.next());
            QCOMPARE(check.value(0).toInt(), 0);
        }
    }

    // 5. SseParser: UTF-8 BOM + chunk-split \r\n + bare \r.
    {
        SseParser sse;
        const auto bomEvents = sse.feed("\xEF\xBB\xBF" "event: step.delta\r\ndata: hello\r\n\r\n");
        QCOMPARE(bomEvents.size(), 1);
        QCOMPARE(bomEvents[0].name, QByteArray("step.delta"));
        QCOMPARE(bomEvents[0].data, QByteArray("hello"));

        QVERIFY(sse.feed("event: step.delta\r").isEmpty());
        QVERIFY(sse.feed("\ndata: line1\r").isEmpty());
        const auto splitEvents = sse.feed("\ndata: line2\r\n\r\n");
        QCOMPARE(splitEvents.size(), 1);
        QCOMPARE(splitEvents[0].name, QByteArray("step.delta"));
        QCOMPARE(splitEvents[0].data, QByteArray("line1\nline2"));

        // Bare CR preserves a trailing '\r' across chunk boundaries so a chunk-split CRLF
        // isn't prematurely split, and flushes cleanly once followed by another line or finish().
        QVERIFY(sse.feed("data: bare1\rdata: bare2\r\r").isEmpty());
        const auto bareCr = sse.finish();
        QCOMPARE(bareCr.size(), 1);
        QCOMPARE(bareCr[0].data, QByteArray("bare1\nbare2"));
    }

    // 6. GeminiReply: mid-stream SSE error mapping & single-emit abort().
    {
        gemini.setKey(QStringLiteral("test-gemini-key"));
        struct SseCase {
            QByteArray body;
            Ai::Error::Kind expected;
        };
        const QList<SseCase> cases = {
            {"event: error\ndata: {\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"message\":\"quota\"}}\n\n",
             Ai::Error::Limited},
            {"event: interaction.failed\ndata: {\"interaction\":{\"error\":{\"status\":\"UNAUTHENTICATED\",\"message\":\"bad key\"}}}\n\n",
             Ai::Error::BadKey},
            {"event: error\ndata: {\"error\":{\"status\":\"PERMISSION_DENIED\",\"message\":\"denied\"}}\n\n",
             Ai::Error::Refused},
            {"event: error\ndata: {\"error\":{\"code\":404,\"message\":\"missing\"}}\n\n",
             Ai::Error::NotFound},
            {"event: error\ndata: {\"error\":{\"status\":\"INVALID_ARGUMENT\",\"message\":\"bad arg\"}}\n\n",
             Ai::Error::Invalid},
        };
        for (const SseCase &tc : cases) {
            cloudSrv.responses.append({200, "text/event-stream", tc.body});
            Ai::TextRequest req;
            req.feature = QStringLiteral("ask");
            req.stream = true;
            req.messages = {{true, QStringLiteral("hi")}};
            std::unique_ptr<Ai::Reply> reply(gemini.generate(req));
            QSignalSpy fin(reply.get(), &Ai::Reply::finished);
            QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 5000);
            QCOMPARE(reply->error().kind, tc.expected);
        }

        // Single-emit abort() on an active hanging GeminiReply.
        QTcpServer hangSrv;
        QVERIFY(hangSrv.listen(QHostAddress::LocalHost));
        QList<QPointer<QTcpSocket>> socks;
        connect(&hangSrv, &QTcpServer::newConnection, &hangSrv, [&hangSrv, &socks] {
            while (QTcpSocket *s = hangSrv.nextPendingConnection())
                socks.append(s);
        });
        GeminiProvider hangGemini(nullptr, QStringLiteral("http://127.0.0.1:%1/v1beta").arg(hangSrv.serverPort()));
        hangGemini.setKey(QStringLiteral("test-gemini-key"));
        Ai::TextRequest req;
        req.feature = QStringLiteral("ask");
        req.stream = true;
        req.messages = {{true, QStringLiteral("abort me")}};
        std::unique_ptr<Ai::Reply> abortReply(hangGemini.generate(req));
        QSignalSpy abortSpy(abortReply.get(), &Ai::Reply::finished);
        abortReply->abort();
        abortReply->abort();
        QTest::qWait(50);
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(abortReply->error().kind, Ai::Error::Cancelled);
        for (const QPointer<QTcpSocket> &s : std::as_const(socks))
            if (s)
                s->close();
        hangSrv.close();
    }

    // 7. Illustrator::budgetOk / unavailableReason / saveCopyTo().
    {
        const int illBookId = library.addBookFile(epubPath);
        QVERIFY(illBookId > 0);
        QVERIFY(reader.openBook(illBookId));
        reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 90}, {"page", 0}, {"pages", 1}});

        Illustrator illustrator(&services, &reader, library.database().connectionName(),
                                dir.filePath(QStringLiteral("illustrations")));
        // No key
        gemini.setKey({});
        services.setPreset(QStringLiteral("cloud"));
        QVERIFY(!illustrator.available());
        QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("add your key")));

        // Key present, strict local mode
        gemini.setKey(QStringLiteral("test-gemini-key"));
        services.setPreset(QStringLiteral("local"));
        QVERIFY(!illustrator.available());
        QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("Hybrid or Cloud")));

        // Cloud mode with $0 monthly cap
        services.setPreset(QStringLiteral("cloud"));
        services.setUnlimitedBudget(false);
        services.setBudget(0.0);
        QVERIFY(illustrator.available());
        QVERIFY(!illustrator.budgetOk());
        QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("$0")));

        // Cloud mode with non-zero cap below picture cost
        services.setBudget(0.001);
        QVERIFY(!illustrator.budgetOk());
        QVERIFY(illustrator.unavailableReason().contains(QStringLiteral("monthly budget")));

        // Cloud mode with healthy budget -> generate and saveCopyTo()
        services.setBudget(10.0);
        QVERIFY(illustrator.budgetOk());
        QVERIFY(illustrator.unavailableReason().isEmpty());

        const QByteArray imgPng = png(48, 36);
        const QByteArray promptReply = QJsonDocument(QJsonObject{
            {QStringLiteral("steps"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("model_output")},
                {QStringLiteral("content"), QJsonArray{QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("{\"prompt\":\"Victorian fog over Baker Street lantern\",\"caption\":\"The brass lantern\"}")},
                }}},
            }}},
        }).toJson(QJsonDocument::Compact);
        const QByteArray imageReply = QJsonDocument(QJsonObject{
            {QStringLiteral("candidates"), QJsonArray{QJsonObject{
                {QStringLiteral("content"), QJsonObject{
                    {QStringLiteral("parts"), QJsonArray{QJsonObject{
                        {QStringLiteral("inlineData"), QJsonObject{
                            {QStringLiteral("mimeType"), QStringLiteral("image/png")},
                            {QStringLiteral("data"), QString::fromLatin1(imgPng.toBase64())},
                        }},
                    }}},
                }},
            }}},
        }).toJson(QJsonDocument::Compact);
        cloudSrv.responses.append({200, "application/json", promptReply});
        cloudSrv.responses.append({200, "application/json", imageReply});
        QSignalSpy madeSpy(&illustrator, &Illustrator::made);
        illustrator.illustrate(QStringLiteral("watched the brass lantern"), 0, 60);
        QTRY_COMPARE_WITH_TIMEOUT(madeSpy.count(), 1, 5000);
        const int picId = madeSpy[0][0].toInt();
        QVERIFY(picId > 0);

        const QString explicitDest = dir.filePath(QStringLiteral("exports/lantern-copy.png"));
        QCOMPARE(illustrator.saveCopyTo(picId, explicitDest), explicitDest);
        QFile copied(explicitDest);
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), imgPng);

        const QString exportDir = dir.filePath(QStringLiteral("exports-dir"));
        QVERIFY(QDir().mkpath(exportDir));
        const QString intoDir = illustrator.saveCopyTo(picId, exportDir);
        QVERIFY(intoDir.startsWith(exportDir + QLatin1Char('/')));
        QVERIFY(QFileInfo::exists(intoDir));
        QVERIFY(illustrator.saveCopyTo(999999, explicitDest).isEmpty());
    }

    // 8. Translator::setLanguage() / languageOverride isolation & live reload.
    {
        const QString langFile = dir.filePath(QStringLiteral("omarchy-lang"));
        const QString catDir = dir.filePath(QStringLiteral("i18n"));
        QVERIFY(QDir().mkpath(catDir));
        write(langFile, "de\n");
        write(catDir + QStringLiteral("/en.json"), R"({"Hello":"Hello"})");
        write(catDir + QStringLiteral("/de.json"), R"({"Hello":"Hallo"})");
        write(catDir + QStringLiteral("/fr.json"), R"({"Hello":"Bonjour"})");
        write(catDir + QStringLiteral("/ar.json"), R"({"Hello":"مرحبا"})");

        Translator isolated(langFile, catDir);
        QCOMPARE(isolated.language(), QStringLiteral("de"));
        QCOMPARE(isolated.languageOverride(), QString());
        QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("Hallo"));

        QSignalSpy changedSpy(&isolated, &Translator::changed);
        isolated.setLanguage(QStringLiteral("fr"));
        QCOMPARE(isolated.languageOverride(), QStringLiteral("fr"));
        QCOMPARE(isolated.language(), QStringLiteral("fr"));
        QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("Bonjour"));
        QCOMPARE(changedSpy.count(), 1);
        // Explicit languageFile stays isolated from settings.ini.
        QVERIFY(QSettings(settingsPath, QSettings::IniFormat).value(QStringLiteral("appearance/language")).toString().isEmpty());

        // Invalid language codes are ignored safely.
        isolated.setLanguage(QStringLiteral("../evil"));
        QCOMPARE(isolated.languageOverride(), QStringLiteral("fr"));
        QCOMPARE(changedSpy.count(), 1);

        // Live RTL reload on Arabic, then reset to desktop ("") restores German + LTR.
        isolated.setLanguage(QStringLiteral("ar"));
        QCOMPARE(isolated.language(), QStringLiteral("ar"));
        QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("مرحبا"));
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::RightToLeft);

        isolated.setLanguage(QString());
        QCOMPARE(isolated.languageOverride(), QString());
        QCOMPARE(isolated.language(), QStringLiteral("de"));
        QCOMPARE(isolated.translate("", "Hello"), QStringLiteral("Hallo"));
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);

        // Default constructor (empty languageFile) persists override under appearance/language.
        {
            Translator persisted(QString(), catDir);
            persisted.setLanguage(QStringLiteral("fr"));
            QCOMPARE(QSettings(settingsPath, QSettings::IniFormat).value(QStringLiteral("appearance/language")).toString(),
                     QStringLiteral("fr"));
        }
        {
            Translator reloaded(QString(), catDir);
            QCOMPARE(reloaded.languageOverride(), QStringLiteral("fr"));
            QCOMPARE(reloaded.language(), QStringLiteral("fr"));
            reloaded.setLanguage(QString());
            QVERIFY(QSettings(settingsPath, QSettings::IniFormat).value(QStringLiteral("appearance/language")).toString().isEmpty());
        }
    }
}

void ClothboundTests::wave4McpProtocolKillSwitchToolsAndResources() {
    QTemporaryDir dir;
    const QString epubPath = dir.filePath(QStringLiteral("lantern.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

    // Multi-page PDF with distinct text on pages 1 and 2.
    const QString pdfPath = dir.filePath(QStringLiteral("quantum.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(72);
        writer.setTitle(QStringLiteral("Quantum Circuits Primer"));
        QPainter p(&writer);
        p.drawText(QRect(20, 60, 550, 120),
                   QStringLiteral("Page one introduces Hadamard gates and quantum entanglement clearly."));
        writer.newPage();
        p.drawText(QRect(20, 60, 550, 120),
                   QStringLiteral("Page two derives Bell state fidelity bounds with concise proofs."));
        p.end();
    }
    const QString hiddenPath = dir.filePath(QStringLiteral("hidden.epub"));
    QVERIFY(TestBooks::write(hiddenPath, TestBooks::epub3(adobeObfuscate(fontPayload()))));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int epubId = library.addBookFile(epubPath);
    const int pdfId = library.addBookFile(pdfPath);
    const int hiddenId = library.addBookFile(hiddenPath);
    QVERIFY(epubId > 0 && pdfId > 0 && hiddenId > 0);
    library.setHidden(hiddenId, true);

    const int colId = library.createCollection(QStringLiteral("Physics"));
    QVERIFY(colId > 0);
    QVERIFY(library.addToCollection(pdfId, colId));

    Marks marks(&library);
    marks.setBook(epubId);
    const int markId = marks.addHighlight(0, 12, 36, QStringLiteral("lamp's light & the sea"),
                                          QStringLiteral("yellow"), QStringLiteral("Chapter One"));
    QVERIFY(markId > 0);
    marks.setNote(markId, QStringLiteral("Opening harbor motif"));

    const QString settingsIni = dir.filePath(QStringLiteral("mcp-settings.ini"));
    const QString indexDb = dir.filePath(QStringLiteral("index.sqlite"));
    LibraryIndex libIndex(indexDb);
    QVERIFY(libIndex.available());

    // Set up fake local embedder so we can exercise BOTH lexical and semantic paths of search_passages.
    const QString appSettings =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    auto restoreSettings = qScopeGuard([&] {
        QSettings s(appSettings, QSettings::IniFormat);
        s.remove(QStringLiteral("ai/preset"));
    });
    FakeServer embedServer;
    embedServer.routes.insert("/api/embed",
                              {200, "application/json", "{\"embeddings\":[[0.0,1.0,0.0]]}"});
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    LocalModels::Server lcfg;
    lcfg.kind = LocalModels::Ollama;
    lcfg.url = QUrl(embedServer.origin());
    local.configure(lcfg, QStringLiteral("qwen3.5:4b"), QStringLiteral("nomic-embed-text"));
    services.setLocalModels(&local);
    services.setPreset(QStringLiteral("local"));

    McpServer server(&library, &libIndex, &services, {settingsIni, indexDb});

    auto rpc = [&](const QJsonValue &id, const QString &method, const QJsonObject &params = {}) {
        QJsonObject req{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}};
        if (!id.isUndefined())
            req.insert(QStringLiteral("id"), id);
        if (!params.isEmpty())
            req.insert(QStringLiteral("params"), params);
        return server.handleRequest(req);
    };

    // 1. Handshake, framing, notifications, ping, and protocol error codes.
    const QJsonObject initOk = rpc(1, QStringLiteral("initialize"), {{QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")}});
    QCOMPARE(initOk.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(),
             QStringLiteral("2025-03-26"));
    QCOMPARE(initOk.value(QStringLiteral("result")).toObject().value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("clothbound"));

    const QJsonObject initFallback = rpc(2, QStringLiteral("initialize"), {{QStringLiteral("protocolVersion"), QStringLiteral("1999-01-01")}});
    QCOMPARE(initFallback.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString(),
             QStringLiteral("2024-11-05"));

    // Notification without id produces empty QJsonObject (no stdout reply).
    QVERIFY(rpc(QJsonValue::Undefined, QStringLiteral("notifications/initialized")).isEmpty());

    const QJsonObject pingRes = rpc(3, QStringLiteral("ping"));
    QVERIFY(pingRes.contains(QStringLiteral("result")));
    QCOMPARE(rpc(4, QStringLiteral("nonexistent/method")).value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(),
             -32601);
    const QJsonObject badRpc = server.handleRequest({{QStringLiteral("jsonrpc"), QStringLiteral("1.0")},
                                                     {QStringLiteral("id"), 5},
                                                     {QStringLiteral("method"), QStringLiteral("ping")}});
    QCOMPARE(badRpc.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32600);

    // 2. tools/list advertises all 6 strictly read-only tools.
    const QJsonArray tools = rpc(6, QStringLiteral("tools/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 6);
    QStringList toolNames;
    for (const QJsonValue &tv : tools) {
        const QJsonObject t = tv.toObject();
        toolNames.append(t.value(QStringLiteral("name")).toString());
        const QJsonObject ann = t.value(QStringLiteral("annotations")).toObject();
        QVERIFY(ann.value(QStringLiteral("readOnlyHint")).toBool());
        QVERIFY(!ann.value(QStringLiteral("destructiveHint")).toBool());
    }
    for (const QString &expected : {QStringLiteral("list_books"), QStringLiteral("search_library"),
                                    QStringLiteral("search_passages"), QStringLiteral("get_book_outline"),
                                    QStringLiteral("read_passage"), QStringLiteral("list_marks")}) {
        QVERIFY2(toolNames.contains(expected), qPrintable(expected));
    }

    // 3. Kill-switch: ai/mcpEnabled=false blocks tools/call & resources/read with -32001, re-enabling restores immediately.
    QVERIFY(server.isEnabled());
    {
        QSettings s(settingsIni, QSettings::IniFormat);
        s.setValue(QStringLiteral("ai/mcpEnabled"), false);
        s.sync();
    }
    QVERIFY(!server.isEnabled());
    const QJsonObject blockedTool = rpc(7, QStringLiteral("tools/call"),
                                        {{QStringLiteral("name"), QStringLiteral("list_books")},
                                         {QStringLiteral("arguments"), QJsonObject{}}});
    QCOMPARE(blockedTool.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32001);
    QVERIFY(blockedTool.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("Settings")));
    const QJsonObject blockedRes = rpc(8, QStringLiteral("resources/read"),
                                       {{QStringLiteral("uri"), QStringLiteral("clothbound://library/books")}});
    QCOMPARE(blockedRes.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32001);
    {
        QSettings s(settingsIni, QSettings::IniFormat);
        s.setValue(QStringLiteral("ai/mcpEnabled"), true);
        s.sync();
    }
    QVERIFY(server.isEnabled());

    auto callTool = [&](int id, const QString &name, const QJsonObject &args = {}) {
        return rpc(id, QStringLiteral("tools/call"),
                   {{QStringLiteral("name"), name}, {QStringLiteral("arguments"), args}})
            .value(QStringLiteral("result"))
            .toObject();
    };

    // 4. list_books (filtering by format, query, collection name & id, hidden book excluded).
    const QJsonObject allBooks = callTool(10, QStringLiteral("list_books")).value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(allBooks.value(QStringLiteral("count")).toInt(), 2);
    const QJsonObject pdfOnly = callTool(11, QStringLiteral("list_books"), {{QStringLiteral("format"), QStringLiteral("pdf")}})
                                    .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(pdfOnly.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(pdfOnly.value(QStringLiteral("books")).toArray()[0].toObject().value(QStringLiteral("id")).toInt(), pdfId);
    const QJsonObject byColName = callTool(12, QStringLiteral("list_books"), {{QStringLiteral("collection"), QStringLiteral("physics")}})
                                      .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(byColName.value(QStringLiteral("count")).toInt(), 1);
    const QJsonObject byColId = callTool(13, QStringLiteral("list_books"), {{QStringLiteral("collection"), QString::number(colId)}})
                                    .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(byColId.value(QStringLiteral("count")).toInt(), 1);

    // 5. search_library (whole-library & book_id scoped FTS5).
    const QJsonObject ftsAll = callTool(14, QStringLiteral("search_library"), {{QStringLiteral("query"), QStringLiteral("Hadamard")}})
                                   .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(ftsAll.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(ftsAll.value(QStringLiteral("results")).toArray()[0].toObject().value(QStringLiteral("book_id")).toInt(), pdfId);
    QCOMPARE(ftsAll.value(QStringLiteral("results")).toArray()[0].toObject().value(QStringLiteral("page")).toInt(), 1);

    const QJsonObject ftsScopedMiss = callTool(15, QStringLiteral("search_library"),
                                               {{QStringLiteral("query"), QStringLiteral("Hadamard")},
                                                {QStringLiteral("book_id"), epubId}})
                                          .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(ftsScopedMiss.value(QStringLiteral("count")).toInt(), 0);

    // 6. search_passages: lexical FTS5+BM25 fallback first (no vectors yet), then semantic path after building PassageIndex.
    const QJsonObject lexPassages = callTool(16, QStringLiteral("search_passages"),
                                             {{QStringLiteral("query"), QStringLiteral("Bell state fidelity")},
                                              {QStringLiteral("collection"), QStringLiteral("Physics")}})
                                        .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(lexPassages.value(QStringLiteral("mode")).toString(), QStringLiteral("lexical"));
    QVERIFY(lexPassages.value(QStringLiteral("count")).toInt() >= 1);
    QCOMPARE(lexPassages.value(QStringLiteral("passages")).toArray()[0].toObject().value(QStringLiteral("book_id")).toInt(), pdfId);

    {
        PassageIndex pi(&services, library.database().connectionName());
        bool built = false;
        pi.build(pdfId, {{0, 0, 60, QStringLiteral("Page one introduces Hadamard gates and quantum entanglement.")}},
                 nullptr, [&](const QString &err) {
                     QVERIFY2(err.isEmpty(), qPrintable(err));
                     built = true;
                 });
        QTRY_VERIFY_WITH_TIMEOUT(built, 5000);
    }
    const QJsonObject semPassages = callTool(17, QStringLiteral("search_passages"),
                                             {{QStringLiteral("query"), QStringLiteral("entanglement")},
                                              {QStringLiteral("book_id"), pdfId},
                                              {QStringLiteral("limit"), 1}})
                                        .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(semPassages.value(QStringLiteral("mode")).toString(), QStringLiteral("semantic"));
    QCOMPARE(semPassages.value(QStringLiteral("count")).toInt(), 1);

    // 7. get_book_outline (EPUB TOC/spine/page_map & PDF pages) + error for missing book.
    const QJsonObject epubOutline = callTool(18, QStringLiteral("get_book_outline"), {{QStringLiteral("book_id"), epubId}})
                                        .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(epubOutline.value(QStringLiteral("chapters")).toInt(), 2);
    QVERIFY(!epubOutline.value(QStringLiteral("toc")).toArray().isEmpty());
    QVERIFY(!epubOutline.value(QStringLiteral("spine")).toArray().isEmpty());
    QVERIFY(!epubOutline.value(QStringLiteral("page_map")).toArray().isEmpty());

    const QJsonObject pdfOutline = callTool(19, QStringLiteral("get_book_outline"), {{QStringLiteral("book_id"), pdfId}})
                                       .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(pdfOutline.value(QStringLiteral("pages")).toInt(), 2);
    QVERIFY(callTool(20, QStringLiteral("get_book_outline"), {{QStringLiteral("book_id"), 99999}}).value(QStringLiteral("isError")).toBool());

    // 8. read_passage (EPUB chapter, PDF multi-page span, offset pagination, citation header, out-of-range error).
    const QJsonObject epubRead = callTool(21, QStringLiteral("read_passage"),
                                          {{QStringLiteral("book_id"), epubId},
                                           {QStringLiteral("chapter"), 0},
                                           {QStringLiteral("offset"), 5},
                                           {QStringLiteral("max_chars"), 999999}});
    QVERIFY(!epubRead.value(QStringLiteral("isError")).toBool());
    const QJsonObject epubStruct = epubRead.value(QStringLiteral("structuredContent")).toObject();
    QVERIFY(epubStruct.value(QStringLiteral("citation")).toString().startsWith(QStringLiteral("[Citation:")));
    QCOMPARE(epubStruct.value(QStringLiteral("offset")).toInt(), 5);
    QVERIFY(epubStruct.value(QStringLiteral("text")).toString().contains(QStringLiteral("storm")));

    const QJsonObject pdfRead = callTool(22, QStringLiteral("read_passage"),
                                         {{QStringLiteral("book_id"), pdfId},
                                          {QStringLiteral("page"), 1},
                                          {QStringLiteral("page_end"), 2}});
    const QJsonObject pdfStruct = pdfRead.value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(pdfStruct.value(QStringLiteral("page")).toInt(), 1);
    QCOMPARE(pdfStruct.value(QStringLiteral("page_end")).toInt(), 2);
    QVERIFY(pdfStruct.value(QStringLiteral("text")).toString().contains(QStringLiteral("Hadamard")));
    QVERIFY(pdfStruct.value(QStringLiteral("text")).toString().contains(QStringLiteral("Bell state")));
    QVERIFY(callTool(23, QStringLiteral("read_passage"), {{QStringLiteral("book_id"), pdfId}, {QStringLiteral("page"), 50}})
                .value(QStringLiteral("isError")).toBool());

    // 9. list_marks (book-scoped & cross-book + query filter).
    const QJsonObject bookMarks = callTool(24, QStringLiteral("list_marks"),
                                           {{QStringLiteral("book_id"), epubId}, {QStringLiteral("query"), QStringLiteral("harbor")}})
                                      .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(bookMarks.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(bookMarks.value(QStringLiteral("marks")).toArray()[0].toObject().value(QStringLiteral("note")).toString(),
             QStringLiteral("Opening harbor motif"));
    const QJsonObject allMarks = callTool(25, QStringLiteral("list_marks")).value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(allMarks.value(QStringLiteral("count")).toInt(), 1);

    // 10. resources/{list,templates/list,read} + unknown URI -32002.
    const QJsonArray resList = rpc(26, QStringLiteral("resources/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("resources")).toArray();
    QVERIFY(resList.size() >= 5);  // library/books + 2 visible books * (outline + marks)
    const QJsonArray resTemplates = rpc(27, QStringLiteral("resources/templates/list")).value(QStringLiteral("result")).toObject().value(QStringLiteral("resourceTemplates")).toArray();
    QCOMPARE(resTemplates.size(), 2);

    for (const QString &uri : {QStringLiteral("clothbound://library/books"),
                               QStringLiteral("clothbound://book/%1/outline").arg(epubId),
                               QStringLiteral("clothbound://book/%1/marks").arg(epubId)}) {
        const QJsonObject readRes = rpc(28, QStringLiteral("resources/read"), {{QStringLiteral("uri"), uri}});
        const QJsonArray contents = readRes.value(QStringLiteral("result")).toObject().value(QStringLiteral("contents")).toArray();
        QCOMPARE(contents.size(), 1);
        QCOMPARE(contents[0].toObject().value(QStringLiteral("uri")).toString(), uri);
        QVERIFY(QJsonDocument::fromJson(contents[0].toObject().value(QStringLiteral("text")).toString().toUtf8()).isObject());
    }
    const QJsonObject badUri = rpc(29, QStringLiteral("resources/read"), {{QStringLiteral("uri"), QStringLiteral("clothbound://book/99999/outline")}});
    QCOMPARE(badUri.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32002);
}

void ClothboundTests::wave4CliSearchPassageAndMcpStdioPurity() {
    QVERIFY(isCliCommand(QStringLiteral("mcp")));
    QVERIFY(isCliCommand(QStringLiteral("search")));
    QVERIFY(isCliCommand(QStringLiteral("passage")));

    const QString clothboundBin = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../clothbound"));
    if (!QFileInfo::exists(clothboundBin))
        QSKIP("build/clothbound binary not built yet");

    QTemporaryDir dir;
    QDir(dir.path()).mkpath(QStringLiteral("config"));
    QDir(dir.path()).mkpath(QStringLiteral("data"));
    QDir(dir.path()).mkpath(QStringLiteral("cache"));
    const QString epubPath = dir.filePath(QStringLiteral("lantern.epub"));
    const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(72);
        writer.setTitle(QStringLiteral("Spectral Graph Theory"));
        QPainter p(&writer);
        p.drawText(QRect(20, 60, 550, 120), QStringLiteral("Laplacian eigenvalues bound graph expansion tightly."));
        writer.newPage();
        p.drawText(QRect(20, 60, 550, 120), QStringLiteral("Cheeger inequality relates conductance to spectral gap."));
        p.end();
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("DISPLAY"));
    env.remove(QStringLiteral("WAYLAND_DISPLAY"));
    env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), dir.filePath(QStringLiteral("config")));
    env.insert(QStringLiteral("XDG_DATA_HOME"), dir.filePath(QStringLiteral("data")));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), dir.filePath(QStringLiteral("cache")));

    auto runSub = [&](const QStringList &args, const QByteArray &stdinBytes = {}) {
        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(clothboundBin, args);
        if (!proc.waitForStarted(5000))
            return std::tuple<int, QByteArray, QByteArray>{-1, {}, QByteArray("failed to start")};
        if (!stdinBytes.isEmpty())
            proc.write(stdinBytes);
        proc.closeWriteChannel();
        if (!proc.waitForFinished(10000)) {
            proc.kill();
            proc.waitForFinished(2000);
            return std::tuple<int, QByteArray, QByteArray>{-2, proc.readAllStandardOutput(), proc.readAllStandardError()};
        }
        return std::tuple<int, QByteArray, QByteArray>{proc.exitCode(), proc.readAllStandardOutput(), proc.readAllStandardError()};
    };

    // 1. Add both books headlessly.
    auto [addCode, addOut, addErr] = runSub({QStringLiteral("add"), epubPath, pdfPath, QStringLiteral("--json")});
    QCOMPARE(addCode, 0);
    QVERIFY2(addErr.isEmpty(), addErr.constData());
    const QJsonObject addJson = QJsonDocument::fromJson(addOut).object();
    QCOMPARE(addJson.value(QStringLiteral("books")).toInt(), 2);

    // 2. clothbound search --json (whole library & --book scoped).
    auto [sCode, sOut, sErr] = runSub({QStringLiteral("search"), QStringLiteral("Cheeger"), QStringLiteral("--json")});
    QCOMPARE(sCode, 0);
    QVERIFY2(sErr.isEmpty(), sErr.constData());
    const QJsonObject sJson = QJsonDocument::fromJson(sOut).object();
    QCOMPARE(sJson.value(QStringLiteral("count")).toInt(), 1);
    const QJsonObject firstHit = sJson.value(QStringLiteral("results")).toArray()[0].toObject();
    const int pdfId = firstHit.value(QStringLiteral("book_id")).toInt();
    QVERIFY(pdfId > 0);
    QCOMPARE(firstHit.value(QStringLiteral("page")).toInt(), 2);

    // 3. clothbound passage --page / --page-end --json + invalid book/page exit 1.
    auto [pCode, pOut, pErr] = runSub({QStringLiteral("passage"), QString::number(pdfId),
                                       QStringLiteral("--page"), QStringLiteral("1"),
                                       QStringLiteral("--page-end"), QStringLiteral("2"),
                                       QStringLiteral("--json")});
    QCOMPARE(pCode, 0);
    QVERIFY2(pErr.isEmpty(), pErr.constData());
    const QJsonObject pJson = QJsonDocument::fromJson(pOut).object();
    QVERIFY(pJson.value(QStringLiteral("citation")).toString().startsWith(QStringLiteral("[Citation:")));
    QVERIFY(pJson.value(QStringLiteral("text")).toString().contains(QStringLiteral("Laplacian")));
    QVERIFY(pJson.value(QStringLiteral("text")).toString().contains(QStringLiteral("Cheeger")));

    auto [badCode, badOut, badErr] = runSub({QStringLiteral("passage"), QStringLiteral("99999"), QStringLiteral("--page"), QStringLiteral("1")});
    Q_UNUSED(badOut);
    QCOMPARE(badCode, 1);
    QVERIFY(!badErr.isEmpty());

    // 4. Live subprocess `clothbound mcp` stdio purity check: 0 stderr bytes, pure newline-delimited JSON-RPC stdout.
    const QByteArray mcpStdin =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_books\",\"arguments\":{}}}\n";
    auto [mCode, mOut, mErr] = runSub({QStringLiteral("mcp")}, mcpStdin);
    QCOMPARE(mCode, 0);
    QCOMPARE(mErr.size(), qsizetype(0));
    const QList<QByteArray> lines = mOut.split('\n');
    QList<QJsonObject> replies;
    for (const QByteArray &line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        QVERIFY2(err.error == QJsonParseError::NoError && doc.isObject(), line.constData());
        replies.append(doc.object());
    }
    QCOMPARE(replies.size(), 3);
    QCOMPARE(replies[0].value(QStringLiteral("id")).toInt(), 1);
    QCOMPARE(replies[0].value(QStringLiteral("result")).toObject().value(QStringLiteral("serverInfo")).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("clothbound"));
    QCOMPARE(replies[1].value(QStringLiteral("id")).toInt(), 2);
    QCOMPARE(replies[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray().size(), 6);
    QCOMPARE(replies[2].value(QStringLiteral("id")).toInt(), 3);
    QCOMPARE(replies[2].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 2);
}

void ClothboundTests::wave5SlimListenBarAndToastClearance() {
    QTemporaryDir dir;
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    BookScheme scheme;
    Reader reader(&library, &scheme);
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    AudioCache cache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
    VoiceCatalog voices(&services, &cache, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("voices.json")));
    Narration narration(&services, &cache, &reader, &library);
    narration.m_player->setHeadless(true);
    Theme theme;

    QQmlEngine engine;
    const QString srcRoot = QFINDTESTDATA("../src");
    QVERIFY(QFileInfo::exists(srcRoot));
    engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
    engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
    engine.rootContext()->setContextProperty(QStringLiteral("ai"), &services);
    engine.rootContext()->setContextProperty(QStringLiteral("voices"), &voices);
    engine.rootContext()->setContextProperty(QStringLiteral("narration"), &narration);

    // 1. Live ListenBar.qml instantiation: resting 44px capsule -> expanded 86px hover state.
    QQmlComponent barComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/audio/ListenBar.qml")));
    QVERIFY2(barComp.status() == QQmlComponent::Ready, qPrintable(barComp.errorString()));
    std::unique_ptr<QObject> barObj(barComp.createWithInitialProperties(
        {{QStringLiteral("availableWidth"), 1280.0},
         {QStringLiteral("look"), QVariantMap{{QStringLiteral("dark"), false}}}}));
    QVERIFY2(barObj != nullptr, qPrintable(barComp.errorString()));

    QCOMPARE(barObj->property("showBookInfo").toBool(), false);
    QCOMPARE(barObj->property("animateSize").toBool(), true);
    QCOMPARE(barObj->property("expanded").toBool(), false);
    QCOMPARE(qRound(barObj->property("height").toDouble()), 44);
    QCOMPARE(qRound(barObj->property("restingTargetWidth").toDouble()), 368);
    QCOMPARE(qRound(barObj->property("width").toDouble()), 368);

    // Unfollowing widens the resting capsule cleanly to make room for the Follow chip.
    narration.setFollowing(false);
    QCOMPARE(qRound(barObj->property("restingTargetWidth").toDouble()), 412);
    QCOMPARE(qRound(barObj->property("expandedTargetWidth").toDouble()), 556);
    narration.setFollowing(true);
    QCOMPARE(qRound(barObj->property("restingTargetWidth").toDouble()), 368);
    QCOMPARE(qRound(barObj->property("expandedTargetWidth").toDouble()), 544);

    // Expanding transitions to 86px height & 544px width, clamped cleanly by availableWidth.
    barObj->setProperty("hoverExpanded", true);
    QCOMPARE(barObj->property("expanded").toBool(), true);
    QCOMPARE(qRound(barObj->property("expandedTargetHeight").toDouble()), 86);
    QTRY_COMPARE_WITH_TIMEOUT(qRound(barObj->property("height").toDouble()), 86, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(qRound(barObj->property("width").toDouble()), 544, 1000);

    barObj->setProperty("availableWidth", 460.0);
    QTRY_VERIFY_WITH_TIMEOUT(barObj->property("width").toDouble() <= 436.5, 1000);
    barObj->setProperty("hoverExpanded", false);
    QTRY_COMPARE_WITH_TIMEOUT(qRound(barObj->property("height").toDouble()), 44, 1000);

    // 2. Dynamic Toast.bottomClearance above ListenBar while narration.active matches Main.qml.
    const QString harnessQml = QStringLiteral(R"QML(
import QtQuick
import "ui/audio"
import "ui/components"
Item {
    width: 1280; height: 800
    property bool readerSurfaceVisible: true
    property bool narrationActive: false
    property alias bar: bar
    property alias toast: toast
    ListenBar {
        id: bar
        availableWidth: parent.width
        look: ({ dark: false })
    }
    Toast {
        id: toast
        bottomClearance: !parent.readerSurfaceVisible ? 28
                       : parent.narrationActive
                         ? Math.round(bar.height) + 28
                         : 28
    }
}
)QML");
    QQmlComponent harnessComp(&engine);
    harnessComp.setData(harnessQml.toUtf8(), QUrl::fromLocalFile(srcRoot + QStringLiteral("/Harness.qml")));
    QVERIFY2(harnessComp.status() == QQmlComponent::Ready, qPrintable(harnessComp.errorString()));
    std::unique_ptr<QObject> harnessObj(harnessComp.create());
    QVERIFY2(harnessObj != nullptr, qPrintable(harnessComp.errorString()));
    QObject *hBar = harnessObj->property("bar").value<QObject *>();
    QObject *hToast = harnessObj->property("toast").value<QObject *>();
    QVERIFY(hBar && hToast);

    QCOMPARE(qRound(hToast->property("bottomClearance").toDouble()), 28);
    harnessObj->setProperty("narrationActive", true);
    QCOMPARE(qRound(hToast->property("bottomClearance").toDouble()), 72);  // 44 + 28
    hBar->setProperty("hoverExpanded", true);
    QTRY_COMPARE_WITH_TIMEOUT(qRound(hToast->property("bottomClearance").toDouble()), 114, 1000);  // 86 + 28
    harnessObj->setProperty("readerSurfaceVisible", false);
    QCOMPARE(qRound(hToast->property("bottomClearance").toDouble()), 28);

    // 3. Main.qml & EpubReader.qml retain exact dynamic Toast & bottom clearance wiring.
    QFile mainFile(srcRoot + QStringLiteral("/ui/Main.qml"));
    QVERIFY(mainFile.open(QIODevice::ReadOnly));
    const QString mainQml = QString::fromUtf8(mainFile.readAll());
    QVERIFY(mainQml.contains(QStringLiteral("readerLoader.item.listenBar.height")));
    QVERIFY(mainQml.contains(QStringLiteral("bottomClearance:")));

    QFile readerFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
    QVERIFY(readerFile.open(QIODevice::ReadOnly));
    const QString readerQml = QString::fromUtf8(readerFile.readAll());
    QVERIFY(readerQml.contains(QStringLiteral("animateSize: !view.companionResizing")));
    QVERIFY(readerQml.contains(QStringLiteral("showBookInfo: false")));
}

void ClothboundTests::fixAiMcpServerLockdownAndFraming() {
    QTemporaryDir dir;
    const QByteArray origEnvKey = qgetenv("GEMINI_API_KEY");
    const QString appSettings =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    auto restoreEnv = qScopeGuard([&] {
        if (origEnvKey.isEmpty())
            qunsetenv("GEMINI_API_KEY");
        else
            qputenv("GEMINI_API_KEY", origEnvKey);
        QSettings(appSettings, QSettings::IniFormat).remove(QStringLiteral("ai"));
    });

    // Build two EPUBs (one with hostile %3/%4 placeholders in its title).
    auto makeBook = [&](const QString &fileName, const QString &title, const QString &bodyText) {
        const QString path = dir.filePath(fileName);
        const QByteArray opf =
            QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                "    <dc:title>%1</dc:title><dc:creator>Author Name</dc:creator><dc:language>en</dc:language>\n"
                "  </metadata>\n"
                "  <manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
                "  <spine><itemref idref=\"c1\"/></spine>\n"
                "</package>")
                .arg(title)
                .toUtf8();
        const bool ok = TestBooks::write(
            path, TestBooks::epub({{"OEBPS/content.opf", opf},
                                   {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>" + bodyText.toUtf8() + "</p>")}}));
        return ok ? path : QString();
    };

    const QString book1Path = makeBook(QStringLiteral("b1.epub"), QStringLiteral("Report %3 %4 Title"),
                                       QStringLiteral("Quantum decoherence limits qubit coherence time."));
    const QString book2Path = makeBook(QStringLiteral("b2.epub"), QStringLiteral("Second Book"),
                                       QStringLiteral("Stellar nucleosynthesis forges heavy elements."));
    QVERIFY(!book1Path.isEmpty() && !book2Path.isEmpty());

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int b1 = library.addBookFile(book1Path);
    const int b2 = library.addBookFile(book2Path);
    QVERIFY(b1 > 0 && b2 > 0);

    const QString settingsIni = dir.filePath(QStringLiteral("mcp.ini"));
    const QString indexDb = dir.filePath(QStringLiteral("index.sqlite"));
    LibraryIndex libIndex(indexDb);
    QVERIFY(libIndex.available());

    // 1. resources/list is gated by isEnabled() (blocks with -32001 when disabled).
    {
        QSettings s(settingsIni, QSettings::IniFormat);
        s.setValue(QStringLiteral("ai/mcpEnabled"), false);
        s.sync();
    }
    McpServer gatedServer(&library, &libIndex, nullptr, {settingsIni, indexDb});
    const QJsonObject resListBlocked = gatedServer.handleRequest({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QStringLiteral("resources/list")},
    });
    QCOMPARE(resListBlocked.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32001);
    {
        QSettings s(settingsIni, QSettings::IniFormat);
        s.setValue(QStringLiteral("ai/mcpEnabled"), true);
        s.sync();
    }

    // 2. Read-only guarantee: search_library finds unindexed books via lexical fallback
    //    WITHOUT writing any rows to index.sqlite.
    const QJsonObject searchRes = gatedServer.handleRequest({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 2},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("search_library")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("query"), QStringLiteral("decoherence")}}},
        }},
    }).value(QStringLiteral("result")).toObject();
    QCOMPARE(searchRes.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(libIndex.bookCount(), 0);  // ZERO writes to index.sqlite!

    // 3. Chained .arg() immunity + untrusted-data boundary markers in read_passage.
    const QJsonObject readRes = gatedServer.handleRequest({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 3},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("read_passage")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("book_id"), b1}, {QStringLiteral("chapter"), 0}}},
        }},
    }).value(QStringLiteral("result")).toObject();
    const QString citation = readRes.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("citation")).toString();
    QVERIFY2(citation.contains(QStringLiteral("Report %3 %4 Title")), qPrintable(citation));
    const QString displayText = readRes.value(QStringLiteral("content")).toArray()[0].toObject().value(QStringLiteral("text")).toString();
    QVERIFY(displayText.contains(QStringLiteral("<<<UNTRUSTED_BOOK_CONTENT>>>")));
    QVERIFY(displayText.contains(QStringLiteral("<<<END_UNTRUSTED_BOOK_CONTENT>>>")));

    // 4. Local-only guarantee: even with GEMINI_API_KEY set in env and passage_vectors present,
    //    MCP search_passages NEVER sends queries to Gemini cloud or writes ai_usage.
    qputenv("GEMINI_API_KEY", "test-env-cloud-key");
    FakeServer cloudSrv;
    GeminiProvider gemini(nullptr, cloudSrv.endpoint());
    MemoryKeyStore keys;
    AiServices cloudAi(&gemini, &keys, library.database().connectionName());
    cloudAi.setPreset(QStringLiteral("cloud"));
    {
        QSqlQuery q(library.database());
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
            "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))")));
        const QList<float> v{1.0f, 0.0f, 0.0f};
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors VALUES (?, ?, ?, ?)"));
        q.addBindValue(b1);
        q.addBindValue(QStringLiteral("gemini/gemini-embedding-2"));
        q.addBindValue(QStringLiteral("0:0:45:aa"));
        q.addBindValue(QByteArray(reinterpret_cast<const char *>(v.constData()), v.size() * qsizetype(sizeof(float))));
        QVERIFY(q.exec());
    }
    McpServer cloudGuardServer(&library, &libIndex, &cloudAi, {settingsIni, indexDb});
    const QJsonObject noCloudRes = cloudGuardServer.handleRequest({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 4},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("search_passages")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("query"), QStringLiteral("decoherence")}}},
        }},
    }).value(QStringLiteral("result")).toObject();
    QCOMPARE(cloudSrv.requests.size(), 0);
    QCOMPARE(noCloudRes.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("mode")).toString(),
             QStringLiteral("lexical"));
    QCOMPARE(cloudAi.spentThisMonth(), 0.0);

    // 5. allowedBooks is passed directly to PassageIndex::searchLibrary() in toolSearchPassages().
    FakeServer localEmbedSrv;
    localEmbedSrv.routes.insert("/api/embed", {200, "application/json", "{\"embeddings\":[[1.0,0.0,0.0]]}"});
    LocalModels localEmbed;
    LocalModels::Server lcfg;
    lcfg.kind = LocalModels::Ollama;
    lcfg.url = QUrl(localEmbedSrv.origin());
    localEmbed.configure(lcfg, {}, QStringLiteral("nomic-embed-text"));
    {
        // Seed 15 decoy chapters in b1 with score 1.0 and 1 chapter in b2 with score 0.6.
        QSqlQuery q(library.database());
        const QList<float> highV{1.0f, 0.0f, 0.0f};
        const QList<float> midV{0.6f, 0.8f, 0.0f};
        for (int ch = 0; ch < 15; ++ch) {
            q.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors VALUES (?, ?, ?, ?)"));
            q.addBindValue(b1);
            q.addBindValue(QStringLiteral("local/nomic-embed-text"));
            q.addBindValue(QStringLiteral("%1:0:40:aa").arg(ch));
            q.addBindValue(QByteArray(reinterpret_cast<const char *>(highV.constData()), highV.size() * qsizetype(sizeof(float))));
            QVERIFY(q.exec());
        }
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO passage_vectors VALUES (?, ?, ?, ?)"));
        q.addBindValue(b2);
        q.addBindValue(QStringLiteral("local/nomic-embed-text"));
        q.addBindValue(QStringLiteral("0:0:45:bb"));
        q.addBindValue(QByteArray(reinterpret_cast<const char *>(midV.constData()), midV.size() * qsizetype(sizeof(float))));
        QVERIFY(q.exec());
    }
    McpServer localSemServer(&library, &libIndex, nullptr, {settingsIni, indexDb});
    localSemServer.setLocalEmbedder(&localEmbed);
    const QJsonObject scopedSem = localSemServer.handleRequest({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 5},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("search_passages")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("query"), QStringLiteral("elements")},
                {QStringLiteral("book_id"), b2},
                {QStringLiteral("limit"), 1},
            }},
        }},
    }).value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(scopedSem.value(QStringLiteral("mode")).toString(), QStringLiteral("semantic"));
    QCOMPARE(scopedSem.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(scopedSem.value(QStringLiteral("passages")).toArray()[0].toObject().value(QStringLiteral("book_id")).toInt(), b2);

    // 6. Bounded fread() framing: rejects embedded NUL bytes and oversized frames without corrupting subsequent frames.
    QByteArray frames;
    frames += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
    frames += QByteArray("{\"jsonrpc\":\"2.0\",") + '\0' + QByteArray("\"id\":2,\"method\":\"ping\"}\n");
    frames += QByteArray(300, 'x') + '\n';  // exceeds test cap of 128 bytes
    frames += "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}\n";

    std::FILE *inFile = std::tmpfile();
    std::FILE *outFile = std::tmpfile();
    QVERIFY(inFile && outFile);
    QCOMPARE(std::fwrite(frames.constData(), 1, size_t(frames.size()), inFile), size_t(frames.size()));
    std::rewind(inFile);

    McpServer::runStdioLoop(
        inFile, outFile,
        [&gatedServer](const QJsonObject &req) { return gatedServer.handleRequest(req); },
        128);

    std::rewind(outFile);
    QByteArray outBytes;
    char buf[1024];
    while (const size_t n = std::fread(buf, 1, sizeof(buf), outFile))
        outBytes.append(buf, qsizetype(n));
    std::fclose(inFile);
    std::fclose(outFile);

    QList<QJsonObject> replies;
    for (const QByteArray &line : outBytes.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(line);
        QVERIFY2(d.isObject(), line.constData());
        replies.append(d.object());
    }
    QCOMPARE(replies.size(), 4);
    QCOMPARE(replies[0].value(QStringLiteral("id")).toInt(), 1);
    QVERIFY(replies[0].contains(QStringLiteral("result")));
    QCOMPARE(replies[1].value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32700);
    QCOMPARE(replies[2].value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32600);
    QCOMPARE(replies[3].value(QStringLiteral("id")).toInt(), 4);
    QVERIFY(replies[3].contains(QStringLiteral("result")));
}

void ClothboundTests::aPlusCoreParserFuzzStress() {
    QElapsedTimer timer;
    timer.start();
    std::mt19937_64 rng(0xC107B00DuLL);

    auto mutateBytes = [&rng](QByteArray data) {
        const int ops = 1 + int(rng() % 6);
        for (int op = 0; op < ops; ++op) {
            const int kind = int(rng() % 5);
            if (kind == 0 && !data.isEmpty()) {
                // Flip random byte
                const qsizetype pos = qsizetype(rng() % quint64(data.size()));
                data[pos] = char(rng() & 0xFF);
            } else if (kind == 1 && !data.isEmpty()) {
                // Truncate
                data.truncate(qsizetype(rng() % quint64(data.size())));
            } else if (kind == 2 && data.size() < 4096) {
                // Splice random adversarial tokens
                static const QByteArray tokens[] = {
                    "<", ">", "</", "<!--", "-->", "<![CDATA[", "]]>", "<!DOCTYPE [", "<?xml ", "&amp;",
                    "&#x", "&#99999999;", "\0", "\xFF\xFE", "\xEF\xBB\xBF", "\"", "'", "{", "}", "[", "]",
                    "PK\x03\x04", "PK\x01\x02", "PK\x05\x06", "PK\x06\x06", "PK\x06\x07", "javascript:alert(1)"};
                const QByteArray &tok = tokens[rng() % (sizeof(tokens) / sizeof(tokens[0]))];
                const qsizetype pos = data.isEmpty() ? 0 : qsizetype(rng() % quint64(data.size() + 1));
                data.insert(pos, tok);
            } else if (kind == 3 && data.size() > 4) {
                // Delete a slice
                const qsizetype pos = qsizetype(rng() % quint64(data.size()));
                const qsizetype len = qsizetype(1 + rng() % quint64(qMin<qsizetype>(64, data.size() - pos)));
                data.remove(pos, len);
            } else if (data.size() < 2048) {
                // Append random bytes
                const int n = 1 + int(rng() % 32);
                for (int k = 0; k < n; ++k)
                    data.append(char(rng() & 0xFF));
            }
        }
        return data;
    };

    int totalCases = 0;

    // 1. HtmlTokenizer & Html::parse (350 adversarial + mutated cases)
    const QList<QByteArray> htmlSeeds{
        "<!DOCTYPE html [<!ENTITY x 'y'>]><html xmlns='http://www.w3.org/1999/xhtml'><body><p>Hello &amp; &mdash; &#8217;</p></body></html>",
        "<?xml version='1.0' encoding='ISO-8859-1'?><script>var a = '</scriptx>'; </script><style>body{}</style><p>caf\xE9</p>",
        "\xFF\xFE<\0p\0>\0h\0i\0<\0/\0p\0>",
        "<![CDATA[raw <tag> &amp;]]><!-- comment --!><div a='1' b=2 c/>a < b && c > d",
        QByteArray(600, '<') + "div>" + QByteArray(600, '&'),
        QByteArray("<div>").repeated(600) + "deep" + QByteArray("</div>").repeated(600),
    };
    for (int i = 0; i < 350; ++i) {
        const QByteArray input = (i < htmlSeeds.size()) ? htmlSeeds[i] : mutateBytes(htmlSeeds[rng() % htmlSeeds.size()]);
        const QString decoded = HtmlTokenizer::decodeDocument(input);
        (void)HtmlTokenizer::decodeEntities(decoded);
        auto tree = Html::parse(decoded, (i & 1) != 0);
        QVERIFY(tree != nullptr);
        (void)tree->textContent();
        (void)tree->elements(QStringLiteral("p"));
        ++totalCases;
    }

    // 2. ZipArchive (250 adversarial + mutated ZIP/Zip64 cases)
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray validZip = TestBooks::epub2(png(8, 8));
    const QList<QByteArray> zipSeeds{
        validZip,
        QByteArray(),
        QByteArray("PK\x05\x06") + QByteArray(18, '\0'),
        QByteArray("PK\x06\x07") + QByteArray(16, '\xFF') + QByteArray("PK\x05\x06") + QByteArray(18, '\xFF'),
        QByteArray("PK\x03\x04") + QByteArray(40, '\xFF') + QByteArray("PK\x01\x02") + QByteArray(46, '\xFF') +
            QByteArray("PK\x05\x06") + QByteArray(18, '\0'),
    };
    const QString zipPath = dir.filePath(QStringLiteral("fuzz.zip"));
    for (int i = 0; i < 250; ++i) {
        const QByteArray input = (i < zipSeeds.size()) ? zipSeeds[i] : mutateBytes(zipSeeds[rng() % zipSeeds.size()]);
        QFile f(zipPath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(input);
        f.close();

        ZipArchive zip(zipPath);
        zip.setMaxArchiveInflateBytes(256ull * 1024);
        if (zip.open()) {
            const QStringList names = zip.names();
            const int checkCount = qMin(int(names.size()), 4);
            for (int n = 0; n < checkCount; ++n) {
                (void)zip.size(names[n]);
                (void)zip.encrypted(names[n]);
                QString err;
                (void)zip.read(names[n], &err);
            }
        }
        ++totalCases;
    }

    // 3. OpdsParser (350 adversarial + mutated Atom / OPDS2 / OpenSearch cases)
    const QUrl opdsBase(QStringLiteral("https://catalog.example/opds/root.xml"));
    const QList<QByteArray> opdsSeeds{
        R"raw(<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><id>urn:1</id><title>Feed</title>
           <link rel="search" type="application/opensearchdescription+xml" href="/search.xml"/>
           <entry><id>e1</id><title>Book</title><author><name>Ada</name></author>
           <link rel="http://opds-spec.org/acquisition/open-access" type="application/epub+zip" href="/b.epub"/>
           <link rel="http://opds-spec.org/image" href="javascript:alert(1)"/></entry></feed>)raw",
        R"raw({"metadata":{"title":"Catalog"},"links":[{"rel":"self","href":"/opds2.json"},{"rel":"search","href":"/s?q={searchTerms}","templated":true}],
            "publications":[{"metadata":{"title":"Work","author":["Ada",{"name":"Bob"}]},"links":[{"rel":"http://opds-spec.org/acquisition","href":"/x.epub","type":"application/epub+zip"}],"images":[{"href":"/c.jpg"}]}]})raw",
        R"raw(<?xml version="1.0"?><OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
           <Url type="application/atom+xml" template="https://catalog.example/search?q={searchTerms}&amp;p={startPage?}"/></OpenSearchDescription>)raw",
        R"raw({"groups":[{"metadata":{"title":"G"},"publications":[null,123,{"metadata":[]}]}]})raw",
    };
    for (int i = 0; i < 350; ++i) {
        const QByteArray input = (i < opdsSeeds.size()) ? opdsSeeds[i] : mutateBytes(opdsSeeds[rng() % opdsSeeds.size()]);
        (void)OpdsParser::detectFormat(input);
        const OpdsFeed feed = OpdsParser::parse(input, opdsBase);
        (void)feed.toVariantMap();
        const QString tmpl = OpdsParser::parseOpenSearchTemplate(input, opdsBase);
        if (!tmpl.isEmpty())
            (void)OpdsParser::expandSearchTemplate(tmpl, QStringLiteral("test query & <tag>"));
        if ((i & 3) == 0)
            (void)OpdsParser::stripHtmlToPlainText(QString::fromUtf8(input));
        ++totalCases;
    }

    // 4. ArticleExtractor (300 adversarial + mutated HTML article cases)
    const QUrl pageUrl(QStringLiteral("https://example.org/articles/lantern"));
    const QList<QByteArray> articleSeeds{
        R"raw(<!doctype html><html lang="en"><head><title>Lantern Essay</title>
           <meta property="og:title" content="The Lantern"/><meta name="author" content="Ada Marsh"/>
           <script type="application/ld+json">{"@type":"Article","headline":"The Lantern","author":{"name":"Ada"}}</script>
           </head><body><nav>Menu</nav><article><h1>The Lantern</h1>
           <p>A long enough paragraph about optics, coastal fog, and quiet craftsmanship along the northern pier so scoring selects this article cleanly.</p>
           <img src="/img/lamp.jpg" alt="Lamp"/><iframe src="https://evil.test"></iframe><script>alert(1)</script>
           <h2>Second Section</h2><p>More prose with &mdash; entities and <a href="javascript:void(0)">bad link</a> and <a href="/ok">good link</a>.</p>
           </article></body></html>)raw",
        R"raw(<html><body><table><tr><td><div><p>Nested content</p><svg><script>x</script></svg><style>p{color:red}</style></div></td></tr></table></body></html>)raw",
        QByteArray("<div><p>").repeated(200) + "deep article" + QByteArray("</p></div>").repeated(200),
    };
    for (int i = 0; i < 300; ++i) {
        const QByteArray input = (i < articleSeeds.size()) ? articleSeeds[i] : mutateBytes(articleSeeds[rng() % articleSeeds.size()]);
        const ExtractedArticle art = ArticleExtractor::extract(input, pageUrl);
        if (!art.bodyXhtml.isEmpty()) {
            QHash<QString, QString> map;
            for (const ExtractedImage &img : art.images)
                map.insert(img.localPath, img.localPath);
            (void)ArticleExtractor::finalizeImages(art.bodyXhtml, map);
        }
        ++totalCases;
    }

    QVERIFY(totalCases >= 1250);
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QStringLiteral("Parser fuzz took %1 ms").arg(timer.elapsed())));
}

void ClothboundTests::finalAuditPolishRegressions() {
    // 1. MathFormatter CommonMark tilde fences (~~~) & backslash-escaped backticks (\`)
    //    must never desync raw HTML escaping or math extraction.
    const QString tildePayload = QStringLiteral("~~~\n```\n~~~\nhello <a href=\"http://evil.invalid\">LINK</a>");
    const QString tildeClean = MathFormatter::cleanMarkdown(tildePayload);
    QVERIFY2(!tildeClean.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(tildeClean));
    QVERIFY2(tildeClean.contains(QStringLiteral("&lt;a href=")), qPrintable(tildeClean));
    const QString tildeHtml = MathFormatter::formatDisplayHtml(tildePayload, QStringLiteral("rgba(0,0,0,0.1)"));
    QVERIFY2(!tildeHtml.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(tildeHtml));

    const QString escapedTickPayload = QStringLiteral("\\`<a href=\"http://evil.invalid\">LINK</a>`");
    const QString escapedTickClean = MathFormatter::cleanMarkdown(escapedTickPayload);
    QVERIFY2(!escapedTickClean.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(escapedTickClean));
    QVERIFY2(escapedTickClean.contains(QStringLiteral("&lt;a href=")), qPrintable(escapedTickClean));

    const QString escapedBackslashCode = QStringLiteral("\\\\`<a href=\"http://evil.invalid\">CODE</a>` <a href=\"http://evil.invalid\">OUT</a>");
    const QString escapedBsClean = MathFormatter::cleanMarkdown(escapedBackslashCode);
    QVERIFY2(escapedBsClean.contains(QStringLiteral("&lt;a href=\"http://evil.invalid\"&gt;OUT")), qPrintable(escapedBsClean));
    const QString escapedBsHtml = MathFormatter::formatDisplayHtml(escapedBackslashCode, QStringLiteral("rgba(0,0,0,0.1)"));
    QVERIFY2(!escapedBsHtml.contains(QStringLiteral("<a "), Qt::CaseInsensitive), qPrintable(escapedBsHtml));

    const QString tildeMath = QStringLiteral("~~~latex\n$x^2$\n~~~\nOutside $y^2$.");
    const QString tildeMathOut = MathFormatter::formatLatexInMarkdown(tildeMath);
    QVERIFY(tildeMathOut.contains(QStringLiteral("~~~latex\n$x^2$\n~~~")));
    QVERIFY(tildeMathOut.contains(QStringLiteral("class=\"math-inline\"")));

    // 2. McpServer::readPassageFromIndexReadOnly uses chapter_rows B-tree JOIN first
    //    and falls back cleanly to chapter_text when chapter_rows is absent.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("mcp-rows.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub({
        {"OEBPS/content.opf",
         "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
         "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>MCP Rows</dc:title></metadata>"
         "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
         "<spine><itemref idref=\"c1\"/></spine></package>"},
        {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Original fallback passage text for verification.</p>")},
    })));
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(epubPath);
    QVERIFY(bookId > 0);
    const QString indexDb = dir.filePath(QStringLiteral("index.sqlite"));
    const QString settingsIni = dir.filePath(QStringLiteral("mcp.ini"));
    LibraryIndex idx(indexDb);
    QVERIFY(idx.available());
    QVERIFY(LibraryIndex::indexBook(indexDb, {bookId, epubPath, QStringLiteral("epub"), 1}));

    // Point chapter_rows(bookId, 0) to a distinct FTS rowid whose chapter_text.chapter is 99
    // to prove the fast chapter_rows JOIN runs before the unindexed chapter_text scan.
    const QString conn = QStringLiteral("final-audit-mcp-rows");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(indexDb);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO chapter_text (book, chapter, body) VALUES (%1, 99, 'Fast chapter_rows indexed passage hit.')")
                           .arg(bookId)));
        const qint64 fastRowId = q.lastInsertId().toLongLong();
        QVERIFY(fastRowId > 0);
        QVERIFY(q.exec(QStringLiteral("UPDATE chapter_rows SET fts_rowid = %1 WHERE book = %2 AND chapter = 0")
                           .arg(fastRowId)
                           .arg(bookId)));
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);

    McpServer server(&library, nullptr, nullptr, {settingsIni, indexDb});
    const QString fastHit = server.extractPassageText(bookId, 0, 0, 80);
    QCOMPARE(fastHit, QStringLiteral("Fast chapter_rows indexed passage hit."));

    // Drop chapter_rows (simulating pre-migration index.sqlite) and verify clean fallback.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(indexDb);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("DROP TABLE chapter_rows")));
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);

    const QString fallbackHit = server.extractPassageText(bookId, 0, 0, 80);
    QVERIFY2(fallbackHit.contains(QStringLiteral("Original fallback passage text")), qPrintable(fallbackHit));

    // 3. Reader::openPdf rapid open+close stress exercising MainThreadRelease{std::move(book)}.
    const QString pdfPath = dir.filePath(QStringLiteral("rapid.pdf"));
    QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Rapid Close", "Ada Marsh")));
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(pdfId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    for (int iter = 0; iter < 8; ++iter) {
        QVERIFY(reader.openBook(pdfId));
        reader.close();
        QCoreApplication::processEvents();
    }
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::processEvents();
}

void ClothboundTests::w5AiMcpDagCyclesAndAudioPcm() {
    // 1. Strict DAG check: ZERO #include "audio/" or #include "app/" anywhere in src/ai/,
    //    and ZERO audio/ai/app/QtMultimedia includes in pure leaf src/formats/audiopcm.{h,cpp}.
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());
    QDirIterator aiIt(srcRoot + QStringLiteral("/ai"), {QStringLiteral("*.h"), QStringLiteral("*.cpp")},
                      QDir::Files, QDirIterator::Subdirectories);
    int aiFilesChecked = 0;
    while (aiIt.hasNext()) {
        const QString path = aiIt.next();
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(f.readAll());
        QVERIFY2(!content.contains(QStringLiteral("#include \"audio/")), qPrintable(path));
        QVERIFY2(!content.contains(QStringLiteral("#include \"app/")), qPrintable(path));
        ++aiFilesChecked;
    }
    QVERIFY(aiFilesChecked >= 18);

    for (const QString &rel : {QStringLiteral("formats/audiopcm.h"), QStringLiteral("formats/audiopcm.cpp")}) {
        QFile f(srcRoot + QLatin1Char('/') + rel);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(f.readAll());
        QVERIFY(!content.contains(QStringLiteral("#include \"audio/")));
        QVERIFY(!content.contains(QStringLiteral("#include \"ai/")));
        QVERIFY(!content.contains(QStringLiteral("#include \"app/")));
        QVERIFY(!content.contains(QStringLiteral("#include <QtMultimedia")));
        QVERIFY(!content.contains(QStringLiteral("#include <QAudio")));
    }

    // 2. AudioPcm WAV encoding/validation/stereo downmix + Blackman-sinc anti-aliased resampling.
    QByteArray stereoPcm;
    for (int i = 0; i < 64; ++i) {
        const qint16 left = 12000;
        const qint16 right = 4000;
        stereoPcm.append(char(left & 0xff)).append(char((left >> 8) & 0xff));
        stereoPcm.append(char(right & 0xff)).append(char((right >> 8) & 0xff));
    }
    const QByteArray stereoWav = AudioPcm::wavFromPcm16(stereoPcm, 48000, 2);
    int parsedRate = 0;
    const QByteArray mono = AudioPcm::pcmFromWav(stereoWav, &parsedRate, true);
    QCOMPARE(parsedRate, 48000);
    QCOMPARE(mono.size(), qsizetype(64 * 2));
    const QVector<float> monoFloats = AudioPcm::pcm16ToFloat(mono);
    QCOMPARE(monoFloats.size(), qsizetype(64));
    QVERIFY(std::abs(monoFloats[10] - 8000.0f / 32768.0f) < 1e-4f);
    QVERIFY(AudioPcm::pcmFromWav("<html>502</html>", nullptr, true).isEmpty());
    QVERIFY(AudioPcm::pcmFromWav("raw-unwrapped", nullptr, true).isEmpty());

    constexpr double kTwoPi = 6.28318530717958647692;
    const int n48 = 4800;
    QVector<float> pass48(n48), stop48(n48), dc48(n48, 0.5f);
    for (int i = 0; i < n48; ++i) {
        pass48[i] = float(std::sin(kTwoPi * 1000.0 * i / 48000.0));
        stop48[i] = float(std::sin(kTwoPi * 18000.0 * i / 48000.0));
    }
    auto rms = [](const QVector<float> &v) {
        double sum = 0.0;
        const qsizetype margin = qMin<qsizetype>(64, v.size() / 4);
        for (qsizetype i = margin; i < v.size() - margin; ++i)
            sum += double(v[i]) * double(v[i]);
        return std::sqrt(sum / double(qMax<qsizetype>(1, v.size() - 2 * margin)));
    };
    const QVector<float> pass24 = AudioPcm::resample(pass48, 48000, 24000);
    const QVector<float> stop24 = AudioPcm::resample(stop48, 48000, 24000);
    const QVector<float> dc24 = AudioPcm::resample(dc48, 48000, 24000);
    QCOMPARE(pass24.size(), qsizetype(2400));
    QVERIFY(rms(pass24) > 0.65);
    QVERIFY2(rms(stop24) < 0.005, qPrintable(QString::number(rms(stop24))));
    QVERIFY(std::abs(dc24[1200] - 0.5f) < 1e-4f);

    const QByteArray resampledPcm = AudioPcm::resamplePcm16(AudioPcm::floatToPcm16(pass48), 48000, 24000);
    QCOMPARE(resampledPcm.size(), qsizetype(2400 * 2));

    // 3. LocalHardwareProbe decouples LocalSetup from SystemCheck while preserving implicit upcast.
    class FakeProbe : public LocalHardwareProbe {
    public:
        LocalHardwareInfo info;
        LocalHardwareInfo probeHardware() const override { return info; }
    };
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices ai(&gemini, &keys, library.database().connectionName());
    ai.setPreset(QStringLiteral("local"));

    // Verify SystemCheck* implicitly converts to LocalHardwareProbe* and Machine converts to LocalHardwareInfo.
    SystemCheck sysCheck(&ai, nullptr);
    LocalHardwareProbe *upcastProbe = &sysCheck;
    QVERIFY(upcastProbe != nullptr);
    SystemCheck::Machine appleMachine;
    appleMachine.memoryGb = 24.0;
    appleMachine.graphics = QStringLiteral("Apple");
    QCOMPARE(LocalSetup::modelsFor(appleMachine).first, QStringLiteral("qwen3.5:9b"));

    FakeProbe fakeProbe;
    fakeProbe.info = {32.0, QStringLiteral("NVIDIA RTX 4080"), 16.0, true};
    LocalSetup setup(&ai, &fakeProbe);
    setup.refresh();
    QCOMPARE(setup.textModel().value(QStringLiteral("model")).toString(), QStringLiteral("qwen3.5:9b"));
    QCOMPARE(setup.searchModel().value(QStringLiteral("model")).toString(), QStringLiteral("qwen3-embedding:0.6b"));

    fakeProbe.info = {4.0, {}, 0.0, false};
    setup.setHardwareProbe(&fakeProbe);
    setup.refresh();
    QVERIFY(setup.textModel().isEmpty());
}

void ClothboundTests::w5AiMcpDagReadOnlyMcpAndSimdPassageIndex() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString xdgRoot = dir.filePath(QStringLiteral("ro-xdg"));
    const QString dataDir = xdgRoot + QStringLiteral("/data/clothbound");
    const QString cacheDir = xdgRoot + QStringLiteral("/cache/clothbound");
    const QString configDir = xdgRoot + QStringLiteral("/config/clothbound");
    QVERIFY(QDir().mkpath(dataDir));
    QVERIFY(QDir().mkpath(cacheDir));
    QVERIFY(QDir().mkpath(configDir));

    const QString epubPath = dir.filePath(QStringLiteral("ro-book.epub"));
    QVERIFY(TestBooks::write(
        epubPath,
        TestBooks::epub({
            {"OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
             "<dc:title>Read-Only Atlas</dc:title><dc:creator>Nora Bell</dc:creator>"
             "</metadata>"
             "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
             "<spine><itemref idref=\"c1\"/></spine></package>"},
            {"OEBPS/c1.xhtml",
             TestBooks::chapter("Chapter One", "<p>Astrolabes calibrated nocturnal navigation across calm straits.</p>")},
        })));

    int bookId = 0;
    const QString libraryDb = dataDir + QStringLiteral("/library.sqlite");
    const QString indexDb = cacheDir + QStringLiteral("/index.sqlite");
    {
        Library lib(dataDir, cacheDir);
        QVERIFY(lib.open());
        bookId = lib.addBookFile(epubPath);
        QVERIFY(bookId > 0);
        const int colId = lib.createCollection(QStringLiteral("Maritime"));
        QVERIFY(colId > 0);
        QVERIFY(lib.addToCollection(bookId, colId));

        Marks marks(&lib);
        marks.setBook(bookId);
        const int mid = marks.addHighlight(0, 0, 22, QStringLiteral("Astrolabes calibrated"), QStringLiteral("yellow"),
                                           QStringLiteral("Chapter One"));
        QVERIFY(mid > 0);
        marks.setNote(mid, QStringLiteral("Key navigation instrument"));

        // Seed both a pre-normalized vector (via PassageIndex::build) AND a legacy unnormalized BLOB.
        FakeServer localSrv;
        localSrv.routes.insert("/api/embed", {200, "application/json", "{\"embeddings\":[[3.0,4.0,0.0]]}"});
        GeminiProvider gem;
        MemoryKeyStore ks;
        AiServices ai(&gem, &ks, lib.database().connectionName());
        LocalModels lm;
        LocalModels::Server scfg;
        scfg.kind = LocalModels::Ollama;
        scfg.url = QUrl(localSrv.origin());
        lm.configure(scfg, QStringLiteral("qwen3.5:4b"), QStringLiteral("nomic-embed-text"));
        ai.setLocalModels(&lm);
        ai.setPreset(QStringLiteral("local"));

        PassageIndex pi(&ai, lib.database().connectionName());
        QString buildErr = QStringLiteral("unset");
        pi.build(bookId, {{0, 0, 50, QStringLiteral("Astrolabes calibrated nocturnal navigation.")}}, nullptr,
                 [&](const QString &e) { buildErr = e; });
        QTRY_COMPARE(buildErr, QString());

        // Verify stored vector in SQLite was pre-normalized to unit L2 norm ([0.6, 0.8, 0.0]).
        QSqlQuery checkVec(lib.database());
        QVERIFY(checkVec.exec(QStringLiteral("SELECT vector FROM passage_vectors WHERE book_id = %1").arg(bookId)));
        QVERIFY(checkVec.next());
        const QByteArray storedBlob = checkVec.value(0).toByteArray();
        QCOMPARE(storedBlob.size(), qsizetype(3 * sizeof(float)));
        float vals[3] = {};
        std::memcpy(vals, storedBlob.constData(), sizeof(vals));
        const double norm = std::sqrt(double(vals[0]) * vals[0] + double(vals[1]) * vals[1] + double(vals[2]) * vals[2]);
        QVERIFY2(std::abs(norm - 1.0) < 1e-5, qPrintable(QString::number(norm)));
        QVERIFY(std::abs(vals[0] - 0.6f) < 1e-5f);
        QVERIFY(std::abs(vals[1] - 0.8f) < 1e-5f);

        // Close writable Library + index cleanly.
        LibraryIndex idx(indexDb);
        QVERIFY(idx.available());
        QVERIFY(LibraryIndex::indexBook(indexDb, {bookId, epubPath, QStringLiteral("epub"), 1}));
    }

    auto sha256File = [](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray libHashBefore = sha256File(libraryDb);
    const QByteArray idxHashBefore = sha256File(indexDb);
    QVERIFY(!libHashBefore.isEmpty() && !idxHashBefore.isEmpty());

    // Lock files to 0444 and parent directories to 0555 to enforce OS-level read-only guarantees!
    QVERIFY(::chmod(libraryDb.toUtf8().constData(), 0444) == 0);
    QVERIFY(::chmod(indexDb.toUtf8().constData(), 0444) == 0);
    QVERIFY(::chmod(dataDir.toUtf8().constData(), 0555) == 0);
    QVERIFY(::chmod(cacheDir.toUtf8().constData(), 0555) == 0);
    QVERIFY(::chmod(configDir.toUtf8().constData(), 0555) == 0);

    const QByteArray origData = qgetenv("XDG_DATA_HOME");
    const QByteArray origCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray origConfig = qgetenv("XDG_CONFIG_HOME");
    auto restorePermsAndEnv = qScopeGuard([&] {
        ::chmod(dataDir.toUtf8().constData(), 0755);
        ::chmod(cacheDir.toUtf8().constData(), 0755);
        ::chmod(configDir.toUtf8().constData(), 0755);
        ::chmod(libraryDb.toUtf8().constData(), 0644);
        ::chmod(indexDb.toUtf8().constData(), 0644);
        qputenv("XDG_DATA_HOME", origData);
        qputenv("XDG_CACHE_HOME", origCache);
        qputenv("XDG_CONFIG_HOME", origConfig);
    });
    qputenv("XDG_DATA_HOME", (xdgRoot + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CACHE_HOME", (xdgRoot + QStringLiteral("/cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", (xdgRoot + QStringLiteral("/config")).toUtf8());

    // Run McpServer::runStdioWithStreams() (the exact code path of McpServer::runStdio()) against read-only tree!
    QByteArray reqFrames;
    reqFrames += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\"}}\n";
    reqFrames += "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"list_books\",\"arguments\":{\"collection\":\"Maritime\"}}}\n";
    reqFrames += "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"search_library\",\"arguments\":{\"query\":\"nocturnal\"}}}\n";
    reqFrames += "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"search_passages\",\"arguments\":{\"query\":\"astrolabes navigation\"}}}\n";
    reqFrames += QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"get_book_outline\",\"arguments\":{\"book_id\":%1}}}\n")
                     .arg(bookId)
                     .toUtf8();
    reqFrames += QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"read_passage\",\"arguments\":{\"book_id\":%1,\"chapter\":0}}}\n")
                     .arg(bookId)
                     .toUtf8();
    reqFrames += "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"list_marks\",\"arguments\":{\"query\":\"navigation\"}}}\n";
    reqFrames += "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/list\"}\n";

    std::FILE *inFile = std::tmpfile();
    std::FILE *outFile = std::tmpfile();
    QVERIFY(inFile && outFile);
    QCOMPARE(std::fwrite(reqFrames.constData(), 1, size_t(reqFrames.size()), inFile), size_t(reqFrames.size()));
    std::rewind(inFile);

    const int rc = McpServer::runStdioWithStreams({}, inFile, outFile);
    QCOMPARE(rc, 0);

    std::rewind(outFile);
    QByteArray outBytes;
    char buf[4096];
    while (const size_t n = std::fread(buf, 1, sizeof(buf), outFile))
        outBytes.append(buf, qsizetype(n));
    std::fclose(inFile);
    std::fclose(outFile);

    QList<QJsonObject> replies;
    for (const QByteArray &line : outBytes.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(line);
        QVERIFY2(d.isObject(), line.constData());
        replies.append(d.object());
    }
    QCOMPARE(replies.size(), 8);
    // id 2: list_books
    QCOMPARE(replies[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 1);
    // id 3: search_library (read-only FTS5)
    QCOMPARE(replies[2].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 1);
    // id 4: search_passages
    QVERIFY(replies[3].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt() >= 1);
    // id 5: get_book_outline
    QCOMPARE(replies[4].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("title")).toString(),
             QStringLiteral("Read-Only Atlas"));
    // id 6: read_passage
    QVERIFY(replies[5].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("Astrolabes")));
    // id 7: list_marks (read-only annotations query)
    QCOMPARE(replies[6].value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("count")).toInt(), 1);
    // id 8: resources/list
    QVERIFY(replies[7].value(QStringLiteral("result")).toObject().value(QStringLiteral("resources")).toArray().size() >= 3);

    // Byte-for-byte immutability: zero modifications & zero -wal/-shm sidecars created!
    QCOMPARE(sha256File(libraryDb), libHashBefore);
    QCOMPARE(sha256File(indexDb), idxHashBefore);
    QVERIFY(!QFileInfo::exists(libraryDb + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(libraryDb + QStringLiteral("-shm")));
    QVERIFY(!QFileInfo::exists(indexDb + QStringLiteral("-wal")));
    QVERIFY(!QFileInfo::exists(indexDb + QStringLiteral("-shm")));

    // 2. SIMD PassageIndex accuracy & backward-compatibility with legacy unnormalized 768-dim BLOBs.
    constexpr int kDim = 768;
    std::mt19937 rng(0x5EED1234u);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    QList<float> query(kDim);
    for (int j = 0; j < kDim; ++j)
        query[j] = dist(rng);

    auto refDoubleCosine = [](const QList<float> &a, const QList<float> &b) {
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (qsizetype i = 0; i < a.size(); ++i) {
            dot += double(a[i]) * double(b[i]);
            na += double(a[i]) * double(a[i]);
            nb += double(b[i]) * double(b[i]);
        }
        return (na > 0.0 && nb > 0.0) ? dot / std::sqrt(na * nb) : 0.0;
    };

    const QString simdDb = dir.filePath(QStringLiteral("simd-index.sqlite"));
    const QString simdConn = QStringLiteral("w5-simd-test");
    QList<double> expectedTop;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), simdConn);
        db.setDatabaseName(simdDb);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
            "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))")));
        QVERIFY(db.transaction());
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, 'm', ?, ?)"));
        for (int row = 0; row < 200; ++row) {
            QList<float> v(kDim);
            const float scale = (row % 2 == 0) ? 1.0f : (0.05f + float(row) * 0.25f);  // mix unit & legacy unnormalized
            for (int j = 0; j < kDim; ++j)
                v[j] = (row == 42 ? query[j] * 17.5f : (row == 99 ? (query[j] * 0.8f + dist(rng) * 0.2f) : dist(rng))) * scale;
            const QList<float> toStore = (row % 2 == 0) ? PassageIndex::normalize(v) : v;
            const double ref = refDoubleCosine(query, v);
            const double fast = PassageIndex::cosine(query, toStore);
            QVERIFY2(std::abs(fast - ref) < 1e-5,
                     qPrintable(QStringLiteral("cosine drift row %1: fast=%2 ref=%3").arg(row).arg(fast).arg(ref)));
            if (row == 42 || row == 99)
                expectedTop.append(ref);
            ins.addBindValue(row + 1);
            ins.addBindValue(QStringLiteral("0:0:100:r%1").arg(row));
            ins.addBindValue(QByteArray(reinterpret_cast<const char *>(toStore.constData()),
                                        toStore.size() * qsizetype(sizeof(float))));
            QVERIFY(ins.exec());
        }
        QVERIFY(db.commit());
        db.close();
    }
    QSqlDatabase::removeDatabase(simdConn);

    const auto ranked = PassageIndex::rankLibrary(simdDb, QStringLiteral("m"), query, 5, 0.25);
    QVERIFY(ranked.size() >= 2);
    QCOMPARE(ranked[0].bookId, 43);  // row 42 (legacy unnormalized 17.5x scaled exact match!)
    QVERIFY2(std::abs(ranked[0].score - 1.0) < 1e-5, qPrintable(QString::number(ranked[0].score)));
    QCOMPARE(ranked[1].bookId, 100);  // row 99 (unnormalized high-similarity match)
    QVERIFY2(std::abs(ranked[1].score - expectedTop[1]) < 1e-5, qPrintable(QString::number(ranked[1].score)));
}

void ClothboundTests::w6CmakeAotExternalizedI18nAndWebEngineFreeMcp() {
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());

    // 1. src/resources.qrc embeds ONLY en.json (all 49 non-English catalogs are externalized).
    {
        QFile qrcFile(srcRoot + QStringLiteral("/resources.qrc"));
        QVERIFY(qrcFile.open(QIODevice::ReadOnly));
        const QString qrc = QString::fromUtf8(qrcFile.readAll());
        QVERIFY(qrc.contains(QStringLiteral("i18n/en.json")));
        QVERIFY(!qrc.contains(QStringLiteral("i18n/de.json")));
        QVERIFY(!qrc.contains(QStringLiteral("i18n/fr.json")));
        QVERIFY(!qrc.contains(QStringLiteral("i18n/ar.json")));
        QVERIFY(!qrc.contains(QStringLiteral("i18n/zh-CN.json")));
    }

    // 2. Externalized lazy i18n resolution order:
    //    CLOTHBOUND_I18N_DIR > <appDir>/i18n > <appDir>/../share/clothbound/i18n > CLOTHBOUND_SOURCE_I18N_DIR > :/i18n.
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString envI18nDir = dir.filePath(QStringLiteral("env-i18n"));
        QVERIFY(QDir().mkpath(envI18nDir));
        write(envI18nDir + QStringLiteral("/de.json"), R"({"Save":"EnvSpeichern"})");

        const QByteArray prevEnv = qgetenv("CLOTHBOUND_I18N_DIR");
        auto restoreEnv = qScopeGuard([&] {
            if (prevEnv.isEmpty())
                qunsetenv("CLOTHBOUND_I18N_DIR");
            else
                qputenv("CLOTHBOUND_I18N_DIR", prevEnv);
        });

        qputenv("CLOTHBOUND_I18N_DIR", envI18nDir.toUtf8());
        const QStringList paths = Translator::catalogSearchPaths();
        QVERIFY(!paths.isEmpty());
        QCOMPARE(paths.first(), QDir::cleanPath(envI18nDir));
        QCOMPARE(paths.last(), QStringLiteral(":/i18n"));

        const QString langFile = dir.filePath(QStringLiteral("language"));
        write(langFile, "de\n");
        {
            Translator tr(langFile);
            QCOMPARE(tr.language(), QStringLiteral("de"));
            QCOMPARE(tr.translate("Main", "Save"), QStringLiteral("EnvSpeichern"));
            // English plural rules still resolve via lower search tiers when not in CLOTHBOUND_I18N_DIR.
            QCOMPARE(tr.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));
        }

        // Without CLOTHBOUND_I18N_DIR, lazy disk load resolves shipped catalogs (e.g. de, pt-BR)
        // from CLOTHBOUND_SOURCE_I18N_DIR / install share dir without embedding them in QRC.
        qunsetenv("CLOTHBOUND_I18N_DIR");
        QVERIFY(QFile::exists(QStringLiteral(":/i18n/en.json")));
        QVERIFY(!QFile::exists(QStringLiteral(":/i18n/de.json")));
        {
            Translator tr(langFile);
            QCOMPARE(tr.language(), QStringLiteral("de"));
            QVERIFY(!tr.translate("Main", "Save").isEmpty());
            QVERIFY(tr.translate("Main", "Save") != QStringLiteral("EnvSpeichern"));
            tr.setLanguage(QStringLiteral("pt-BR"));
            QCOMPARE(tr.language(), QStringLiteral("pt-BR"));
            QVERIFY(!tr.translate("Main", "Save").isEmpty());
        }

        // Direct embedded :/i18n fallback still carries en.json plurals even without disk files.
        write(langFile, "en\n");
        {
            Translator embeddedFallback(langFile, QStringLiteral(":/i18n"));
            QCOMPARE(embeddedFallback.language(), QStringLiteral("en"));
            QVERIFY(!embeddedFallback.isEmpty());
            QCOMPARE(embeddedFallback.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));
            QCOMPARE(embeddedFallback.translate("Main", "%n books", nullptr, 4), QStringLiteral("%n books"));
        }
    }

    // 3. WebEngine-free clothbound-mcp executable: --version, zero forbidden shared libs,
    //    and full stdio JSON-RPC 2.0 initialize + tools/list + list_books handshake.
    {
        const QString mcpBin = QCoreApplication::applicationDirPath() + QStringLiteral("/clothbound-mcp");
        QVERIFY2(QFileInfo::exists(mcpBin), qPrintable(QStringLiteral("Missing clothbound-mcp at %1").arg(mcpBin)));

        // 3a. --version exits 0 and prints version string.
        {
            QProcess proc;
            proc.start(mcpBin, {QStringLiteral("--version")});
            QVERIFY(proc.waitForFinished(10000));
            QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
            QCOMPARE(proc.exitCode(), 0);
            const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            QVERIFY2(out.startsWith(QStringLiteral("clothbound-mcp ")), qPrintable(out));
        }

        // 3b. ldd verifies zero QtWebEngine / QtQuick / QtQml / QtMultimedia linkage.
        {
            QProcess ldd;
            ldd.start(QStringLiteral("ldd"), {mcpBin});
            if (ldd.waitForStarted(3000) && ldd.waitForFinished(10000) && ldd.exitCode() == 0) {
                const QString deps = QString::fromUtf8(ldd.readAllStandardOutput()).toLower();
                QVERIFY2(!deps.contains(QStringLiteral("webengine")), qPrintable(deps));
                QVERIFY2(!deps.contains(QStringLiteral("qt6quick")), qPrintable(deps));
                QVERIFY2(!deps.contains(QStringLiteral("qt6qml")), qPrintable(deps));
                QVERIFY2(!deps.contains(QStringLiteral("qt6multimedia")), qPrintable(deps));
            }
        }

        // 3c. Stdio JSON-RPC 2.0 handshake against a seeded read-only library (both default and `mcp` arg).
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        const QString cfgDir = xdg.filePath(QStringLiteral("config"));
        const QString dataDir = xdg.filePath(QStringLiteral("data"));
        const QString cacheDir = xdg.filePath(QStringLiteral("cache"));
        QVERIFY(QDir().mkpath(cfgDir + QStringLiteral("/clothbound")));
        QVERIFY(QDir().mkpath(dataDir + QStringLiteral("/clothbound")));
        QVERIFY(QDir().mkpath(cacheDir + QStringLiteral("/clothbound")));

        const QString epubPath = xdg.filePath(QStringLiteral("mcp_smoke.epub"));
        write(epubPath, TestBooks::epub2({}));
        {
            Library lib(dataDir + QStringLiteral("/clothbound"), cacheDir + QStringLiteral("/clothbound"));
            QVERIFY(lib.open());
            QVERIFY(lib.addBookFile(epubPath) > 0);
        }

        const QByteArray payload =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"list_books\",\"arguments\":{}}}\n";

        for (const QStringList &invokeArgs : {QStringList{}, QStringList{QStringLiteral("mcp")}}) {
            QProcess proc;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("XDG_CONFIG_HOME"), cfgDir);
            env.insert(QStringLiteral("XDG_DATA_HOME"), dataDir);
            env.insert(QStringLiteral("XDG_CACHE_HOME"), cacheDir);
            proc.setProcessEnvironment(env);
            proc.start(mcpBin, invokeArgs);
            QVERIFY(proc.waitForStarted(10000));
            QCOMPARE(proc.write(payload), payload.size());
            proc.closeWriteChannel();
            QVERIFY2(proc.waitForFinished(15000), "clothbound-mcp timed out on stdio handshake");
            QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
            const QByteArray errOut = proc.readAllStandardError();
            QCOMPARE(proc.exitCode(), 0);
            QVERIFY2(errOut.isEmpty(), errOut.constData());

            const QList<QByteArray> lines = proc.readAllStandardOutput().trimmed().split('\n');
            QCOMPARE(lines.size(), 3);

            const QJsonObject initResp = QJsonDocument::fromJson(lines[0]).object();
            QCOMPARE(initResp.value(QStringLiteral("id")).toInt(), 1);
            QVERIFY(initResp.value(QStringLiteral("result")).toObject().contains(QStringLiteral("serverInfo")));

            const QJsonObject toolsResp = QJsonDocument::fromJson(lines[1]).object();
            QCOMPARE(toolsResp.value(QStringLiteral("id")).toInt(), 2);
            QCOMPARE(toolsResp.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray().size(), 6);

            const QJsonObject listResp = QJsonDocument::fromJson(lines[2]).object();
            QCOMPARE(listResp.value(QStringLiteral("id")).toInt(), 3);
            const QJsonObject structured =
                listResp.value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).toObject();
            QCOMPARE(structured.value(QStringLiteral("count")).toInt(), 1);
            const QJsonArray books = structured.value(QStringLiteral("books")).toArray();
            QCOMPARE(books.size(), 1);
            QVERIFY(books[0].toObject().value(QStringLiteral("title")).toString().contains(QStringLiteral("Lantern Keeper")));
        }
    }
}
