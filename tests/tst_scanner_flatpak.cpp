#include "library/scanner.h"
#include "testbooks.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
QByteArray tinyPng() {
    QImage img(8, 12, QImage::Format_RGB32);
    img.fill(Qt::darkBlue);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return out;
}

bool makeSymlink(const QString &target, const QString &linkPath) {
#ifdef Q_OS_UNIX
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(linkPath).constData()) == 0;
#else
    return QFile::link(target, linkPath);
#endif
}

QString canonical(const QString &path) {
    return QFileInfo(path).canonicalFilePath();
}
}  // namespace

class TestScannerFlatpak : public QObject {
    Q_OBJECT

private slots:
    void prunesHiddenAndDependencyDirectories();
    void symlinkCyclesFinishQuicklyAndValidSymlinksWork();
    void looseFilesScannedBeforeWatchedFolders();
    void depthCapBoundsDeepTrees();
};

void TestScannerFlatpak::prunesHiddenAndDependencyDirectories() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString root = dir.filePath(QStringLiteral("library"));
    const QStringList dirsToCreate = {
        root + QStringLiteral("/.cache/nested"),
        root + QStringLiteral("/.local/share"),
        root + QStringLiteral("/.git/objects"),
        root + QStringLiteral("/.var/app/other"),
        root + QStringLiteral("/.venv/lib"),
        root + QStringLiteral("/venv/lib"),
        root + QStringLiteral("/node_modules/pkg"),
        root + QStringLiteral("/__pycache__"),
        root + QStringLiteral("/Sub"),
        root + QStringLiteral("/Docs"),
    };
    for (const QString &d : dirsToCreate)
        QVERIFY(QDir().mkpath(d));

    const QByteArray sampleEpub = TestBooks::epub2(tinyPng());
    const QByteArray samplePdf = TestBooks::pdf("Sample", "Author");

    // Pruned paths: hidden directories, dependency/cache trees, and hidden files.
    QVERIFY(TestBooks::write(root + QStringLiteral("/.cache/nested/secret.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/.local/share/book.epub"), sampleEpub));
    QVERIFY(TestBooks::write(root + QStringLiteral("/.git/objects/packed.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/.var/app/other/manual.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/.venv/lib/guide.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/venv/lib/readme.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/node_modules/pkg/doc.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/__pycache__/cached.pdf"), samplePdf));
    QVERIFY(TestBooks::write(root + QStringLiteral("/.hidden_root.epub"), sampleEpub));
    QVERIFY(TestBooks::write(root + QStringLiteral("/Docs/notes.txt"), "not a book"));

    // Legitimate nested books that must be discovered.
    const QString subBook = root + QStringLiteral("/Sub/Book.epub");
    const QString docsPaper = root + QStringLiteral("/Docs/Paper.pdf");
    QVERIFY(TestBooks::write(subBook, sampleEpub));
    QVERIFY(TestBooks::write(docsPaper, samplePdf));

    Scanner scanner(dir.filePath(QStringLiteral("covers")));
    QSignalSpy listedSpy(&scanner, &Scanner::listed);
    QSignalSpy bookReadSpy(&scanner, &Scanner::bookRead);
    QSignalSpy finishedSpy(&scanner, &Scanner::finished);

    scanner.scan({root}, {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

    QCOMPARE(listedSpy.count(), 1);
    const QStringList listedPaths = listedSpy.at(0).at(0).toStringList();
    const QStringList listedFolders = listedSpy.at(0).at(1).toStringList();
    QCOMPARE(listedFolders, QStringList{root});

    const QSet<QString> expected{canonical(subBook), canonical(docsPaper)};
    const QSet<QString> actual(listedPaths.cbegin(), listedPaths.cend());
    QCOMPARE(listedPaths.size(), 2);
    QCOMPARE(actual, expected);
    QCOMPARE(bookReadSpy.count(), 2);

    // Unchanged books in `known` are listed without re-reading.
    QHash<QString, QPair<qint64, qint64>> known;
    for (const QString &p : listedPaths) {
        const QFileInfo info(p);
        known.insert(p, {info.size(), info.lastModified().toSecsSinceEpoch()});
    }

    listedSpy.clear();
    bookReadSpy.clear();
    finishedSpy.clear();
    scanner.scan({root}, {}, known);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(listedSpy.count(), 1);
    QCOMPARE(listedSpy.at(0).at(0).toStringList().size(), 2);
    QCOMPARE(bookReadSpy.count(), 0);
}

void TestScannerFlatpak::symlinkCyclesFinishQuicklyAndValidSymlinksWork() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dirA = dir.filePath(QStringLiteral("dirA"));
    const QString dirASub = dirA + QStringLiteral("/sub");
    const QString extBooks = dir.filePath(QStringLiteral("external_books"));
    QVERIFY(QDir().mkpath(dirASub));
    QVERIFY(QDir().mkpath(extBooks));

    const QString rootPdf = dirA + QStringLiteral("/root_book.pdf");
    const QString subEpub = dirASub + QStringLiteral("/nested_book.epub");
    const QString extPdf = extBooks + QStringLiteral("/external_book.pdf");
    QVERIFY(TestBooks::write(rootPdf, TestBooks::pdf("Root", "Author")));
    QVERIFY(TestBooks::write(subEpub, TestBooks::epub2(tinyPng())));
    QVERIFY(TestBooks::write(extPdf, TestBooks::pdf("External", "Author")));

    // Direct self-cycle, ancestor cycle, nested cycle, mutual cycle, dangling link,
    // plus a legitimate non-cyclic symlink to an external books directory.
    QVERIFY(makeSymlink(dirA, dirA + QStringLiteral("/loop")));
    QVERIFY(makeSymlink(dirA, dirASub + QStringLiteral("/up")));
    QVERIFY(makeSymlink(dirASub, dirASub + QStringLiteral("/self_loop")));
    QVERIFY(makeSymlink(extBooks, dirA + QStringLiteral("/ext_link")));
    QVERIFY(makeSymlink(dirA, extBooks + QStringLiteral("/back_to_a")));
    QVERIFY(makeSymlink(dir.filePath(QStringLiteral("missing_target")), dirA + QStringLiteral("/dangling")));

    Scanner scanner(dir.filePath(QStringLiteral("covers")));
    QSignalSpy listedSpy(&scanner, &Scanner::listed);
    QSignalSpy bookReadSpy(&scanner, &Scanner::bookRead);
    QSignalSpy finishedSpy(&scanner, &Scanner::finished);

    QElapsedTimer timer;
    qint64 listedMs = -1;
    connect(&scanner, &Scanner::listed, &scanner, [&timer, &listedMs](const QStringList &, const QStringList &) {
        listedMs = timer.elapsed();
    });

    timer.start();
    scanner.scan({dirA}, {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

    QCOMPARE(listedSpy.count(), 1);
    QVERIFY2(listedMs >= 0 && listedMs < 200,
             qPrintable(QStringLiteral("Directory walk with symlink cycles took %1 ms").arg(listedMs)));

    const QStringList listedPaths = listedSpy.at(0).at(0).toStringList();
    const QSet<QString> expected{canonical(rootPdf), canonical(subEpub), canonical(extPdf)};
    const QSet<QString> actual(listedPaths.cbegin(), listedPaths.cend());
    QCOMPARE(listedPaths.size(), 3);
    QCOMPARE(actual, expected);
    QCOMPARE(bookReadSpy.count(), 3);

    // Rescan with known files finishes end-to-end well under 200 ms.
    QHash<QString, QPair<qint64, qint64>> known;
    for (const QString &p : listedPaths) {
        const QFileInfo info(p);
        known.insert(p, {info.size(), info.lastModified().toSecsSinceEpoch()});
    }
    finishedSpy.clear();
    timer.restart();
    scanner.scan({dirA}, {}, known);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    const qint64 rescanMs = timer.elapsed();
    QVERIFY2(rescanMs < 200,
             qPrintable(QStringLiteral("End-to-end symlink cycle rescan took %1 ms").arg(rescanMs)));
}

void TestScannerFlatpak::looseFilesScannedBeforeWatchedFolders() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString watched = dir.filePath(QStringLiteral("watched"));
    const QString looseDir = dir.filePath(QStringLiteral("loose"));
    QVERIFY(QDir().mkpath(watched));
    QVERIFY(QDir().mkpath(looseDir));

    const QString watchedAlpha = watched + QStringLiteral("/alpha.pdf");
    const QString watchedBeta = watched + QStringLiteral("/beta.epub");
    const QString looseFirst = looseDir + QStringLiteral("/first_loose.pdf");
    const QString looseSecond = looseDir + QStringLiteral("/second_loose.epub");

    const QByteArray sampleEpub = TestBooks::epub2(tinyPng());
    QVERIFY(TestBooks::write(watchedAlpha, TestBooks::pdf("Alpha", "Author")));
    QVERIFY(TestBooks::write(watchedBeta, sampleEpub));
    QVERIFY(TestBooks::write(looseFirst, TestBooks::pdf("First", "Author")));
    QVERIFY(TestBooks::write(looseSecond, sampleEpub));

    Scanner scanner(dir.filePath(QStringLiteral("covers")));
    QSignalSpy listedSpy(&scanner, &Scanner::listed);
    QSignalSpy finishedSpy(&scanner, &Scanner::finished);

    const QString missingFolder = dir.filePath(QStringLiteral("missing_drive"));
    scanner.scan({watched, missingFolder}, {looseFirst, looseSecond}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

    QCOMPARE(listedSpy.count(), 1);
    const QStringList paths = listedSpy.at(0).at(0).toStringList();
    const QStringList scannedFolders = listedSpy.at(0).at(1).toStringList();
    QCOMPARE(scannedFolders, QStringList{watched});
    QCOMPARE(paths.size(), 4);
    QCOMPARE(paths.at(0), canonical(looseFirst));
    QCOMPARE(paths.at(1), canonical(looseSecond));
    const QSet<QString> tail{paths.at(2), paths.at(3)};
    const QSet<QString> expectedTail{canonical(watchedAlpha), canonical(watchedBeta)};
    QCOMPARE(tail, expectedTail);

    // A loose file that also resides inside a watched folder is placed first
    // and deduplicated rather than listed twice.
    listedSpy.clear();
    finishedSpy.clear();
    scanner.scan({watched}, {watchedBeta}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(listedSpy.count(), 1);
    const QStringList dedupPaths = listedSpy.at(0).at(0).toStringList();
    QCOMPARE(dedupPaths, (QStringList{canonical(watchedBeta), canonical(watchedAlpha)}));
}

void TestScannerFlatpak::depthCapBoundsDeepTrees() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString root = dir.filePath(QStringLiteral("deep"));
    QString current = root;
    QString shallowDir;
    for (int depth = 1; depth <= 36; ++depth) {
        current += QStringLiteral("/d%1").arg(depth);
        if (depth == 5)
            shallowDir = current;
    }
    QVERIFY(QDir().mkpath(current));

    const QString withinCap = shallowDir + QStringLiteral("/within_cap.pdf");
    const QString beyondCap = current + QStringLiteral("/beyond_cap.pdf");
    const QByteArray samplePdf = TestBooks::pdf("Deep", "Author");
    QVERIFY(TestBooks::write(withinCap, samplePdf));
    QVERIFY(TestBooks::write(beyondCap, samplePdf));

    Scanner scanner(dir.filePath(QStringLiteral("covers")));
    QSignalSpy listedSpy(&scanner, &Scanner::listed);
    QSignalSpy finishedSpy(&scanner, &Scanner::finished);

    scanner.scan({root}, {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);

    QCOMPARE(listedSpy.count(), 1);
    const QStringList paths = listedSpy.at(0).at(0).toStringList();
    QCOMPARE(paths, QStringList{canonical(withinCap)});
}

QTEST_MAIN(TestScannerFlatpak)
#include "tst_scanner_flatpak.moc"
