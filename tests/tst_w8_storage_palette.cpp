#include <QtTest>

#include "tsan_shims.h"

#include "ai/aiservices.h"
#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/gemini.h"
#include "ai/illustrator.h"
#include "ai/keystore.h"
#include "ai/localsetup.h"
#include "app/theme.h"
#include "app/translator.h"
#include "audio/audiocache.h"
#include "audio/narration.h"
#include "formats/bookscheme.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "library/librarymodel.h"
#include "library/libraryschema.h"
#include "library/marks.h"
#include "reader/reader.h"
#include "testbooks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

namespace {

QByteArray makeNamedEpub(const QByteArray &title, const QByteArray &author, const QByteArray &body,
                         const QByteArray &series = {}, double seriesIndex = 0.0) {
    QByteArray seriesMeta;
    if (!series.isEmpty()) {
        seriesMeta = "<meta name=\"calibre:series\" content=\"" + series + "\"/>"
                     "<meta name=\"calibre:series_index\" content=\"" + QByteArray::number(seriesIndex) + "\"/>";
    }
    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"2.0\" xmlns=\"http://www.idpf.org/opf\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:title>" + title + "</dc:title>\n"
        "    <dc:creator>" + author + "</dc:creator>\n"
        "    <dc:language>en</dc:language>\n" +
        seriesMeta +
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
}

int countRows(QSqlDatabase &db, const QString &table, int bookId = -1) {
    QSqlQuery q(db);
    if (bookId >= 0) {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE book_id = ?").arg(table));
        q.addBindValue(bookId);
        return q.exec() && q.next() ? q.value(0).toInt() : -1;
    }
    return q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)) && q.next() ? q.value(0).toInt() : -1;
}

bool hasCascadeFkToBooks(QSqlDatabase &db, const QString &table) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA foreign_key_list(%1)").arg(table)))
        return false;
    while (q.next()) {
        const QString refTable = q.value(2).toString();
        const QString fromCol = q.value(3).toString();
        const QString toCol = q.value(4).toString();
        const QString onDelete = q.value(6).toString();
        if (refTable == QLatin1String("books") && fromCol == QLatin1String("book_id") &&
            toCol == QLatin1String("id") && onDelete.compare(QLatin1String("CASCADE"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool hasIndex(QSqlDatabase &db, const QString &indexName) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = ?"));
    q.addBindValue(indexName);
    return q.exec() && q.next();
}

}  // namespace

class W8StoragePaletteTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qputenv("CLOTHBOUND_MUTE", "1");
    }

    // 1. SQLite v4 -> v5 migration: orphan pruning, data preservation, index
    //    recreation, ON DELETE CASCADE across all 5 child tables, FK enforcement.
    void v4ToV5MigrationCascadeAndOrphanCleanup() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        QVERIFY(QDir().mkpath(dataDir));
        const QString dbPath = dataDir + QStringLiteral("/library.sqlite");

        // Seed an exact v4 database (NO foreign keys on the 4 child tables) with
        // valid rows for books 1 & 2 PLUS orphaned rows pointing to nonexistent book 9999.
        const QString seedConn = QStringLiteral("w8-seed-v4");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seedConn);
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA foreign_keys = OFF")));
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
                " hidden INTEGER NOT NULL DEFAULT 0, loose INTEGER NOT NULL DEFAULT 0,"
                " fingerprint TEXT NOT NULL DEFAULT '', want_at INTEGER NOT NULL DEFAULT 0, listened_at INTEGER NOT NULL DEFAULT 0)")));
            QVERIFY(q.exec(QStringLiteral("CREATE INDEX books_work ON books (work_key)")));
            QVERIFY(q.exec(QStringLiteral("CREATE INDEX books_fingerprint ON books (fingerprint)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE chapter_lengths (book_id INTEGER NOT NULL, chapter INTEGER NOT NULL,"
                " length INTEGER NOT NULL, PRIMARY KEY (book_id, chapter))")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE annotations ("
                " id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, kind TEXT NOT NULL,"
                " chapter INTEGER NOT NULL DEFAULT 0, start INTEGER NOT NULL DEFAULT 0, finish INTEGER NOT NULL DEFAULT 0,"
                " text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '',"
                " label TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
                " prefix TEXT NOT NULL DEFAULT '', suffix TEXT NOT NULL DEFAULT '')")));
            QVERIFY(q.exec(QStringLiteral("CREATE INDEX annotations_book ON annotations (book_id)")));
            QVERIFY(q.exec(QStringLiteral("CREATE INDEX annotations_updated ON annotations (updated_at DESC)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE reading_days (book_id INTEGER NOT NULL, day TEXT NOT NULL,"
                " seconds INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (book_id, day))")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE book_settings (book_id INTEGER NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,"
                " PRIMARY KEY (book_id, key))")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE collections (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL COLLATE NOCASE, created_at INTEGER NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE book_collections ("
                " book_id INTEGER NOT NULL, collection_id INTEGER NOT NULL, added_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, collection_id),"
                " FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
                " FOREIGN KEY (collection_id) REFERENCES collections(id) ON DELETE CASCADE)")));
            QVERIFY(q.exec(QStringLiteral("CREATE INDEX book_collections_col ON book_collections (collection_id)")));
            QVERIFY(q.exec(QStringLiteral("CREATE INDEX book_collections_book ON book_collections (book_id)")));

            // Insert 2 real books and 1 collection.
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO books (id, path, format, title, sort_title, author, sort_author, work_key, added_at) "
                "VALUES (1, '/v4/book1.epub', 'epub', 'Book One', 'book one', 'Author A', 'a, author', 'wk1', 100)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO books (id, path, format, title, sort_title, author, sort_author, work_key, added_at) "
                "VALUES (2, '/v4/book2.epub', 'epub', 'Book Two', 'book two', 'Author B', 'b, author', 'wk2', 200)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO collections (id, name, created_at) VALUES (10, 'Favorites', 100)")));

            // Seed valid child rows for book 1 & book 2, plus orphaned rows for book 9999.
            for (const int bid : {1, 2, 9999}) {
                QVERIFY(q.exec(QStringLiteral("INSERT INTO chapter_lengths (book_id, chapter, length) VALUES (%1, 0, 1200), (%1, 1, 2400)").arg(bid)));
                QVERIFY(q.exec(QStringLiteral(
                    "INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color, label, created_at, updated_at, prefix, suffix) "
                    "VALUES (%1, %2, 'highlight', 0, 10, 30, 'quote %2', 'note %2', 'yellow', 'Ch 1', 100, 200, 'pre %2', 'suf %2')")
                                   .arg(bid == 9999 ? 99 : bid * 10)
                                   .arg(bid)));
                QVERIFY(q.exec(QStringLiteral("INSERT INTO reading_days (book_id, day, seconds) VALUES (%1, '2026-07-01', 300)").arg(bid)));
                QVERIFY(q.exec(QStringLiteral("INSERT INTO book_settings (book_id, key, value) VALUES (%1, 'voiceName', 'Kokoro')").arg(bid)));
                QVERIFY(q.exec(QStringLiteral("INSERT INTO book_collections (book_id, collection_id, added_at) VALUES (%1, 10, 100)").arg(bid)));
            }

            QVERIFY(q.exec(QStringLiteral("INSERT INTO meta (key, value) VALUES ('schema', '4')")));
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 4")));
            db.close();
        }
        QSqlDatabase::removeDatabase(seedConn);

        // Open via Library -> must atomically migrate v4 -> v5.
        Library lib(dataDir, cacheDir);
        QVERIFY2(lib.open(), qPrintable(lib.errorString()));
        QCOMPARE(LibrarySchema::kLatestVersion, 5);
        QCOMPARE(lib.schemaVersion(), 5);
        QCOMPARE(LibrarySavepoint::activeDepth(), 0);

        QSqlDatabase db = lib.database();
        {
            QSqlQuery fkOn(db);
            QVERIFY(fkOn.exec(QStringLiteral("PRAGMA foreign_keys")) && fkOn.next());
            QCOMPARE(fkOn.value(0).toInt(), 1);
        }
        {
            QSqlQuery fkChk(db);
            QVERIFY(fkChk.exec(QStringLiteral("PRAGMA foreign_key_check")));
            QVERIFY(!fkChk.next());
        }
        {
            QSqlQuery metaQ(db);
            QVERIFY(metaQ.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'schema'")) && metaQ.next());
            QCOMPARE(metaQ.value(0).toString(), QStringLiteral("5"));
        }

        // Verify CASCADE foreign keys exist on all 5 child tables and annotation indexes were recreated.
        const QStringList childTables = {
            QStringLiteral("annotations"),
            QStringLiteral("chapter_lengths"),
            QStringLiteral("reading_days"),
            QStringLiteral("book_settings"),
            QStringLiteral("book_collections"),
        };
        for (const QString &table : childTables)
            QVERIFY2(hasCascadeFkToBooks(db, table), qPrintable(table));
        QVERIFY(hasIndex(db, QStringLiteral("annotations_book")));
        QVERIFY(hasIndex(db, QStringLiteral("annotations_updated")));

        // Verify all orphans (book_id = 9999) were pruned, while valid data for books 1 & 2 survived intact.
        for (const QString &table : childTables) {
            QCOMPARE(countRows(db, table, 9999), 0);
            QVERIFY2(countRows(db, table, 1) > 0, qPrintable(table));
            QVERIFY2(countRows(db, table, 2) > 0, qPrintable(table));
        }
        {
            QSqlQuery ann(db);
            QVERIFY(ann.exec(QStringLiteral("SELECT id, text, note, prefix, suffix FROM annotations WHERE book_id = 2")));
            QVERIFY(ann.next());
            QCOMPARE(ann.value(0).toInt(), 20);
            QCOMPARE(ann.value(1).toString(), QStringLiteral("quote 2"));
            QCOMPARE(ann.value(2).toString(), QStringLiteral("note 2"));
            QCOMPARE(ann.value(3).toString(), QStringLiteral("pre 2"));
            QCOMPARE(ann.value(4).toString(), QStringLiteral("suf 2"));
        }

        // Direct SQL delete of book 1 must cascade across ALL 5 child tables without touching book 2.
        {
            QSqlQuery del(db);
            QVERIFY2(del.exec(QStringLiteral("DELETE FROM books WHERE id = 1")), qPrintable(del.lastError().text()));
        }
        for (const QString &table : childTables) {
            QCOMPARE(countRows(db, table, 1), 0);
            QVERIFY2(countRows(db, table, 2) > 0, qPrintable(table));
        }

        // Foreign key enforcement rejects inserting child rows for a nonexistent book_id.
        {
            QSqlQuery bad(db);
            QVERIFY(!bad.exec(QStringLiteral("INSERT INTO book_settings (book_id, key, value) VALUES (7777, 'k', 'v')")));
            QVERIFY(!bad.exec(QStringLiteral("INSERT INTO chapter_lengths (book_id, chapter, length) VALUES (7777, 0, 10)")));
            QVERIFY(!bad.exec(QStringLiteral("INSERT INTO reading_days (book_id, day, seconds) VALUES (7777, '2026-07-02', 10)")));
            QVERIFY(!bad.exec(QStringLiteral(
                "INSERT INTO annotations (book_id, kind, text, created_at, updated_at) VALUES (7777, 'bookmark', 'x', 1, 1)")));
        }
    }

    // 2. Fail-closed user_version > 5 (6 and 99) for both open() and openReadOnly(),
    //    plus pre-v1 -> v5 full migration chain.
    void failClosedFutureVersionAndLegacyUpgrade() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        const QString dbPath = dataDir + QStringLiteral("/library.sqlite");

        {
            Library lib(dataDir, cacheDir);
            QVERIFY(lib.open());
            QCOMPARE(lib.schemaVersion(), 5);
        }

        // Bump user_version to 6 (immediately above kLatestVersion = 5): must fail closed.
        for (const int futureVer : {6, 99}) {
            const QString conn = QStringLiteral("w8-future-%1").arg(futureVer);
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(dbPath);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = %1").arg(futureVer)));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);

            Library rw(dataDir, cacheDir);
            QVERIFY(!rw.open());
            QVERIFY2(rw.errorString().contains(QString::number(futureVer)), qPrintable(rw.errorString()));
            QVERIFY2(rw.errorString().contains(QStringLiteral("5")), qPrintable(rw.errorString()));

            Library ro(dataDir, cacheDir);
            QVERIFY(!ro.openReadOnly(dbPath));
            QVERIFY2(ro.errorString().contains(QString::number(futureVer)), qPrintable(ro.errorString()));
            QVERIFY2(ro.errorString().contains(QStringLiteral("5")), qPrintable(ro.errorString()));
        }

        // Reset back to 5 and confirm openReadOnly succeeds.
        {
            const QString conn = QStringLiteral("w8-reset-v5");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(dbPath);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 5")));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);

            Library ro(dataDir, cacheDir);
            QVERIFY2(ro.openReadOnly(dbPath), qPrintable(ro.errorString()));
            QCOMPARE(ro.schemaVersion(), 5);
        }

        // Pre-v1 (user_version = 0) legacy DB migrates cleanly 0 -> 1 -> 2 -> 3 -> 4 -> 5
        // preserving legacy rows and enforcing ON DELETE CASCADE on all 5 child tables.
        const QString legData = dir.filePath(QStringLiteral("legacy-data"));
        const QString legCache = dir.filePath(QStringLiteral("legacy-cache"));
        QVERIFY(QDir().mkpath(legData));
        const QString legDb = legData + QStringLiteral("/library.sqlite");
        {
            const QString conn = QStringLiteral("w8-legacy-v0");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(legDb);
                QVERIFY(db.open());
                QSqlQuery q(db);
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
                    "INSERT INTO books (id, path, format, title, sort_title, work_key, added_at) "
                    "VALUES (1, '/v0/a.epub', 'epub', 'Legacy V0', 'legacy v0', 'wk0', 100)")));
                QVERIFY(q.exec(QStringLiteral(
                    "INSERT INTO annotations (id, book_id, kind, text, note, created_at, updated_at) "
                    "VALUES (5, 1, 'highlight', 'v0 quote', 'v0 note', 100, 100)")));
                QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 0")));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);

            Library migrated(legData, legCache);
            QVERIFY2(migrated.open(), qPrintable(migrated.errorString()));
            QCOMPARE(migrated.schemaVersion(), 5);
            QCOMPARE(migrated.record(1).title, QStringLiteral("Legacy V0"));
            QSqlDatabase db = migrated.database();
            QVERIFY(hasCascadeFkToBooks(db, QStringLiteral("annotations")));
            QVERIFY(hasCascadeFkToBooks(db, QStringLiteral("chapter_lengths")));
            QVERIFY(hasCascadeFkToBooks(db, QStringLiteral("reading_days")));
            QVERIFY(hasCascadeFkToBooks(db, QStringLiteral("book_settings")));
            QVERIFY(hasCascadeFkToBooks(db, QStringLiteral("book_collections")));
            QCOMPARE(countRows(db, QStringLiteral("annotations"), 1), 1);
            QSqlQuery del(db);
            QVERIFY(del.exec(QStringLiteral("DELETE FROM books WHERE id = 1")));
            QCOMPARE(countRows(db, QStringLiteral("annotations"), 1), 0);
        }
    }

    // 3. Shared Library / WorksModel canonical BookRecord store (zero duplicate
    //    QHash<int, BookRecord> copy) + standalone WorksModel without Library.
    void sharedLibraryWorksModelStoreAndStandalone() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));

        const QString epubA = dir.filePath(QStringLiteral("alpha.epub"));
        const QString epubB = dir.filePath(QStringLiteral("beta.epub"));
        QVERIFY(TestBooks::write(epubA, makeNamedEpub("Alpha Signal", "Ada Lovelace", "<p>First book text.</p>",
                                                      "Chronicles", 1.0)));
        QVERIFY(TestBooks::write(epubB, makeNamedEpub("Beta Signal", "Grace Hopper", "<p>Second book text.</p>",
                                                      "Chronicles", 2.0)));

        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        WorksModel *works = library.works();
        QVERIFY(works != nullptr);
        QVERIFY(works->usesLibraryStore());
        QCOMPARE(works->ownedBookCount(), qsizetype(0));

        const int idA = library.addBookFile(epubA);
        const int idB = library.addBookFile(epubB);
        QVERIFY(idA > 0 && idB > 0);
        QCOMPARE(works->count(), 2);
        QVERIFY(works->usesLibraryStore());
        QCOMPARE(works->ownedBookCount(), qsizetype(0));

        // Canonical pointer check: bookRecord() points directly into bookRecords().
        const BookRecord *ptrA = library.bookRecord(idA);
        QVERIFY(ptrA != nullptr);
        QCOMPARE(ptrA, &library.bookRecords().constFind(idA).value());
        QCOMPARE(library.bookRecord(99999), nullptr);

        // In-place savePosition under "recent" sort emits targeted single-row dataChanged (0 modelReset).
        library.markOpened(idA);
        library.markOpened(idB);
        QTest::qWait(80);
        works->setSortOrder(QStringLiteral("recent"));
        QSignalSpy resetSpy(works, &QAbstractItemModel::modelReset);
        QSignalSpy dataSpy(works, &QAbstractItemModel::dataChanged);
        library.savePosition(idA, QStringLiteral(R"({"chapter":0,"offset":12})"), 0.25);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(dataSpy.count(), 1);
        QCOMPARE(works->data(works->index(works->rowOfBook(idA)), WorksModel::ProgressRole).toDouble(), 0.25);
        QCOMPARE(works->ownedBookCount(), qsizetype(0));

        // Under "progress" sort, advancing idA past idB re-sorts accurately using previous-record diff.
        library.savePosition(idB, QStringLiteral(R"({"chapter":0,"offset":30})"), 0.50);
        works->setSortOrder(QStringLiteral("progress"));
        QCOMPARE(works->get(0).value(QStringLiteral("bookId")).toInt(), idB);
        QCOMPARE(works->get(1).value(QStringLiteral("bookId")).toInt(), idA);

        library.savePosition(idA, QStringLiteral(R"({"chapter":0,"offset":90})"), 0.85);
        QCOMPARE(works->get(0).value(QStringLiteral("bookId")).toInt(), idA);
        QCOMPARE(works->get(1).value(QStringLiteral("bookId")).toInt(), idB);
        QCOMPARE(works->ownedBookCount(), qsizetype(0));

        // Shelf & series filters remain consistent with Library mutations.
        library.setFinished(idB, true);
        QTest::qWait(80);
        works->setStatusFilter(QStringLiteral("finished"));
        QCOMPARE(works->count(), 1);
        QCOMPARE(works->get(0).value(QStringLiteral("bookId")).toInt(), idB);
        QCOMPARE(works->ownedBookCount(), qsizetype(0));

        // Standalone WorksModel without a Library (using setBooks(QList<BookRecord>)).
        WorksModel standalone(nullptr);
        QVERIFY(!standalone.usesLibraryStore());
        BookRecord r1, r2, r3;
        r1.id = 101;
        r1.title = QStringLiteral("Gödel, Escher, Bach");
        r1.sortTitle = QStringLiteral("godel, escher, bach");
        r1.author = QStringLiteral("Douglas Hofstadter");
        r1.sortAuthor = QStringLiteral("hofstadter, douglas");
        r1.format = QStringLiteral("epub");
        r1.workKey = QStringLiteral("geb");
        r1.series = QStringLiteral("Mind");
        r1.seriesIndex = 1.0;
        r1.progress = 0.2;
        r1.openedAt = 100;
        r1.addedAt = 100;

        r2.id = 102;
        r2.title = QStringLiteral("The Art of Computer Programming");
        r2.sortTitle = QStringLiteral("art of computer programming");
        r2.author = QStringLiteral("Donald Knuth");
        r2.sortAuthor = QStringLiteral("knuth, donald");
        r2.format = QStringLiteral("pdf");
        r2.workKey = QStringLiteral("taocp");
        r2.progress = 0.7;
        r2.openedAt = 200;
        r2.addedAt = 200;

        r3.id = 103;
        r3.title = QStringLiteral("Hidden Draft");
        r3.sortTitle = QStringLiteral("hidden draft");
        r3.format = QStringLiteral("epub");
        r3.hidden = true;

        standalone.setBooks(QList<BookRecord>{r1, r2, r3});
        QVERIFY(!standalone.usesLibraryStore());
        QCOMPARE(standalone.ownedBookCount(), qsizetype(3));
        QCOMPARE(standalone.count(), 2);
        QCOMPARE(standalone.get(0).value(QStringLiteral("bookId")).toInt(), 102);

        standalone.setSearchText(QStringLiteral("godel"));
        QCOMPARE(standalone.count(), 1);
        QCOMPARE(standalone.get(0).value(QStringLiteral("bookId")).toInt(), 101);

        standalone.setSearchText(QString());
        standalone.setSortOrder(QStringLiteral("progress"));
        QCOMPARE(standalone.get(0).value(QStringLiteral("bookId")).toInt(), 102);
        r1.progress = 0.9;
        standalone.updateBook(r1);
        QCOMPARE(standalone.get(0).value(QStringLiteral("bookId")).toInt(), 101);
        QCOMPARE(standalone.data(standalone.index(0), WorksModel::MinutesLeftRole).toInt(), 0);
    }

    // 4. Off-GUI-thread searchPaletteAsync: books (diacritics/fuzzy) + FTS5
    //    passages + annotations marks + scoping + stale-request cancellation +
    //    read-only safety + immediate-destroy safety.
    void offGuiThreadPaletteSearchAndCancellation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));

        const QString epub1 = dir.filePath(QStringLiteral("miserables.epub"));
        const QString epub2 = dir.filePath(QStringLiteral("lantern.epub"));
        const QString epub3 = dir.filePath(QStringLiteral("uber.epub"));
        QVERIFY(TestBooks::write(
            epub1, makeNamedEpub("Les Misérables", "Victor Hugo",
                                 "<p>Jean Valjean crossed the Paris barricades under autumn stars.</p>")));
        QVERIFY(TestBooks::write(
            epub2, makeNamedEpub("The Lantern Keeper", "Eleanor Vance",
                                 "<p>The lantern keeper walked along the weathered basalt seawall watching northern fog.</p>")));
        QVERIFY(TestBooks::write(
            epub3, makeNamedEpub("Über die Liebe", "Rainer Maria Rilke",
                                 "<p>Ein ruhiger Abend am Meer mit fernen Glocken.</p>")));

        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        const int id1 = library.addBookFile(epub1);
        const int id2 = library.addBookFile(epub2);
        const int id3 = library.addBookFile(epub3);
        QVERIFY(id1 > 0 && id2 > 0 && id3 > 0);

        // Index books into cacheDir/index.sqlite for FTS5 passage search.
        const QString idxPath = cacheDir + QStringLiteral("/index.sqlite");
        LibraryIndex libraryIndex(idxPath);
        QVERIFY(libraryIndex.available());
        QVERIFY(LibraryIndex::indexBook(idxPath, {id1, epub1, QStringLiteral("epub"), 100}));
        QVERIFY(LibraryIndex::indexBook(idxPath, {id2, epub2, QStringLiteral("epub"), 100}));
        QVERIFY(LibraryIndex::indexBook(idxPath, {id3, epub3, QStringLiteral("epub"), 100}));

        // Add annotations in both book 1 and book 2 to verify mark search and currentBookId boost.
        Marks marks(&library);
        marks.setBook(id1);
        const int m1 = marks.addHighlight(0, 0, 20, QStringLiteral("Paris barricades"), QStringLiteral("yellow"),
                                          QStringLiteral("Ch 1"));
        QVERIFY(m1 > 0);
        marks.setNote(m1, QStringLiteral("shared motif marginalia"));

        marks.setBook(id2);
        const int m2 = marks.addHighlight(0, 4, 24, QStringLiteral("basalt seawall"), QStringLiteral("blue"),
                                          QStringLiteral("Ch 1"));
        QVERIFY(m2 > 0);
        marks.setNote(m2, QStringLiteral("shared motif marginalia"));

        QSignalSpy readySpy(&library, &Library::paletteSearchReady);

        // 4a. Normalized diacritic-insensitive fuzzy book match runs asynchronously off GUI thread.
        const int reqBooks = library.searchPaletteAsync(QStringLiteral("miserables"), 0);
        QVERIFY(reqBooks > 0);
        QCOMPARE(readySpy.count(), 0);  // Must deliver via QueuedConnection, never synchronously inline!
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        {
            const QList<QVariant> args = readySpy.takeFirst();
            QCOMPARE(args.at(0).toInt(), reqBooks);
            const QVariantMap res = args.at(1).toMap();
            const QVariantList books = res.value(QStringLiteral("books")).toList();
            QVERIFY(!books.isEmpty());
            QCOMPARE(books.first().toMap().value(QStringLiteral("id")).toInt(), id1);
            QCOMPARE(books.first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Les Misérables"));
        }

        // 4b. Combined FTS5 passage search + annotations mark search.
        const int reqPassage = library.searchPaletteAsync(QStringLiteral("basalt seawall"), id2);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        {
            const QList<QVariant> args = readySpy.takeFirst();
            QCOMPARE(args.at(0).toInt(), reqPassage);
            const QVariantMap res = args.at(1).toMap();
            const QVariantList passages = res.value(QStringLiteral("passages")).toList();
            QVERIFY(!passages.isEmpty());
            const QVariantMap p = passages.first().toMap();
            QCOMPARE(p.value(QStringLiteral("bookId")).toInt(), id2);
            QCOMPARE(p.value(QStringLiteral("chapter")).toInt(), 0);
            QVERIFY(p.value(QStringLiteral("offset")).toDouble() > 0.0);
            QCOMPARE(p.value(QStringLiteral("bookTitle")).toString(), QStringLiteral("The Lantern Keeper"));

            const QVariantList foundMarks = res.value(QStringLiteral("marks")).toList();
            QVERIFY(!foundMarks.isEmpty());
            QCOMPARE(foundMarks.first().toMap().value(QStringLiteral("bookId")).toInt(), id2);
        }

        // 4c. currentBookId boosts active book's matching mark ahead of other books.
        const int reqBoost = library.searchPaletteAsync(QStringLiteral("shared motif marginalia"), id1);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        {
            const QList<QVariant> args = readySpy.takeFirst();
            QCOMPARE(args.at(0).toInt(), reqBoost);
            const QVariantMap res = args.at(1).toMap();
            const QVariantList foundMarks = res.value(QStringLiteral("marks")).toList();
            QCOMPARE(foundMarks.size(), 2);
            QCOMPARE(foundMarks.first().toMap().value(QStringLiteral("bookId")).toInt(), id1);
        }

        // 4d. scopeBookId restricts passages and marks to one book and omits cross-book books list.
        const int reqScoped = library.searchPaletteAsync(QStringLiteral("barricades"), id2, 6, 6, 6, id2,
                                                         QStringLiteral("all"), idxPath);
        QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
        {
            const QVariantMap res = readySpy.takeFirst().at(1).toMap();
            QCOMPARE(res.value(QStringLiteral("requestId")).toInt(), reqScoped);
            QVERIFY(res.value(QStringLiteral("books")).toList().isEmpty());
            QVERIFY(res.value(QStringLiteral("passages")).toList().isEmpty());
            QVERIFY(res.value(QStringLiteral("marks")).toList().isEmpty());
        }

        // 4e. Rapid-fire stale-request cancellation: only the final generation may emit.
        int lastReq = 0;
        for (int i = 0; i < 30; ++i) {
            const QString q = (i % 2 == 0) ? QStringLiteral("lantern") : QStringLiteral("barricades");
            lastReq = library.searchPaletteAsync(q, 0);
        }
        lastReq = library.searchPaletteAsync(QStringLiteral("uber"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);
        QTest::qWait(80);
        QCOMPARE(readySpy.count(), 1);
        {
            const QList<QVariant> args = readySpy.takeFirst();
            QCOMPARE(args.at(0).toInt(), lastReq);
            const QVariantMap res = args.at(1).toMap();
            QCOMPARE(res.value(QStringLiteral("query")).toString(), QStringLiteral("uber"));
            const QVariantList books = res.value(QStringLiteral("books")).toList();
            QVERIFY(!books.isEmpty());
            QCOMPARE(books.first().toMap().value(QStringLiteral("id")).toInt(), id3);
        }

        // 4f. Explicit cancelPaletteSearch() suppresses emission.
        library.searchPaletteAsync(QStringLiteral("lantern"), 0);
        library.cancelPaletteSearch();
        QTest::qWait(80);
        QCOMPARE(readySpy.count(), 0);

        // 4g. Read-only Library supports searchPaletteAsync without touching disk.
        {
            Library ro(dataDir, cacheDir);
            QVERIFY2(ro.openReadOnly(dataDir + QStringLiteral("/library.sqlite")), qPrintable(ro.errorString()));
            QSignalSpy roSpy(&ro, &Library::paletteSearchReady);
            const int roReq = ro.searchPaletteAsync(QStringLiteral("basalt seawall"), id2);
            QTRY_COMPARE_WITH_TIMEOUT(roSpy.count(), 1, 5000);
            const QVariantMap res = roSpy.takeFirst().at(1).toMap();
            QCOMPARE(res.value(QStringLiteral("requestId")).toInt(), roReq);
            QVERIFY(!res.value(QStringLiteral("passages")).toList().isEmpty());
            QVERIFY(!res.value(QStringLiteral("marks")).toList().isEmpty());
        }

        // 4h. Immediate destruction while async searches are queued/in-flight joins cleanly via waitForDone().
        {
            Library ephemeral(dataDir, cacheDir);
            QVERIFY(ephemeral.open());
            for (int i = 0; i < 12; ++i)
                ephemeral.searchPaletteAsync(QStringLiteral("basalt seawall"), 0);
        }
    }

    // 5. Live CommandPalette.qml async wiring + zero QML warnings + zero sync
    //    libraryIndex.search() / marks.searchAll() on GUI thread.
    void commandPaletteQmlAsyncWiring() {
        const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
        QVERIFY(!srcRoot.isEmpty());

        QFile qmlFile(srcRoot + QStringLiteral("/ui/CommandPalette.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly));
        const QString cpQml = QString::fromUtf8(qmlFile.readAll());
        QVERIFY2(cpQml.contains(QStringLiteral("library.searchPaletteAsync(")),
                 "CommandPalette.qml must dispatch search via library.searchPaletteAsync()");
        QVERIFY2(cpQml.contains(QStringLiteral("onPaletteSearchReady(")),
                 "CommandPalette.qml must receive async results via onPaletteSearchReady()");
        QVERIFY2(!cpQml.contains(QStringLiteral("libraryIndex.search(")),
                 "CommandPalette.qml must not call synchronous libraryIndex.search() on GUI thread");
        QVERIFY2(!cpQml.contains(QStringLiteral("marks.searchAll(")),
                 "CommandPalette.qml must not call synchronous marks.searchAll() on GUI thread");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dataDir = dir.filePath(QStringLiteral("data"));
        const QString cacheDir = dir.filePath(QStringLiteral("cache"));
        const QString epubPath = dir.filePath(QStringLiteral("seawall.epub"));
        QVERIFY(TestBooks::write(
            epubPath, makeNamedEpub("The Lantern Keeper", "Eleanor Vance",
                                    "<p>Weathered basalt seawall standing beside northern harbor fog.</p>")));

        Library library(dataDir, cacheDir);
        QVERIFY(library.open());
        const int bookId = library.addBookFile(epubPath);
        QVERIFY(bookId > 0);

        const QString idxPath = cacheDir + QStringLiteral("/index.sqlite");
        LibraryIndex libraryIndex(idxPath);
        QVERIFY(libraryIndex.available());
        QVERIFY(LibraryIndex::indexBook(idxPath, {bookId, epubPath, QStringLiteral("epub"), 100}));

        Marks marks(&library);
        marks.setBook(bookId);
        const int markId = marks.addHighlight(0, 0, 24, QStringLiteral("Weathered basalt seawall"),
                                              QStringLiteral("yellow"), QStringLiteral("Ch 1"));
        QVERIFY(markId > 0);
        marks.setNote(markId, QStringLiteral("Basalt note for palette"));

        BookScheme scheme;
        Reader reader(&library, &scheme);
        BookSearch bookSearch(&reader);
        GeminiProvider gemini;
        MemoryKeyStore keys;
        AiServices ai(&gemini, &keys, library.database().connectionName());
        AudioCache audioCache(dir.filePath(QStringLiteral("audio")), library.database().connectionName());
        Narration narration(&ai, &audioCache, &reader, &library);
        Companion companion(&ai, &reader, library.database().connectionName());
        Illustrator illustrator(&ai, &reader, library.database().connectionName(), dir.filePath(QStringLiteral("pics")));
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

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent cpComp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/CommandPalette.qml")));
        QVERIFY2(cpComp.status() == QQmlComponent::Ready, qPrintable(cpComp.errorString()));
        std::unique_ptr<QObject> cpObj(cpComp.createWithInitialProperties({
            {QStringLiteral("width"), 1000.0},
            {QStringLiteral("height"), 760.0},
        }));
        QVERIFY(cpObj != nullptr);

        QMetaObject::invokeMethod(cpObj.get(), "search", Q_ARG(QVariant, QStringLiteral("basalt")));
        QTRY_VERIFY_WITH_TIMEOUT(cpObj->property("hasAsyncResults").toBool(), 5000);

        const QVariantList results = cpObj->property("results").toList();
        bool sawPassage = false;
        bool sawMark = false;
        for (const QVariant &rowVar : results) {
            const QVariantMap row = rowVar.toMap();
            const QString kind = row.value(QStringLiteral("kind")).toString();
            if (kind == QLatin1String("text") && row.value(QStringLiteral("bookId")).toInt() == bookId)
                sawPassage = true;
            if (kind == QLatin1String("mark") && row.value(QStringLiteral("bookId")).toInt() == bookId)
                sawMark = true;
        }
        QVERIFY2(sawPassage, "Expected off-GUI-thread FTS5 passage hit in CommandPalette results");
        QVERIFY2(sawMark, "Expected off-GUI-thread annotation mark hit in CommandPalette results");

        QMetaObject::invokeMethod(cpObj.get(), "close");
        QVERIFY(cpObj->property("cachedBooks").toList().isEmpty());
        QCOMPARE(cpObj->property("activeSearchRequestId").toInt(), 0);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
    }
};

QTEST_MAIN(W8StoragePaletteTests)
#include "tst_w8_storage_palette.moc"
