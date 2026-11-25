#include "clothbound_tests.h"

#include "ai/aiservices.h"
#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/gemini.h"
#include "ai/keystore.h"
#include "ai/localmodels.h"
#include "ai/passageindex.h"
#include "app/cli.h"
#include "app/portalfilepicker.h"
#include "app/theme.h"
#include "app/translator.h"
#include "fakeserver.h"
#include "formats/bookinfo.h"
#include "formats/bookscheme.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "formats/ziparchive.h"
#include "library/library.h"
#include "library/libraryfinder.h"
#include "library/libraryindex.h"
#include "library/libraryschema.h"
#include "library/marks.h"
#include "library/readingtelemetry.h"
#include "library/scanner.h"
#include "online/epubwriter.h"
#include "online/opdsclient.h"
#include "online/webimporter.h"
#include "reader/reader.h"
#include "testbooks.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
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

void ClothboundTests::libraryStoresBooksAndReadingState() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Marsh, Ada - Lantern Keeper, The (Harbor, 1911).epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY2(library.open(), qPrintable(library.errorString()));
    const int id = library.addBookFile(path);
    QVERIFY2(id > 0, qPrintable(library.errorString()));
    QCOMPARE(library.addBookFile(path), id);
    const BookRecord book = library.record(id);
    QCOMPARE(book.title, QString("The Lantern Keeper"));
    QVERIFY(book.loose);
    QCOMPARE(library.chapterLengths(id).size(), 2);
    QVERIFY(book.length > 0);
    QVERIFY(QFile::exists(dir.filePath("cache/covers/") + book.cover));

    library.savePosition(id, R"({"chapter":0,"offset":12})", 0.4);
    library.setFinished(id, true);
    Library reopened(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(reopened.open());
    QCOMPARE(reopened.record(id).progress, 0.4);
    QVERIFY(reopened.record(id).finishedAt > 0);
    QCOMPARE(reopened.record(id).position, QString(R"({"chapter":0,"offset":12})"));
}

void ClothboundTests::libraryGroupsEditionsIntoWorks() {
    QTemporaryDir dir;
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    for (const char *name : {"Marsh, Ada - Lantern Keeper, The [ed. Reed] (Harbor, 1911).epub",
                             "Marsh, Ada - Lantern Keeper, The [ed. Gray] (Penguin, 2005).epub"}) {
        const QString path = dir.filePath(name);
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        QVERIFY(library.addBookFile(path) > 0);
    }
    QTRY_COMPARE(library.works()->count(), 1);
    QCOMPARE(library.works()->get(0).value("editions").toInt(), 2);
    library.works()->setGrouped(false);
    QCOMPARE(library.works()->count(), 2);
    library.works()->setSearchText("penguin");
    QCOMPARE(library.works()->count(), 1);
}

void ClothboundTests::marksAreKeptAndUndone() {
    QTemporaryDir dir;
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    QVERIFY(TestBooks::write(dir.filePath("book.epub"), TestBooks::epub2(png(20, 30))));
    const int book = library.addBookFile(dir.filePath("book.epub"));
    Marks marks(&library);
    marks.setBook(book);
    QCOMPARE(marks.count(), 0);

    const int later = marks.addHighlight(1, 50, 60, "a later  passage", "green", "Notes");
    const int earlier = marks.addHighlight(0, 10, 20, "an earlier one", "", "Chapter One");
    const int bookmark = marks.addBookmark(0, 5, "The lamp", "Chapter One");
    QVERIFY(later && earlier && bookmark);
    // In the book's order, whatever order they were made in.
    QCOMPARE(marks.data(marks.index(0), Marks::IdRole).toInt(), bookmark);
    QCOMPARE(marks.data(marks.index(1), Marks::IdRole).toInt(), earlier);
    QCOMPARE(marks.data(marks.index(2), Marks::IdRole).toInt(), later);
    QCOMPARE(marks.mark(earlier).value("color").toString(), QString("yellow"));
    QCOMPARE(marks.mark(later).value("text").toString(), QString("a later passage"));

    QCOMPARE(marks.highlightAt(0, 15), earlier);
    QCOMPARE(marks.highlightAt(0, 25), 0);
    QCOMPARE(marks.bookmarkBetween(0, 0, 100), bookmark);
    QCOMPARE(marks.bookmarkBetween(0, 6, 100), 0);
    QCOMPARE(marks.highlightsIn(0).size(), 1);

    marks.setNote(earlier, "  Remember this.  ");
    QCOMPARE(marks.mark(earlier).value("note").toString(), QString("Remember this."));
    QVERIFY(marks.highlightsIn(0).value(0).toMap().value("noted").toBool());
    marks.setColor(earlier, "pink");

    // Undo takes back each step, newest first; redo puts it back.
    QVERIFY(marks.canUndo());
    marks.undo();
    QCOMPARE(marks.mark(earlier).value("color").toString(), QString("yellow"));
    marks.undo();
    QVERIFY(marks.mark(earlier).value("note").toString().isEmpty());
    marks.redo();
    QCOMPARE(marks.mark(earlier).value("note").toString(), QString("Remember this."));

    // A mark taken away comes back with its note, and its id.
    marks.remove(earlier);
    QCOMPARE(marks.count(), 2);
    marks.undo();
    QCOMPARE(marks.count(), 3);
    QCOMPARE(marks.mark(earlier).value("note").toString(), QString("Remember this."));

    // Kept in the library: a new look at the book finds them.
    Marks again(&library);
    again.setBook(book);
    QCOMPARE(again.count(), 3);
    QCOMPARE(again.mark(earlier).value("note").toString(), QString("Remember this."));
    QVERIFY(!again.canUndo());

    const QString notes = marks.markdown("The Lantern Keeper", "Ada Marsh");
    QVERIFY(notes.startsWith("# The Lantern Keeper\n"));
    QVERIFY(notes.contains("## Chapter One"));
    QVERIFY(notes.contains("> an earlier one\n\nRemember this."));
    QVERIFY(notes.indexOf("## Chapter One") < notes.indexOf("## Notes"));
}

void ClothboundTests::searchIgnoresCaseAccentsAndQuotes() {
    const QString text = QString::fromUtf8("The naïve Café — don’t “Look” Ǖ");
    const QString folded = BookSearch::fold(text);
    QCOMPARE(folded.size(), text.size());
    QCOMPARE(folded, QString("the naive cafe - don't \"look\" u"));
    QCOMPARE(BookSearch::matches(folded, BookSearch::fold("NAIVE"), 10).value(0).first, 4);
    QCOMPARE(BookSearch::matches(folded, BookSearch::fold("don't"), 10).size(), 1);
    // Every occurrence, not overlapping, up to the limit.
    QCOMPARE(BookSearch::matches("aaaa", "aa", 10).size(), 2);
    QCOMPARE(BookSearch::matches("ab ab ab", "ab", 2).size(), 2);
}

void ClothboundTests::searchShowsAHitInContext() {
    const QString text = "It was a dark and stormy night; the rain fell in torrents, except at occasional intervals, "
                         "when it was checked by a violent gust of wind which swept up the streets.";
    const qint64 start = text.indexOf("rain");
    const QVariantMap hit = BookSearch::hit(text, 3, start, start + 4);
    QCOMPARE(hit.value("chapter").toInt(), 3);
    QCOMPARE(hit.value("match").toString(), QString("rain"));
    QVERIFY(hit.value("before").toString().endsWith("the "));
    QVERIFY(hit.value("after").toString().startsWith(" fell"));
    QVERIFY(hit.value("after").toString().endsWith(QChar(0x2026)));
    // Cut at words, not inside them.
    QVERIFY(!hit.value("after").toString().contains("swep…"));
}

void ClothboundTests::searchFindsWordsAcrossTheBook() {
    QTemporaryDir dir;
    const QString path = dir.filePath("book.epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int id = library.addBookFile(path);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(id));
    BookSearch search(&reader);

    search.find("STORM");
    QVERIFY(search.searching());
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);
    const QVariantMap storm = search.results().value(0).toMap();
    QCOMPARE(storm.value("chapter").toInt(), 0);
    QCOMPARE(storm.value("match").toString(), QString("storm"));
    QCOMPARE(search.inChapter(0).size(), 1);
    QCOMPARE(search.inChapter(1).size(), 0);

    // "the lamp's" finds "The lamp’s", and the notes chapter is searched too.
    search.find("the lamp's");
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);
    search.find("note");
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.results().value(0).toMap().value("chapter").toInt(), 1);

    // Stepping wraps around.
    search.find("a");
    QCOMPARE(search.count(), 0);  // too short to search
    search.find("in");
    QTRY_VERIFY(!search.searching());
    QVERIFY(search.count() >= 2);
    search.setCurrent(-1);
    QCOMPARE(search.current(), search.count() - 1);
    search.setCurrent(search.count());
    QCOMPARE(search.current(), 0);

    // Another book clears it.
    reader.close();
    QCOMPARE(search.count(), 0);
    QVERIFY(search.query().isEmpty());
}

void ClothboundTests::libraryRelinksMovedFilesByFingerprintAndRollsBackFailedTransactions() {
    QTemporaryDir dir;
    const QString shelf = dir.filePath("shelf");
    QVERIFY(QDir().mkpath(shelf));
    const QString origPath = QDir(shelf).filePath("original.epub");
    QVERIFY(TestBooks::write(origPath, TestBooks::epub2(png(20, 30))));

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    library.addFolder(shelf);
    QTRY_VERIFY(!library.scanning());

    const int id = library.idForPath(origPath);
    QVERIFY(id > 0);
    library.savePosition(id, R"({"chapter":0,"offset":42})", 0.65);
    library.setBookSetting(id, QStringLiteral("voice"), QStringLiteral("sulafat"));

    Marks marks(&library);
    marks.setBook(id);
    const int hl = marks.addHighlight(0, 4, 14, QStringLiteral("lamp light"), QStringLiteral("blue"), QStringLiteral("Ch 1"));
    marks.setNote(hl, QStringLiteral("Keep across move"));
    const int bm = marks.addBookmark(0, 2, QStringLiteral("Chapter One"), QStringLiteral("Ch 1"));
    QVERIFY(hl > 0 && bm > 0);

    // Move and rename the book inside the watched folder, then rescan.
    QVERIFY(QDir(shelf).mkpath("nested"));
    const QString movedPath = QDir(shelf).filePath("nested/renamed.epub");
    QVERIFY(QFile::rename(origPath, movedPath));
    library.rescan();
    QTRY_VERIFY(!library.scanning());

    // Same book_id relinked via SHA-256 content fingerprint; reading state and marks intact.
    QCOMPARE(library.idForPath(movedPath), id);
    QCOMPARE(library.idForPath(origPath), 0);
    const BookRecord relinked = library.record(id);
    QVERIFY(!relinked.missing);
    QCOMPARE(relinked.progress, 0.65);
    QCOMPARE(relinked.position, QString(R"({"chapter":0,"offset":42})"));
    QCOMPARE(library.bookSetting(id, QStringLiteral("voice")), QStringLiteral("sulafat"));

    Marks reloaded(&library);
    reloaded.setBook(id);
    QCOMPARE(reloaded.count(), 2);
    QCOMPARE(reloaded.mark(hl).value("note").toString(), QStringLiteral("Keep across move"));

    // Checked SAVEPOINT rollback on multi-statement write failure (addReadingTime).
    QSqlDatabase db = library.database();
    QSqlQuery trigger(db);
    QVERIFY(trigger.exec(
        QStringLiteral("CREATE TEMP TRIGGER fail_reading_days BEFORE INSERT ON reading_days "
                       "BEGIN SELECT RAISE(ABORT, 'injected failure'); END")));
    library.addReadingTime(id, 180, 1200);
    QCOMPARE(library.record(id).secondsRead, qint64(0));
    {
        QSqlQuery check(db);
        QVERIFY(check.exec(QStringLiteral("SELECT seconds_read FROM books WHERE id = %1").arg(id)));
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toLongLong(), qint64(0));
    }
    QVERIFY(trigger.exec(QStringLiteral("DROP TRIGGER fail_reading_days")));
    library.addReadingTime(id, 180, 1200);
    QCOMPARE(library.record(id).secondsRead, qint64(180));

    // Checked SAVEPOINT rollback on setChapterLengths(): earlier DELETE is rolled back.
    const QList<int> beforeLengths = library.chapterLengths(id);
    QVERIFY(!beforeLengths.isEmpty());
    QVERIFY(trigger.exec(
        QStringLiteral("CREATE TEMP TRIGGER fail_chapter_lengths BEFORE INSERT ON chapter_lengths "
                       "BEGIN SELECT RAISE(ABORT, 'injected failure'); END")));
    library.setChapterLengths(id, {999, 888});
    QCOMPARE(library.chapterLengths(id), beforeLengths);
    QVERIFY(trigger.exec(QStringLiteral("DROP TRIGGER fail_chapter_lengths")));
}

void ClothboundTests::marksMarkdownUsesValidUtf8BookmarkIconAndGuardsClosedDb() {
    QTemporaryDir dir;
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const QString path = dir.filePath("book.epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
    const int id = library.addBookFile(path);
    QVERIFY(id > 0);

    Marks marks(&library);
    // No book selected -> rejected cleanly.
    QCOMPARE(marks.addBookmark(0, 0, QStringLiteral("x"), QStringLiteral("Ch 1")), 0);
    QCOMPARE(marks.addHighlight(0, 0, 5, QStringLiteral("hello"), QStringLiteral("yellow"), QStringLiteral("Ch 1")), 0);

    marks.setBook(id);
    const int bm = marks.addBookmark(0, 1, QStringLiteral("Opening line"), QStringLiteral("Chapter One"));
    const int hl = marks.addHighlight(0, 10, 20, QStringLiteral("Glowing lamp"), QStringLiteral("green"), QStringLiteral("Chapter One"));
    marks.setNote(hl, QStringLiteral("Important motif"));
    Mark multi;
    multi.kind = QStringLiteral("highlight");
    multi.chapter = 0;
    multi.start = 30;
    multi.end = 50;
    multi.text = QStringLiteral("First line\nSecond line");
    multi.color = QStringLiteral("yellow");
    multi.label = QStringLiteral("Chapter One");
    const int mId = marks.insert(multi);
    QVERIFY(bm > 0 && hl > 0 && mId > 0);

    const QString md = marks.markdown(QStringLiteral("The Lantern"), QStringLiteral("Ada Marsh"));
    // Valid 4-byte UTF-8 U+1F516 bookmark emoji (not truncated BMP U+F516).
    QVERIFY2(md.contains(QStringLiteral("- \U0001F516 Opening line")), qPrintable(md));
    QVERIFY(md.toUtf8().contains("\xF0\x9F\x94\x96"));
    QVERIFY(!md.contains(QChar(0xF516)));
    QVERIFY(md.contains(QStringLiteral("> First line\n> Second line")));

    QCOMPARE(marks.filtered(QStringLiteral("bookmarks")).size(), 1);
    QCOMPARE(marks.filtered(QStringLiteral("highlights")).size(), 2);
    QCOMPARE(marks.filtered(QStringLiteral("notes")).size(), 1);
    QCOMPARE(marks.filtered(QStringLiteral("all")).size(), 3);
}

void ClothboundTests::bookSearchNormalizesPdfAndEpubTextAllowsSingleCjkAndCancelsMidIndex() {
    QTemporaryDir dir;
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());

    // 1. EPUB with indented multi-line phrase, soft hyphen, ligature, and CJK.
    const QString epubPath = dir.filePath("norm.epub");
    const QByteArray normOpf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\"><metadata><dc:title>Norm</dc:title></metadata>\n"
        "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
        "<spine><itemref idref=\"c1\"/></spine></package>";
    const QByteArray normBytes = TestBooks::epub({
        {"OEBPS/content.opf", normOpf},
        {"OEBPS/c1.xhtml",
         TestBooks::chapter("C1", "<p>across\n      indented   lines</p>"
                                  "<p>of&#xFB01;ce lan&#xAD;tern</p>"
                                  "<p>\xE6\xA3\xAE\xE3\x81\xAE\xE4\xB8\xAD\xE3\x81\xAE\xE6\x9C\x88</p>")},
    });
    QVERIFY(TestBooks::write(epubPath, normBytes));
    const int epubId = library.addBookFile(epubPath);
    QVERIFY(epubId > 0);

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(epubId));
    BookSearch search(&reader);

    search.find(QStringLiteral("across indented lines"));
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);
    const QVariantList spans = search.inChapter(0);
    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans[0].toMap().value("chapter").toInt(), 0);

    search.find(QStringLiteral("office lantern"));
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);

    // Single-character CJK searches work; single-character Latin is ignored.
    search.find(QString::fromUtf8("\xE6\x9C\x88"));
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);
    search.find(QStringLiteral("o"));
    QCOMPARE(search.count(), 0);

    // 2. PDF with \r\n line break, PDFium U+FFFE line-break hyphen, and fi ligature.
    const QString pdfPath = dir.filePath("search.pdf");
    {
        QByteArray out = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        auto obj = [&](const QByteArray &body) {
            offsets.append(out.size());
            out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
        };
        obj("<< /Type /Catalog /Pages 2 0 R >>");
        obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
        const QByteArray stream =
            "BT /F1 12 Tf 20 350 Td (silver) Tj 0 -20 Td (lantern) Tj 0 -20 Td (of\\200ce) Tj 0 -20 Td (recon-) Tj 0 -20 Td (dite) Tj ET";
        obj("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
        obj("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [128 /fi] >> >>");
        const qsizetype xref = out.size();
        out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
        for (const qsizetype offset : offsets)
            out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
        out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        QVERIFY(TestBooks::write(pdfPath, out));
    }
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(pdfId > 0);
    QVERIFY(reader.openBook(pdfId));

    search.find(QStringLiteral("silver lantern"));
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);
    const QVariantList pdfSpans = search.inChapter(0);
    QCOMPARE(pdfSpans.size(), 1);
    QCOMPARE(pdfSpans[0].toMap().value("chapter").toInt(), 0);

    search.find(QStringLiteral("office"));
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);

    search.find(QStringLiteral("recondite"));
    QTRY_VERIFY(!search.searching());
    QCOMPARE(search.count(), 1);

    const QVariantMap hyphHit = BookSearch::hit(QStringLiteral("pre recon\uFFFE\r\ndite post"), 2, 4, 16);
    QCOMPARE(hyphHit.value("match").toString(), QStringLiteral("recondite"));
    QCOMPARE(hyphHit.value("chapter").toInt(), 2);

    // 3. Mid-index semantic cancel: switching books while Book A is indexing never leaks results.
    QTcpServer slowServer;
    QVERIFY(slowServer.listen(QHostAddress::LocalHost));
    QList<QPointer<QTcpSocket>> heldSockets;
    int embedRequests = 0;
    QObject::connect(&slowServer, &QTcpServer::newConnection, &slowServer, [&] {
        while (QTcpSocket *sock = slowServer.nextPendingConnection()) {
            auto buf = std::make_shared<QByteArray>();
            QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, &heldSockets, &embedRequests] {
                *buf += sock->readAll();
                if (buf->contains("\r\n\r\n")) {
                    ++embedRequests;
                    heldSockets.append(sock);
                }
            });
        }
    });

    qunsetenv("GEMINI_API_KEY");
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    services.setLocalModels(&local);
    LocalModels::Server where;
    where.kind = LocalModels::Ollama;
    where.url = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(slowServer.serverPort()));
    local.configure(where, QStringLiteral("qwen"), QStringLiteral("embed"));
    services.setPreset(QStringLiteral("local"));

    search.setMeaning(&services, library.database().connectionName());
    search.setByMeaning(true);
    QVERIFY(reader.openBook(epubId));
    search.find(QStringLiteral("indented lantern"));
    QTRY_VERIFY_WITH_TIMEOUT(embedRequests >= 1, 5000);

    // Switch to the PDF mid-index; Book A's in-flight embed must be cancelled cleanly.
    QVERIFY(reader.openBook(pdfId));
    for (const QPointer<QTcpSocket> &sock : heldSockets) {
        if (sock && sock->state() == QAbstractSocket::ConnectedState) {
            const QByteArray body = "{\"embeddings\":[[1.0,0.0]]}";
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            sock->disconnectFromHost();
        }
    }
    QTest::qWait(80);
    QCOMPARE(search.count(), 0);
    QVERIFY(!search.searching());
    QVERIFY(search.preparing().isEmpty());
    QVERIFY(search.query().isEmpty());
    services.setPreset(QStringLiteral("cloud"));
    QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini", QSettings::IniFormat)
        .remove("ai/preset");
}

void ClothboundTests::meaningSearchSpansTheLibrary() {
    QTemporaryDir dir;
    const QString path = dir.filePath("library.sqlite");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("meaning-seed"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE passage_vectors (book_id INTEGER NOT NULL, model TEXT NOT NULL, "
                                      "passage TEXT NOT NULL, vector BLOB NOT NULL, PRIMARY KEY (book_id, model, passage))")));
        auto add = [&](int book, const QString &model, const QString &key, QList<float> v) {
            q.prepare(QStringLiteral("INSERT INTO passage_vectors VALUES (?, ?, ?, ?)"));
            q.addBindValue(book);
            q.addBindValue(model);
            q.addBindValue(key);
            q.addBindValue(QByteArray(reinterpret_cast<const char *>(v.constData()), v.size() * qsizetype(sizeof(float))));
            QVERIFY(q.exec());
        };
        add(1, "m", "2:100:400:aa", {1, 0, 0});       // exactly what's asked
        add(1, "m", "2:400:700:bb", {0.9f, 0.1f, 0}); // the same chapter again
        add(2, "m", "5:0:300:cc", {0.7f, 0.7f, 0});   // another book, close
        add(3, "m", "1:0:300:dd", {0, 0, 1});         // unrelated
        add(4, "other", "1:0:300:ee", {1, 0, 0});     // another embedder's vectors
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("meaning-seed"));

    const auto hits = PassageIndex::rankLibrary(path, "m", {1, 0, 0}, 5);
    QCOMPARE(hits.size(), 2);
    QCOMPARE(hits[0].bookId, 1);
    QCOMPARE(hits[0].chapter, 2);
    QCOMPARE(hits[0].start, qint64(100));
    QCOMPARE(hits[0].end, qint64(400));
    QCOMPARE(hits[1].bookId, 2);
    QVERIFY(hits[0].score > hits[1].score);
    QVERIFY(PassageIndex::rankLibrary(path, "missing", {1, 0, 0}, 5).isEmpty());
    QCOMPARE(PassageIndex::rankLibrary(path, "m", {1, 0, 0}, 1).size(), 1);
}

void ClothboundTests::libraryIndexFindsWordsAcrossBooks() {
    QTemporaryDir dir;
    const QString epubPath = dir.filePath("book.epub");
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));
    QCOMPARE(LibraryIndex::matchExpression(QStringLiteral("time  trav")), QStringLiteral("\"time\" \"trav\"*"));
    QCOMPARE(LibraryIndex::matchExpression(QStringLiteral("\" OR NEAR(")), QStringLiteral("\"OR\" \"NEAR\"*"));
    QCOMPARE(LibraryIndex::matchExpression(QStringLiteral("—")), QString());

    LibraryIndex index(dir.filePath("index.sqlite"));
    if (!index.available())
        QSKIP("SQLite without FTS5");
    QVERIFY(LibraryIndex::indexBook(dir.filePath("index.sqlite"), {7, epubPath, "epub", 100}));
    EpubBook epub(epubPath);
    QVERIFY(epub.open());
    // A word from the book's text is found, with where it is.
    QString word;
    for (int c = 0; c < int(epub.spine().size()) && word.isEmpty(); ++c) {
        const auto m = QRegularExpression(QStringLiteral("\\b\\p{L}{6,}\\b")).match(epub.chapterText(c));
        if (m.hasMatch())
            word = m.captured(0);
    }
    QVERIFY(!word.isEmpty());
    const QVariantList hits = index.search(word.left(4));
    QVERIFY(!hits.isEmpty());
    const QVariantMap hit = hits.first().toMap();
    QCOMPARE(hit.value("bookId").toInt(), 7);
    const QString chapterText = epub.chapterText(hit.value("chapter").toInt());
    QVERIFY(chapterText.mid(qint64(hit.value("offset").toDouble()), 4).compare(word.left(4), Qt::CaseInsensitive) == 0);
    QVERIFY(hit.value("snippet").toString().contains(QStringLiteral("‹")));
    QCOMPARE(index.bookCount(), 1);
    QVERIFY(index.search(QStringLiteral("zzqxv")).isEmpty());

    // A book no longer in the library leaves the index.
    index.update({});
    QCOMPARE(index.bookCount(), 0);
    QVERIFY(index.search(word.left(4)).isEmpty());

    // Accents don't matter: "cafe" finds "café".
    QVERIFY(LibraryIndex::indexBook(dir.filePath("index.sqlite"), {8, epubPath, "epub", 100}));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("idx-accents"));
        db.setDatabaseName(dir.filePath("index.sqlite"));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("INSERT INTO chapter_text (body, book, chapter) VALUES ('Un café noir, s''il vous plaît.', 8, 99)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("idx-accents"));
    const QVariantList accents = index.search(QStringLiteral("cafe noir"));
    QVERIFY(!accents.isEmpty());
    QCOMPARE(accents.first().toMap().value("chapter").toInt(), 99);
    QCOMPARE(qint64(accents.first().toMap().value("offset").toDouble()), qint64(3));
}

void ClothboundTests::libraryFinderOffersFoldersWithBooks() {
    QTemporaryDir dir;
    const QString home = dir.path();
    // ~/Books holds three books (one too deep to count), Documents one,
    // Downloads none, and ~/Library is already watched.
    for (const char *rawPath : {"Books/a.epub", "Books/Wells/b.EPUB", "Books/c.pdf", "Books/1/2/3/4/5/deep.epub",
                                "Books/notes.txt", "Documents/d.epub", "Downloads/e.zip", "Library/f.epub"}) {
        const QString path = QString::fromLatin1(rawPath);
        QVERIFY(QDir().mkpath(QFileInfo(home + "/" + path).absolutePath()));
        write(home + "/" + path, "x");
    }
    const QStringList places = LibraryFinder::candidates(home, home + "/Documents", home + "/Downloads");
    QVERIFY(places.contains(home + "/Books"));
    QVERIFY(!places.contains(home + "/books") || QFileInfo(home + "/books").exists());
    QCOMPARE(LibraryFinder::countBooks(home + "/Books"), 3);
    QCOMPARE(LibraryFinder::countBooks(home + "/Books", 4, 2), 2);

    const QVariantList found = LibraryFinder::search(places + QStringList{home + "/Library"},
                                                     {home + "/Library"}, home);
    QCOMPARE(found.size(), 2);
    QCOMPARE(found[0].toMap().value("label").toString(), QStringLiteral("~/Books"));
    QCOMPARE(found[0].toMap().value("count").toInt(), 3);
    QCOMPARE(found[1].toMap().value("label").toString(), QStringLiteral("~/Documents"));
    // A folder already watched, or one inside it, is never offered again.
    QCOMPARE(LibraryFinder::search(places, {home + "/Books"}, home).size(), 1);
    QCOMPARE(LibraryFinder::label("/srv/books", home), QStringLiteral("/srv/books"));
}

void ClothboundTests::libraryShelvesWantToReadListeningAndChapter() {
    QTemporaryDir dir;
    const QString dataDir = dir.filePath("data");
    QVERIFY(QDir().mkpath(dataDir));
    // Four books written straight into a fresh library, then read back.
    {
        Library seed(dataDir, dir.filePath("cache"));
        QVERIFY2(seed.open(), qPrintable(seed.errorString()));
        QSqlQuery insert(seed.database());
        for (int id = 1; id <= 4; ++id)
            QVERIFY(insert.exec(QStringLiteral("INSERT INTO books (id, path, format, title, sort_title, work_key, added_at) "
                                               "VALUES (%1, '/b/%1.epub', 'epub', 'Book %1', 'book %1', 'w%1', 100)")
                                    .arg(id)));
        // Book 2 is under way in Chapter III; book 3 is finished.
        QVERIFY(insert.exec(QStringLiteral("UPDATE books SET opened_at = 500, progress = 0.2, position = "
                                           "'{\"chapter\":3,\"offset\":10,\"title\":\"Chapter III\"}' WHERE id = 2")));
        QVERIFY(insert.exec(QStringLiteral("UPDATE books SET opened_at = 400, finished_at = 600, "
                                           "fingerprint = 'fp3' WHERE id = 3")));
        // Back to a version 3 library, as an existing install has it.
        QVERIFY(insert.exec(QStringLiteral("ALTER TABLE books DROP COLUMN want_at")));
        QVERIFY(insert.exec(QStringLiteral("ALTER TABLE books DROP COLUMN listened_at")));
        QVERIFY(insert.exec(QStringLiteral("PRAGMA user_version = 3")));
    }
    Library library(dataDir, dir.filePath("cache"));
    QVERIFY2(library.open(), qPrintable(library.errorString()));
    QCOMPARE(library.schemaVersion(), LibrarySchema::kLatestVersion);
    QCOMPARE(library.bookCount(), 4);
    QCOMPARE(library.record(3).title, QStringLiteral("Book 3"));

    library.setWantToRead(1, true);
    library.setWantToRead(3, true);  // finished books never sit on Want to read
    library.markListened(2);
    library.markListened(2);
    const QVariantMap counts = library.shelfCounts();
    QCOMPARE(counts.value("all").toInt(), 4);
    QCOMPARE(counts.value("reading").toInt(), 1);
    QCOMPARE(counts.value("finished").toInt(), 1);
    QCOMPARE(counts.value("want").toInt(), 1);
    QCOMPARE(counts.value("listening").toInt(), 1);
    QCOMPARE(library.book(1).value("wantToRead").toBool(), true);
    QCOMPARE(library.book(3).value("wantToRead").toBool(), false);
    QCOMPARE(library.book(2).value("chapterTitle").toString(), QStringLiteral("Chapter III"));
    QCOMPARE(library.book(1).value("chapterTitle").toString(), QString());

    library.works()->setStatusFilter(QStringLiteral("want"));
    QTRY_COMPARE(library.works()->rowCount(), 1);
    QCOMPARE(library.works()->get(0).value("bookId").toInt(), 1);
    library.works()->setStatusFilter(QStringLiteral("listening"));
    QTRY_COMPARE(library.works()->rowCount(), 1);
    QCOMPARE(library.works()->get(0).value("bookId").toInt(), 2);

    // Kept across a restart.
    Library reopened(dataDir, dir.filePath("cache"));
    QVERIFY(reopened.open());
    QVERIFY(reopened.record(1).wantAt > 0);
    QVERIFY(reopened.record(2).listenedAt > 0);
    reopened.setWantToRead(1, false);
    QCOMPARE(reopened.shelfCounts().value("want").toInt(), 0);
}

void ClothboundTests::librarySchemaMigrationsCollectionsStatsAndSeriesSort() {
    QTemporaryDir dir;
    const QString dataDir = dir.filePath("data");
    const QString cacheDir = dir.filePath("cache");
    QVERIFY(QDir().mkpath(dataDir));
    const QString dbPath = dataDir + QStringLiteral("/library.sqlite");

    // 1. Simulate a pre-v1 (user_version = 0) legacy database without fingerprint/collections/prefix/suffix.
    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-v0"));
        legacy.setDatabaseName(dbPath);
        QVERIFY(legacy.open());
        QSqlQuery q(legacy);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")));
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE folders (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, added_at INTEGER NOT NULL)")));
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE books ("
            " id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, format TEXT NOT NULL,"
            " size INTEGER NOT NULL DEFAULT 0, mtime INTEGER NOT NULL DEFAULT 0,"
            " title TEXT NOT NULL DEFAULT '', sort_title TEXT NOT NULL DEFAULT '',"
            " author TEXT NOT NULL DEFAULT '', sort_author TEXT NOT NULL DEFAULT '',"
            " editor TEXT NOT NULL DEFAULT '', publisher TEXT NOT NULL DEFAULT '', year INTEGER NOT NULL DEFAULT 0,"
            " language TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '', subjects TEXT NOT NULL DEFAULT '',"
            " isbn TEXT NOT NULL DEFAULT '', series TEXT NOT NULL DEFAULT '', series_index REAL NOT NULL DEFAULT 0,"
            " work_key TEXT NOT NULL DEFAULT '', chapters INTEGER NOT NULL DEFAULT 0, pages INTEGER NOT NULL DEFAULT 0,"
            " length INTEGER NOT NULL DEFAULT 0, cover TEXT NOT NULL DEFAULT '', problem TEXT NOT NULL DEFAULT '',"
            " added_at INTEGER NOT NULL DEFAULT 0, opened_at INTEGER NOT NULL DEFAULT 0,"
            " finished_at INTEGER NOT NULL DEFAULT 0, progress REAL NOT NULL DEFAULT 0, position TEXT NOT NULL DEFAULT '',"
            " seconds_read INTEGER NOT NULL DEFAULT 0, missing INTEGER NOT NULL DEFAULT 0,"
            " hidden INTEGER NOT NULL DEFAULT 0, loose INTEGER NOT NULL DEFAULT 0)")));
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE annotations ("
            " id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, kind TEXT NOT NULL,"
            " chapter INTEGER NOT NULL DEFAULT 0, start INTEGER NOT NULL DEFAULT 0, finish INTEGER NOT NULL DEFAULT 0,"
            " text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '',"
            " label TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO books (id, path, format, title, sort_title, author, sort_author, work_key, added_at) "
            "VALUES (1, '/legacy/book.epub', 'epub', 'Legacy Book', 'legacy book', 'Old Author', 'author, old', 'wk1', 100)")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color, label, created_at, updated_at) "
            "VALUES (1, 1, 'highlight', 0, 5, 15, 'kept quote', 'kept note', 'yellow', 'Ch 1', 100, 100)")));
        QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 0")));
        legacy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy-v0"));

    // Opening migrates v0 -> v1 -> v2 -> v3 -> v4 cleanly while preserving legacy rows.
    {
        Library migrated(dataDir, cacheDir);
        QVERIFY2(migrated.open(), qPrintable(migrated.errorString()));
        QCOMPARE(migrated.schemaVersion(), LibrarySchema::kLatestVersion);
        QCOMPARE(migrated.record(1).title, QStringLiteral("Legacy Book"));
        QCOMPARE(migrated.record(1).wantAt, qint64(0));
        QCOMPARE(migrated.record(1).listenedAt, qint64(0));

        auto hasCol = [&](const QString &table, const QString &col) {
            QSqlQuery info(migrated.database());
            if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
                return false;
            while (info.next())
                if (info.value(1).toString() == col)
                    return true;
            return false;
        };
        QVERIFY(hasCol(QStringLiteral("books"), QStringLiteral("fingerprint")));
        QVERIFY(hasCol(QStringLiteral("annotations"), QStringLiteral("prefix")));
        QVERIFY(hasCol(QStringLiteral("annotations"), QStringLiteral("suffix")));

        QSqlQuery ann(migrated.database());
        QVERIFY(ann.exec(QStringLiteral("SELECT text, note, prefix, suffix FROM annotations WHERE id = 1")));
        QVERIFY(ann.next());
        QCOMPARE(ann.value(0).toString(), QStringLiteral("kept quote"));
        QCOMPARE(ann.value(1).toString(), QStringLiteral("kept note"));
        QCOMPARE(ann.value(2).toString(), QString());
        QCOMPARE(ann.value(3).toString(), QString());
    }

    // 2. Collections CRUD, readingStats(), and WorksModel series sort on a fresh Library.
    QTemporaryDir freshDir;
    Library library(freshDir.filePath("data"), freshDir.filePath("cache"));
    QVERIFY(library.open());
    QCOMPARE(library.schemaVersion(), LibrarySchema::kLatestVersion);

    auto makeSeriesEpub = [&](const QString &fileName, const QString &title, const QString &author,
                              const QString &series, double index) {
        const QString path = freshDir.filePath(fileName);
        const QString seriesMeta =
            series.isEmpty()
                ? QString()
                : QStringLiteral("<meta name=\"calibre:series\" content=\"%1\"/>"
                                 "<meta name=\"calibre:series_index\" content=\"%2\"/>")
                      .arg(series)
                      .arg(index);
        const QByteArray opf =
            QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
                "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                "    <dc:title>%1</dc:title><dc:creator>%2</dc:creator><dc:language>en</dc:language>\n"
                "    %3\n"
                "  </metadata>\n"
                "  <manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
                "  <spine><itemref idref=\"c1\"/></spine>\n"
                "</package>")
                .arg(title, author, seriesMeta)
                .toUtf8();
        const bool ok = TestBooks::write(path, TestBooks::epub({
                                                   {"OEBPS/content.opf", opf},
                                                   {"OEBPS/c1.xhtml", TestBooks::chapter("Ch 1", "<p>Text.</p>")},
                                               }));
        return ok ? library.addBookFile(path) : 0;
    };

    const int bDune2 = makeSeriesEpub("d2.epub", "Dune Messiah", "Frank Herbert", "Dune", 2.0);
    const int bDune1 = makeSeriesEpub("d1.epub", "Dune", "Frank Herbert", "Dune", 1.0);
    const int bAlpha1 = makeSeriesEpub("a1.epub", "Alpha Rising", "Cara Lin", "Alpha", 1.0);
    const int bStandalone = makeSeriesEpub("z1.epub", "Zeta Standalone", "Zed Author", "", 0.0);
    QVERIFY(bDune2 > 0 && bDune1 > 0 && bAlpha1 > 0 && bStandalone > 0);

    // Series sort places series books alphabetically by series then ascending by seriesIndex, standalones last.
    library.works()->setSortOrder(QStringLiteral("series"));
    QTRY_COMPARE(library.works()->count(), 4);
    QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), bAlpha1);
    QCOMPARE(library.works()->get(1).value(QStringLiteral("bookId")).toInt(), bDune1);
    QCOMPARE(library.works()->get(2).value(QStringLiteral("bookId")).toInt(), bDune2);
    QCOMPARE(library.works()->get(3).value(QStringLiteral("bookId")).toInt(), bStandalone);
    QCOMPARE(library.works()->seriesList(), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Dune")}));

    library.works()->setSeriesFilter(QStringLiteral("Dune"));
    QCOMPARE(library.works()->count(), 2);
    QVERIFY(library.works()->hasActiveFilters());
    library.works()->clearFilters();
    QCOMPARE(library.works()->count(), 4);

    // Collections CRUD + case-insensitive dedup + WorksModel filter + delete reset.
    QCOMPARE(library.createCollection(QStringLiteral("   ")), 0);
    const int colSciFi = library.createCollection(QStringLiteral("Sci-Fi"));
    QVERIFY(colSciFi > 0);
    QCOMPARE(library.createCollection(QStringLiteral("sci-fi")), colSciFi);
    const int colFav = library.createCollection(QStringLiteral("Favorites"));
    QVERIFY(colFav > 0 && colFav != colSciFi);

    QVERIFY(library.addToCollection(bDune1, colSciFi));
    QVERIFY(library.addToCollection(bDune2, colSciFi));
    QVERIFY(library.isInCollection(bDune1, colSciFi));
    QCOMPARE(library.collectionsForBook(bDune1).size(), 1);

    library.works()->setCollectionId(colSciFi);
    QCOMPARE(library.works()->count(), 2);

    QVERIFY(!library.renameCollection(colSciFi, QStringLiteral("favorites")));  // duplicate conflict rejected
    QVERIFY(library.renameCollection(colSciFi, QStringLiteral("Speculative")));
    QCOMPARE(library.collectionsForBook(bDune1).first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Speculative"));

    QVERIFY(library.removeFromCollection(bDune2, colSciFi));
    QVERIFY(!library.isInCollection(bDune2, colSciFi));
    QCOMPARE(library.works()->count(), 1);

    QVERIFY(library.deleteCollection(colSciFi));
    QCOMPARE(library.works()->collectionId(), 0);
    QCOMPARE(library.works()->count(), 4);
    QVERIFY(library.collectionsForBook(bDune1).isEmpty());

    // readingStats() aggregates book statuses, streaks, 14-day history, and bookReadingStats().
    library.setFinished(bDune1, true);
    library.markOpened(bDune2);
    library.addReadingTime(bDune1, 600, 12000);  // today: 600s
    const QDate today = QDate::currentDate();
    auto addDay = [&](int daysAgo, int sec) {
        QSqlQuery ins(library.database());
        ins.prepare(QStringLiteral("INSERT OR REPLACE INTO reading_days (book_id, day, seconds) VALUES (?, ?, ?)"));
        ins.addBindValue(bDune2);
        ins.addBindValue(today.addDays(-daysAgo).toString(Qt::ISODate));
        ins.addBindValue(sec);
        QVERIFY(ins.exec());
    };
    // Current 3-day streak (today, -1, -2) and an older 4-day longest streak (-10..-7).
    addDay(1, 300);
    addDay(2, 200);
    addDay(7, 100);
    addDay(8, 100);
    addDay(9, 100);
    addDay(10, 100);

    const QVariantMap stats = library.readingStats();
    QCOMPARE(stats.value(QStringLiteral("totalBooks")).toInt(), 4);
    QCOMPARE(stats.value(QStringLiteral("finishedBooks")).toInt(), 1);
    QCOMPARE(stats.value(QStringLiteral("readingBooks")).toInt(), 1);
    QCOMPARE(stats.value(QStringLiteral("unreadBooks")).toInt(), 2);
    QCOMPARE(stats.value(QStringLiteral("todaySeconds")).toLongLong(), qint64(600));
    QCOMPARE(stats.value(QStringLiteral("weekSeconds")).toLongLong(), qint64(1100));
    QCOMPARE(stats.value(QStringLiteral("streakDays")).toInt(), 3);
    QCOMPARE(stats.value(QStringLiteral("longestStreak")).toInt(), 4);
    QCOMPARE(stats.value(QStringLiteral("activeDays")).toInt(), 7);
    const QVariantList recent = stats.value(QStringLiteral("recentDays")).toList();
    QCOMPARE(recent.size(), 14);
    QCOMPARE(recent.last().toMap().value(QStringLiteral("seconds")).toLongLong(), qint64(600));

    const QVariantMap bookStats = library.bookReadingStats(bDune1);
    QCOMPARE(bookStats.value(QStringLiteral("secondsRead")).toLongLong(), qint64(600));
    QVERIFY(bookStats.value(QStringLiteral("daysRead")).toInt() >= 1);
}

void ClothboundTests::marksW3cQuoteHealingCrossBookSearchAndObsidianExport() {
    // 1. Marks::locateQuote() disambiguates repeated phrases by W3C prefix/suffix and handles whitespace drift.
    const QString chapter = QStringLiteral("At dawn the harbor slept quietly. By noon the harbor roared with wind.");
    const qint64 secondHarbor = chapter.indexOf(QStringLiteral("the harbor"), 15);
    QVERIFY(secondHarbor > 15);
    const auto disamb = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                           QStringLiteral("By noon "), QStringLiteral(" roared"), 0);
    QCOMPARE(disamb.first, secondHarbor);
    QCOMPARE(disamb.second, secondHarbor + 10);

    const QString wrappedChapter = QStringLiteral("Intro text.\nThe\n  harbor   roared\tloudly.");
    const auto wsHit = Marks::locateQuote(wrappedChapter, QStringLiteral("The harbor roared"),
                                          QStringLiteral("Intro text."), QStringLiteral(" loudly"), -1);
    QVERIFY(wsHit.first >= 0);
    QCOMPARE(wrappedChapter.mid(wsHit.first, wsHit.second - wsHit.first),
             QStringLiteral("The\n  harbor   roared"));
    QCOMPARE(Marks::locateQuote(chapter, QStringLiteral("nonexistent phrase"), {}, {}, 0),
             (QPair<qint64, qint64>{-1, -1}));

    // 2. healChapter(), searchAll(), and Obsidian/Readwise exportMarkdownToFile().
    QTemporaryDir dir;
    const QString epub1 = dir.filePath("b1.epub");
    const QString epub2 = dir.filePath("b2.epub");
    QVERIFY(TestBooks::write(epub1, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epub2, TestBooks::epub3(adobeObfuscate(fontPayload()))));

    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int id1 = library.addBookFile(epub1);
    const int id2 = library.addBookFile(epub2);
    QVERIFY(id1 > 0 && id2 > 0);

    Marks marks(&library);
    marks.setBook(id1);
    const int h1 = marks.addHighlight(0, 0, 10, QStringLiteral("the harbor"), QStringLiteral("blue"),
                                      QStringLiteral("Ch. 1"), QStringLiteral("By noon "),
                                      QStringLiteral(" roared"));
    const int b1 = marks.addBookmark(0, 0, QStringLiteral("the harbor"), QStringLiteral("p. 1"),
                                     QStringLiteral("At dawn "), QStringLiteral(" slept"));
    marks.setNote(h1, QStringLiteral("Great café atmosphere"));

    // resolveOffset() detects drift before healing.
    const QVariantMap resolved = marks.resolveOffset(h1, chapter);
    QVERIFY(resolved.value(QStringLiteral("found")).toBool());
    QVERIFY(resolved.value(QStringLiteral("drifted")).toBool());
    QCOMPARE(resolved.value(QStringLiteral("start")).toLongLong(), secondHarbor);
    QCOMPARE(resolved.value(QStringLiteral("end")).toLongLong(), secondHarbor + 10);

    // healChapter() fixes both highlight and bookmark, persists to SQLite, and is idempotent.
    QCOMPARE(marks.healChapter(0, chapter), 2);
    QCOMPARE(marks.healChapter(0, chapter), 0);
    QCOMPARE(marks.mark(h1).value(QStringLiteral("start")).toLongLong(), secondHarbor);
    QCOMPARE(marks.mark(h1).value(QStringLiteral("end")).toLongLong(), secondHarbor + 10);
    const qint64 firstHarbor = chapter.indexOf(QStringLiteral("the harbor"));
    QCOMPARE(marks.mark(b1).value(QStringLiteral("start")).toLongLong(), firstHarbor);
    QCOMPARE(marks.mark(b1).value(QStringLiteral("end")).toLongLong(), firstHarbor);

    // Reloading Marks verifies healed offsets persisted in SQLite.
    {
        Marks reloaded(&library);
        reloaded.setBook(id1);
        QCOMPARE(reloaded.mark(h1).value(QStringLiteral("start")).toLongLong(), secondHarbor);
        QCOMPARE(reloaded.mark(b1).value(QStringLiteral("start")).toLongLong(), firstHarbor);
    }

    // Add a mark in Book 2 for cross-book searchAll().
    marks.setBook(id2);
    const int h2 = marks.addHighlight(0, 0, 12, QStringLiteral("Café on salt"), QStringLiteral("green"),
                                      QStringLiteral("p. 7"));
    QVERIFY(h2 > 0);
    marks.setBook(id1);

    // Accent-folded searchAll("cafe") returns hits from both books and prioritizes the active book + note match.
    const QVariantList foundBoth = marks.searchAll(QStringLiteral("cafe"));
    QCOMPARE(foundBoth.size(), 2);
    QCOMPARE(foundBoth.first().toMap().value(QStringLiteral("bookId")).toInt(), id1);
    QCOMPARE(foundBoth.last().toMap().value(QStringLiteral("bookId")).toInt(), id2);

    // Hidden books are excluded from searchAll().
    library.setHidden(id2, true);
    const QVariantList foundVisible = marks.searchAll(QStringLiteral("cafe"));
    QCOMPARE(foundVisible.size(), 1);
    QCOMPARE(foundVisible.first().toMap().value(QStringLiteral("bookId")).toInt(), id1);

    // Obsidian & Readwise exportMarkdownToFile() writes YAML frontmatter and metadata atomically.
    const QString obsidianPath = dir.filePath("exports/notes-obsidian.md");
    const QString writtenObs = marks.exportMarkdownToFile(
        QUrl::fromLocalFile(obsidianPath).toString(), QStringLiteral("The \"Lantern\""),
        QStringLiteral("Ada Marsh"), QStringLiteral("obsidian"));
    QCOMPARE(writtenObs, obsidianPath);
    QFile obsFile(obsidianPath);
    QVERIFY(obsFile.open(QIODevice::ReadOnly));
    const QString obsContent = QString::fromUtf8(obsFile.readAll());
    QVERIFY(obsContent.startsWith(QStringLiteral("---\n")));
    QVERIFY(obsContent.contains(QStringLiteral("title: \"The \\\"Lantern\\\"\"\n")));
    QVERIFY(obsContent.contains(QStringLiteral("author: \"Ada Marsh\"\n")));
    QVERIFY(obsContent.contains(QStringLiteral("tags:\n  - clothbound\n  - reading-notes\n")));
    QVERIFY(obsContent.contains(QStringLiteral("> the harbor\n")));
    QVERIFY(obsContent.contains(QStringLiteral("Great café atmosphere\n")));
    QVERIFY(obsContent.contains(QStringLiteral("— *Blue · Ch. 1 · ")));

    const QString readwisePath = dir.filePath("exports/notes-readwise.md");
    QCOMPARE(marks.exportMarkdownToFile(readwisePath, QStringLiteral("Lantern"), QStringLiteral("Ada Marsh"),
                                        QStringLiteral("readwise")),
             readwisePath);
    QFile rwFile(readwisePath);
    QVERIFY(rwFile.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(rwFile.readAll()).startsWith(QStringLiteral("---\n")));

    const QString stdPath = dir.filePath("exports/notes-standard.md");
    QCOMPARE(marks.exportMarkdownToFile(stdPath, QStringLiteral("Lantern"), QStringLiteral("Ada Marsh"),
                                        QStringLiteral("standard")),
             stdPath);
    QFile stdFile(stdPath);
    QVERIFY(stdFile.open(QIODevice::ReadOnly));
    const QString stdContent = QString::fromUtf8(stdFile.readAll());
    QVERIFY(!stdContent.startsWith(QStringLiteral("---\n")));
    QVERIFY(stdContent.startsWith(QStringLiteral("# Lantern\n")));
}

void ClothboundTests::wave2LibraryCancelScanAudioShelvesFinderAndIndexScoping() {
    QTemporaryDir dir;
    const QString dataDir = dir.filePath("data");
    const QString cacheDir = dir.filePath("cache");
    const QString booksDir = dir.filePath("books");
    QVERIFY(QDir().mkpath(booksDir));

    const QString epubA = booksDir + QStringLiteral("/alpha.epub");
    const QString epubB = booksDir + QStringLiteral("/beta.epub");
    const QString pdfPath = booksDir + QStringLiteral("/manual.pdf");
    QVERIFY(TestBooks::write(epubA, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epubB, TestBooks::epub3(adobeObfuscate(fontPayload()))));
    QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Grid Systems", "Josef")));

    Library library(dataDir, cacheDir);
    QVERIFY2(library.open(), qPrintable(library.errorString()));

    // 1. cancelScan(): safe no-op while idle, stops an active scan cleanly.
    QVERIFY(!library.scanning());
    library.cancelScan();
    QVERIFY(!library.scanning());

    QSignalSpy scanningSpy(&library, &Library::scanningChanged);
    library.addFolder(booksDir);
    QVERIFY(library.scanning());
    library.cancelScan();
    QVERIFY(!library.scanning());
    QVERIFY(scanningSpy.count() >= 2);
    library.rescan();
    QVERIFY(library.scanning());
    library.cancelScan();
    QVERIFY(!library.scanning());

    // 2. Audio shelves ("audio_downloaded", "audiobook_saved"), shelfCounts, and book enrichment.
    const int idA = library.addBookFile(epubA);
    const int idB = library.addBookFile(epubB);
    const int idPdf = library.addBookFile(pdfPath);
    QVERIFY(idA > 0 && idB > 0 && idPdf > 0);

    QVariantMap counts = library.shelfCounts();
    QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 3);
    QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 1);
    QCOMPARE(counts.value(QStringLiteral("downloaded")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("audiobooks")).toInt(), 0);

    library.markOpened(idA);
    library.savePosition(idA, QStringLiteral(R"({"chapter":0,"offset":12,"title":"Chapter One","snippet":"A lantern   in the fog."})"), 0.25);
    Marks marks(&library);
    marks.setBook(idA);
    QVERIFY(marks.addHighlight(0, 0, 10, QStringLiteral("The lamp"), QStringLiteral("yellow"), QStringLiteral("Ch 1")) > 0);
    QVERIFY(marks.addHighlight(0, 12, 20, QStringLiteral("sea wind"), QStringLiteral("blue"), QStringLiteral("Ch 1")) > 0);
    library.setBookSetting(idA, QStringLiteral("voiceName"), QStringLiteral("Emma"));
    library.setBookSetting(idA, QStringLiteral("audio_downloaded"), QStringLiteral("1"));
    library.setBookSetting(idB, QStringLiteral("audiobook_saved"), QStringLiteral("1"));

    const QVariantMap mapA = library.book(idA);
    QCOMPARE(mapA.value(QStringLiteral("downloaded")).toBool(), true);
    QCOMPARE(mapA.value(QStringLiteral("hasAudiobook")).toBool(), false);
    QCOMPARE(mapA.value(QStringLiteral("highlightCount")).toInt(), 2);
    QCOMPARE(mapA.value(QStringLiteral("voiceName")).toString(), QStringLiteral("Emma"));
    QCOMPARE(mapA.value(QStringLiteral("snippet")).toString(), QStringLiteral("A lantern in the fog."));

    const QVariantList cont = library.continueReading();
    QVERIFY(!cont.isEmpty());
    QCOMPARE(cont.first().toMap().value(QStringLiteral("id")).toInt(), idA);
    QCOMPARE(cont.first().toMap().value(QStringLiteral("highlightCount")).toInt(), 2);
    QCOMPARE(cont.first().toMap().value(QStringLiteral("voiceName")).toString(), QStringLiteral("Emma"));
    QCOMPARE(cont.first().toMap().value(QStringLiteral("snippet")).toString(), QStringLiteral("A lantern in the fog."));

    counts = library.shelfCounts();
    QCOMPARE(counts.value(QStringLiteral("downloaded")).toInt(), 1);
    QCOMPARE(counts.value(QStringLiteral("audiobooks")).toInt(), 1);

    library.works()->setStatusFilter(QStringLiteral("downloaded"));
    QTRY_COMPARE(library.works()->count(), 1);
    QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), idA);
    QCOMPARE(library.works()->data(library.works()->index(0), WorksModel::DownloadedRole).toBool(), true);

    library.works()->setStatusFilter(QStringLiteral("audiobooks"));
    QTRY_COMPARE(library.works()->count(), 1);
    QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), idB);
    QCOMPARE(library.works()->data(library.works()->index(0), WorksModel::AudiobookRole).toBool(), true);

    // Clearing audio_downloaded removes the book from the Downloaded shelf live.
    library.works()->setStatusFilter(QStringLiteral("downloaded"));
    QTRY_COMPARE(library.works()->count(), 1);
    library.setBookSetting(idA, QStringLiteral("audio_downloaded"), QStringLiteral("0"));
    QTRY_COMPARE(library.works()->count(), 0);
    QCOMPARE(library.shelfCounts().value(QStringLiteral("downloaded")).toInt(), 0);

    // Audio badges persist across restart via loadAudioBadges().
    {
        Library reopened(dataDir, cacheDir);
        QVERIFY(reopened.open());
        QCOMPARE(reopened.shelfCounts().value(QStringLiteral("audiobooks")).toInt(), 1);
        QCOMPARE(reopened.book(idB).value(QStringLiteral("hasAudiobook")).toBool(), true);
        reopened.works()->setStatusFilter(QStringLiteral("audiobooks"));
        QCOMPARE(reopened.works()->count(), 1);
        QCOMPARE(reopened.works()->get(0).value(QStringLiteral("bookId")).toInt(), idB);
    }

    // 3. LibraryFinder::countByFormat + countBooks compat + search() format breakdown.
    const QString home = dir.filePath("finder-home");
    for (const QString &rel : {QStringLiteral("Books/a.epub"), QStringLiteral("Books/sub/b.EPUB"),
                               QStringLiteral("Books/c.PDF"), QStringLiteral("Books/notes.txt"),
                               QStringLiteral("Books/1/2/3/4/5/deep.epub")}) {
        QVERIFY(QDir().mkpath(QFileInfo(home + QLatin1Char('/') + rel).absolutePath()));
        write(home + QLatin1Char('/') + rel, "x");
    }
    const LibraryFinder::FormatCounts fmt = LibraryFinder::countByFormat(home + QStringLiteral("/Books"));
    QCOMPARE(fmt.epub, 2);
    QCOMPARE(fmt.pdf, 1);
    QCOMPARE(fmt.total(), 3);
    QCOMPARE(LibraryFinder::countBooks(home + QStringLiteral("/Books")), 3);
    QCOMPARE(LibraryFinder::countBooks(home + QStringLiteral("/Books"), 4, 2), 2);
    const QVariantList found = LibraryFinder::search({home + QStringLiteral("/Books")}, {}, home);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().toMap().value(QStringLiteral("count")).toInt(), 3);
    QCOMPARE(found.first().toMap().value(QStringLiteral("epubCount")).toInt(), 2);
    QCOMPARE(found.first().toMap().value(QStringLiteral("pdfCount")).toInt(), 1);

    // 4. LibraryIndex::search(query, limit, bookId) scoping.
    const QString idxPath = dir.filePath("scoped-index.sqlite");
    LibraryIndex index(idxPath);
    if (index.available()) {
        const QString epub1 = dir.filePath("s1.epub");
        const QString epub2 = dir.filePath("s2.epub");
        auto simpleEpub = [](const QByteArray &title, const QByteArray &body) {
            return TestBooks::epub({
                {"OEBPS/content.opf",
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">"
                 "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title + "</dc:title></metadata>"
                 "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                 "<spine><itemref idref=\"c\"/></spine></package>"},
                {"OEBPS/c.xhtml", TestBooks::chapter(title, body)},
            });
        };
        QVERIFY(TestBooks::write(epub1, simpleEpub("One", "<p>The brass compass pointed toward the lighthouse.</p>")));
        QVERIFY(TestBooks::write(epub2, simpleEpub("Two", "<p>Another brass compass lay upon the wooden chart.</p>")));
        QVERIFY(LibraryIndex::indexBook(idxPath, {11, epub1, QStringLiteral("epub"), 100}));
        QVERIFY(LibraryIndex::indexBook(idxPath, {22, epub2, QStringLiteral("epub"), 100}));

        const QVariantList allHits = index.search(QStringLiteral("compass"), 8);
        QCOMPARE(allHits.size(), 2);
        const QVariantList only11 = index.search(QStringLiteral("compass"), 8, 11);
        QCOMPARE(only11.size(), 1);
        QCOMPARE(only11.first().toMap().value(QStringLiteral("bookId")).toInt(), 11);
        const QVariantList only22 = index.search(QStringLiteral("compass"), 8, 22);
        QCOMPARE(only22.size(), 1);
        QCOMPARE(only22.first().toMap().value(QStringLiteral("bookId")).toInt(), 22);
        QVERIFY(index.search(QStringLiteral("compass"), 8, 999).isEmpty());
    }
}

void ClothboundTests::wave4AdaptiveReadingModePersistenceAndGlossary() {
    // 1. Companion::detectReadingMode() classification heuristics.
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("pdf"), QStringLiteral("A Novel"), {}, QStringLiteral("Once upon a time.")),
             QStringLiteral("study"));
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Preprint"), {},
                                          QStringLiteral("See arXiv:2401.12345 for details.")),
             QStringLiteral("study"));
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Article"), {},
                                          QStringLiteral("doi:10.1145/1234567.1234568")),
             QStringLiteral("study"));
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Notes"), {},
                                          QStringLiteral("We prove Theorem 3.2 using induction.")),
             QStringLiteral("study"));
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Survey"),
                                          {QStringLiteral("Abstract"), QStringLiteral("1. Intro"), QStringLiteral("References")},
                                          QStringLiteral("Plain overview.")),
             QStringLiteral("study"));
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("The Lantern Keeper"),
                                          {QStringLiteral("Chapter One"), QStringLiteral("The Storm")},
                                          QStringLiteral("The lamp's light shone across the harbor.")),
             QStringLiteral("fiction"));
    // A single weak signal alone (only "Abstract" without references/formal markers) stays fiction.
    QCOMPARE(Companion::detectReadingMode(QStringLiteral("epub"), QStringLiteral("Abstract Art"),
                                          {QStringLiteral("Abstract")},
                                          QStringLiteral("Gallery paintings from the summer.")),
             QStringLiteral("fiction"));

    // 2. Database schema stays at version 4; companion_mode persists in book_settings;
    //    companion_recaps & companion_whoswho stay isolated by mode + migrate legacy tables + cascade on book delete.
    QTemporaryDir dir;
    const QString fictionPath = dir.filePath(QStringLiteral("story.epub"));
    const QString pdfPath = dir.filePath(QStringLiteral("paper.pdf"));
    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:title>Harbor Tale</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en</dc:language>\n"
        "  </metadata>\n"
        "  <manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
        "  <spine><itemref idref=\"c1\"/></spine>\n"
        "</package>";
    QVERIFY(TestBooks::write(
        fictionPath,
        TestBooks::epub({
            {"OEBPS/content.opf", opf},
            {"OEBPS/c1.xhtml",
             TestBooks::chapter("Ch 1",
                                "<p>Ada Marsh studies SGD optimization where θ minimizes the loss ℒ with gradient ∇ℒ(θ).</p>")},
        })));
    QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Optimization Paper", "Jane Doe")));

    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
    auto restoreSettings = qScopeGuard([&] {
        QSettings s(settingsPath, QSettings::IniFormat);
        s.remove(QStringLiteral("ai/preset"));
    });

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    QCOMPARE(library.schemaVersion(), LibrarySchema::kLatestVersion);

    const int fictionId = library.addBookFile(fictionPath);
    const int pdfId = library.addBookFile(pdfPath);
    QVERIFY(fictionId > 0);
    QVERIFY(pdfId > 0);

    // Seed pre-Wave-3 legacy tables (without `mode` column) BEFORE instantiating Companion to test zero-loss migration.
    {
        QSqlQuery q(library.database());
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE companion_whoswho ("
            " book_id INTEGER NOT NULL, chapter INTEGER NOT NULL, offset INTEGER NOT NULL,"
            " page_start INTEGER NOT NULL DEFAULT 0, where_label TEXT NOT NULL DEFAULT '',"
            " is_local INTEGER NOT NULL DEFAULT -1, people TEXT NOT NULL, updated_at INTEGER NOT NULL,"
            " PRIMARY KEY (book_id, chapter, offset))")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO companion_whoswho (book_id, chapter, offset, page_start, where_label, is_local, people, updated_at) "
            "VALUES (%1, 0, 120, 0, 'Ollama', 1, "
            "'[{\"name\":\"Ada Marsh\",\"also\":\"\",\"who\":\"Legacy keeper\",\"chapter\":\"Ch 1\",\"chapterIndex\":0,\"offset\":0}]', 100)")
                           .arg(fictionId)));
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE companion_recaps ("
            " book_id INTEGER NOT NULL, chapter INTEGER NOT NULL, offset INTEGER NOT NULL,"
            " page_start INTEGER NOT NULL DEFAULT 0, where_label TEXT NOT NULL DEFAULT '',"
            " is_local INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL, updated_at INTEGER NOT NULL,"
            " PRIMARY KEY (book_id, chapter, offset))")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO companion_recaps (book_id, chapter, offset, page_start, where_label, is_local, text, updated_at) "
            "VALUES (%1, 0, 120, 0, 'Ollama', 1, 'Legacy fiction recap.', 100)")
                           .arg(fictionId)));
    }

    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(fictionId));
    reader.relocated({{"chapter", 0}, {"offset", 0}, {"end", 120}, {"page", 0}, {"pages", 1}});

    FakeServer server;
    GeminiProvider gemini;
    MemoryKeyStore keys;
    AiServices services(&gemini, &keys, library.database().connectionName());
    LocalModels local;
    LocalModels::Server cfg;
    cfg.kind = LocalModels::Ollama;
    cfg.url = QUrl(server.origin());
    local.configure(cfg, QStringLiteral("qwen3.5:4b"));
    services.setLocalModels(&local);
    services.setPreset(QStringLiteral("local"));

    Companion companion(&services, &reader, library.database().connectionName());
    // Fiction book auto-detects "fiction" + default scope "upto" + restores migrated legacy rows!
    QCOMPARE(companion.readingMode(), QStringLiteral("fiction"));
    QVERIFY(!companion.studyMode());
    QCOMPARE(companion.scope(), QStringLiteral("upto"));
    QVERIFY(companion.recapLoaded());
    QCOMPARE(companion.recapText(), QStringLiteral("Legacy fiction recap."));
    QVERIFY(companion.peopleLoaded());
    QCOMPARE(companion.people().first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Ada Marsh"));

    // Switch manually to Study mode: persisted in book_settings, scope flips to "book", caches stay isolated.
    companion.setReadingMode(QStringLiteral("study"));
    QCOMPARE(companion.readingMode(), QStringLiteral("study"));
    QVERIFY(companion.studyMode());
    QCOMPARE(companion.scope(), QStringLiteral("book"));
    QCOMPARE(library.bookSetting(fictionId, QStringLiteral("companion_mode")), QStringLiteral("study"));
    QCOMPARE(library.schemaVersion(), LibrarySchema::kLatestVersion);
    QVERIFY(!companion.recapLoaded());
    QVERIFY(!companion.peopleLoaded());

    // Generate Study Brief & Study Glossary (with symbols θ, ℒ, ∇, SGD and First-defined offsets).
    server.responses.append({200, "application/x-ndjson",
                             "{\"message\":{\"content\":\"**Core Claims**\\nMinimizes loss $\\\\mathcal{L}(\\\\theta)$.\"},\"done\":true}\n"});
    companion.recap();
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
    QVERIFY(companion.recapLoaded());
    QVERIFY(companion.recapText().contains(QStringLiteral("Core Claims")));

    const QByteArray glossaryReply =
        "{\"message\":{\"content\":\"{\\\"people\\\":["
        "{\\\"name\\\":\\\"θ\\\",\\\"also\\\":\\\"parameters\\\",\\\"who\\\":\\\"Model weights\\\",\\\"chapter\\\":\\\"Ch 1\\\"},"
        "{\\\"name\\\":\\\"ℒ\\\",\\\"also\\\":\\\"loss\\\",\\\"who\\\":\\\"Objective function\\\",\\\"chapter\\\":\\\"Ch 1\\\"},"
        "{\\\"name\\\":\\\"∇\\\",\\\"also\\\":\\\"gradient\\\",\\\"who\\\":\\\"Gradient operator\\\",\\\"chapter\\\":\\\"Ch 1\\\"},"
        "{\\\"name\\\":\\\"SGD\\\",\\\"also\\\":\\\"\\\",\\\"who\\\":\\\"Stochastic Gradient Descent\\\",\\\"chapter\\\":\\\"Ch 1\\\"}"
        "]}\"},\"done\":true}\n";
    server.responses.append({200, "application/x-ndjson", glossaryReply});
    companion.whosWho();
    QTRY_VERIFY_WITH_TIMEOUT(!companion.busy(), 5000);
    QVERIFY2(companion.problem().isEmpty(), qPrintable(companion.problem()));
    QVERIFY(companion.peopleLoaded());
    QCOMPARE(companion.people().size(), 4);
    for (const QVariant &v : companion.people()) {
        const QVariantMap entry = v.toMap();
        QCOMPARE(entry.value(QStringLiteral("chapterIndex")).toInt(), 0);
        QVERIFY2(entry.value(QStringLiteral("offset")).toLongLong() > 0,
                 qPrintable(entry.value(QStringLiteral("name")).toString()));
    }

    // Fresh Companion restores persisted "study" override + Study caches; switching back to "fiction" restores legacy fiction caches.
    {
        Companion restored(&services, &reader, library.database().connectionName());
        QCOMPARE(restored.readingMode(), QStringLiteral("study"));
        QVERIFY(restored.recapLoaded());
        QVERIFY(restored.recapText().contains(QStringLiteral("Core Claims")));
        QCOMPARE(restored.people().size(), 4);

        restored.setReadingMode(QStringLiteral("fiction"));
        QVERIFY(restored.recapLoaded());
        QCOMPARE(restored.recapText(), QStringLiteral("Legacy fiction recap."));
        QCOMPARE(restored.people().size(), 1);
        QCOMPARE(restored.people().first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Ada Marsh"));
    }

    // PDF book auto-detects "study" mode with default scope "book".
    QVERIFY(reader.openBook(pdfId));
    QCOMPARE(companion.readingMode(), QStringLiteral("study"));
    QCOMPARE(companion.scope(), QStringLiteral("book"));

    // AFTER DELETE ON books trigger cascades both fiction & study rows in companion_recaps / companion_whoswho.
    {
        QSqlQuery del(library.database());
        QVERIFY(del.exec(QStringLiteral("DELETE FROM books WHERE id = %1").arg(fictionId)));
        QSqlQuery check(library.database());
        QVERIFY(check.exec(QStringLiteral("SELECT COUNT(*) FROM companion_recaps WHERE book_id = %1").arg(fictionId)));
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toInt(), 0);
        QVERIFY(check.exec(QStringLiteral("SELECT COUNT(*) FROM companion_whoswho WHERE book_id = %1").arg(fictionId)));
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toInt(), 0);
    }
}

void ClothboundTests::wave5LibraryCollectionsRecentAndSidebarIcons() {
    QTemporaryDir dir;
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));

    auto makePdf = [](const QString &path, const QString &title, const QString &body) {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(72);
        writer.setTitle(title);
        QPainter p(&writer);
        p.drawText(QRect(30, 60, 520, 120), body);
        p.end();
    };

    const QString existingPdf = dir.filePath(QStringLiteral("alpha_archive.pdf"));
    const QString freshPdf = dir.filePath(QStringLiteral("zebra_paper.pdf"));
    const QString epubPath = dir.filePath(QStringLiteral("lantern.epub"));
    makePdf(existingPdf, QStringLiteral("Alpha Archive"), QStringLiteral("Existing PDF paper already in library."));
    makePdf(freshPdf, QStringLiteral("Zebra Paper"), QStringLiteral("Brand new research PDF added straight to Research."));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(16, 16))));

    int colId = 0;
    int existingId = 0;
    int freshId = 0;
    int epubId = 0;

    {
        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        existingId = library.addBookFile(existingPdf);
        epubId = library.addBookFile(epubPath);
        QVERIFY(existingId > 0 && epubId > 0);

        colId = library.createCollection(QStringLiteral("Research"));
        QVERIFY(colId > 0);
        library.works()->setCollectionId(colId);
        QCOMPARE(library.works()->count(), 0);

        // 1. Re-adding an already-in-library PDF while viewing Research tags it immediately (0 -> 1) and emits booksAdded.
        QSignalSpy addedSpy(&library, &Library::booksAdded);
        library.addUrls({QUrl::fromLocalFile(existingPdf)});
        QCOMPARE(library.works()->count(), 1);
        QVERIFY(library.isInCollection(existingId, colId));
        QCOMPARE(addedSpy.count(), 1);
        QCOMPARE(addedSpy[0][0].toInt(), existingId);
        QCOMPARE(addedSpy[0][1].toInt(), 1);
        QCOMPARE(addedSpy[0][3].toInt(), colId);

        // 2. Adding a brand-new PDF while viewing Research scans, tags, updates works() live, and emits booksAdded.
        library.addUrls({QUrl::fromLocalFile(freshPdf)});
        QTRY_COMPARE_WITH_TIMEOUT(library.works()->count(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(addedSpy.count() >= 2, 5000);
        freshId = library.idForPath(QFileInfo(freshPdf).canonicalFilePath());
        QVERIFY(freshId > 0);
        QVERIFY(library.isInCollection(freshId, colId));
        QCOMPARE(addedSpy.last()[3].toInt(), colId);

        // Set explicit added_at timestamps to exercise 14-day window and newest-first ordering across reopen.
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        QSqlQuery q(library.database());
        QVERIFY(q.exec(QStringLiteral("UPDATE books SET added_at = %1 WHERE id = %2").arg(nowSec - 3600).arg(freshId)));
        QVERIFY(q.exec(QStringLiteral("UPDATE books SET added_at = %1 WHERE id = %2").arg(nowSec - 3LL * 86400).arg(epubId)));
        QVERIFY(q.exec(QStringLiteral("UPDATE books SET added_at = %1 WHERE id = %2").arg(nowSec - 20LL * 86400).arg(existingId)));
    }

    // 3. Reopen SQLite library: collection memberships persist, shelfCounts & recent shelf behave accurately.
    {
        Library reopened(dataDir, cacheDir);
        QVERIFY(reopened.open());
        QCOMPARE(reopened.schemaVersion(), LibrarySchema::kLatestVersion);
        QVERIFY(reopened.isInCollection(existingId, colId));
        QVERIFY(reopened.isInCollection(freshId, colId));

        const QVariantMap counts = reopened.shelfCounts();
        QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 2);
        QCOMPARE(counts.value(QStringLiteral("recent")).toInt(), 2);

        // statusFilter == "recent": 14-day window, strictly newest-first regardless of sortOrder == "title".
        reopened.works()->setSortOrder(QStringLiteral("title"));
        reopened.works()->setStatusFilter(QStringLiteral("recent"));
        QCOMPARE(reopened.works()->count(), 2);
        QCOMPARE(reopened.works()->get(0).value(QStringLiteral("bookId")).toInt(), freshId);
        QCOMPARE(reopened.works()->get(1).value(QStringLiteral("bookId")).toInt(), epubId);

        // Format shelves match shelfCounts["epub"] and shelfCounts["pdf"].
        reopened.works()->clearFilters();
        reopened.works()->setFormatFilter(QStringLiteral("epub"));
        QCOMPARE(reopened.works()->count(), counts.value(QStringLiteral("epub")).toInt());
        reopened.works()->setFormatFilter(QStringLiteral("pdf"));
        QCOMPARE(reopened.works()->count(), counts.value(QStringLiteral("pdf")).toInt());
    }

    // 4. LibrarySidebar.qml icon check (Icons.download for Downloaded, Icons.headphones for Audiobooks)
    //    and live component compilation.
    {
        const QString srcRoot = QFINDTESTDATA("../src");
        QFile sidebarFile(srcRoot + QStringLiteral("/ui/library/LibrarySidebar.qml"));
        QVERIFY(sidebarFile.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(sidebarFile.readAll());
        const qsizetype dlPos = qml.indexOf(QStringLiteral("qsTr(\"Downloaded\")"));
        const qsizetype abPos = qml.indexOf(QStringLiteral("qsTr(\"Audiobooks\")"));
        QVERIFY(dlPos > 0 && abPos > dlPos);
        const QString dlBlock = qml.mid(dlPos, 120);
        const QString abBlock = qml.mid(abPos, 120);
        QVERIFY2(dlBlock.contains(QStringLiteral("glyph: Icons.download")), qPrintable(dlBlock));
        QVERIFY2(abBlock.contains(QStringLiteral("glyph: Icons.headphones")), qPrintable(abBlock));
        QVERIFY2(qml.contains(QStringLiteral("readonly property string title: qsTr(\"Settings\")")),
                 "LibrarySidebar footer must be labeled Settings");
        QVERIFY2(qml.contains(QStringLiteral("path: Icons.gear")),
                 "LibrarySidebar Settings footer must use Icons.gear");
        QVERIFY2(qml.contains(QStringLiteral("onClicked: side.settingsRequested()")),
                 "LibrarySidebar Settings footer must emit settingsRequested()");

        QFile detailsFile(srcRoot + QStringLiteral("/ui/library/BookDetails.qml"));
        QVERIFY(detailsFile.open(QIODevice::ReadOnly));
        const QString detailsQml = QString::fromUtf8(detailsFile.readAll());
        QVERIFY(detailsQml.contains(QStringLiteral("text: \"BibTeX\"")));
        QVERIFY(detailsQml.contains(QStringLiteral("text: \"APA\"")));
        QVERIFY(detailsQml.contains(QStringLiteral("shelfPill.member ? theme.accentSoft")));

        QFile shelfFile(srcRoot + QStringLiteral("/ui/library/ContinueShelf.qml"));
        QVERIFY(shelfFile.open(QIODevice::ReadOnly));
        const QString shelfQml = QString::fromUtf8(shelfFile.readAll());
        QVERIFY(shelfQml.contains(QStringLiteral("book.format === \"pdf\"")));
        QVERIFY(shelfQml.contains(QStringLiteral("qsTr(\"Page %1 of %2\")")));

        QFile libViewFile(srcRoot + QStringLiteral("/ui/library/LibraryView.qml"));
        QVERIFY(libViewFile.open(QIODevice::ReadOnly));
        const QString libViewQml = QString::fromUtf8(libViewFile.readAll());
        QVERIFY(libViewQml.contains(QStringLiteral("onSettingsRequested: win.showSettings()")));
        QVERIFY(libViewQml.contains(QStringLiteral("qsTr(\"Open any book while the rest arrive.\")")));

        QFile readerFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
        QVERIFY(readerFile.open(QIODevice::ReadOnly));
        const QString readerQml = QString::fromUtf8(readerFile.readAll());
        QVERIFY(readerQml.contains(QStringLiteral("key: \"X\", what: qsTr(\"Snip\")")));
        QVERIFY(readerQml.contains(QStringLiteral("key: \"Ctrl K\", what: qsTr(\"Find\")")));
    }
}

void ClothboundTests::articlesShelfAndDesktopV040Wiring() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));

    // 1. Articles shelf detection across all 3 sources, epub split, sourceUrl, and schema v4 invariant.
    {
        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        QCOMPARE(library.schemaVersion(), LibrarySchema::kLatestVersion);

        const QString bookEpub = dir.filePath(QStringLiteral("regular-novel.epub"));
        const QString settingArtEpub = dir.filePath(QStringLiteral("setting-article.epub"));
        const QString subjectArtEpub = dir.filePath(QStringLiteral("subject-article.epub"));
        const QString dirArtEpub = dataDir + QStringLiteral("/articles/dir-article.epub");
        const QString samplePdf = dir.filePath(QStringLiteral("paper.pdf"));

        auto writeSimpleEpub = [](const QString &path, const QByteArray &title, const QByteArray &author) {
            const QByteArray opf =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<package version=\"3.0\" unique-identifier=\"uid\" xmlns=\"http://www.idpf.org/2007/opf\">"
                "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                "<dc:identifier id=\"uid\">urn:test:" + title + "</dc:identifier>"
                "<dc:title>" + title + "</dc:title>"
                "<dc:creator>" + author + "</dc:creator>"
                "<dc:language>en</dc:language>"
                "</metadata>"
                "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                "<spine><itemref idref=\"c\"/></spine></package>";
            return TestBooks::write(
                path, TestBooks::epub({{"OEBPS/content.opf", opf},
                                       {"OEBPS/c.xhtml", TestBooks::chapter(title, "<p>Body prose.</p>")}}));
        };

        QVERIFY(writeSimpleEpub(bookEpub, "Regular Novel", "Ada Lovelace"));
        QVERIFY(writeSimpleEpub(settingArtEpub, "Setting Essay", "Grace Hopper"));
        QVERIFY(TestBooks::write(samplePdf, TestBooks::pdf("Research Paper", "Alan Turing")));

        EpubArticle art;
        art.title = QStringLiteral("Subject Article");
        art.author = QStringLiteral("Alan Kay");
        art.sourceUrl = QStringLiteral("https://example.org/subject-article");
        art.subjects = {QStringLiteral("Clothbound Article")};
        art.bodyXhtml = QStringLiteral("<p>Readable article prose packaged offline.</p>");
        QString writeErr;
        QVERIFY2(EpubWriter::writeArticle(subjectArtEpub, art, &writeErr), qPrintable(writeErr));

        QVERIFY(QDir().mkpath(dataDir + QStringLiteral("/articles")));
        QVERIFY(writeSimpleEpub(dirArtEpub, "Folder Article", "Donald Knuth"));

        const int idBook = library.addBookFile(bookEpub);
        const int idSettingArt = library.addBookFile(settingArtEpub);
        const int idSubjectArt = library.addBookFile(subjectArtEpub);
        const int idDirArt = library.addBookFile(dirArtEpub);
        const int idPdf = library.addBookFile(samplePdf);
        QVERIFY(idBook > 0 && idSettingArt > 0 && idSubjectArt > 0 && idDirArt > 0 && idPdf > 0);

        library.setBookSetting(idSettingArt, QStringLiteral("content_kind"), QStringLiteral("article"));
        library.setBookSetting(idSettingArt, QStringLiteral("source_url"), QStringLiteral("https://example.org/essay"));
        library.setBookSetting(idSubjectArt, QStringLiteral("source_url"), QStringLiteral("https://example.org/subject-article"));
        library.works()->setGrouped(false);

        QVariantMap counts = library.shelfCounts();
        QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 5);
        QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("articles")).toInt(), 3);

        // Articles shelf shows all 3 articles and marks ArticleRole == true.
        library.works()->setStatusFilter(QStringLiteral("articles"));
        QCOMPARE(library.works()->count(), 3);
        for (int row = 0; row < library.works()->count(); ++row)
            QCOMPARE(library.works()->get(row).value(QStringLiteral("article")).toBool(), true);

        // Books (epub) shelf excludes articles so Books and Articles stay mutually distinct.
        library.works()->clearFilters();
        library.works()->setFormatFilter(QStringLiteral("epub"));
        QCOMPARE(library.works()->count(), 1);
        QCOMPARE(library.works()->get(0).value(QStringLiteral("bookId")).toInt(), idBook);
        QCOMPARE(library.works()->get(0).value(QStringLiteral("article")).toBool(), false);

        // Library::toMap() & Library::book() expose isArticle & sourceUrl.
        const QVariantMap artMap = library.book(idSettingArt);
        QCOMPARE(artMap.value(QStringLiteral("isArticle")).toBool(), true);
        QCOMPARE(artMap.value(QStringLiteral("sourceUrl")).toString(), QStringLiteral("https://example.org/essay"));
        const QVariantMap regularMap = library.book(idBook);
        QCOMPARE(regularMap.value(QStringLiteral("isArticle")).toBool(), false);
        QCOMPARE(regularMap.value(QStringLiteral("sourceUrl")).toString(), QString());

        // Clearing content_kind on setting-only article moves it back into Books (epub).
        library.setBookSetting(idSettingArt, QStringLiteral("content_kind"), QString());
        counts = library.shelfCounts();
        QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 2);
        QCOMPARE(counts.value(QStringLiteral("articles")).toInt(), 2);
        library.setBookSetting(idSettingArt, QStringLiteral("content_kind"), QStringLiteral("article"));
        QCOMPARE(library.shelfCounts().value(QStringLiteral("articles")).toInt(), 3);
    }

    // 2. Persistence across reopen preserves schemaVersion() == 4, Articles shelf, and sourceUrl.
    {
        Library reopened(dataDir, cacheDir);
        QVERIFY(reopened.open());
        QCOMPARE(reopened.schemaVersion(), LibrarySchema::kLatestVersion);
        const QVariantMap counts = reopened.shelfCounts();
        QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 5);
        QCOMPARE(counts.value(QStringLiteral("epub")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("pdf")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("articles")).toInt(), 3);

        reopened.works()->setGrouped(false);
        reopened.works()->setStatusFilter(QStringLiteral("articles"));
        QCOMPARE(reopened.works()->count(), 3);
        const int idSettingArt = reopened.idForPath(dir.filePath(QStringLiteral("setting-article.epub")));
        QVERIFY(idSettingArt > 0);
        QCOMPARE(reopened.book(idSettingArt).value(QStringLiteral("isArticle")).toBool(), true);
        QCOMPARE(reopened.book(idSettingArt).value(QStringLiteral("sourceUrl")).toString(),
                 QStringLiteral("https://example.org/essay"));
    }

    // 3. Live QML smoke test for AddUrlSheet.qml, LibraryView.qml (with Online section & sheets),
    //    and BookDetails.qml (with Source URL row & Web article label) — zero runtime QML warnings.
    {
        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices ai(&gemini, &keys, library.database().connectionName());
        LibraryFinder finder;
        PortalFilePicker picker;
        Theme theme(dir.filePath(QStringLiteral("no-omarchy")));
        Translator translator;
        WebImporter webImporter(&library, dataDir);
        OpdsClient opds(&library, &keys, dataDir, dir.filePath(QStringLiteral("opds.ini")));

        QQmlEngine engine;
        const QString srcRoot = QFINDTESTDATA("../src");
        QVERIFY(QFileInfo::exists(srcRoot));
        engine.addImportPath(srcRoot + QStringLiteral("/ui/style"));
        engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("library"), &library);
        engine.rootContext()->setContextProperty(QStringLiteral("libraryFinder"), &finder);
        engine.rootContext()->setContextProperty(QStringLiteral("picker"), &picker);
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), &reader);
        engine.rootContext()->setContextProperty(QStringLiteral("ai"), &ai);
        engine.rootContext()->setContextProperty(QStringLiteral("webImporter"), &webImporter);
        engine.rootContext()->setContextProperty(QStringLiteral("opds"), &opds);

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent addUrlComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/AddUrlSheet.qml")));
        QVERIFY2(addUrlComp.status() == QQmlComponent::Ready, qPrintable(addUrlComp.errorString()));
        std::unique_ptr<QObject> addUrlObj(addUrlComp.create());
        QVERIFY2(addUrlObj != nullptr, qPrintable(addUrlComp.errorString()));
        QMetaObject::invokeMethod(addUrlObj.get(), "show", Q_ARG(QVariant, QStringLiteral("https://arxiv.org/abs/2301.00001")),
                                  Q_ARG(QVariant, 0));
        QCOMPARE(addUrlObj->property("open").toBool(), true);
        QCOMPARE(addUrlObj->property("detectedKind").toString(), QStringLiteral("arxiv"));
        QMetaObject::invokeMethod(addUrlObj.get(), "close");
        QCOMPARE(addUrlObj->property("open").toBool(), false);

        QQmlComponent libComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/LibraryView.qml")));
        QVERIFY2(libComp.status() == QQmlComponent::Ready, qPrintable(libComp.errorString()));
        std::unique_ptr<QObject> libObj(libComp.create());
        QVERIFY2(libObj != nullptr, qPrintable(libComp.errorString()));
        QCOMPARE(libObj->property("addUrlOpen").toBool(), false);
        QCOMPARE(libObj->property("catalogsOpen").toBool(), false);
        QMetaObject::invokeMethod(libObj.get(), "openAddUrl", Q_ARG(QVariant, QString()));
        QCOMPARE(libObj->property("addUrlOpen").toBool(), true);
        QCOMPARE(libObj->property("typing").toBool(), true);
        QMetaObject::invokeMethod(libObj.get(), "closeAddUrl");
        QCOMPARE(libObj->property("addUrlOpen").toBool(), false);
        QMetaObject::invokeMethod(libObj.get(), "openCatalogs");
        QCOMPARE(libObj->property("catalogsOpen").toBool(), true);
        QCOMPARE(libObj->property("typing").toBool(), true);
        QMetaObject::invokeMethod(libObj.get(), "closeCatalogs");
        QCOMPARE(libObj->property("catalogsOpen").toBool(), false);

        const int idSettingArt = library.idForPath(dir.filePath(QStringLiteral("setting-article.epub")));
        QMetaObject::invokeMethod(libObj.get(), "showDetails", Q_ARG(QVariant, 0));
        QQmlComponent detComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/BookDetails.qml")));
        QVERIFY2(detComp.status() == QQmlComponent::Ready, qPrintable(detComp.errorString()));
        std::unique_ptr<QObject> detObj(detComp.create());
        QVERIFY2(detObj != nullptr, qPrintable(detComp.errorString()));
        QMetaObject::invokeMethod(detObj.get(), "show", Q_ARG(QVariant, idSettingArt), Q_ARG(QVariant, QString()));
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(detObj.get(), "close");

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
    }

    // 4. CLI `clothbound add <url>` routes http(s):// and arxiv: arguments through WebImporter.
    {
        QVERIFY(cliSummary().contains(QStringLiteral("add <folder|book|url>")));
        // Private/loopback URL is rejected by UrlGuard (proving WebImporter ran instead of local file check).
        QCOMPARE(runCli({QStringLiteral("clothbound"), QStringLiteral("add"), QStringLiteral("--json"),
                         QStringLiteral("http://127.0.0.1/blocked.epub")}),
                 1);
    }
}

void ClothboundTests::fixCoreLibraryFutureSchemaAndTargetedSavePosition() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    auto makeNamedEpub = [](const QByteArray &title, const QByteArray &body) {
        return TestBooks::epub({
            {"OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title + "</dc:title></metadata>"
             "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
             "<spine><itemref idref=\"c\"/></spine></package>"},
            {"OEBPS/c.xhtml", TestBooks::chapter(title, body)},
        });
    };
    const QString bookA = dir.filePath(QStringLiteral("book-a.epub"));
    const QString bookB = dir.filePath(QStringLiteral("book-b.epub"));
    QVERIFY(TestBooks::write(bookA, makeNamedEpub("Alpha Book", "<p>First distinct story.</p>")));
    QVERIFY(TestBooks::write(bookB, makeNamedEpub("Beta Book", "<p>Second distinct story.</p>")));

    // 1. savePosition emits targeted bookUpdated/bookChanged and updates
    //    WorksModel in place without booksChanged/readingStatsChanged/rebuild storms.
    {
        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        const int idA = library.addBookFile(bookA);
        const int idB = library.addBookFile(bookB);
        QVERIFY(idA > 0 && idB > 0);
        library.markOpened(idA);
        library.markOpened(idB);
        QTest::qWait(100);

        // Activate a status filter ("reading") that previously forced O(N log N)
        // rebuild() on every single savePosition() call.
        library.works()->setStatusFilter(QStringLiteral("reading"));
        QCOMPARE(library.works()->count(), 2);

        QSignalSpy booksSpy(&library, &Library::booksChanged);
        QSignalSpy statsSpy(&library, &Library::readingStatsChanged);
        QSignalSpy colsSpy(&library, &Library::collectionsChanged);
        QSignalSpy updatedSpy(&library, &Library::bookUpdated);
        QSignalSpy bookSpy(&library, &Library::bookChanged);
        QSignalSpy resetSpy(library.works(), &QAbstractItemModel::modelReset);
        QSignalSpy dataSpy(library.works(), &QAbstractItemModel::dataChanged);

        library.savePosition(idA, QStringLiteral(R"({"chapter":1,"offset":42})"), 0.35);
        QTest::qWait(100);

        QCOMPARE(booksSpy.count(), 0);
        QCOMPARE(statsSpy.count(), 0);
        QCOMPARE(colsSpy.count(), 0);
        QCOMPARE(updatedSpy.count(), 1);
        QCOMPARE(updatedSpy.first().first().toInt(), idA);
        QCOMPARE(bookSpy.count(), 1);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(dataSpy.count(), 1);
        const QModelIndex topLeft = dataSpy.first().at(0).toModelIndex();
        const QModelIndex bottomRight = dataSpy.first().at(1).toModelIndex();
        QCOMPARE(topLeft.row(), bottomRight.row());
        QCOMPARE(library.works()->data(topLeft, WorksModel::BookIdRole).toInt(), idA);

        // Recording actual reading time DOES emit readingStatsChanged().
        library.addReadingTime(idA, 60, 1200);
        QCOMPARE(statsSpy.count(), 1);

        // 2. flushPending() isolates failures per book via nested savepoints:
        //    rejecting one book via a DB trigger must NOT discard its sibling.
        const QString scanFolder = dir.filePath(QStringLiteral("scan-batch"));
        QVERIFY(QDir().mkpath(scanFolder));
        const QString goodBook = scanFolder + QStringLiteral("/good.epub");
        const QString badBook = scanFolder + QStringLiteral("/poison.epub");
        QVERIFY(TestBooks::write(goodBook, makeNamedEpub("Good Sibling", "<p>Good content.</p>")));
        QVERIFY(TestBooks::write(badBook, makeNamedEpub("Poison Sibling", "<p>Blocked by trigger.</p>")));
        {
            QSqlQuery trig(library.database());
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER block_poison BEFORE INSERT ON books WHEN NEW.path LIKE '%poison.epub' "
                "BEGIN SELECT RAISE(ABORT, 'simulated failure'); END")));
        }
        QSignalSpy scanDone(&library, &Library::scanFinished);
        library.addFolder(scanFolder);
        QTRY_VERIFY_WITH_TIMEOUT(scanDone.count() >= 1, 5000);
        QVERIFY(library.idForPath(goodBook) > 0);
        QCOMPARE(library.idForPath(badBook), 0);
        {
            QSqlQuery dropTrig(library.database());
            QVERIFY(dropTrig.exec(QStringLiteral("DROP TRIGGER block_poison")));
        }
    }

    // 3. Future schema version (> kLatestVersion) must fail closed on open().
    {
        const QString conn = QStringLiteral("fix-core-future-schema");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dataDir + QStringLiteral("/library.sqlite"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 999")));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        Library futureLib(dataDir, cacheDir);
        QVERIFY(!futureLib.open());
        QVERIFY2(futureLib.errorString().contains(QStringLiteral("999")), qPrintable(futureLib.errorString()));
    }
}

void ClothboundTests::fixCoreLibraryIndexRebuildCancelAndIndexedPassage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString idxPath = dir.filePath(QStringLiteral("index.sqlite"));
    const QString epub1 = dir.filePath(QStringLiteral("one.epub"));
    const QString epub2 = dir.filePath(QStringLiteral("two.epub"));
    auto makeSearchEpub = [](const QByteArray &title, const QByteArray &body) {
        return TestBooks::epub({
            {"OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title + "</dc:title></metadata>"
             "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
             "<spine><itemref idref=\"c\"/></spine></package>"},
            {"OEBPS/c.xhtml", TestBooks::chapter(title, body)},
        });
    };
    QVERIFY(TestBooks::write(epub1, makeSearchEpub("One", "<p>The amber lantern glowed across the harbor.</p>")));
    QVERIFY(TestBooks::write(epub2, makeSearchEpub("Two", "<p>A second amber lantern guided the ship home.</p>")));

    // Pre-cancelled indexBook returns false immediately without writing rows.
    std::atomic<bool> preCancel{true};
    QVERIFY(!LibraryIndex::indexBook(idxPath, {1, epub1, QStringLiteral("epub"), 10}, &preCancel));

    LibraryIndex index(idxPath);
    if (!index.available())
        QSKIP("SQLite without FTS5");
    QCOMPARE(index.bookCount(), 0);

    const QList<LibraryIndex::Source> sources{
        {1, epub1, QStringLiteral("epub"), 10},
        {2, epub2, QStringLiteral("epub"), 20},
    };
    index.update(sources);
    // Trigger back-to-back rebuilds while indexing is active; per-run cancel
    // tokens + 1-thread pool must prevent dual chains or stale rows.
    index.rebuild();
    index.rebuild();
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 10000);
    QCOMPARE(index.bookCount(), 2);
    QCOMPARE(index.search(QStringLiteral("lantern")).size(), 2);
    QVERIFY(index.passage(1, 0, 0, 30).contains(QStringLiteral("lantern"), Qt::CaseInsensitive));

    // Confirm chapter_rows mapping exists for O(log N) lookups and deletes.
    const QString conn = QStringLiteral("fix-core-chapter-rows-check");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(idxPath);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT count(*) FROM chapter_rows")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);

    // Removing all books via batched update() clears both indexed and chapter_rows.
    index.update({});
    QCOMPARE(index.bookCount(), 0);
    QVERIFY(index.passage(1, 0, 0, 30).isEmpty());
}

void ClothboundTests::aPlusCoreMarksAnchoringHealingAndUndo() {
    // 1. locateQuote() fast-path MUST validate prefix/suffix so a shifted hint landing on
    // an identical repeated phrase does NOT silently anchor to the wrong occurrence.
    const QString chapter =
        QStringLiteral("At dawn the harbor slept quietly. By noon the harbor roared with wind.");
    const qint64 firstHarbor = chapter.indexOf(QStringLiteral("the harbor"));
    const qint64 secondHarbor = chapter.indexOf(QStringLiteral("the harbor"), firstHarbor + 1);
    QVERIFY(firstHarbor >= 0 && secondHarbor > firstHarbor);

    const auto disamb = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                           QStringLiteral("By noon "), QStringLiteral(" roared"), firstHarbor);
    QCOMPARE(disamb.first, secondHarbor);
    QCOMPARE(disamb.second, secondHarbor + 10);

    // Matching hint AND matching context hits fast path accurately.
    const auto exact = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                          QStringLiteral("By noon "), QStringLiteral(" roared"), secondHarbor);
    QCOMPARE(exact.first, secondHarbor);
    QCOMPARE(exact.second, secondHarbor + 10);

    // Extreme hintStart does not overflow.
    const auto hugeHint = Marks::locateQuote(chapter, QStringLiteral("the harbor"),
                                             QStringLiteral("At dawn "), QStringLiteral(" slept"),
                                             std::numeric_limits<qint64>::max());
    QCOMPARE(hugeHint.first, firstHarbor);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epub1 = dir.filePath(QStringLiteral("m1.epub"));
    const QString epub2 = dir.filePath(QStringLiteral("m2.epub"));
    QVERIFY(TestBooks::write(epub1, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epub2, TestBooks::epub3(adobeObfuscate(fontPayload()))));

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int book1 = library.addBookFile(epub1);
    const int book2 = library.addBookFile(epub2);
    QVERIFY(book1 > 0 && book2 > 0);

    Marks marks(&library);
    marks.setBook(book1);

    // 2. Batched healChapter() emits a single model refresh + single changed() signal.
    const QString multiChapter = QStringLiteral(
        "Prefix text. First alpha quote here. Second beta quote here. Third gamma quote here. Fourth delta quote here.");
    const int m1 = marks.addHighlight(0, 0, 11, QStringLiteral("alpha quote"), QStringLiteral("yellow"),
                                      QStringLiteral("p. 1"), QStringLiteral("First "), QStringLiteral(" here."));
    const int m2 = marks.addHighlight(0, 1, 11, QStringLiteral("beta quote"), QStringLiteral("green"),
                                      QStringLiteral("p. 1"), QStringLiteral("Second "), QStringLiteral(" here."));
    const int m3 = marks.addHighlight(0, 2, 13, QStringLiteral("gamma quote"), QStringLiteral("blue"),
                                      QStringLiteral("p. 1"), QStringLiteral("Third "), QStringLiteral(" here."));
    const int m4 = marks.addBookmark(0, 3, QStringLiteral("delta quote"), QStringLiteral("p. 1"),
                                     QStringLiteral("Fourth "), QStringLiteral(" here."));
    QVERIFY(m1 > 0 && m2 > 0 && m3 > 0 && m4 > 0);

    QSignalSpy changedSpy(&marks, &Marks::changed);
    QSignalSpy resetSpy(&marks, &QAbstractItemModel::modelReset);
    QSignalSpy dataSpy(&marks, &QAbstractItemModel::dataChanged);
    QCOMPARE(marks.healChapter(0, multiChapter), 4);
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(resetSpy.count() + dataSpy.count(), 1);
    QCOMPARE(marks.healChapter(0, multiChapter), 0);
    QCOMPARE(changedSpy.count(), 1);

    // 3. Atomic Savepoint rollback in healChapter(): if SQLite aborts mid-batch, zero marks mutate.
    const QString shiftedChapter = QStringLiteral("INSERTED EXTRA TEXT BEFORE EVERYTHING. ") + multiChapter;
    const qint64 preStart1 = marks.mark(m1).value(QStringLiteral("start")).toLongLong();
    const qint64 preStart2 = marks.mark(m2).value(QStringLiteral("start")).toLongLong();
    {
        QSqlDatabase db = library.database();
        QSqlQuery trig(db);
        QVERIFY(trig.exec(QStringLiteral(
            "CREATE TEMP TRIGGER fail_second_heal BEFORE UPDATE ON annotations "
            "WHEN NEW.id = %1 BEGIN SELECT RAISE(ABORT, 'injected heal failure'); END")
                              .arg(m2)));
        QCOMPARE(marks.healChapter(0, shiftedChapter), 0);
        QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), preStart1);
        QCOMPARE(marks.mark(m2).value(QStringLiteral("start")).toLongLong(), preStart2);
        QSqlQuery check(db);
        QVERIFY(check.exec(QStringLiteral("SELECT start FROM annotations WHERE id = %1").arg(m1)));
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toLongLong(), preStart1);
        QVERIFY(trig.exec(QStringLiteral("DROP TRIGGER fail_second_heal")));
    }

    // 4. Undo/redo of Change (note/color) after healChapter() preserves freshly healed offsets!
    marks.setNote(m1, QStringLiteral("Café note"));
    marks.setColor(m1, QStringLiteral("pink"));
    QCOMPARE(marks.healChapter(0, shiftedChapter), 4);
    const qint64 healedStart1 = marks.mark(m1).value(QStringLiteral("start")).toLongLong();
    const qint64 healedEnd1 = marks.mark(m1).value(QStringLiteral("end")).toLongLong();
    QVERIFY(healedStart1 > preStart1);

    marks.undo();  // undo color -> yellow
    QCOMPARE(marks.mark(m1).value(QStringLiteral("color")).toString(), QStringLiteral("yellow"));
    QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), healedStart1);
    QCOMPARE(marks.mark(m1).value(QStringLiteral("end")).toLongLong(), healedEnd1);

    marks.undo();  // undo note -> ""
    QVERIFY(marks.mark(m1).value(QStringLiteral("note")).toString().isEmpty());
    QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), healedStart1);
    QCOMPARE(marks.mark(m1).value(QStringLiteral("end")).toLongLong(), healedEnd1);

    marks.redo();  // redo note -> "Café note"
    QCOMPARE(marks.mark(m1).value(QStringLiteral("note")).toString(), QStringLiteral("Café note"));
    QCOMPARE(marks.mark(m1).value(QStringLiteral("start")).toLongLong(), healedStart1);
    QCOMPARE(marks.mark(m1).value(QStringLiteral("end")).toLongLong(), healedEnd1);

    // 5. searchAll() cache serves repeated queries accurately and reflects mutations immediately.
    QCOMPARE(marks.searchAll(QStringLiteral("cafe")).size(), 1);
    QCOMPARE(marks.searchAll(QStringLiteral("cafe")).size(), 1);
    marks.setNote(m1, QStringLiteral("Updated periwinkle remark"));
    QCOMPARE(marks.searchAll(QStringLiteral("cafe")).size(), 0);
    QCOMPARE(marks.searchAll(QStringLiteral("periwinkle")).size(), 1);

    // 6. annotations.id undo-of-delete guards against rowid reuse (both in-session and external).
    const int latest = marks.addHighlight(0, 5, 15, QStringLiteral("reusable"), QStringLiteral("yellow"),
                                          QStringLiteral("p. 2"));
    marks.setNote(latest, QStringLiteral("Keep across collision"));
    marks.remove(latest);

    // Same-session add must NOT reuse the deleted mark's id.
    const int nextAdded = marks.addHighlight(0, 16, 26, QStringLiteral("brand new"), QStringLiteral("green"),
                                             QStringLiteral("p. 2"));
    QVERIFY(nextAdded > 0 && nextAdded != latest);
    marks.undo();  // remove brand new

    // Simulate an external writer (e.g. MCP / another book) stealing `latest`'s rowid in SQLite.
    {
        QSqlQuery hijack(library.database());
        hijack.prepare(QStringLiteral(
            "INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color, label, "
            "prefix, suffix, created_at, updated_at) "
            "VALUES (?, ?, 'highlight', 0, 1, 5, 'book2 text', 'book2 note', 'blue', 'Ch 1', '', '', 100, 100)"));
        hijack.addBindValue(latest);
        hijack.addBindValue(book2);
        QVERIFY(hijack.exec());
    }

    // Undo of delete detects collision on `latest`, allocates fresh id, and keeps book2's row intact.
    marks.undo();
    int restoredId = 0;
    for (int r = 0; r < marks.count(); ++r) {
        if (marks.data(marks.index(r), Marks::TextRole).toString() == QStringLiteral("reusable")) {
            restoredId = marks.data(marks.index(r), Marks::IdRole).toInt();
            break;
        }
    }
    QVERIFY(restoredId > 0 && restoredId != latest);
    QCOMPARE(marks.mark(restoredId).value(QStringLiteral("note")).toString(), QStringLiteral("Keep across collision"));

    // Subsequent undo of setNote targets the remapped `restoredId`, never book2's hijacked rowid!
    marks.undo();
    QVERIFY(marks.mark(restoredId).value(QStringLiteral("note")).toString().isEmpty());
    {
        QSqlQuery verifyBook2(library.database());
        verifyBook2.prepare(QStringLiteral("SELECT book_id, note FROM annotations WHERE id = ?"));
        verifyBook2.addBindValue(latest);
        QVERIFY(verifyBook2.exec() && verifyBook2.next());
        QCOMPARE(verifyBook2.value(0).toInt(), book2);
        QCOMPARE(verifyBook2.value(1).toString(), QStringLiteral("book2 note"));
    }
}

void ClothboundTests::w5CoreLibNoReaderCycleAndDecoupledBookSearch() {
    // 1. Strict DAG check: ZERO #include "reader/..." or "ai/..." anywhere in src/library/.
    const QString libDir = QFINDTESTDATA("../src/library");
    QVERIFY2(QFileInfo(libDir).isDir(), qPrintable(libDir));
    int checkedFiles = 0;
    QDirIterator it(libDir, QStringList{QStringLiteral("*.h"), QStringLiteral("*.cpp")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString filePath = it.next();
        QFile f(filePath);
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(filePath));
        const QByteArray bytes = f.readAll();
        QVERIFY2(!bytes.contains("#include \"reader/") && !bytes.contains("#include <reader/"),
                 qPrintable(QStringLiteral("Forbidden reader/ include found in %1").arg(filePath)));
        QVERIFY2(!bytes.contains("#include \"ai/") && !bytes.contains("#include <ai/"),
                 qPrintable(QStringLiteral("Forbidden ai/ include found in %1").arg(filePath)));
        ++checkedFiles;
    }
    QVERIFY(checkedFiles >= 14);

    // 2. BookSearch works cleanly with a decoupled QObject exposing meta-object
    // properties and bookChanged() signal (zero Reader dependency).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("decoupled.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(20, 30))));

    QQmlEngine qmlEngine;
    QQmlComponent sourceComp(&qmlEngine);
    sourceComp.setData(QByteArrayLiteral(R"QML(
import QtQml
QtObject {
    property bool open: true
    property int bookId: 42
    property string format: "epub"
    property string bookPath: ""
    property int chapter: 0
    property int pageStart: 0
    property int pageEnd: 0
    signal bookChanged()
}
)QML"),
                       QUrl::fromLocalFile(dir.filePath(QStringLiteral("DecoupledBookSource.qml"))));
    QVERIFY2(sourceComp.status() == QQmlComponent::Ready, qPrintable(sourceComp.errorString()));
    std::unique_ptr<QObject> source(sourceComp.create());
    QVERIFY(source != nullptr);
    QVERIFY(source->setProperty("bookPath", epubPath));

    BookSearch search(source.get());
    search.find(QStringLiteral("storm"));
    QVERIFY(search.searching());
    QTRY_VERIFY_WITH_TIMEOUT(!search.searching(), 5000);
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.results().first().toMap().value(QStringLiteral("chapter")).toInt(), 0);
    QCOMPARE(search.results().first().toMap().value(QStringLiteral("match")).toString(), QStringLiteral("storm"));

    // Emitting bookChanged() on the decoupled QObject resets cache and clears results.
    QVERIFY(source->setProperty("open", false));
    QVERIFY(source->setProperty("bookPath", QString()));
    QVERIFY(QMetaObject::invokeMethod(source.get(), "bookChanged", Qt::DirectConnection));
    QCOMPARE(search.count(), 0);
    QVERIFY(search.query().isEmpty());

    // 3. Also supports a plain QObject exposing dynamic `path` + `format` properties.
    QObject pathOnly;
    pathOnly.setProperty("path", epubPath);
    pathOnly.setProperty("format", QStringLiteral("epub"));
    BookSearch pathSearch(&pathOnly);
    pathSearch.find(QStringLiteral("lamp"));
    QTRY_VERIFY_WITH_TIMEOUT(!pathSearch.searching(), 5000);
    QCOMPARE(pathSearch.count(), 1);
}

void ClothboundTests::w5CoreLibSchemaTelemetryAndReadOnlyOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    const QString dbPath = dataDir + QStringLiteral("/library.sqlite");

    const QString epub1 = dir.filePath(QStringLiteral("one.epub"));
    const QString epub2 = dir.filePath(QStringLiteral("two.epub"));
    QVERIFY(TestBooks::write(epub1, TestBooks::epub2(png(20, 30))));
    QVERIFY(TestBooks::write(epub2, TestBooks::epub3(QByteArray(128, 'a'))));

    int id1 = 0, id2 = 0, colId = 0;
    {
        Library rw(dataDir, cacheDir);
        QVERIFY2(rw.open(), qPrintable(rw.errorString()));
        QVERIFY(!rw.isReadOnly());
        QCOMPARE(rw.schemaVersion(), LibrarySchema::kLatestVersion);
        QCOMPARE(LibrarySavepoint::activeDepth(), 0);

        id1 = rw.addBookFile(epub1);
        id2 = rw.addBookFile(epub2);
        QVERIFY(id1 > 0 && id2 > 0);
        colId = rw.createCollection(QStringLiteral("Essays"));
        QVERIFY(colId > 0);
        QVERIFY(rw.addToCollection(id1, colId));
        rw.setFinished(id2, true);
        rw.markOpened(id1);
        rw.setBookSetting(id1, QStringLiteral("voiceName"), QStringLiteral("Kokoro"));

        // Verify nested LibrarySavepoint depth tracking and automatic rollback.
        {
            QSqlDatabase db = rw.database();
            LibrarySavepoint outer(db, "test_outer");
            QVERIFY(outer.isActive());
            QCOMPARE(LibrarySavepoint::activeDepth(), 1);
            {
                LibrarySavepoint inner(db, "test_inner");
                QVERIFY(inner.isActive());
                QCOMPARE(LibrarySavepoint::activeDepth(), 2);
                QSqlQuery ins(db);
                QVERIFY(ins.exec(QStringLiteral("INSERT INTO meta (key, value) VALUES ('rolled_back', '1')")));
            }  // inner auto-rolls back
            QCOMPARE(LibrarySavepoint::activeDepth(), 1);
            QVERIFY(outer.commit());
            QCOMPARE(LibrarySavepoint::activeDepth(), 0);
            QSqlQuery chk(db);
            QVERIFY(chk.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'rolled_back'")));
            QVERIFY(!chk.next());
        }

        // Verify ReadingTelemetry deterministic pace learning, streaks, and per-book stats.
        const QDate refDay(2026, 6, 15);
        QSqlDatabase db = rw.database();
        const auto r1 = ReadingTelemetry::recordReadingTime(db, id1, 300, 7500, 1300.0, refDay.addDays(-2));
        QVERIFY(r1.ok);
        QVERIFY(!r1.paceUpdated);  // < 600s paced so far
        const auto r2 = ReadingTelemetry::recordReadingTime(db, id1, 360, 10800, r1.newCharsPerMinute, refDay.addDays(-1));
        QVERIFY(r2.ok);
        QVERIFY(r2.paceUpdated);  // 660s total paced -> (18300 / 660) * 60 = 1663.6 cpm
        QVERIFY(r2.newCharsPerMinute > 1600.0 && r2.newCharsPerMinute < 1700.0);
        const auto r3 = ReadingTelemetry::recordReadingTime(db, id1, 120, 2400, r2.newCharsPerMinute, refDay);
        QVERIFY(r3.ok);
        QCOMPARE(LibrarySavepoint::activeDepth(), 0);

        const QVariantMap stats = ReadingTelemetry::readingStats(db, {{id1, rw.record(id1)}, {id2, rw.record(id2)}},
                                                                 r3.newCharsPerMinute, refDay);
        QCOMPARE(stats.value(QStringLiteral("streakDays")).toInt(), 3);
        QCOMPARE(stats.value(QStringLiteral("longestStreak")).toInt(), 3);
        QCOMPARE(stats.value(QStringLiteral("todaySeconds")).toLongLong(), qint64(120));
        QCOMPARE(stats.value(QStringLiteral("weekSeconds")).toLongLong(), qint64(780));
        QCOMPARE(stats.value(QStringLiteral("recentDays")).toList().size(), 14);

        const QVariantMap bStats = ReadingTelemetry::bookReadingStats(db, rw.record(id1), r3.newCharsPerMinute);
        QCOMPARE(bStats.value(QStringLiteral("daysRead")).toInt(), 3);
        QCOMPARE(bStats.value(QStringLiteral("firstDay")).toString(), QStringLiteral("2026-06-13"));
        QCOMPARE(bStats.value(QStringLiteral("lastDay")).toString(), QStringLiteral("2026-06-15"));
    }

    // Record exact DB digest before locking file + directory strictly read-only.
    auto sha256Of = [](const QString &path) -> QByteArray {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray digestBefore = sha256Of(dbPath);
    QVERIFY(!digestBefore.isEmpty());

    QVERIFY(QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                               QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                               QFileDevice::ReadOther | QFileDevice::ExeOther));
    auto restorePerms = qScopeGuard([&] {
        QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    });

    const QString untouchedCache = dir.filePath(QStringLiteral("never_created_cache"));
    {
        Library ro(dataDir, untouchedCache);
        QVERIFY2(ro.openReadOnly(dbPath), qPrintable(ro.errorString()));
        QVERIFY(ro.isReadOnly());
        QCOMPARE(ro.schemaVersion(), LibrarySchema::kLatestVersion);
        QCOMPARE(ro.bookCount(), 2);
        QCOMPARE(ro.record(id1).title, QStringLiteral("The Lantern Keeper"));
        QCOMPARE(ro.record(id2).title, QStringLiteral("Salt Roads"));
        QCOMPARE(ro.collections().size(), 1);
        QVERIFY(ro.isInCollection(id1, colId));
        QCOMPARE(ro.bookSetting(id1, QStringLiteral("voiceName")), QStringLiteral("Kokoro"));
        QCOMPARE(ro.chapterLengths(id1).size(), 2);
        QCOMPARE(ro.shelfCounts().value(QStringLiteral("all")).toInt(), 2);
        QCOMPARE(ro.bookReadingStats(id1).value(QStringLiteral("daysRead")).toInt(), 3);

        // Mutations on read-only Library fail cleanly without touching disk.
        QCOMPARE(ro.createCollection(QStringLiteral("Forbidden")), 0);
        QVERIFY(!ro.errorString().isEmpty());
        QCOMPARE(ro.addBookFile(epub1), 0);
        QVERIFY(!ro.deleteCollection(colId));
        QVERIFY(!ro.addToCollection(id2, colId));
        ro.setFinished(id1, true);
        QCOMPARE(ro.record(id1).finishedAt, qint64(0));
        ro.addReadingTime(id1, 60, 1000);
    }

    QVERIFY(!QFileInfo::exists(untouchedCache));
    QCOMPARE(sha256Of(dbPath), digestBefore);

    // Verify fail-closed on future schema version > kLatestVersion for both open() and openReadOnly().
    QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    {
        const QString conn = QStringLiteral("w5-future-schema");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 99")));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }
    {
        Library roFuture(dataDir, cacheDir);
        QVERIFY(!roFuture.openReadOnly(dbPath));
        QVERIFY2(roFuture.errorString().contains(QStringLiteral("99")), qPrintable(roFuture.errorString()));
        Library rwFuture(dataDir, cacheDir);
        QVERIFY(!rwFuture.open());
        QVERIFY2(rwFuture.errorString().contains(QStringLiteral("99")), qPrintable(rwFuture.errorString()));
    }
}

void ClothboundTests::w5CoreLibFastEpubImportScanPerformance() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Build a realistic ~200k-word, 40-chapter EPUB (~1.35 MB uncompressed XHTML).
    constexpr int kChapters = 40;
    QByteArray manifestItems;
    QByteArray spineRefs;
    QList<QPair<QString, QByteArray>> files;
    files.reserve(kChapters + 2);

    const QByteArray para =
        "<p>The lantern keeper walked slowly along the weathered basalt seawall watching the northern fog drift "
        "past the iron bell tower while distant harbor buoys chimed across the cold dark tide.</p>\n";
    QByteArray longBody;
    longBody.reserve(para.size() * 190);
    for (int p = 0; p < 190; ++p)  // ~5,300 words / ~33 KB per chapter -> ~212k words total
        longBody += para;
    const QByteArray longChapter = TestBooks::chapter("Chapter", longBody);
    const QByteArray shortChapter = TestBooks::chapter("Short", "<p>A brief epigraph.</p>");

    for (int i = 0; i < kChapters; ++i) {
        const QByteArray id = "c" + QByteArray::number(i);
        const QByteArray href = "text/ch" + QByteArray::number(i) + ".xhtml";
        manifestItems += "    <item id=\"" + id + "\" href=\"" + href + "\" media-type=\"application/xhtml+xml\"/>\n";
        spineRefs += "    <itemref idref=\"" + id + "\"/>\n";
        files.append({QStringLiteral("OEBPS/") + QString::fromLatin1(href), (i == 0) ? shortChapter : longChapter});
    }
    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:title>Large Novel</dc:title><dc:creator>Ada Marsh</dc:creator><dc:language>en</dc:language>\n"
        "    <meta name=\"cover\" content=\"cover-img\"/>\n"
        "  </metadata>\n"
        "  <manifest>\n"
        "    <item id=\"cover-img\" href=\"cover.png\" media-type=\"image/png\"/>\n" +
        manifestItems +
        "  </manifest>\n"
        "  <spine>\n" +
        spineRefs +
        "  </spine>\n"
        "</package>";
    files.prepend({QStringLiteral("OEBPS/cover.png"), png(40, 60)});
    files.prepend({QStringLiteral("OEBPS/content.opf"), opf});

    const QString epubPath = dir.filePath(QStringLiteral("big_200k.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub(files)));

    // 1. EpubBook::estimatedChapterLengths() inflates literally ZERO chapter bytes.
    EpubBook book(epubPath);
    QVERIFY2(book.open(), qPrintable(book.errorString()));
    const quint64 inflatedBefore = book.inflatedBytes();
    const QList<int> estLengths = book.estimatedChapterLengths();
    QCOMPARE(book.inflatedBytes(), inflatedBefore);
    QCOMPARE(estLengths.size(), kChapters);
    QVERIFY(estLengths[0] > 0);
    QVERIFY(estLengths[1] > estLengths[0] * 10);

    // 2. Fast default BookFiles::read() avoids chapter inflation and runs much faster than exact measurement.
    QList<int> fastLengths, exactLengths;
    QElapsedTimer fastTimer;
    fastTimer.start();
    const BookInfo fastInfo = BookFiles::read(epubPath, 0, &fastLengths, false);
    const qint64 fastUs = fastTimer.nsecsElapsed() / 1000;

    QElapsedTimer exactTimer;
    exactTimer.start();
    const BookInfo exactInfo = BookFiles::read(epubPath, 0, &exactLengths, true);
    const qint64 exactUs = exactTimer.nsecsElapsed() / 1000;

    QVERIFY(fastInfo.ok() && exactInfo.ok());
    QCOMPARE(fastLengths.size(), kChapters);
    QCOMPARE(exactLengths.size(), kChapters);
    QCOMPARE(fastLengths, estLengths);
    QVERIFY2(fastUs * 5 < exactUs,
             qPrintable(QStringLiteral("Expected fast scan (%1 us) to be >5x faster than exact scan (%2 us)")
                            .arg(fastUs)
                            .arg(exactUs)));

    // 3. Library::addBookFile() imports the 200k-word EPUB quickly with populated proportional chapter lengths.
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    QElapsedTimer importTimer;
    importTimer.start();
    const int bookId = library.addBookFile(epubPath);
    const qint64 importMs = importTimer.elapsed();
    QVERIFY(bookId > 0);
    QCOMPARE(library.chapterLengths(bookId), fastLengths);
    QVERIFY(library.record(bookId).length > 500000);
    QVERIFY2(importMs < 250, qPrintable(QStringLiteral("200k-word EPUB import took %1 ms").arg(importMs)));
}
