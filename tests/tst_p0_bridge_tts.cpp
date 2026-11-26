// Phase 0 Commit 3 end-to-end verification suite:
//   1. Pure C11 ABI bridge (`clothbound_c.h`) invoked from a NON-Qt Thread 0
//      `main()` (NO QCoreApplication on Thread 0 — faithfully simulating
//      SwiftUI's `@main App` / AppKit `NSApplicationMain`!).
//   2. Real out-of-process `clothbound-tts` binary over raw POSIX stdin/stdout
//      pipes (PING/PONG, invalid-frame rejection, SHUTDOWN, stdin-EOF < 200ms).
//   3. `TtsProcessClient` (Qt6::Core QProcess lazy spawn, idle timeout auto-exit,
//      crash auto-respawn with backoff) + Qt-hosted `cb_engine_create()` mode.

#include "bridge/clothbound_c.h"
#include "testbooks.h"
#include "tts/tts_process_client.h"
#include "tts/tts_protocol.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);                                   \
            ++g_failures;                                                                                            \
            return false;                                                                                            \
        }                                                                                                            \
    } while (0)

QByteArray makeTinyPng() {
    QImage img(16, 24, QImage::Format_RGB32);
    img.fill(QColor(42, 96, 150));
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

QJsonObject parseJsonAndFree(char *raw) {
    if (!raw)
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(raw));
    cb_string_free(raw);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

// ---------------------------------------------------------------------------
// Part 1: Pure C ABI Bridge on Dedicated std::thread (NO QCoreApplication on Thread 0)
// ---------------------------------------------------------------------------

struct EventRecorder {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> events;
    std::atomic<cb_engine_t *> engine{nullptr};
    std::atomic_bool allEventsOnActorThread{true};
};

void onBridgeEvent(void *userData, const char *eventType, const char *jsonPayload) {
    auto *rec = static_cast<EventRecorder *>(userData);
    cb_engine_t *eng = rec->engine.load(std::memory_order_acquire);
    if (eng && !cb_engine_is_on_actor_thread(eng))
        rec->allEventsOnActorThread.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(rec->mutex);
    rec->events.emplace_back(eventType ? eventType : "", jsonPayload ? jsonPayload : "");
}

bool hasEvent(EventRecorder &rec, const char *type) {
    std::lock_guard<std::mutex> lock(rec.mutex);
    for (const auto &[k, _] : rec.events) {
        if (k == type)
            return true;
    }
    return false;
}

bool testBridgeStandaloneActorThread() {
    std::printf("[1/3] Testing C ABI bridge on dedicated Qt Core Actor Thread (Thread 0 has NO QCoreApplication)...\n");

    // Thread 0 MUST NOT have a QCoreApplication (faithfully simulating SwiftUI @main App).
    CHECK(QCoreApplication::instance() == nullptr);
    CHECK(cb_engine_version() != nullptr && std::strlen(cb_engine_version()) > 0);

    QTemporaryDir tempDir;
    CHECK(tempDir.isValid());
    const QString dataDir = tempDir.filePath(QStringLiteral("data"));
    const QString cacheDir = tempDir.filePath(QStringLiteral("cache"));
    const QString booksDir = tempDir.filePath(QStringLiteral("books"));
    CHECK(QDir().mkpath(booksDir));

    const QString epubPath = booksDir + QStringLiteral("/lantern.epub");
    const QString pdfPath = booksDir + QStringLiteral("/systems.pdf");
    const QString utf16EpubPath = booksDir + QStringLiteral("/utf16.epub");

    CHECK(TestBooks::write(epubPath, TestBooks::epub2(makeTinyPng())));
    CHECK(TestBooks::write(pdfPath, TestBooks::pdf("Systems Craft", "Ada Lovelace")));

    // EPUB chapter with a non-BMP emoji (U+1F4D6 = 4 UTF-8 bytes, 2 UTF-16 code units!)
    // to verify the UTF-16 code-unit offset contract explicitly.
    const QByteArray utf16Epub = TestBooks::epub({
        {"OEBPS/content.opf", R"(<?xml version="1.0" encoding="utf-8"?>
<package version="2.0" unique-identifier="uid" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Unicode Offsets</dc:title>
    <dc:creator>Ada</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="uid">urn:uuid:utf16-test</dc:identifier>
  </metadata>
  <manifest>
    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="ch1"/></spine>
</package>)"},
        {"OEBPS/ch1.xhtml",
         TestBooks::chapter("U16", "<p>\xF0\x9F\x93\x96 Alpha beta target phrase gamma.</p>")},
    });
    CHECK(TestBooks::write(utf16EpubPath, utf16Epub));

    EventRecorder recorder;
    char *err = nullptr;
    const QByteArray dataUtf8 = dataDir.toUtf8();
    const QByteArray cacheUtf8 = cacheDir.toUtf8();
#ifdef CLOTHBOUND_SOURCE_I18N_DIR
    const char *i18nDir = CLOTHBOUND_SOURCE_I18N_DIR;
#else
    const char *i18nDir = nullptr;
#endif

    cb_engine_t *rawEngine = cb_engine_create(dataUtf8.constData(), cacheUtf8.constData(), i18nDir,
                                              &onBridgeEvent, &recorder, &err);
    CHECK(err == nullptr);
    CHECK(rawEngine != nullptr);
    std::unique_ptr<cb_engine_t, decltype(&cb_engine_destroy)> engineGuard(rawEngine, &cb_engine_destroy);
    cb_engine_t *engine = engineGuard.get();
    recorder.engine.store(engine, std::memory_order_release);

    // QCoreApplication now lives on the background actor thread, NOT Thread 0!
    CHECK(QCoreApplication::instance() != nullptr);
    CHECK(!cb_engine_is_on_actor_thread(engine));

    // Language switching + invalid code rejection.
    CHECK(cb_engine_set_language(engine, "fr"));
    char *lang = cb_engine_language(engine);
    CHECK(lang != nullptr && std::strcmp(lang, "fr") == 0);
    cb_string_free(lang);
    CHECK(hasEvent(recorder, "language.changed"));
    CHECK(!cb_engine_set_language(engine, "invalid language code!!!"));
    CHECK(cb_engine_set_language(engine, "en"));

    // Import real EPUB + PDF + UTF-16 test EPUB.
    const QByteArray epubUtf8 = epubPath.toUtf8();
    const QByteArray pdfUtf8 = pdfPath.toUtf8();
    const QByteArray u16Utf8 = utf16EpubPath.toUtf8();
    const char *importPaths[3] = {epubUtf8.constData(), pdfUtf8.constData(), u16Utf8.constData()};
    CHECK(cb_engine_add_paths(engine, importPaths, 3));

    const QJsonObject listed = parseJsonAndFree(cb_engine_list_books_json(engine));
    CHECK(listed.value(QStringLiteral("bookCount")).toInt() == 3);
    const QJsonArray books = listed.value(QStringLiteral("books")).toArray();
    CHECK(books.size() == 3);
    const QJsonObject shelves = listed.value(QStringLiteral("shelfCounts")).toObject();
    CHECK(shelves.value(QStringLiteral("all")).toInt() == 3);
    CHECK(shelves.value(QStringLiteral("epub")).toInt() == 2);
    CHECK(shelves.value(QStringLiteral("pdf")).toInt() == 1);

    int epubId = 0;
    int pdfId = 0;
    int u16Id = 0;
    for (const QJsonValue &v : books) {
        const QJsonObject b = v.toObject();
        const QString title = b.value(QStringLiteral("title")).toString();
        const QString fmt = b.value(QStringLiteral("format")).toString();
        if (fmt == QLatin1String("pdf"))
            pdfId = b.value(QStringLiteral("id")).toInt();
        else if (title.contains(QStringLiteral("Unicode Offsets")))
            u16Id = b.value(QStringLiteral("id")).toInt();
        else if (fmt == QLatin1String("epub"))
            epubId = b.value(QStringLiteral("id")).toInt();
    }
    CHECK(epubId > 0 && pdfId > 0 && u16Id > 0);

    // Cover bytes must be valid JPEG (FF D8 FF).
    const cb_bytes_t epubCover = cb_engine_book_cover_jpeg(engine, epubId);
    CHECK(epubCover.data != nullptr && epubCover.len > 16);
    CHECK(epubCover.data[0] == 0xFF && epubCover.data[1] == 0xD8 && epubCover.data[2] == 0xFF);
    cb_bytes_free(epubCover);

    const cb_bytes_t pdfCover = cb_engine_book_cover_jpeg(engine, pdfId);
    CHECK(pdfCover.data != nullptr && pdfCover.len > 16);
    CHECK(pdfCover.data[0] == 0xFF && pdfCover.data[1] == 0xD8 && pdfCover.data[2] == 0xFF);
    cb_bytes_free(pdfCover);

    // Open TWO (and THREE!) cb_session_t* simultaneously: EPUB + PDF + sibling EPUB!
    cb_session_t *sessionEpub = cb_session_open(engine, epubId, &err);
    CHECK(err == nullptr && sessionEpub != nullptr);
    CHECK(cb_session_book_id(sessionEpub) == epubId);

    cb_session_t *sessionPdf = cb_session_open(engine, pdfId, &err);
    CHECK(err == nullptr && sessionPdf != nullptr);
    CHECK(cb_session_book_id(sessionPdf) == pdfId);

    cb_session_t *sessionEpubSibling = cb_session_open(engine, epubId, &err);
    CHECK(err == nullptr && sessionEpubSibling != nullptr);

    // Verify EPUB session metadata & resource resolution (including WKURLSchemeHandler empty-Referer fallback!).
    const QJsonObject epubMeta = parseJsonAndFree(cb_session_metadata_json(sessionEpub));
    CHECK(epubMeta.value(QStringLiteral("format")).toString() == QLatin1String("epub"));
    CHECK(epubMeta.value(QStringLiteral("chapterCount")).toInt() == 2);
    CHECK(epubMeta.value(QStringLiteral("toc")).toArray().size() == 3);
    CHECK(epubMeta.value(QStringLiteral("pageList")).toArray().size() == 2);
    const QString token = epubMeta.value(QStringLiteral("resourceToken")).toString();
    CHECK(!token.isEmpty());

    const QByteArray tokenUtf8 = token.toUtf8();
    const QByteArray cssUrl = "clothbound://local/book/" + tokenUtf8 + "/OEBPS/style.css";
    cb_resource_t res{};
    CHECK(cb_engine_resolve_resource(engine, cssUrl.constData(), nullptr, nullptr, &res));
    CHECK(res.status == 200);
    CHECK(res.mime_type != nullptr && std::strcmp(res.mime_type, "text/css") == 0);
    CHECK(res.body.data != nullptr && res.body.len > 0);
    CHECK(QByteArray(reinterpret_cast<const char *>(res.body.data), static_cast<qsizetype>(res.body.len))
              .contains("text-indent"));
    bool sawCsp = false;
    bool sawNosniff = false;
    for (size_t i = 0; i < res.header_count; ++i) {
        if (std::strcmp(res.headers[i].name, "Content-Security-Policy") == 0 &&
            std::strstr(res.headers[i].value, "script-src 'none'") != nullptr)
            sawCsp = true;
        if (std::strcmp(res.headers[i].name, "X-Content-Type-Options") == 0 &&
            std::strcmp(res.headers[i].value, "nosniff") == 0)
            sawNosniff = true;
    }
    CHECK(sawCsp && sawNosniff);
    cb_resource_free(&res);

    // WebKit empty-Referer root-relative fallback test!
    CHECK(cb_engine_resolve_resource(engine, "clothbound://local/OEBPS/style.css", nullptr, tokenUtf8.constData(), &res));
    CHECK(res.status == 200 && res.body.len > 0);
    cb_resource_free(&res);

    // Token isolation + bad host rejection.
    CHECK(!cb_engine_resolve_resource(engine, "clothbound://local/book/invalidtoken/OEBPS/style.css", nullptr, nullptr, &res));
    CHECK(res.status == 404);
    cb_resource_free(&res);

    CHECK(!cb_engine_resolve_resource(engine, "https://example.com/OEBPS/style.css", nullptr, tokenUtf8.constData(), &res));
    CHECK(res.status == 400);
    cb_resource_free(&res);

    // W3C quote+context anchoring with drifted hint!
    const QJsonObject located =
        parseJsonAndFree(cb_session_locate_quote_json(sessionEpub, 0, "storm came in.", "A ", "", 0));
    CHECK(located.value(QStringLiteral("found")).toBool());
    CHECK(located.value(QStringLiteral("drifted")).toBool());
    const qint64 quoteStart = located.value(QStringLiteral("start")).toInteger();
    const qint64 quoteEnd = located.value(QStringLiteral("end")).toInteger();
    CHECK(quoteStart > 0 && quoteEnd == quoteStart + 14);
    CHECK(located.value(QStringLiteral("matchedText")).toString() == QLatin1String("storm came in."));

    // UTF-16 code-unit offset contract check (U+1F4D6 📖 is 2 UTF-16 code units, 4 UTF-8 bytes!).
    cb_session_t *sessionU16 = cb_session_open(engine, u16Id, &err);
    CHECK(err == nullptr && sessionU16 != nullptr);
    const QJsonObject u16Loc =
        parseJsonAndFree(cb_session_locate_quote_json(sessionU16, 0, "target phrase", "beta ", " gamma", -1));
    CHECK(u16Loc.value(QStringLiteral("found")).toBool());
    // "📖 Alpha beta " = 2 (surrogate pair) + 12 ASCII = 14 UTF-16 code units!
    CHECK(u16Loc.value(QStringLiteral("start")).toInteger() == 14);
    CHECK(u16Loc.value(QStringLiteral("end")).toInteger() == 27);
    cb_session_close(sessionU16);

    // Add mark in sessionEpub, verify sibling session sees it immediately, verify search finds it, then delete.
    const QByteArray markReq = QJsonDocument(QJsonObject{
                                                 {QStringLiteral("kind"), QStringLiteral("highlight")},
                                                 {QStringLiteral("chapter"), 0},
                                                 {QStringLiteral("start"), quoteStart},
                                                 {QStringLiteral("end"), quoteEnd},
                                                 {QStringLiteral("text"), QStringLiteral("storm came in.")},
                                                 {QStringLiteral("color"), QStringLiteral("yellow")},
                                                 {QStringLiteral("note"), QStringLiteral("Memorable harbor weather")},
                                                 {QStringLiteral("label"), QStringLiteral("Chapter One")},
                                                 {QStringLiteral("prefix"), QStringLiteral("A ")},
                                                 {QStringLiteral("suffix"), QStringLiteral("")},
                                             })
                                   .toJson(QJsonDocument::Compact);
    const QJsonObject createdMark = parseJsonAndFree(cb_session_add_mark_json(sessionEpub, markReq.constData()));
    const int markId = createdMark.value(QStringLiteral("id")).toInt();
    CHECK(markId > 0);
    CHECK(createdMark.value(QStringLiteral("note")).toString() == QLatin1String("Memorable harbor weather"));

    const QJsonObject marksA = parseJsonAndFree(cb_session_marks_json(sessionEpub));
    const QJsonObject marksSibling = parseJsonAndFree(cb_session_marks_json(sessionEpubSibling));
    CHECK(marksA.value(QStringLiteral("count")).toInt() == 1);
    CHECK(marksSibling.value(QStringLiteral("count")).toInt() == 1);
    CHECK(hasEvent(recorder, "marks.changed"));

    // Command palette synchronous search (zero nested event loops!).
    const QJsonObject palette = parseJsonAndFree(cb_engine_search_palette_json(engine, "harbor", 0));
    CHECK(palette.value(QStringLiteral("books")).toArray().size() >= 1);
    CHECK(palette.value(QStringLiteral("marks")).toArray().size() >= 1);

    const QJsonObject passagePalette = parseJsonAndFree(cb_engine_search_palette_json(engine, "storm", epubId));
    CHECK(passagePalette.value(QStringLiteral("books")).toArray().isEmpty());
    CHECK(passagePalette.value(QStringLiteral("passages")).toArray().size() >= 1);

    CHECK(cb_session_delete_mark(sessionEpub, markId));
    const QJsonObject marksAfterDelete = parseJsonAndFree(cb_session_marks_json(sessionEpub));
    CHECK(marksAfterDelete.value(QStringLiteral("count")).toInt() == 0);

    // PDF session metadata, PNG render, text JSON, and quote anchoring.
    const QJsonObject pdfMeta = parseJsonAndFree(cb_session_metadata_json(sessionPdf));
    CHECK(pdfMeta.value(QStringLiteral("format")).toString() == QLatin1String("pdf"));
    CHECK(pdfMeta.value(QStringLiteral("pageCount")).toInt() == 1);
    CHECK(pdfMeta.value(QStringLiteral("title")).toString() == QLatin1String("Systems Craft"));

    const cb_bytes_t pagePng = cb_session_pdf_page_png(sessionPdf, 0, 200, 0);
    CHECK(pagePng.data != nullptr && pagePng.len > 32);
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(pagePng.data, kPngSig, sizeof(kPngSig)) == 0);
    cb_bytes_free(pagePng);

    const QJsonObject pdfText = parseJsonAndFree(cb_session_pdf_text_json(sessionPdf, 0));
    CHECK(pdfText.value(QStringLiteral("page")).toInt() == 0);
    CHECK(pdfText.value(QStringLiteral("text")).toString().contains(QStringLiteral("Hello reader")));
    CHECK(pdfText.value(QStringLiteral("widthPt")).toDouble() > 199.0);
    CHECK(pdfText.value(QStringLiteral("heightPt")).toDouble() > 299.0);

    const QJsonObject pdfLoc =
        parseJsonAndFree(cb_session_locate_quote_json(sessionPdf, 0, "reader", "Hello ", "", 0));
    CHECK(pdfLoc.value(QStringLiteral("found")).toBool());
    CHECK(pdfLoc.value(QStringLiteral("matchedText")).toString() == QLatin1String("reader"));

    // Save reading positions on both open sessions simultaneously.
    CHECK(cb_session_save_position(sessionEpub, 1, 0.42, R"({"offset":128,"title":"Notes"})"));
    CHECK(cb_session_save_position(sessionPdf, 0, 0.75, R"({"page":0,"scrollY":0.5})"));
    CHECK(hasEvent(recorder, "library.book_updated"));

    // Close all sessions cleanly and destroy engine.
    cb_session_close(sessionEpubSibling);
    cb_session_close(sessionEpub);
    cb_session_close(sessionPdf);
    engineGuard.reset();

    CHECK(recorder.allEventsOnActorThread.load(std::memory_order_acquire));
    CHECK(QCoreApplication::instance() == nullptr);

    // Re-open engine on the same dataDir to verify clean restart & SQLite persistence.
    cb_engine_t *engine2 =
        cb_engine_create(dataUtf8.constData(), cacheUtf8.constData(), i18nDir, nullptr, nullptr, &err);
    CHECK(err == nullptr && engine2 != nullptr);
    cb_session_t *reopened = cb_session_open(engine2, epubId, &err);
    CHECK(err == nullptr && reopened != nullptr);
    const QJsonObject reopenedMeta = parseJsonAndFree(cb_session_metadata_json(reopened));
    CHECK(reopenedMeta.value(QStringLiteral("progress")).toDouble() > 0.41 &&
          reopenedMeta.value(QStringLiteral("progress")).toDouble() < 0.43);
    CHECK(reopenedMeta.value(QStringLiteral("savedPosition")).toObject().value(QStringLiteral("chapter")).toInt() == 1);
    cb_session_close(reopened);
    cb_engine_destroy(engine2);
    CHECK(QCoreApplication::instance() == nullptr);

    std::printf("  PASS: C ABI bridge actor thread, dual sessions, resources, marks, UTF-16 quotes, PDF, persistence\n");
    return true;
}

// ---------------------------------------------------------------------------
// Part 2: Real `clothbound-tts` Binary over Raw POSIX Pipes (ZERO Qt)
// ---------------------------------------------------------------------------

struct PipeChild {
    pid_t pid = -1;
    int toChildFd = -1;
    int fromChildFd = -1;

    ~PipeChild() { cleanup(); }

    void cleanup() {
        if (toChildFd >= 0) {
            ::close(toChildFd);
            toChildFd = -1;
        }
        if (fromChildFd >= 0) {
            ::close(fromChildFd);
            fromChildFd = -1;
        }
        if (pid > 0) {
            int st = 0;
            if (::waitpid(pid, &st, WNOHANG) == 0) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &st, 0);
            }
            pid = -1;
        }
    }
};

bool spawnTtsHelper(const std::string &binPath, PipeChild &child) {
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (::pipe(inPipe) != 0)
        return false;
    if (::pipe(outPipe) != 0) {
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(inPipe[0], STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        char *const argv[] = {const_cast<char *>(binPath.c_str()), nullptr};
        ::execv(binPath.c_str(), argv);
        ::_exit(127);
    }

    ::close(inPipe[0]);
    ::close(outPipe[1]);
    child.pid = pid;
    child.toChildFd = inPipe[1];
    child.fromChildFd = outPipe[0];
    return true;
}

bool writeAllFd(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t w = ::write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

bool readExactTimed(int fd, uint8_t *buf, size_t len, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t got = 0;
    while (got < len) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, std::max(1, rem));
        if (pr <= 0)
            return false;
        const ssize_t r = ::read(fd, buf + got, len - got);
        if (r <= 0)
            return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool readFrameTimed(int fd, ClothboundTts::Frame &out, int timeoutMs = 2000) {
    uint8_t hdrBytes[ClothboundTts::kFrameHeaderBytes];
    if (!readExactTimed(fd, hdrBytes, sizeof(hdrBytes), timeoutMs))
        return false;
    ClothboundTts::FrameHeader hdr;
    if (ClothboundTts::parseFrameHeader(hdrBytes, sizeof(hdrBytes), hdr) != ClothboundTts::DecodeStatus::Ok)
        return false;
    out.type = static_cast<ClothboundTts::FrameType>(hdr.type);
    out.payload.resize(hdr.payload_len);
    if (hdr.payload_len > 0 && !readExactTimed(fd, out.payload.data(), hdr.payload_len, timeoutMs))
        return false;
    return true;
}

bool waitExitTimed(PipeChild &child, int timeoutMs, int &statusOut, long long &elapsedMsOut) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const pid_t r = ::waitpid(child.pid, &statusOut, WNOHANG);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (r == child.pid) {
            elapsedMsOut = elapsed;
            child.pid = -1;
            return true;
        }
        if (r < 0 || elapsed >= timeoutMs) {
            elapsedMsOut = elapsed;
            return false;
        }
        ::usleep(2000);
    }
}

bool testTtsHelperPipes(const std::string &ttsBin) {
    std::printf("[2/3] Testing out-of-process clothbound-tts helper binary over raw pipes (%s)...\n", ttsBin.c_str());
    using namespace ClothboundTts;

    // 2a. --self-test mode
    {
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            const int devNull = ::open("/dev/null", O_WRONLY);
            if (devNull >= 0) {
                ::dup2(devNull, STDOUT_FILENO);
                ::close(devNull);
            }
            char *const argv[] = {const_cast<char *>(ttsBin.c_str()), const_cast<char *>("--self-test"), nullptr};
            ::execv(ttsBin.c_str(), argv);
            ::_exit(127);
        }
        int st = 0;
        CHECK(::waitpid(pid, &st, 0) == pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    // 2b. PING -> PONG, recoverable errors (SYNTHESIZE w/o model, invalid JSON, unknown frame), SHUTDOWN
    {
        PipeChild child;
        CHECK(spawnTtsHelper(ttsBin, child));

        const auto pingWire = encodeFrame(FrameType::Ping);
        CHECK(writeAllFd(child.toChildFd, pingWire.data(), pingWire.size()));
        Frame reply;
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Pong);
        const auto pongObj = Json::parseObject(
            std::string_view(reinterpret_cast<const char *>(reply.payload.data()), reply.payload.size()));
        CHECK(pongObj.has_value());
        CHECK(Json::getInt(*pongObj, "protocol") == static_cast<int64_t>(kProtocolVersion));
        CHECK(Json::getInt(*pongObj, "pid") == static_cast<int64_t>(child.pid));

        // SYNTHESIZE with no model loaded -> ERROR frame, helper stays alive!
        const auto synthWire =
            encodeFrame(FrameType::Synthesize, R"({"request_id":7,"text":"Hello","speaker_id":0,"speed":1.0})");
        CHECK(writeAllFd(child.toChildFd, synthWire.data(), synthWire.size()));
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Error);
        const auto errObj = Json::parseObject(
            std::string_view(reinterpret_cast<const char *>(reply.payload.data()), reply.payload.size()));
        CHECK(errObj.has_value());
        CHECK(Json::getUint(*errObj, "request_id") == 7);
        CHECK(Json::getString(*errObj, "code") == "model_not_loaded");

        // Malformed JSON inside valid frame -> ERROR frame, helper stays alive!
        const auto badJsonWire = encodeFrame(FrameType::LoadModel, "{not-valid-json");
        CHECK(writeAllFd(child.toChildFd, badJsonWire.data(), badJsonWire.size()));
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Error);

        // Unknown frame type -> ERROR frame, helper stays alive!
        const auto unknownWire = encodeFrame(static_cast<FrameType>(0x9999u));
        CHECK(writeAllFd(child.toChildFd, unknownWire.data(), unknownWire.size()));
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Error);

        // Still responsive to PING -> PONG after recoverable errors!
        CHECK(writeAllFd(child.toChildFd, pingWire.data(), pingWire.size()));
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Pong);

        // SHUTDOWN -> clean exit code 0 within 200 ms.
        const auto shutdownWire = encodeFrame(FrameType::Shutdown);
        CHECK(writeAllFd(child.toChildFd, shutdownWire.data(), shutdownWire.size()));
        int st = 0;
        long long ms = 0;
        CHECK(waitExitTimed(child, 200, st, ms));
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    // 2c. Corrupt frame magic -> ERROR frame + clean non-zero exit
    {
        PipeChild child;
        CHECK(spawnTtsHelper(ttsBin, child));

        uint8_t badMagic[12];
        writeLe32(badMagic + 0, 0xDEADBEEFu);
        writeLe32(badMagic + 4, static_cast<uint32_t>(FrameType::Ping));
        writeLe32(badMagic + 8, 0u);
        CHECK(writeAllFd(child.toChildFd, badMagic, sizeof(badMagic)));

        Frame reply;
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Error);
        const auto errObj = Json::parseObject(
            std::string_view(reinterpret_cast<const char *>(reply.payload.data()), reply.payload.size()));
        CHECK(errObj.has_value() && Json::getString(*errObj, "code") == "invalid_frame");

        int st = 0;
        long long ms = 0;
        CHECK(waitExitTimed(child, 200, st, ms));
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) != 0);
    }

    // 2d. Oversized payload_len (> 16 MiB) -> immediate ERROR frame + clean exit (no OOM allocation!)
    {
        PipeChild child;
        CHECK(spawnTtsHelper(ttsBin, child));

        uint8_t oversized[12];
        writeLe32(oversized + 0, kFrameMagic);
        writeLe32(oversized + 4, static_cast<uint32_t>(FrameType::Synthesize));
        writeLe32(oversized + 8, kMaxFramePayloadBytes + 1024u);
        CHECK(writeAllFd(child.toChildFd, oversized, sizeof(oversized)));

        Frame reply;
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Error);
        const auto errObj = Json::parseObject(
            std::string_view(reinterpret_cast<const char *>(reply.payload.data()), reply.payload.size()));
        CHECK(errObj.has_value() && Json::getString(*errObj, "code") == "invalid_frame");

        int st = 0;
        long long ms = 0;
        CHECK(waitExitTimed(child, 200, st, ms));
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) != 0);
    }

    // 2e. Stdin EOF (parent crash / force-quit simulation) -> exits cleanly with code 0 within 200 ms!
    {
        PipeChild child;
        CHECK(spawnTtsHelper(ttsBin, child));

        const auto pingWire = encodeFrame(FrameType::Ping);
        CHECK(writeAllFd(child.toChildFd, pingWire.data(), pingWire.size()));
        Frame reply;
        CHECK(readFrameTimed(child.fromChildFd, reply));
        CHECK(reply.type == FrameType::Pong);

        // Close stdin write-end abruptly without SHUTDOWN.
        ::close(child.toChildFd);
        child.toChildFd = -1;

        int st = 0;
        long long ms = 0;
        CHECK(waitExitTimed(child, 200, st, ms));
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        std::printf("  PASS: clothbound-tts pipes, invalid-frame rejection, SHUTDOWN, stdin-EOF exit in %lld ms (<200ms)\n",
                    ms);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Part 3: TtsProcessClient (Qt6::Core QProcess) + Qt-Hosted Engine Mode
// ---------------------------------------------------------------------------

template <typename Pred>
bool spinUntil(Pred &&pred, int timeoutMs = 3000) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!pred())
            ::usleep(5000);
    }
    return pred();
}

bool testTtsProcessClientAndQtHostedBridge(const std::string &ttsBin) {
    std::printf("[3/3] Testing TtsProcessClient (lazy spawn, idle exit, crash respawn) + Qt-hosted cb_engine...\n");

    int argc = 1;
    char arg0[] = "tst_p0_bridge_tts";
    char *argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    // 3a. TtsProcessClient lazy spawn, idle timeout exit, transparent respawn, crash backoff respawn.
    {
        TtsProcessClient client;
        client.setExecutablePath(QString::fromStdString(ttsBin));
        client.setIdleTimeoutMs(180);

        CHECK(!client.isRunning());

        int pongs = 0;
        qint64 lastPid = 0;
        QString lastErrCode;
        uint64_t lastErrReq = 0;
        int idleExits = 0;
        int crashExits = 0;
        int respawnsScheduled = 0;

        QObject::connect(&client, &TtsProcessClient::pongReceived, &client,
                         [&](int proto, bool, qint64 pid) {
                             if (proto == int(ClothboundTts::kProtocolVersion)) {
                                 ++pongs;
                                 lastPid = pid;
                             }
                         });
        QObject::connect(&client, &TtsProcessClient::requestError, &client,
                         [&](uint64_t reqId, const QString &code, const QString &) {
                             lastErrReq = reqId;
                             lastErrCode = code;
                         });
        QObject::connect(&client, &TtsProcessClient::helperExited, &client,
                         [&](int code, bool crashed) {
                             if (crashed)
                                 ++crashExits;
                             else if (code == 0)
                                 ++idleExits;
                         });
        QObject::connect(&client, &TtsProcessClient::helperRespawnScheduled, &client,
                         [&](int, int) { ++respawnsScheduled; });

        // Lazy spawn on first ping().
        client.ping();
        CHECK(spinUntil([&] { return pongs == 1; }, 2000));
        const qint64 firstPid = lastPid;
        CHECK(firstPid > 0 && client.isRunning());

        // Synthesize without model -> requestError with exact requestId.
        const uint64_t reqId = client.synthesize(QStringLiteral("Hello world"));
        CHECK(spinUntil([&] { return lastErrReq == reqId && lastErrCode == QLatin1String("model_not_loaded"); }, 2000));

        // Idle timeout fires -> helper exits cleanly with status 0 (0 MB idle ONNX RAM!).
        CHECK(spinUntil([&] { return idleExits >= 1 && !client.isRunning(); }, 2000));

        // Next ping() transparently re-spawns helper!
        client.setIdleTimeoutMs(10000);
        client.ping();
        CHECK(spinUntil([&] { return pongs == 2; }, 2000));
        const qint64 secondPid = lastPid;
        CHECK(secondPid > 0 && client.isRunning());

        // Simulate helper crash (SIGKILL) -> crash detected, backoff respawn scheduled, helper recovers!
        CHECK(::kill(static_cast<pid_t>(secondPid), SIGKILL) == 0);
        CHECK(spinUntil([&] { return crashExits == 1 && respawnsScheduled == 1; }, 2000));
        CHECK(spinUntil([&] { return client.isRunning() && client.helperPid() != secondPid; }, 2000));

        client.ping();
        CHECK(spinUntil([&] { return pongs == 3; }, 2000));

        client.shutdown();
        CHECK(!client.isRunning());
    }

    // 3b. cb_engine_create() when QCoreApplication ALREADY exists on Thread 0 (Qt test host mode).
    {
        QTemporaryDir tempDir;
        CHECK(tempDir.isValid());
        const QByteArray d = tempDir.filePath(QStringLiteral("d")).toUtf8();
        const QByteArray c = tempDir.filePath(QStringLiteral("c")).toUtf8();
        char *err = nullptr;
        cb_engine_t *eng = cb_engine_create(d.constData(), c.constData(), nullptr, nullptr, nullptr, &err);
        CHECK(err == nullptr && eng != nullptr);
        CHECK(!cb_engine_is_on_actor_thread(eng));
        const QJsonObject snap = parseJsonAndFree(cb_engine_list_books_json(eng));
        CHECK(snap.value(QStringLiteral("bookCount")).toInt() == 0);
        cb_engine_destroy(eng);
    }

    std::printf("  PASS: TtsProcessClient lifecycle + Qt-hosted cb_engine mode\n");
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    std::string ttsBin;
    if (const char *env = std::getenv("CLOTHBOUND_TTS_BIN"); env && env[0] != '\0') {
        ttsBin = env;
    } else if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        ttsBin = argv[1];
    } else if (argc > 0 && argv[0]) {
        const QFileInfo self(QString::fromLocal8Bit(argv[0]));
        ttsBin = (self.absolutePath() + QStringLiteral("/clothbound-tts")).toStdString();
    }

    if (!testBridgeStandaloneActorThread())
        return 1;

    if (ttsBin.empty() || !QFileInfo::exists(QString::fromStdString(ttsBin))) {
        std::fprintf(stderr, "FAIL: clothbound-tts binary not found at '%s' (pass via CLOTHBOUND_TTS_BIN or argv[1])\n",
                     ttsBin.c_str());
        return 1;
    }

    if (!testTtsHelperPipes(ttsBin))
        return 1;

    if (!testTtsProcessClientAndQtHostedBridge(ttsBin))
        return 1;

    if (g_failures != 0)
        return 1;

    std::printf("ALL Phase 0 Bridge + Out-of-Process TTS tests PASSED (0 failures).\n");
    return 0;
}
