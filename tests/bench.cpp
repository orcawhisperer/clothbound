// Repeatable C++ micro-benchmarks for Clothbound's core hot paths:
// PDF rendering + theme recoloring + 2-page spread concurrency + margin crop,
// 500-book SQLite library loading / querying / WorksModel filtering,
// and FTS5 full-text indexing & search.

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
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <memory>
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
    return 0;
}
