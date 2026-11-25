// Repeatable C++ micro-benchmarks for Clothbound's core hot paths:
// PDF rendering + theme recoloring + 2-page spread concurrency + margin crop,
// 500-book SQLite library loading / querying / WorksModel filtering,
// and FTS5 full-text indexing & search.

#include "ai/passageindex.h"
#include "audio/voicepacks.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "library/librarymodel.h"
#include "library/marks.h"
#include "testbooks.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace {

double median(std::vector<double> values) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return (n % 2 == 1) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

void printRow(const char *name, double ms, const char *unit, const char *note = "") {
    printf("BENCH %-38s %9.3f %-10s %s\n", name, ms, unit, note);
    fflush(stdout);
}

// Build a realistic multi-page PDF with text paragraphs, headings, and a colored diagram
// using Qt's own QPdfWriter so PDFium exercises real glyph rasterization + color pixels.
bool generateRichPdf(const QString &path, int pages) {
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(150);
    writer.setTitle(QStringLiteral("Clothbound Benchmark Monograph"));
    writer.setCreator(QStringLiteral("Clothbound Bench"));
    QPainter painter(&writer);
    if (!painter.isActive())
        return false;

    const QFont heading(QStringLiteral("Sans"), 18, QFont::Bold);
    const QFont body(QStringLiteral("Serif"), 11);
    const QString paragraph = QStringLiteral(
        "Typography on screens improves when line length, leading, and contrast stay calm. "
        "Clothbound renders vector pages directly through PDFium and adapts monochrome ink to "
        "the active reading theme while preserving chromatic illustrations and charts intact.");

    for (int p = 0; p < pages; ++p) {
        if (p > 0)
            writer.newPage();
        const int w = writer.width();
        const int h = writer.height();
        const int margin = w / 10;

        painter.setPen(Qt::black);
        painter.setFont(heading);
        painter.drawText(QRect(margin, margin, w - 2 * margin, 80), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Chapter %1 — Notes on Reading Craft").arg(p + 1));

        painter.setFont(body);
        int y = margin + 100;
        for (int para = 0; para < 8 && y < h - margin - 260; ++para) {
            const QRect box(margin, y, w - 2 * margin, 140);
            painter.drawText(box, Qt::TextWordWrap,
                             QStringLiteral("[%1.%2] %3 %3").arg(p + 1).arg(para + 1).arg(paragraph));
            y += 148;
        }

        // Colored figure block so recolor exercises both monochrome text and chromatic pixels.
        const QRect fig(margin, h - margin - 220, w - 2 * margin, 180);
        painter.fillRect(fig, QColor(232, 240, 248));
        painter.setPen(QPen(QColor(46, 98, 168), 3));
        painter.drawRect(fig);
        for (int bar = 0; bar < 6; ++bar) {
            const QColor c = QColor::fromHsv((bar * 52 + p * 17) % 360, 175, 200);
            const int bw = (fig.width() - 80) / 6;
            const int bh = 40 + ((bar * 23 + p * 11) % 110);
            painter.fillRect(fig.x() + 40 + bar * bw, fig.bottom() - bh - 12, bw - 14, bh, c);
        }
    }
    painter.end();
    return true;
}

QByteArray makeRichEpub(int index, const QByteArray &coverPng) {
    QList<QPair<QString, QByteArray>> files;
    QByteArray manifestItems =
        R"(<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
           <item id="cover-image" href="images/cover.png" media-type="image/png"/>)";
    QByteArray spineItems;
    QByteArray navPoints;

    static const char *const vocab[] = {
        "harbor", "lantern", "astronomy", "architecture", "typography", "compass", "cartography", "orchard",
        "meridian", "telemetry", "chronicle", "almanac", "solstice", "manuscript", "parchment", "voyage"};

    for (int ch = 0; ch < 6; ++ch) {
        const QString id = QStringLiteral("ch%1").arg(ch + 1);
        const QString href = QStringLiteral("text/ch%1.xhtml").arg(ch + 1);
        manifestItems += QStringLiteral(R"(<item id="%1" href="%2" media-type="application/xhtml+xml"/>)").arg(id, href).toUtf8();
        spineItems += QStringLiteral(R"(<itemref idref="%1"/>)").arg(id).toUtf8();
        navPoints += QStringLiteral(
                         R"(<navPoint id="n%1" playOrder="%1"><navLabel><text>Chapter %1</text></navLabel><content src="%2"/></navPoint>)")
                         .arg(ch + 1)
                         .arg(href)
                         .toUtf8();
        QByteArray body = "<h1>Chapter " + QByteArray::number(ch + 1) + "</h1>";
        for (int p = 0; p < 18; ++p) {
            const char *w1 = vocab[(index + ch + p) % 16];
            const char *w2 = vocab[(index * 3 + ch * 5 + p) % 16];
            body += "<p>The quiet " + QByteArray(w1) + " stood beside the " + QByteArray(w2) +
                    " while readers turned crisp pages through winter evenings in volume " +
                    QByteArray::number(index) + " section " + QByteArray::number(ch) + ".</p>";
        }
        files.append({QStringLiteral("OEBPS/") + href, TestBooks::chapter("Ch", body)});
    }

    const QByteArray opf =
        QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<package version="2.0" unique-identifier="uid" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Chronicle of the Lantern %1</dc:title>
    <dc:creator opf:role="aut">Author %2</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="uid">urn:uuid:bench-book-%1</dc:identifier>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>%3</manifest>
  <spine toc="ncx">%4</spine>
</package>)")
            .arg(index)
            .arg(index % 40)
            .arg(QString::fromUtf8(manifestItems), QString::fromUtf8(spineItems))
            .toUtf8();

    const QByteArray ncx =
        QStringLiteral(R"(<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap>%1</navMap></ncx>)")
            .arg(QString::fromUtf8(navPoints))
            .toUtf8();

    files.prepend({"OEBPS/images/cover.png", coverPng});
    files.prepend({"OEBPS/toc.ncx", ncx});
    files.prepend({"OEBPS/content.opf", opf});
    return TestBooks::epub(files);
}

void benchPdf(const QString &dir) {
    const QString pdfPath = dir + QStringLiteral("/bench_24p.pdf");
    if (!generateRichPdf(pdfPath, 24)) {
        fprintf(stderr, "Failed to generate benchmark PDF\n");
        return;
    }

    auto book = std::make_shared<PdfBook>(pdfPath);
    if (!book->open()) {
        fprintf(stderr, "Failed to open benchmark PDF\n");
        return;
    }

    // 1. Single-page PDFium render at 1080p-ish (1400x1980) and HiDPI (2200x3112).
    for (const QSize sz : {QSize(1400, 1980), QSize(2200, 3112)}) {
        // Warm up PDFium once.
        (void)book->renderPage(0, sz);
        std::vector<double> renderTimes;
        std::vector<double> recolorPaperTimes;
        std::vector<double> recolorDarkTimes;
        std::vector<double> fusedDarkTimes;
        for (int i = 0; i < 9; ++i) {
            QElapsedTimer t;
            t.start();
            QImage img = book->renderPage(i % 12, sz);
            renderTimes.push_back(double(t.nsecsElapsed()) / 1e6);

            QImage copyPaper = img.copy();
            t.restart();
            PdfPages::recolor(copyPaper, QColor(QStringLiteral("#F7F4ED")), QColor(QStringLiteral("#26221D")));
            recolorPaperTimes.push_back(double(t.nsecsElapsed()) / 1e6);

            QImage copyDark = img.copy();
            t.restart();
            PdfPages::recolor(copyDark, QColor(QStringLiteral("#1A1B26")), QColor(QStringLiteral("#C0CAF5")));
            recolorDarkTimes.push_back(double(t.nsecsElapsed()) / 1e6);

            t.restart();
            QImage raw = book->renderPageRaw(i % 12, sz);
            PdfPages::compositeAndRecolor(raw, QColor(QStringLiteral("#1A1B26")), QColor(QStringLiteral("#C0CAF5")));
            fusedDarkTimes.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        const QByteArray labelRender = QStringLiteral("pdf.renderPage (%1x%2)").arg(sz.width()).arg(sz.height()).toUtf8();
        const QByteArray labelPaper = QStringLiteral("pdf.recolor paper (%1x%2)").arg(sz.width()).arg(sz.height()).toUtf8();
        const QByteArray labelDark = QStringLiteral("pdf.recolor dark (%1x%2)").arg(sz.width()).arg(sz.height()).toUtf8();
        const QByteArray labelFused = QStringLiteral("pdf.fused render+dark (%1x%2)").arg(sz.width()).arg(sz.height()).toUtf8();
        printRow(labelRender.constData(), median(renderTimes), "ms/page", "(median of 9)");
        printRow(labelPaper.constData(), median(recolorPaperTimes), "ms/page", "(median of 9)");
        printRow(labelDark.constData(), median(recolorDarkTimes), "ms/page", "(median of 9)");
        printRow(labelFused.constData(), median(fusedDarkTimes), "ms/page", "(median of 9)");
    }

    // 2. 2-page spread wall-clock latency via PdfPages async provider (uncached pairs).
    {
        PdfPages provider;
        const QString token = QStringLiteral("bench-spread");
        PdfPages::add(token, book);
        const QSize spreadPageSize(1200, 1700);
        const QString q = QStringLiteral("?bg=%231a1b26&fg=%23c0caf5");
        std::vector<double> spreadTimes;
        for (int pair = 0; pair < 5; ++pair) {
            const int p0 = pair * 2;
            const int p1 = pair * 2 + 1;
            QElapsedTimer t;
            t.start();
            auto *r0 = provider.requestImageResponse(QStringLiteral("%1/%2%3").arg(token).arg(p0).arg(q), spreadPageSize);
            auto *r1 = provider.requestImageResponse(QStringLiteral("%1/%2%3").arg(token).arg(p1).arg(q), spreadPageSize);
            QEventLoop loop;
            int done = 0;
            auto onDone = [&]() {
                if (++done >= 2)
                    loop.quit();
            };
            QObject::connect(r0, &QQuickImageResponse::finished, &loop, onDone);
            QObject::connect(r1, &QQuickImageResponse::finished, &loop, onDone);
            loop.exec();
            spreadTimes.push_back(double(t.nsecsElapsed()) / 1e6);
            delete r0;
            delete r1;
        }
        PdfPages::remove(token);
        printRow("pdf.spread_2page (1200x1700x2)", median(spreadTimes), "ms/spread", "(wall-clock, dark theme)");
    }

    // 3. bookCrop margin detection across 24 pages.
    {
        std::vector<double> cropTimes;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer t;
            t.start();
            const QRectF crop = PdfPages::bookCrop(*book);
            Q_UNUSED(crop);
            cropTimes.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        printRow("pdf.bookCrop (24 pages)", median(cropTimes), "ms", "(median of 3)");
    }
}

void benchLibrary(const QString &dir) {
    const QString dataDir = dir + QStringLiteral("/lib_data");
    const QString cacheDir = dir + QStringLiteral("/lib_cache");

    // Seed 500 books, 5 collections, 2500 annotations, and 60 reading_days directly in SQLite.
    {
        Library seed(dataDir, cacheDir);
        if (!seed.open()) {
            fprintf(stderr, "Failed to open seed library\n");
            return;
        }
        QSqlDatabase db = seed.database();
        QSqlQuery q(db);
        q.exec(QStringLiteral("BEGIN TRANSACTION"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();

        QSqlQuery insBook(db);
        insBook.prepare(QStringLiteral(
            "INSERT INTO books (path, format, size, mtime, title, sort_title, author, sort_author, "
            "publisher, year, language, series, series_index, work_key, chapters, pages, length, cover, "
            "added_at, opened_at, finished_at, progress, position, seconds_read, missing, hidden, loose, want_at, listened_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'en', ?, ?, ?, ?, ?, ?, '', ?, ?, ?, ?, ?, ?, 0, 0, 0, ?, 0)"));

        for (int i = 1; i <= 500; ++i) {
            const bool isPdf = (i % 4 == 0);
            const QString fmt = isPdf ? QStringLiteral("pdf") : QStringLiteral("epub");
            const QString title = QStringLiteral("The %1 Chronicle Vol. %2")
                                      .arg((i % 7 == 0) ? QStringLiteral("Lantern") : QStringLiteral("Meridian"))
                                      .arg(i);
            const QString author = QStringLiteral("Author %1").arg(i % 60);
            const QString series = (i % 5 == 0) ? QStringLiteral("Cycle %1").arg(i % 15) : QStringLiteral("");
            const qint64 openedAt = (i <= 40) ? (now - i * 3600) : 0;
            const qint64 finishedAt = (i > 15 && i <= 40) ? (now - i * 1800) : 0;
            const double progress = finishedAt > 0 ? 1.0 : (openedAt > 0 ? 0.12 + (i % 7) * 0.11 : 0.0);
            const QString pos = openedAt > 0
                                    ? QStringLiteral(R"({"chapter":2,"offset":420,"title":"Chapter Two","snippet":"Quiet pages turned softly."})")
                                    : QStringLiteral("");
            insBook.addBindValue(QStringLiteral("/fake/books/book_%1.%2").arg(i).arg(fmt));
            insBook.addBindValue(fmt);
            insBook.addBindValue(250000 + i * 1024);
            insBook.addBindValue(now - i * 86400);
            insBook.addBindValue(title);
            insBook.addBindValue(title.toLower());
            insBook.addBindValue(author);
            insBook.addBindValue(author.toLower());
            insBook.addBindValue(QStringLiteral("Harbor Press"));
            insBook.addBindValue(1980 + (i % 45));
            insBook.addBindValue(series);
            insBook.addBindValue(series.isEmpty() ? 0.0 : double((i % 6) + 1));
            insBook.addBindValue(QStringLiteral("work-%1").arg(i));
            insBook.addBindValue(12);
            insBook.addBindValue(isPdf ? 280 : 0);
            insBook.addBindValue(180000);
            insBook.addBindValue(now - i * 43200);
            insBook.addBindValue(openedAt);
            insBook.addBindValue(finishedAt);
            insBook.addBindValue(progress);
            insBook.addBindValue(pos);
            insBook.addBindValue(openedAt > 0 ? 1800 : 0);
            insBook.addBindValue((i > 40 && i <= 90) ? now - i * 1000 : 0);
            if (!insBook.exec()) {
                fprintf(stderr, "insBook failed: %s\n", qPrintable(insBook.lastError().text()));
                break;
            }
        }

        for (int c = 1; c <= 5; ++c) {
            QSqlQuery ic(db);
            ic.prepare(QStringLiteral("INSERT INTO collections (name, created_at) VALUES (?, ?)"));
            ic.addBindValue(QStringLiteral("Collection %1").arg(c));
            ic.addBindValue(now - c * 100);
            ic.exec();
            for (int b = c; b <= 500; b += 10) {
                QSqlQuery link(db);
                link.prepare(QStringLiteral("INSERT INTO book_collections (book_id, collection_id, added_at) VALUES (?, ?, ?)"));
                link.addBindValue(b);
                link.addBindValue(c);
                link.addBindValue(now);
                link.exec();
            }
        }

        QSqlQuery insAnn(db);
        insAnn.prepare(QStringLiteral(
            "INSERT INTO annotations (book_id, kind, chapter, start, finish, text, note, color, label, created_at, updated_at) "
            "VALUES (?, 'highlight', ?, ?, ?, 'Sample highlighted sentence from chapter.', '', 'yellow', 'Ch 1', ?, ?)"));
        for (int i = 0; i < 2500; ++i) {
            insAnn.addBindValue((i % 500) + 1);
            insAnn.addBindValue(i % 8);
            insAnn.addBindValue(100 + (i % 500));
            insAnn.addBindValue(160 + (i % 500));
            insAnn.addBindValue(now - i);
            insAnn.addBindValue(now - i);
            if (!insAnn.exec()) {
                fprintf(stderr, "insAnn failed: %s\n", qPrintable(insAnn.lastError().text()));
                break;
            }
        }

        QSqlQuery insDay(db);
        insDay.prepare(QStringLiteral("INSERT INTO reading_days (day, book_id, seconds) VALUES (?, ?, ?)"));
        const QDate today = QDate::currentDate();
        for (int d = 0; d < 60; ++d) {
            insDay.addBindValue(today.addDays(-d).toString(Qt::ISODate));
            insDay.addBindValue((d % 15) + 1);
            insDay.addBindValue(900 + (d % 4) * 300);
            insDay.exec();
        }
        q.exec(QStringLiteral("COMMIT"));
    }

    // 1. Cold Library::open() on 500 books.
    std::vector<double> openTimes;
    for (int run = 0; run < 5; ++run) {
        Library lib(dataDir, cacheDir);
        QElapsedTimer t;
        t.start();
        lib.open();
        openTimes.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    printRow("library.open (500 books)", median(openTimes), "ms", "(median of 5)");

    Library lib(dataDir, cacheDir);
    lib.open();
    if (lib.bookCount() != 500)
        fprintf(stderr, "WARNING: expected 500 seeded books, got %d\n", lib.bookCount());

    // 2. continueReading() (exercises enrichBookMap N+1 queries!).
    {
        std::vector<double> times;
        for (int i = 0; i < 30; ++i) {
            QElapsedTimer t;
            t.start();
            const QVariantList items = lib.continueReading();
            Q_UNUSED(items);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        printRow("library.continueReading (8 cards)", median(times), "ms/call", "(median of 30)");
    }

    // 3. book(id) x 100.
    {
        QElapsedTimer t;
        t.start();
        for (int i = 1; i <= 100; ++i) {
            const QVariantMap m = lib.book(i);
            Q_UNUSED(m);
        }
        printRow("library.book(id) x100", double(t.nsecsElapsed()) / 1e6, "ms", "(100 calls)");
    }

    // 4. shelfCounts() x 100.
    {
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 100; ++i) {
            const QVariantMap counts = lib.shelfCounts();
            Q_UNUSED(counts);
        }
        printRow("library.shelfCounts x100", double(t.nsecsElapsed()) / 1e6, "ms", "(100 calls)");
    }

    // 5. readingStats() x 30.
    {
        std::vector<double> times;
        for (int i = 0; i < 30; ++i) {
            QElapsedTimer t;
            t.start();
            const QVariantMap stats = lib.readingStats();
            Q_UNUSED(stats);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        printRow("library.readingStats", median(times), "ms/call", "(median of 30)");
    }

    // 6. WorksModel search & sort across 500 books.
    {
        WorksModel *works = lib.works();
        std::vector<double> searchTimes;
        for (int i = 0; i < 20; ++i) {
            QElapsedTimer t;
            t.start();
            works->setSearchText(QStringLiteral("lantern"));
            works->setSearchText(QString());
            searchTimes.push_back(double(t.nsecsElapsed()) / 2e6);
        }
        printRow("worksModel.searchFilter (500 books)", median(searchTimes), "ms/rebuild", "(median of 20)");

        std::vector<double> sortTimes;
        for (int i = 0; i < 20; ++i) {
            QElapsedTimer t;
            t.start();
            works->setSortOrder(QStringLiteral("title"));
            works->setSortOrder(QStringLiteral("author"));
            works->setSortOrder(QStringLiteral("recent"));
            sortTimes.push_back(double(t.nsecsElapsed()) / 3e6);
        }
        printRow("worksModel.sortOrder (500 books)", median(sortTimes), "ms/rebuild", "(median of 20)");
    }
}

void benchFts(const QString &dir) {
    const QString epubDir = dir + QStringLiteral("/epubs");
    QDir().mkpath(epubDir);

    // Tiny valid 1x1 PNG for cover.
    QImage px(64, 96, QImage::Format_RGB32);
    px.fill(QColor(80, 110, 145));
    QByteArray pngBytes;
    {
        QBuffer buf(&pngBytes);
        buf.open(QIODevice::WriteOnly);
        px.save(&buf, "PNG");
    }

    QList<LibraryIndex::Source> sources;
    constexpr int bookCount = 30;
    for (int i = 1; i <= bookCount; ++i) {
        const QString path = epubDir + QStringLiteral("/book_%1.epub").arg(i);
        TestBooks::write(path, makeRichEpub(i, pngBytes));
        sources.append({i, path, QStringLiteral("epub"), 1700000000 + i});
    }

    const QString idxPath = dir + QStringLiteral("/index.sqlite");
    LibraryIndex index(idxPath);
    QElapsedTimer t;
    t.start();
    for (const auto &s : sources)
        LibraryIndex::indexBook(idxPath, s);
    const double indexTotalMs = double(t.nsecsElapsed()) / 1e6;
    const QByteArray perBookNote = QStringLiteral("(%1 ms/book)").arg(indexTotalMs / bookCount, 0, 'f', 2).toUtf8();
    printRow("fts5.indexBook (30 EPUBs, 180 ch)", indexTotalMs, "ms total", perBookNote.constData());

    const QStringList queries = {QStringLiteral("lantern"), QStringLiteral("quiet harbor"),
                                 QStringLiteral("typography winter"), QStringLiteral("cartogr"),
                                 QStringLiteral("meridian compass")};
    std::vector<double> queryTimes;
    for (int round = 0; round < 10; ++round) {
        for (const QString &q : queries) {
            t.restart();
            const QVariantList hits = index.search(q, 12);
            Q_UNUSED(hits);
            queryTimes.push_back(double(t.nsecsElapsed()) / 1e6);
        }
    }
    printRow("fts5.search (180 chapters)", median(queryTimes), "ms/query", "(median of 50)");
}

void benchLibrary5000(const QString &dir) {
    const QString dataDir = dir + QStringLiteral("/lib5k_data");
    const QString cacheDir = dir + QStringLiteral("/lib5k_cache");
    constexpr int bookCount = 5000;

    // Seed 5,000 books, 10 collections, 5,000 annotations, and 90 reading_days in SQLite.
    {
        Library seed(dataDir, cacheDir);
        if (!seed.open()) {
            fprintf(stderr, "Failed to open 5k seed library\n");
            return;
        }
        QSqlDatabase db = seed.database();
        QSqlQuery q(db);
        q.exec(QStringLiteral("BEGIN TRANSACTION"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();

        QSqlQuery insBook(db);
        insBook.prepare(QStringLiteral(
            "INSERT INTO books (path, format, size, mtime, title, sort_title, author, sort_author, "
            "publisher, year, language, series, series_index, work_key, chapters, pages, length, cover, "
            "added_at, opened_at, finished_at, progress, position, seconds_read, missing, hidden, loose, want_at, listened_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'en', ?, ?, ?, ?, ?, ?, '', ?, ?, ?, ?, ?, ?, 0, 0, 0, ?, 0)"));

        for (int i = 1; i <= bookCount; ++i) {
            const bool isPdf = (i % 4 == 0);
            const QString fmt = isPdf ? QStringLiteral("pdf") : QStringLiteral("epub");
            const QString title = QStringLiteral("The %1 Chronicle Vol. %2")
                                      .arg((i % 11 == 0) ? QStringLiteral("Lantern") : QStringLiteral("Meridian"))
                                      .arg(i);
            const QString author = QStringLiteral("Author %1").arg(i % 250);
            const QString series = (i % 6 == 0) ? QStringLiteral("Cycle %1").arg(i % 40) : QStringLiteral("");
            const qint64 openedAt = (i <= 80) ? (now - i * 3600) : 0;
            const qint64 finishedAt = (i > 30 && i <= 80) ? (now - i * 1800) : 0;
            const double progress = finishedAt > 0 ? 1.0 : (openedAt > 0 ? 0.15 + (i % 7) * 0.10 : 0.0);
            const QString pos = openedAt > 0
                                    ? QStringLiteral(R"({"chapter":2,"offset":420,"title":"Chapter Two","snippet":"Quiet pages turned softly."})")
                                    : QStringLiteral("");
            insBook.addBindValue(QStringLiteral("/fake/books5k/book_%1.%2").arg(i).arg(fmt));
            insBook.addBindValue(fmt);
            insBook.addBindValue(250000 + i * 512);
            insBook.addBindValue(now - i * 3600);
            insBook.addBindValue(title);
            insBook.addBindValue(title.toLower());
            insBook.addBindValue(author);
            insBook.addBindValue(author.toLower());
            insBook.addBindValue(QStringLiteral("Harbor Press"));
            insBook.addBindValue(1950 + (i % 75));
            insBook.addBindValue(series);
            insBook.addBindValue(series.isEmpty() ? 0.0 : double((i % 8) + 1));
            insBook.addBindValue(QStringLiteral("work5k-%1").arg(i));
            insBook.addBindValue(12);
            insBook.addBindValue(isPdf ? 280 : 0);
            insBook.addBindValue(180000);
            insBook.addBindValue(now - i * 1800);
            insBook.addBindValue(openedAt);
            insBook.addBindValue(finishedAt);
            insBook.addBindValue(progress);
            insBook.addBindValue(pos);
            insBook.addBindValue(openedAt > 0 ? 1800 : 0);
            insBook.addBindValue((i > 80 && i <= 250) ? now - i * 1000 : 0);
            if (!insBook.exec()) {
                fprintf(stderr, "insBook(5k) failed: %s\n", qPrintable(insBook.lastError().text()));
                break;
            }
        }

        for (int c = 1; c <= 10; ++c) {
            QSqlQuery ic(db);
            ic.prepare(QStringLiteral("INSERT INTO collections (name, created_at) VALUES (?, ?)"));
            ic.addBindValue(QStringLiteral("Shelf %1").arg(c));
            ic.addBindValue(now - c * 100);
            ic.exec();
            QSqlQuery link(db);
            link.prepare(QStringLiteral("INSERT INTO book_collections (book_id, collection_id, added_at) VALUES (?, ?, ?)"));
            for (int b = c; b <= bookCount; b += 25) {
                link.addBindValue(b);
                link.addBindValue(c);
                link.addBindValue(now);
                link.exec();
            }
        }

        QSqlQuery insAnn(db);
        insAnn.prepare(QStringLiteral(
            "INSERT INTO annotations (book_id, kind, chapter, start, finish, text, note, color, label, created_at, updated_at) "
            "VALUES (?, 'highlight', ?, ?, ?, 'Sample highlighted sentence from chapter.', '', 'yellow', 'Ch 1', ?, ?)"));
        for (int i = 0; i < bookCount; ++i) {
            insAnn.addBindValue((i % bookCount) + 1);
            insAnn.addBindValue(i % 8);
            insAnn.addBindValue(100 + (i % 500));
            insAnn.addBindValue(160 + (i % 500));
            insAnn.addBindValue(now - i);
            insAnn.addBindValue(now - i);
            if (!insAnn.exec()) {
                fprintf(stderr, "insAnn(5k) failed: %s\n", qPrintable(insAnn.lastError().text()));
                break;
            }
        }

        QSqlQuery insDay(db);
        insDay.prepare(QStringLiteral("INSERT INTO reading_days (day, book_id, seconds) VALUES (?, ?, ?)"));
        const QDate today = QDate::currentDate();
        for (int d = 0; d < 90; ++d) {
            insDay.addBindValue(today.addDays(-d).toString(Qt::ISODate));
            insDay.addBindValue((d % 25) + 1);
            insDay.addBindValue(900 + (d % 4) * 300);
            insDay.exec();
        }
        q.exec(QStringLiteral("COMMIT"));
    }

    // 1. Cold Library::open() at N=5,000 books.
    std::vector<double> openTimes;
    for (int run = 0; run < 3; ++run) {
        Library lib(dataDir, cacheDir);
        QElapsedTimer t;
        t.start();
        lib.open();
        openTimes.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    printRow("library5k.open (5000 books)", median(openTimes), "ms", "(median of 3)");

    Library lib(dataDir, cacheDir);
    lib.open();
    if (lib.bookCount() != bookCount)
        fprintf(stderr, "WARNING: expected %d seeded books, got %d\n", bookCount, lib.bookCount());

    WorksModel *works = lib.works();

    // 2. Incremental savePosition() latency at N=5,000 books (proving zero full rebuild/modelReset!).
    {
        int modelResets = 0;
        int booksChangedCount = 0;
        int singleRowUpdates = 0;
        QObject::connect(works, &QAbstractItemModel::modelReset, works, [&] { ++modelResets; });
        QObject::connect(&lib, &Library::booksChanged, &lib, [&] { ++booksChangedCount; });
        QObject::connect(works, &QAbstractItemModel::dataChanged, works,
                         [&](const QModelIndex &tl, const QModelIndex &br) {
                             if (tl.row() == br.row())
                                 ++singleRowUpdates;
                         });

        constexpr int iters = 50;
        std::vector<double> saveTimes;
        saveTimes.reserve(iters);
        for (int i = 0; i < iters; ++i) {
            const int bookId = (i % 15) + 1;  // already-opened books
            const double prog = 0.20 + double(i) * 0.01;
            const QString pos = QStringLiteral(R"({"chapter":3,"offset":%1,"title":"Chapter Three","snippet":"Steady turn."})")
                                    .arg(500 + i * 24);
            QElapsedTimer t;
            t.start();
            lib.savePosition(bookId, pos, prog);
            saveTimes.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        if (modelResets != 0 || booksChangedCount != 0 || singleRowUpdates != iters) {
            fprintf(stderr, "WARNING: savePosition(5k) resets=%d booksChanged=%d singleRow=%d (expected 0/0/%d)\n",
                    modelResets, booksChangedCount, singleRowUpdates, iters);
        }
        const QByteArray note =
            QStringLiteral("(median of %1, resets=%2, booksChanged=%3, rowUpdates=%4)")
                .arg(iters)
                .arg(modelResets)
                .arg(booksChangedCount)
                .arg(singleRowUpdates)
                .toUtf8();
        printRow("library5k.savePosition (5000 books)", median(saveTimes), "ms/call", note.constData());
    }

    // 3. WorksModel searchFilter at N=5,000 books.
    {
        std::vector<double> searchTimes;
        for (int i = 0; i < 10; ++i) {
            QElapsedTimer t;
            t.start();
            works->setSearchText(QStringLiteral("lantern"));
            works->setSearchText(QString());
            searchTimes.push_back(double(t.nsecsElapsed()) / 2e6);
        }
        printRow("worksModel5k.searchFilter (5k books)", median(searchTimes), "ms/rebuild", "(median of 10)");
    }
}

void benchPassageIndex10k(const QString &dir) {
    const QString dbPath = dir + QStringLiteral("/passage_10k.sqlite");
    const QString model = QStringLiteral("local/nomic-embed-text");
    constexpr int numBooks = 200;
    constexpr int passagesPerBook = 50;  // 200 * 50 = 10,000 vectors
    constexpr int dim = 768;

    auto makeNormalizedVec = [](std::mt19937 &rng, const QList<float> *anchor, float mix) {
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        QList<float> v(dim);
        double normSq = 0.0;
        for (int i = 0; i < dim; ++i) {
            const float x = dist(rng);
            v[i] = x;
            normSq += double(x) * double(x);
        }
        const float invNoise = normSq > 0.0 ? float(1.0 / std::sqrt(normSq)) : 1.0f;
        if (!anchor || mix <= 0.0f) {
            for (int i = 0; i < dim; ++i)
                v[i] *= invNoise;
            return v;
        }
        const float noiseWeight = std::sqrt(std::max(0.0f, 1.0f - mix * mix));
        double mixedNormSq = 0.0;
        for (int i = 0; i < dim; ++i) {
            const float x = mix * (*anchor)[i] + noiseWeight * (v[i] * invNoise);
            v[i] = x;
            mixedNormSq += double(x) * double(x);
        }
        const float invMixed = mixedNormSq > 0.0 ? float(1.0 / std::sqrt(mixedNormSq)) : 1.0f;
        for (int i = 0; i < dim; ++i)
            v[i] *= invMixed;
        return v;
    };

    std::mt19937 rng(20261125u);
    const QList<float> asked = makeNormalizedVec(rng, nullptr, 0.0f);

    // Seed 10,000 768-dim float32 vectors using PassageIndex's exact schema and index.
    const QString connName = QStringLiteral("bench-pi10k-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            fprintf(stderr, "Failed to open passage_10k.sqlite\n");
            QSqlDatabase::removeDatabase(connName);
            return;
        }
        {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
            pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
        }
        // Instantiate PassageIndex to create passage_vectors + idx_passage_vectors_model_book.
        PassageIndex schemaInit(nullptr, connName);
        Q_UNUSED(schemaInit);

        db.transaction();
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO passage_vectors (book_id, model, passage, vector) VALUES (?, ?, ?, ?)"));
        QByteArray blob(dim * int(sizeof(float)), Qt::Uninitialized);
        for (int b = 1; b <= numBooks; ++b) {
            for (int p = 0; p < passagesPerBook; ++p) {
                const bool boosted = (p % 10 == 0) && (b <= 10 || b % 2 == 0);
                const float mix = boosted ? (0.35f + float((b * 3 + p) % 10) * 0.055f) : 0.0f;
                const QList<float> vec = makeNormalizedVec(rng, boosted ? &asked : nullptr, mix);
                std::memcpy(blob.data(), vec.constData(), size_t(dim) * sizeof(float));
                const int chapter = p / 5;
                const qint64 start = qint64(p % 5) * 1100;
                const qint64 end = start + 1000;
                const QString key = QStringLiteral("%1:%2:%3:%4")
                                        .arg(chapter)
                                        .arg(start)
                                        .arg(end)
                                        .arg(b * 100 + p, 8, 16, QLatin1Char('0'));
                ins.addBindValue(b);
                ins.addBindValue(model);
                ins.addBindValue(key);
                ins.addBindValue(blob);
                if (!ins.exec()) {
                    fprintf(stderr, "ins(passage_vectors) failed: %s\n", qPrintable(ins.lastError().text()));
                    db.rollback();
                    db.close();
                    QSqlDatabase::removeDatabase(connName);
                    return;
                }
            }
        }
        db.commit();
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    QSet<int> shelfIds;
    for (int b = 1; b <= 10; ++b)
        shelfIds.insert(b);

    // Warm page cache once and validate ranking correctness.
    const QList<PassageIndex::LibraryHit> warmShelf =
        PassageIndex::rankLibrary(dbPath, model, asked, 12, PassageIndex::minSemanticScore, shelfIds);
    const QList<PassageIndex::LibraryHit> warmFull =
        PassageIndex::rankLibrary(dbPath, model, asked, 12, PassageIndex::minSemanticScore);
    bool shelfValid = (warmShelf.size() == 12);
    for (int i = 0; i < warmShelf.size(); ++i) {
        if (!shelfIds.contains(warmShelf[i].bookId) || (i > 0 && warmShelf[i].score > warmShelf[i - 1].score))
            shelfValid = false;
    }
    bool fullValid = (warmFull.size() == 12);
    for (int i = 1; i < warmFull.size(); ++i) {
        if (warmFull[i].score > warmFull[i - 1].score)
            fullValid = false;
    }
    if (!shelfValid || !fullValid) {
        fprintf(stderr, "WARNING: unexpected passageIndex10k hits shelf=%lld (valid=%d) full=%lld (valid=%d)\n",
                static_cast<long long>(warmShelf.size()), int(shelfValid),
                static_cast<long long>(warmFull.size()), int(fullValid));
    }

    // 1. Filtered 10-book shelf (500 / 10,000 vectors via book_id IN (...) + idx_passage_vectors_model_book).
    {
        std::vector<double> times;
        for (int i = 0; i < 9; ++i) {
            QElapsedTimer t;
            t.start();
            const auto hits =
                PassageIndex::rankLibrary(dbPath, model, asked, 12, PassageIndex::minSemanticScore, shelfIds);
            Q_UNUSED(hits);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        const QByteArray note =
            QStringLiteral("(median of 9, hits=%1, 768-dim)").arg(warmShelf.size()).toUtf8();
        printRow("passageIndex.rank10k shelf(10 books)", median(times), "ms/query", note.constData());
    }

    // 2. Full-library semantic ranking across all 10,000 768-dim vectors.
    {
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            QElapsedTimer t;
            t.start();
            const auto hits = PassageIndex::rankLibrary(dbPath, model, asked, 12, PassageIndex::minSemanticScore);
            Q_UNUSED(hits);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        const QByteArray note =
            QStringLiteral("(median of 5, hits=%1, 10k x 768-dim)").arg(warmFull.size()).toUtf8();
        printRow("passageIndex.rank10k full(10k vecs)", median(times), "ms/query", note.constData());
    }
}

void benchVoicePacks(const QString &dir) {
    QByteArray manifest;
    const QStringList candidates = {
#ifdef CLOTHBOUND_SOURCE_DIR
        QStringLiteral(CLOTHBOUND_SOURCE_DIR "/src/audio/voices/packs.json"),
#endif
        QDir::current().filePath(QStringLiteral("src/audio/voices/packs.json")),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../src/audio/voices/packs.json"),
    };
    for (const QString &path : candidates) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            manifest = f.readAll();
            break;
        }
    }
    if (manifest.isEmpty()) {
        fprintf(stderr, "Failed to locate src/audio/voices/packs.json for VoicePacks benchmark\n");
        return;
    }

    const QString voiceDir = dir + QStringLiteral("/voices_packs");
    QDir().mkpath(voiceDir);

    // Mark engine + kokoro + 5 Piper packs installed so listing/sorting exercises both installed & uninstalled branches.
    QStringList packIds;
    {
        VoicePacks probe(voiceDir, nullptr, QStringLiteral("x86_64"), manifest);
        auto markInstalled = [&](const QString &packId) {
            const auto p = probe.pack(packId);
            if (!p || p->dir.isEmpty())
                return;
            const QString pDir = voiceDir + QLatin1Char('/') + p->dir;
            QDir().mkpath(pDir);
            QFile sentinel(pDir + QStringLiteral("/.clothbound-installed"));
            if (sentinel.open(QIODevice::WriteOnly))
                sentinel.write("ok\n");
        };
        markInstalled(QStringLiteral("engine"));
        markInstalled(QStringLiteral("kokoro-v1.0"));
        const QVariantList all = probe.voices(QString());
        int piperMarked = 0;
        for (const QVariant &v : all) {
            const QString pid = v.toMap().value(QStringLiteral("pack")).toString();
            packIds.append(pid);
            if (piperMarked < 5 && pid.startsWith(QStringLiteral("piper:"))) {
                markInstalled(pid);
                ++piperMarked;
            }
        }
    }

    VoicePacks packs(voiceDir, nullptr, QStringLiteral("x86_64"), manifest);
    QCoreApplication::processEvents();
    const QVariantList warm = packs.voices(QString());
    const int voiceCount = int(warm.size());

    // 1. Listing & sorting all built-in voices via cached isInstalled() (proving zero stat() storm).
    {
        std::vector<double> times;
        for (int i = 0; i < 50; ++i) {
            QElapsedTimer t;
            t.start();
            const QVariantList listed = packs.voices(QString());
            Q_UNUSED(listed);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        const QByteArray note =
            QStringLiteral("(median of 50, %1 voices, %2 installed)")
                .arg(voiceCount)
                .arg(packs.installedVoiceCount())
                .toUtf8();
        printRow("voicePacks.voices (all built-in)", median(times), "ms/call", note.constData());
    }

    // 2. Full sweep of isInstalled(packId) across every voice's pack (~224 calls).
    {
        std::vector<double> times;
        for (int i = 0; i < 50; ++i) {
            QElapsedTimer t;
            t.start();
            int installed = 0;
            for (const QString &pid : std::as_const(packIds))
                installed += packs.isInstalled(pid) ? 1 : 0;
            Q_UNUSED(installed);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
        }
        const QByteArray note = QStringLiteral("(median of 50, %1 calls/sweep)").arg(packIds.size()).toUtf8();
        printRow("voicePacks.isInstalled (full sweep)", median(times), "ms/sweep", note.constData());
    }
}

void benchLibraryIndexScale(const QString &dir) {
    const QString idxPath = dir + QStringLiteral("/index_scale.sqlite");
    constexpr int numBooks = 500;
    constexpr int chaptersPerBook = 12;  // 6,000 chapters total
    constexpr int deleteBooks = 100;     // 1,200 chapters batch-deleted

    // Create exact production FTS5 + chapter_rows schema via LibraryIndex.
    LibraryIndex index(idxPath);
    if (!index.available()) {
        fprintf(stderr, "FTS5 unavailable for benchLibraryIndexScale\n");
        return;
    }

    static const char *const vocab[] = {
        "harbor", "lantern", "astronomy", "architecture", "typography", "compass", "cartography", "orchard",
        "meridian", "telemetry", "chronicle", "almanac", "solstice", "manuscript", "parchment", "voyage"};

    QList<LibraryIndex::Source> allSources;
    allSources.reserve(numBooks);

    // Seed 500 books x 12 chapters = 6,000 chapters with chapter_rows populated.
    const QString connName = QStringLiteral("bench-fts-scale-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(idxPath);
        if (!db.open()) {
            fprintf(stderr, "Failed to open index_scale.sqlite\n");
            QSqlDatabase::removeDatabase(connName);
            return;
        }
        {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
            pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
            pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
        }
        db.transaction();
        QSqlQuery insFts(db);
        insFts.prepare(QStringLiteral("INSERT INTO chapter_text (body, book, chapter) VALUES (?, ?, ?)"));
        QSqlQuery insMap(db);
        insMap.prepare(QStringLiteral("INSERT OR REPLACE INTO chapter_rows (book, chapter, fts_rowid) VALUES (?, ?, ?)"));
        QSqlQuery insIdx(db);
        insIdx.prepare(QStringLiteral("INSERT OR REPLACE INTO indexed (book, mtime, path) VALUES (?, ?, ?)"));

        for (int b = 1; b <= numBooks; ++b) {
            const QString path = QStringLiteral("/fake/fts/book_%1.epub").arg(b);
            const qint64 mtime = 1700000000 + b;
            allSources.append({b, path, QStringLiteral("epub"), mtime});
            for (int ch = 0; ch < chaptersPerBook; ++ch) {
                QString body;
                body.reserve(1800);
                for (int p = 0; p < 12; ++p) {
                    body += QStringLiteral("The quiet %1 stood beside the %2 while readers turned crisp pages in volume %3 chapter %4 paragraph %5. ")
                                .arg(QLatin1String(vocab[(b + ch + p) % 16]),
                                     QLatin1String(vocab[(b * 3 + ch * 5 + p) % 16]))
                                .arg(b)
                                .arg(ch)
                                .arg(p);
                }
                insFts.addBindValue(body);
                insFts.addBindValue(b);
                insFts.addBindValue(ch);
                if (!insFts.exec()) {
                    fprintf(stderr, "insFts(scale) failed: %s\n", qPrintable(insFts.lastError().text()));
                    db.rollback();
                    db.close();
                    QSqlDatabase::removeDatabase(connName);
                    return;
                }
                const qint64 rowId = insFts.lastInsertId().toLongLong();
                insMap.addBindValue(b);
                insMap.addBindValue(ch);
                insMap.addBindValue(rowId);
                insMap.exec();
            }
            insIdx.addBindValue(b);
            insIdx.addBindValue(mtime);
            insIdx.addBindValue(path);
            insIdx.exec();
        }
        db.commit();
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // 1. Indexed chapter_rows O(log N) passage() lookups across 6,000 chapters.
    {
        (void)index.passage(1, 0, 40, 240);  // warm
        std::vector<double> times;
        constexpr int lookups = 200;
        times.reserve(lookups);
        int nonEmpty = 0;
        for (int i = 0; i < lookups; ++i) {
            const int bookId = ((i * 37) % numBooks) + 1;
            const int chapter = (i * 7) % chaptersPerBook;
            QElapsedTimer t;
            t.start();
            const QString text = index.passage(bookId, chapter, 64, 280);
            times.push_back(double(t.nsecsElapsed()) / 1e6);
            if (!text.isEmpty())
                ++nonEmpty;
        }
        const QByteArray note =
            QStringLiteral("(median of %1, %2/%1 hits, 6000 ch)").arg(lookups).arg(nonEmpty).toUtf8();
        printRow("fts5.passage indexed (6k chapters)", median(times), "ms/call", note.constData());
    }

    // 2. Batched DELETE via chapter_rows when removing 100 books (1,200 chapters) in LibraryIndex::update().
    {
        const QList<LibraryIndex::Source> remaining = allSources.mid(deleteBooks);
        QElapsedTimer t;
        t.start();
        index.update(remaining);
        const double deleteMs = double(t.nsecsElapsed()) / 1e6;
        const int afterBooks = index.bookCount();
        if (afterBooks != numBooks - deleteBooks || index.indexing()) {
            fprintf(stderr, "WARNING: fts5.batchDelete afterBooks=%d indexing=%d\n", afterBooks, int(index.indexing()));
        }
        const QByteArray note =
            QStringLiteral("(%1 books / %2 ch deleted, %3 left)")
                .arg(deleteBooks)
                .arg(deleteBooks * chaptersPerBook)
                .arg(afterBooks)
                .toUtf8();
        printRow("fts5.batchDelete (100 books/6k ch)", deleteMs, "ms total", note.constData());
    }
}

}  // namespace

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("clothbound_bench"));

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        fprintf(stderr, "Unable to create temporary benchmark directory\n");
        return 1;
    }

    printf("=== Clothbound C++ Core Micro-Benchmarks ===\n");
    benchPdf(tmp.path());
    benchLibrary(tmp.path());
    benchFts(tmp.path());
    printf("=== Clothbound Large-Scale Empirical Benchmarks ===\n");
    benchLibrary5000(tmp.path());
    benchPassageIndex10k(tmp.path());
    benchVoicePacks(tmp.path());
    benchLibraryIndexScale(tmp.path());
    return 0;
}
