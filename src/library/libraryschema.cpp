#include "library/libraryschema.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>
#include <cstring>

namespace {
thread_local int s_savepointDepth = 0;

void setError(QString *errorOut, const QString &message) {
    if (errorOut)
        *errorOut = message;
}

bool execChecked(QSqlQuery &query, QString *errorOut) {
    if (query.exec())
        return true;
    const QString err = query.lastError().text();
    setError(errorOut, err);
    qWarning("Library: %s", qPrintable(err));
    return false;
}
}  // namespace

LibrarySavepoint::LibrarySavepoint(QSqlDatabase database, const char *label)
    : m_db(std::move(database)),
      m_name(QStringLiteral("%1_%2").arg(QLatin1String(label)).arg(s_savepointDepth + 1)) {
    Q_ASSERT(s_savepointDepth >= 0);
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    m_active = q.exec(QStringLiteral("SAVEPOINT ") + m_name);
    if (m_active) {
        ++s_savepointDepth;
        Q_ASSERT(s_savepointDepth > 0);
    } else {
        qWarning("Library: SAVEPOINT %s failed: %s", label, qPrintable(q.lastError().text()));
    }
}

bool LibrarySavepoint::commit() {
    if (!m_active)
        return false;
    Q_ASSERT(s_savepointDepth > 0);
    m_active = false;
    --s_savepointDepth;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("RELEASE SAVEPOINT ") + m_name))
        return true;
    qWarning("Library: RELEASE SAVEPOINT %s failed: %s", qPrintable(m_name), qPrintable(q.lastError().text()));
    QSqlQuery rb(m_db);
    rb.exec(QStringLiteral("ROLLBACK TO SAVEPOINT ") + m_name);
    rb.exec(QStringLiteral("RELEASE SAVEPOINT ") + m_name);
    return false;
}

void LibrarySavepoint::rollback() {
    if (!m_active)
        return;
    Q_ASSERT(s_savepointDepth > 0);
    m_active = false;
    --s_savepointDepth;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("ROLLBACK TO SAVEPOINT ") + m_name);
    q.exec(QStringLiteral("RELEASE SAVEPOINT ") + m_name);
}

LibrarySavepoint::~LibrarySavepoint() {
    rollback();
    Q_ASSERT(!m_active);
    Q_ASSERT(s_savepointDepth >= 0);
}

int LibrarySavepoint::activeDepth() { return s_savepointDepth; }

namespace LibrarySchema {

const QString &bookColumns() {
    static const QString cols = QStringLiteral(
        "id, path, format, size, mtime, title, sort_title, author, sort_author, editor, publisher, year, language, "
        "description, subjects, isbn, series, series_index, work_key, chapters, pages, length, cover, problem, "
        "added_at, opened_at, finished_at, progress, position, seconds_read, missing, hidden, loose, want_at, listened_at");
    return cols;
}

bool configureConnection(QSqlDatabase &db, bool readOnly, QString *errorOut) {
    if (!db.isOpen()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    QSqlQuery pragma(db);
    // Set busy_timeout first so journal_mode=WAL (when read-write) or reads
    // wait rather than failing immediately with SQLITE_BUSY.
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    if (readOnly) {
        pragma.exec(QStringLiteral("PRAGMA query_only=ON"));
    } else {
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    }
    pragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA cache_size=-16000"));
    pragma.exec(QStringLiteral("PRAGMA mmap_size=67108864"));
    return true;
}

void closeConnection(const QString &connectionName) {
    if (!QSqlDatabase::contains(connectionName))
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

bool openConnection(const QString &connectionName, const QString &dbPath, bool readOnly, QString *errorOut) {
    closeConnection(connectionName);
    if (readOnly) {
        if (!QFileInfo::exists(dbPath)) {
            setError(errorOut, QCoreApplication::translate("Library", "%1 doesn't exist.").arg(dbPath));
            return false;
        }
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));

        bool ready = db.open() && configureConnection(db, true, errorOut);
        bool versionReadable = false;
        if (ready) {
            QSqlQuery probe(db);
            versionReadable = probe.exec(QStringLiteral("PRAGMA user_version")) && probe.next();
            if (!versionReadable)
                setError(errorOut, probe.lastError().text());
        } else {
            setError(errorOut, db.lastError().text());
        }

        // A WAL-mode SQLite file inside a read-only directory (e.g. chmod 0555)
        // cannot create a -shm index file. When no non-empty -wal sidecar exists,
        // retry strictly with URI mode=ro&immutable=1 so zero disk writes occur.
        const QFileInfo walInfo(dbPath + QStringLiteral("-wal"));
        if ((!ready || !versionReadable) && (!walInfo.exists() || walInfo.size() == 0)) {
            db = QSqlDatabase();
            closeConnection(connectionName);
            setError(errorOut, QString());
            const QString absPath = QFileInfo(dbPath).absoluteFilePath();
            const QString uri = QStringLiteral("file:") + QString::fromUtf8(QUrl::toPercentEncoding(absPath, "/:")) +
                                QStringLiteral("?mode=ro&immutable=1");
            db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_URI;QSQLITE_OPEN_READONLY"));
            db.setDatabaseName(uri);
            ready = db.open() && configureConnection(db, true, errorOut);
            if (!ready)
                setError(errorOut, db.lastError().text());
        }
        if (!ready || !validateReadOnly(db, errorOut)) {
            db = QSqlDatabase();
            closeConnection(connectionName);
            return false;
        }
        return true;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        setError(errorOut, db.lastError().text());
        db = QSqlDatabase();
        closeConnection(connectionName);
        return false;
    }
    if (!configureConnection(db, false, errorOut) || !migrate(db, errorOut)) {
        db = QSqlDatabase();
        closeConnection(connectionName);
        return false;
    }
    return true;
}

int schemaVersion(const QSqlDatabase &db) {
    if (!db.isOpen())
        return 0;
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
        return q.value(0).toInt();
    return 0;
}

bool hasTable(const QSqlDatabase &db, const QString &table, bool *out, QString *errorOut) {
    Q_ASSERT(out != nullptr);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1"));
    q.addBindValue(table);
    if (!q.exec()) {
        setError(errorOut, q.lastError().text());
        return false;
    }
    *out = q.next();
    return true;
}

bool hasColumn(const QSqlDatabase &db, const QString &table, const QString &column, bool *out, QString *errorOut) {
    Q_ASSERT(out != nullptr);
    QSqlQuery cols(db);
    if (!cols.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        setError(errorOut, cols.lastError().text());
        return false;
    }
    *out = false;
    while (cols.next()) {
        if (cols.value(1).toString() == column) {
            *out = true;
            break;
        }
    }
    return true;
}

bool validateReadOnly(QSqlDatabase &db, QString *errorOut) {
    int version = 0;
    {
        QSqlQuery vq(db);
        if (!vq.exec(QStringLiteral("PRAGMA user_version")) || !vq.next()) {
            setError(errorOut, vq.lastError().text());
            return false;
        }
        version = vq.value(0).toInt();
    }
    if (version > kLatestVersion) {
        setError(errorOut,
                 QCoreApplication::translate(
                     "Library", "This library was created by a newer version of Clothbound (schema %1; supported up to %2).")
                     .arg(version)
                     .arg(kLatestVersion));
        return false;
    }
    bool booksTable = false;
    if (!hasTable(db, QStringLiteral("books"), &booksTable, errorOut))
        return false;
    if (!booksTable) {
        setError(errorOut, QCoreApplication::translate("Library", "This database does not contain a Clothbound library."));
        return false;
    }
    return true;
}

bool migrate(QSqlDatabase &db, QString *errorOut) {
    int version = 0;
    {
        QSqlQuery vq(db);
        if (!vq.exec(QStringLiteral("PRAGMA user_version")) || !vq.next()) {
            setError(errorOut, vq.lastError().text());
            return false;
        }
        version = vq.value(0).toInt();
    }
    if (version > kLatestVersion) {
        setError(errorOut,
                 QCoreApplication::translate(
                     "Library", "This library was created by a newer version of Clothbound (schema %1; supported up to %2).")
                     .arg(version)
                     .arg(kLatestVersion));
        return false;
    }
    if (version == kLatestVersion)
        return true;

    auto execList = [&](const QStringList &statements) -> bool {
        for (const QString &statement : statements) {
            QSqlQuery query(db);
            if (!query.exec(statement)) {
                setError(errorOut, query.lastError().text());
                return false;
            }
        }
        return true;
    };
    auto setVersion = [&](int nextVersion) -> bool {
        Q_ASSERT(nextVersion > version && nextVersion <= kLatestVersion);
        QSqlQuery vq(db);
        if (!vq.exec(QStringLiteral("PRAGMA user_version = %1").arg(nextVersion))) {
            setError(errorOut, vq.lastError().text());
            return false;
        }
        QSqlQuery mq(db);
        mq.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES ('schema', ?)"));
        mq.addBindValue(QString::number(nextVersion));
        if (!mq.exec()) {
            setError(errorOut, mq.lastError().text());
            return false;
        }
        version = nextVersion;
        return true;
    };

    LibrarySavepoint sp(db, "migrate");
    if (!sp.isActive()) {
        setError(errorOut, db.lastError().text());
        return false;
    }

    // Migration 1: base schema + content fingerprint column/index.
    if (version < 1) {
        const QStringList v1 = {
            QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS folders (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, added_at INTEGER NOT NULL)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS books ("
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
                " fingerprint TEXT NOT NULL DEFAULT '')"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS books_work ON books (work_key)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS chapter_lengths (book_id INTEGER NOT NULL, chapter INTEGER NOT NULL,"
                " length INTEGER NOT NULL, PRIMARY KEY (book_id, chapter))"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS annotations ("
                " id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, kind TEXT NOT NULL,"
                " chapter INTEGER NOT NULL DEFAULT 0, start INTEGER NOT NULL DEFAULT 0, finish INTEGER NOT NULL DEFAULT 0,"
                " text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '',"
                " label TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS annotations_book ON annotations (book_id)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS reading_days (book_id INTEGER NOT NULL, day TEXT NOT NULL,"
                " seconds INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (book_id, day))"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS book_settings (book_id INTEGER NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,"
                " PRIMARY KEY (book_id, key))"),
        };
        if (!execList(v1))
            return false;
        bool hasFingerprint = false;
        if (!hasColumn(db, QStringLiteral("books"), QStringLiteral("fingerprint"), &hasFingerprint, errorOut))
            return false;
        if (!hasFingerprint &&
            !execList({QStringLiteral("ALTER TABLE books ADD COLUMN fingerprint TEXT NOT NULL DEFAULT ''")}))
            return false;
        if (!execList({QStringLiteral("CREATE INDEX IF NOT EXISTS books_fingerprint ON books (fingerprint)")}))
            return false;
        if (!setVersion(1))
            return false;
    }

    // Migration 2: user collections / shelves.
    if (version < 2) {
        const QStringList v2 = {
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS collections ("
                " id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL COLLATE NOCASE, created_at INTEGER NOT NULL)"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS book_collections ("
                " book_id INTEGER NOT NULL, collection_id INTEGER NOT NULL, added_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, collection_id),"
                " FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
                " FOREIGN KEY (collection_id) REFERENCES collections(id) ON DELETE CASCADE)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS book_collections_col ON book_collections (collection_id)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS book_collections_book ON book_collections (book_id)"),
        };
        if (!execList(v2) || !setVersion(2))
            return false;
    }

    // Migration 3: resilient W3C Web Annotation context columns + cross-book recency index.
    if (version < 3) {
        bool hasPrefix = false, hasSuffix = false;
        if (!hasColumn(db, QStringLiteral("annotations"), QStringLiteral("prefix"), &hasPrefix, errorOut) ||
            !hasColumn(db, QStringLiteral("annotations"), QStringLiteral("suffix"), &hasSuffix, errorOut))
            return false;
        if (!hasPrefix &&
            !execList({QStringLiteral("ALTER TABLE annotations ADD COLUMN prefix TEXT NOT NULL DEFAULT ''")}))
            return false;
        if (!hasSuffix &&
            !execList({QStringLiteral("ALTER TABLE annotations ADD COLUMN suffix TEXT NOT NULL DEFAULT ''")}))
            return false;
        if (!execList({QStringLiteral("CREATE INDEX IF NOT EXISTS annotations_updated ON annotations (updated_at DESC)")}))
            return false;
        if (!setVersion(3))
            return false;
    }

    // Migration 4: the Want to read shelf, and books that have been listened to.
    if (version < 4) {
        for (const char *column : {"want_at", "listened_at"}) {
            bool has = false;
            if (!hasColumn(db, QStringLiteral("books"), QString::fromLatin1(column), &has, errorOut))
                return false;
            if (!has && !execList({QStringLiteral("ALTER TABLE books ADD COLUMN %1 INTEGER NOT NULL DEFAULT 0")
                                       .arg(QString::fromLatin1(column))}))
                return false;
        }
        if (!setVersion(4))
            return false;
    }

    if (!sp.commit()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    Q_ASSERT(version == kLatestVersion);
    return true;
}

BookRecord readBookRecord(const QSqlQuery &query) {
    BookRecord r;
    int i = 0;
    r.id = query.value(i++).toInt();
    r.path = query.value(i++).toString();
    r.format = query.value(i++).toString();
    r.size = query.value(i++).toLongLong();
    r.mtime = query.value(i++).toLongLong();
    r.title = query.value(i++).toString();
    r.sortTitle = query.value(i++).toString();
    r.author = query.value(i++).toString();
    r.sortAuthor = query.value(i++).toString();
    r.editor = query.value(i++).toString();
    r.publisher = query.value(i++).toString();
    r.year = query.value(i++).toInt();
    r.language = query.value(i++).toString();
    r.description = query.value(i++).toString();
    r.subjects = query.value(i++).toString();
    r.isbn = query.value(i++).toString();
    r.series = query.value(i++).toString();
    r.seriesIndex = query.value(i++).toDouble();
    r.workKey = query.value(i++).toString();
    r.chapters = query.value(i++).toInt();
    r.pages = query.value(i++).toInt();
    r.length = query.value(i++).toLongLong();
    r.cover = query.value(i++).toString();
    r.problem = query.value(i++).toString();
    r.addedAt = query.value(i++).toLongLong();
    r.openedAt = query.value(i++).toLongLong();
    r.finishedAt = query.value(i++).toLongLong();
    r.progress = query.value(i++).toDouble();
    r.position = query.value(i++).toString();
    r.secondsRead = query.value(i++).toLongLong();
    r.missing = query.value(i++).toBool();
    r.hidden = query.value(i++).toBool();
    r.loose = query.value(i++).toBool();
    r.wantAt = query.value(i++).toLongLong();
    r.listenedAt = query.value(i++).toLongLong();
    return r;
}

void loadFolders(const QSqlDatabase &db, QStringList &folders) {
    folders.clear();
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("SELECT path FROM folders ORDER BY added_at"))) {
        while (query.next())
            folders.append(query.value(0).toString());
    }
}

void loadBooks(const QSqlDatabase &db, QHash<int, BookRecord> &books, QHash<QString, int> &pathToId,
               QHash<int, QString> &fingerprints, QMultiHash<QString, int> &fingerprintToIds) {
    books.clear();
    pathToId.clear();
    fingerprints.clear();
    fingerprintToIds.clear();
    if (!db.isOpen())
        return;

    bool hasWant = true, hasListened = true, hasFingerprint = true;
    hasColumn(db, QStringLiteral("books"), QStringLiteral("want_at"), &hasWant);
    hasColumn(db, QStringLiteral("books"), QStringLiteral("listened_at"), &hasListened);
    hasColumn(db, QStringLiteral("books"), QStringLiteral("fingerprint"), &hasFingerprint);

    QString sql;
    if (hasWant && hasListened && hasFingerprint) {
        sql = QStringLiteral("SELECT %1, fingerprint FROM books").arg(bookColumns());
    } else {
        sql = QStringLiteral(
                  "SELECT id, path, format, size, mtime, title, sort_title, author, sort_author, editor, publisher, "
                  "year, language, description, subjects, isbn, series, series_index, work_key, chapters, pages, "
                  "length, cover, problem, added_at, opened_at, finished_at, progress, position, seconds_read, "
                  "missing, hidden, loose, %1, %2, %3 FROM books")
                  .arg(hasWant ? QStringLiteral("want_at") : QStringLiteral("0"),
                       hasListened ? QStringLiteral("listened_at") : QStringLiteral("0"),
                       hasFingerprint ? QStringLiteral("fingerprint") : QStringLiteral("''"));
    }

    QSqlQuery query(db);
    if (!query.exec(sql))
        return;
    while (query.next()) {
        const BookRecord record = readBookRecord(query);
        books.insert(record.id, record);
        pathToId.insert(record.path, record.id);
        // The fingerprint comes after every column readBookRecord() reads.
        const QString fp = query.value(query.record().count() - 1).toString();
        if (!fp.isEmpty()) {
            fingerprints.insert(record.id, fp);
            fingerprintToIds.insert(fp, record.id);
        }
    }
}

void loadCollections(const QSqlDatabase &db, QHash<int, LibraryCollectionRow> &collections,
                     QHash<int, QSet<int>> &collectionBooks, QHash<int, QSet<int>> &bookCollections) {
    collections.clear();
    collectionBooks.clear();
    bookCollections.clear();
    if (!db.isOpen())
        return;
    QSqlQuery colQuery(db);
    if (colQuery.exec(QStringLiteral("SELECT id, name, created_at FROM collections ORDER BY name COLLATE NOCASE"))) {
        while (colQuery.next()) {
            LibraryCollectionRow c;
            c.id = colQuery.value(0).toInt();
            c.name = colQuery.value(1).toString();
            c.createdAt = colQuery.value(2).toLongLong();
            collections.insert(c.id, c);
        }
    }
    QSqlQuery linkQuery(db);
    if (linkQuery.exec(QStringLiteral("SELECT book_id, collection_id FROM book_collections"))) {
        while (linkQuery.next()) {
            const int bookId = linkQuery.value(0).toInt();
            const int colId = linkQuery.value(1).toInt();
            if (collections.contains(colId)) {
                collectionBooks[colId].insert(bookId);
                bookCollections[bookId].insert(colId);
            }
        }
    }
}

void loadAudioBadges(const QSqlDatabase &db, QSet<int> &downloadedBooks, QSet<int> &audiobookBooks) {
    downloadedBooks.clear();
    audiobookBooks.clear();
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT book_id, key, value FROM book_settings "
                               "WHERE key IN ('audio_downloaded', 'audiobook_saved')")))
        return;
    while (q.next()) {
        const int bookId = q.value(0).toInt();
        const QString key = q.value(1).toString();
        const QString val = q.value(2).toString().trimmed().toLower();
        const bool truthy = !val.isEmpty() && val != QLatin1String("0") && val != QLatin1String("false");
        if (!truthy)
            continue;
        if (key == QLatin1String("audio_downloaded"))
            downloadedBooks.insert(bookId);
        else if (key == QLatin1String("audiobook_saved"))
            audiobookBooks.insert(bookId);
    }
}

void loadOnlineMeta(const QSqlDatabase &db, QSet<int> &articleSettingIds, QHash<int, QString> &sourceUrls,
                    QHash<int, QString> &voiceNames) {
    articleSettingIds.clear();
    sourceUrls.clear();
    voiceNames.clear();
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT book_id, key, value FROM book_settings "
                               "WHERE key IN ('content_kind', 'source_url', 'voiceName')")))
        return;
    while (q.next()) {
        const int bookId = q.value(0).toInt();
        const QString key = q.value(1).toString();
        const QString val = q.value(2).toString();
        if (key == QLatin1String("content_kind")) {
            if (val.trimmed().compare(QLatin1String("article"), Qt::CaseInsensitive) == 0)
                articleSettingIds.insert(bookId);
        } else if (key == QLatin1String("source_url")) {
            if (!val.trimmed().isEmpty())
                sourceUrls.insert(bookId, val.trimmed());
        } else if (key == QLatin1String("voiceName")) {
            if (!val.isEmpty())
                voiceNames.insert(bookId, val);
        }
    }
}

void loadHighlightCounts(const QSqlDatabase &db, QHash<int, int> &highlightCounts) {
    highlightCounts.clear();
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT book_id, COUNT(*) FROM annotations WHERE kind = 'highlight' GROUP BY book_id")))
        return;
    while (q.next())
        highlightCounts.insert(q.value(0).toInt(), q.value(1).toInt());
}

void loadAll(const QSqlDatabase &db, LibrarySnapshot &out) {
    loadFolders(db, out.folders);
    loadBooks(db, out.books, out.pathToId, out.fingerprints, out.fingerprintToIds);
    loadCollections(db, out.collections, out.collectionBooks, out.bookCollections);
    loadAudioBadges(db, out.downloadedBooks, out.audiobookBooks);
    loadOnlineMeta(db, out.articleSettingIds, out.sourceUrls, out.voiceNames);
    loadHighlightCounts(db, out.highlightCounts);
}

QList<int> readChapterLengths(const QSqlDatabase &db, int bookId) {
    QList<int> lengths;
    if (!db.isOpen() || bookId <= 0)
        return lengths;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT length FROM chapter_lengths WHERE book_id = ? ORDER BY chapter"));
    query.addBindValue(bookId);
    if (query.exec()) {
        while (query.next())
            lengths.append(query.value(0).toInt());
    }
    return lengths;
}

bool writeChapterLengths(QSqlDatabase &db, int bookId, const QList<int> &lengths, qint64 *totalOut, QString *errorOut) {
    QSqlQuery remove(db);
    remove.prepare(QStringLiteral("DELETE FROM chapter_lengths WHERE book_id = ?"));
    remove.addBindValue(bookId);
    if (!execChecked(remove, errorOut))
        return false;
    QVariantList ids, chapters, values;
    qint64 total = 0;
    if (!lengths.isEmpty()) {
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral("INSERT INTO chapter_lengths (book_id, chapter, length) VALUES (?, ?, ?)"));
        ids.reserve(lengths.size());
        chapters.reserve(lengths.size());
        values.reserve(lengths.size());
        for (int i = 0; i < lengths.size(); ++i) {
            ids << bookId;
            chapters << i;
            values << lengths[i];
            total += lengths[i];
        }
        insert.addBindValue(ids);
        insert.addBindValue(chapters);
        insert.addBindValue(values);
        if (!insert.execBatch()) {
            const QString err = insert.lastError().text();
            setError(errorOut, err);
            qWarning("Library: %s", qPrintable(err));
            return false;
        }
    }
    if (total > 0) {
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE books SET length = ? WHERE id = ?"));
        update.addBindValue(total);
        update.addBindValue(bookId);
        if (!execChecked(update, errorOut))
            return false;
    }
    if (totalOut)
        *totalOut = total;
    return true;
}

bool upsertBook(QSqlDatabase &db, BookRecord &record, int existingId, bool relinked, const QString &fingerprint,
                QString *errorOut) {
    QSqlQuery query(db);
    const QString fields = QStringLiteral(
        "format = ?, size = ?, mtime = ?, title = ?, sort_title = ?, author = ?, sort_author = ?, editor = ?, "
        "publisher = ?, year = ?, language = ?, description = ?, subjects = ?, isbn = ?, series = ?, series_index = ?, "
        "work_key = ?, chapters = ?, pages = ?, length = ?, cover = ?, problem = ?, fingerprint = ?, missing = 0");
    if (relinked) {
        Q_ASSERT(existingId > 0);
        query.prepare(QStringLiteral("UPDATE books SET %1, path = ?, loose = ? WHERE id = ?").arg(fields));
    } else if (existingId > 0) {
        query.prepare(QStringLiteral("UPDATE books SET %1 WHERE id = ?").arg(fields));
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO books (format, size, mtime, title, sort_title, author, sort_author, "
            "editor, publisher, year, language, description, subjects, isbn, series, "
            "series_index, work_key, chapters, pages, length, cover, problem, fingerprint, missing, "
            "path, added_at, loose) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)"));
    }

    for (const QVariant &value :
         QVariantList{record.format, record.size, record.mtime, record.title, record.sortTitle, record.author,
                      record.sortAuthor, record.editor, record.publisher, record.year, record.language,
                      record.description, record.subjects, record.isbn, record.series, record.seriesIndex,
                      record.workKey, record.chapters, record.pages, record.length, record.cover, record.problem,
                      fingerprint}) {
        // A null QString binds as SQL NULL; the columns hold empty text instead.
        query.addBindValue(value.typeId() == QMetaType::QString && value.toString().isNull()
                               ? QVariant(QStringLiteral(""))
                               : value);
    }
    if (relinked) {
        query.addBindValue(record.path);
        query.addBindValue(record.loose ? 1 : 0);
        query.addBindValue(existingId);
    } else if (existingId > 0) {
        query.addBindValue(existingId);
    } else {
        query.addBindValue(record.path);
        query.addBindValue(record.addedAt);
        query.addBindValue(record.loose ? 1 : 0);
    }
    if (!execChecked(query, errorOut))
        return false;
    record.id = existingId > 0 ? existingId : query.lastInsertId().toInt();
    Q_ASSERT(record.id > 0);
    return true;
}

QString readBookSetting(const QSqlDatabase &db, int bookId, const QString &key) {
    if (!db.isOpen() || bookId <= 0 || key.isEmpty())
        return {};
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT value FROM book_settings WHERE book_id = ? AND key = ?"));
    query.addBindValue(bookId);
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool writeBookSetting(QSqlDatabase &db, int bookId, const QString &key, const QString &value, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO book_settings (book_id, key, value) VALUES (?, ?, ?)"));
    query.addBindValue(bookId);
    query.addBindValue(key);
    // An empty setting is "", never NULL (which the table refuses).
    query.addBindValue(value.isNull() ? QStringLiteral("") : value);
    return execChecked(query, errorOut);
}

bool insertFolder(QSqlDatabase &db, const QString &folder, qint64 addedAt, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO folders (path, added_at) VALUES (?, ?)"));
    query.addBindValue(folder);
    query.addBindValue(addedAt);
    return execChecked(query, errorOut);
}

bool removeFolderAndMarkMissing(QSqlDatabase &db, const QString &folder, const QList<int> &idsToMarkMissing,
                                QString *errorOut) {
    LibrarySavepoint sp(db, "remove_folder");
    if (!sp.isActive()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM folders WHERE path = ?"));
    del.addBindValue(folder);
    if (!execChecked(del, errorOut))
        return false;
    for (const int id : idsToMarkMissing) {
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE books SET missing = 1 WHERE id = ?"));
        update.addBindValue(id);
        if (!execChecked(update, errorOut))
            return false;
    }
    if (!sp.commit()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    return true;
}

bool updateMissingFlags(QSqlDatabase &db, const QList<QPair<int, bool>> &updates, QString *errorOut) {
    if (updates.isEmpty())
        return true;
    LibrarySavepoint sp(db, "mark_listed");
    if (!sp.isActive()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    for (const auto &[id, missing] : updates) {
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE books SET missing = ? WHERE id = ?"));
        update.addBindValue(missing ? 1 : 0);
        update.addBindValue(id);
        if (!execChecked(update, errorOut))
            return false;
    }
    if (!sp.commit()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    return true;
}

bool setBookIntColumn(QSqlDatabase &db, int bookId, const char *column, qint64 value, QString *errorOut) {
    const bool valid = std::strcmp(column, "want_at") == 0 || std::strcmp(column, "listened_at") == 0 ||
                       std::strcmp(column, "finished_at") == 0 || std::strcmp(column, "hidden") == 0 ||
                       std::strcmp(column, "opened_at") == 0;
    Q_ASSERT(valid);
    if (!valid)
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE books SET %1 = ? WHERE id = ?").arg(QLatin1String(column)));
    query.addBindValue(value);
    query.addBindValue(bookId);
    return execChecked(query, errorOut);
}

bool updateBookPosition(QSqlDatabase &db, int bookId, const QString &position, double progress, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE books SET position = ?, progress = ? WHERE id = ?"));
    query.addBindValue(position);
    query.addBindValue(progress);
    query.addBindValue(bookId);
    return execChecked(query, errorOut);
}

int insertCollection(QSqlDatabase &db, const QString &name, qint64 createdAt, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO collections (name, created_at) VALUES (?, ?)"));
    query.addBindValue(name);
    query.addBindValue(createdAt);
    if (!execChecked(query, errorOut))
        return 0;
    const int id = query.lastInsertId().toInt();
    Q_ASSERT(id > 0);
    return id;
}

bool renameCollection(QSqlDatabase &db, int id, const QString &name, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE collections SET name = ? WHERE id = ?"));
    query.addBindValue(name);
    query.addBindValue(id);
    return execChecked(query, errorOut);
}

bool deleteCollection(QSqlDatabase &db, int id, QString *errorOut) {
    LibrarySavepoint sp(db, "delete_collection");
    if (!sp.isActive()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    {
        QSqlQuery delLinks(db);
        delLinks.prepare(QStringLiteral("DELETE FROM book_collections WHERE collection_id = ?"));
        delLinks.addBindValue(id);
        if (!execChecked(delLinks, errorOut))
            return false;
    }
    {
        QSqlQuery delCol(db);
        delCol.prepare(QStringLiteral("DELETE FROM collections WHERE id = ?"));
        delCol.addBindValue(id);
        if (!execChecked(delCol, errorOut))
            return false;
    }
    if (!sp.commit()) {
        setError(errorOut, db.lastError().text());
        return false;
    }
    return true;
}

bool addBookToCollection(QSqlDatabase &db, int bookId, int collectionId, qint64 addedAt, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO book_collections (book_id, collection_id, added_at) VALUES (?, ?, ?)"));
    query.addBindValue(bookId);
    query.addBindValue(collectionId);
    query.addBindValue(addedAt);
    return execChecked(query, errorOut);
}

bool removeBookFromCollection(QSqlDatabase &db, int bookId, int collectionId, QString *errorOut) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM book_collections WHERE book_id = ? AND collection_id = ?"));
    query.addBindValue(bookId);
    query.addBindValue(collectionId);
    return execChecked(query, errorOut);
}

}  // namespace LibrarySchema
