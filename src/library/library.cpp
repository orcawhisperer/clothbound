#include "library/library.h"

#include "library/librarymodel.h"
#include "library/scanner.h"

#include <QCoreApplication>
#include <QDate>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace {
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }

// Nested-safe SQLite transaction guard using SAVEPOINT, rolling back
// automatically on early return or error.
struct Savepoint {
    QSqlDatabase db;
    QString name;
    bool active = false;

    Savepoint(QSqlDatabase database, const char *label) : db(std::move(database)), name(QString::fromLatin1(label)) {
        QSqlQuery q(db);
        active = q.exec(QStringLiteral("SAVEPOINT ") + name);
        if (!active)
            qWarning("Library: SAVEPOINT %s failed: %s", label, qPrintable(q.lastError().text()));
    }

    bool commit() {
        if (!active)
            return false;
        active = false;
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("RELEASE SAVEPOINT ") + name))
            return true;
        qWarning("Library: RELEASE SAVEPOINT %s failed: %s", qPrintable(name), qPrintable(q.lastError().text()));
        QSqlQuery rb(db);
        rb.exec(QStringLiteral("ROLLBACK TO SAVEPOINT ") + name);
        rb.exec(QStringLiteral("RELEASE SAVEPOINT ") + name);
        return false;
    }

    void rollback() {
        if (!active)
            return;
        active = false;
        QSqlQuery q(db);
        q.exec(QStringLiteral("ROLLBACK TO SAVEPOINT ") + name);
        q.exec(QStringLiteral("RELEASE SAVEPOINT ") + name);
    }

    ~Savepoint() { rollback(); }
};

const QString bookColumns = QStringLiteral(
    "id, path, format, size, mtime, title, sort_title, author, sort_author, editor, publisher, year, language, "
    "description, subjects, isbn, series, series_index, work_key, chapters, pages, length, cover, problem, "
    "added_at, opened_at, finished_at, progress, position, seconds_read, missing, hidden, loose, want_at, listened_at");

bool isUnder(const QString &path, const QString &folder) {
    return path.startsWith(folder.endsWith('/') ? folder : folder + '/');
}

QString canonical(const QString &path) {
    const QFileInfo info(path);
    const QString resolved = info.canonicalFilePath();
    return resolved.isEmpty() ? info.absoluteFilePath() : resolved;
}
}

QString BookRecord::editionLine() const {
    QStringList parts;
    if (!editor.isEmpty())
        parts << QCoreApplication::translate("Library", "ed. %1").arg(editor);
    if (!publisher.isEmpty())
        parts << publisher;
    if (year > 0)
        parts << QString::number(year);
    return parts.join(QStringLiteral(" · "));
}

Library::Library(const QString &dataDirectory, const QString &cacheDirectory, QObject *parent)
    : QObject(parent),
      m_dataDirectory(dataDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                              : dataDirectory),
      m_coverDirectory((cacheDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                                 : cacheDirectory) +
                       QStringLiteral("/covers")),
      m_connection(QStringLiteral("library-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    m_works = new WorksModel(this);
    m_scanner = new Scanner(m_coverDirectory, this);

    m_flush.setSingleShot(true);
    m_flush.setInterval(200);
    connect(&m_flush, &QTimer::timeout, this, &Library::flushPending);
    // Many changes in a burst (a scan, a shelf toggle) become one refresh.
    m_notify.setSingleShot(true);
    m_notify.setInterval(60);
    connect(&m_notify, &QTimer::timeout, this, [this] {
        if (m_worksDirty) {
            m_worksDirty = false;
            m_works->setBooks(m_books);
        }
        const bool emitBooks = m_booksNotifyDirty;
        const bool emitContinue = m_continueNotifyDirty || emitBooks;
        const bool emitCollections = m_collectionsNotifyDirty;
        const bool emitStats = m_statsNotifyDirty;
        m_booksNotifyDirty = false;
        m_continueNotifyDirty = false;
        m_collectionsNotifyDirty = false;
        m_statsNotifyDirty = false;
        if (emitBooks)
            emit booksChanged();
        if (emitContinue)
            emit continueReadingChanged();
        if (emitCollections)
            emit collectionsChanged();
        if (emitStats)
            emit readingStatsChanged();
    });

    connect(m_scanner, &Scanner::listed, this, &Library::markListed);
    connect(m_scanner, &Scanner::bookRead, this, [this](const ScannedBook &book) {
        m_pending.append(book);
        if (!m_flush.isActive())
            m_flush.start();
    });
    connect(m_scanner, &Scanner::progressChanged, this, [this](int done, int total) {
        m_scanDone = done;
        m_scanTotal = total;
        emit scanProgressChanged();
    });
    connect(m_scanner, &Scanner::finished, this, [this] {
        flushPending();
        m_queuedFiles.clear();
        m_pendingFileCollections.clear();
        m_pendingFolderCollections.clear();
        m_scanning = false;
        emit scanningChanged();
        const ImportBatch batch = m_importBatch;
        m_importBatch.clear();
        if (batch.count > 0)
            flushNotificationsNow();
        emit scanFinished(m_added, m_updated);
        if (batch.count > 0)
            emit booksAdded(batch.firstBookId, batch.count, batch.firstTitle, batch.targetCollectionId);
    });
}

Library::~Library() {
    m_scanner->cancel();
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
}

bool Library::exec(QSqlQuery &query) {
    if (query.exec())
        return true;
    m_error = query.lastError().text();
    qWarning("Library: %s", qPrintable(m_error));
    return false;
}

bool Library::open() {
    if (!QDir().mkpath(m_dataDirectory) || !QDir().mkpath(m_coverDirectory)) {
        m_error = tr("Can't create %1.").arg(m_dataDirectory);
        return false;
    }
    m_canonicalDataDirectory = canonical(m_dataDirectory);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_dataDirectory + QStringLiteral("/library.sqlite"));
    if (!db.open()) {
        m_error = db.lastError().text();
        return false;
    }
    QSqlQuery pragma(db);
    // Set busy_timeout first so journal_mode=WAL waits rather than failing with
    // SQLITE_BUSY when another Clothbound instance or the CLI holds the database.
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA cache_size=-16000"));
    pragma.exec(QStringLiteral("PRAGMA mmap_size=67108864"));
    if (!migrate())
        return false;
    loadFolders();
    loadBooks();
    loadCollections();
    loadAudioBadges();
    loadOnlineMeta();
    loadHighlightCounts();
    refreshArticles();
    QSqlQuery speed(db);
    speed.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'chars_per_minute'"));
    if (speed.next() && speed.value(0).toDouble() > 0)
        m_charsPerMinute = speed.value(0).toDouble();
    m_works->setCollections(m_collectionBooks);
    m_works->setAudioSets(m_downloadedBooks, m_audiobookBooks);
    m_works->setArticleSet(m_articles);
    m_works->setBooks(m_books);
    return true;
}

int Library::schemaVersion() const {
    QSqlQuery q(database());
    if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
        return q.value(0).toInt();
    return 0;
}

bool Library::migrate() {
    QSqlDatabase db = database();
    // Raise with every new migration below, or existing libraries skip it.
    constexpr int kLatestVersion = 4;

    int version = 0;
    {
        QSqlQuery vq(db);
        if (!vq.exec(QStringLiteral("PRAGMA user_version")) || !vq.next()) {
            m_error = vq.lastError().text();
            return false;
        }
        version = vq.value(0).toInt();
    }
    if (version > kLatestVersion) {
        m_error = tr("This library was created by a newer version of Clothbound (schema %1; supported up to %2).")
                      .arg(version)
                      .arg(kLatestVersion);
        return false;
    }
    if (version == kLatestVersion)
        return true;

    auto execList = [&](const QStringList &statements) -> bool {
        for (const QString &statement : statements) {
            QSqlQuery query(db);
            if (!query.exec(statement)) {
                m_error = query.lastError().text();
                return false;
            }
        }
        return true;
    };
    auto hasColumn = [&](const QString &table, const QString &column, bool *out) -> bool {
        QSqlQuery cols(db);
        if (!cols.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            m_error = cols.lastError().text();
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
    };
    auto setVersion = [&](int nextVersion) -> bool {
        QSqlQuery vq(db);
        if (!vq.exec(QStringLiteral("PRAGMA user_version = %1").arg(nextVersion))) {
            m_error = vq.lastError().text();
            return false;
        }
        QSqlQuery mq(db);
        mq.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES ('schema', ?)"));
        mq.addBindValue(QString::number(nextVersion));
        if (!mq.exec()) {
            m_error = mq.lastError().text();
            return false;
        }
        version = nextVersion;
        return true;
    };

    Savepoint sp(db, "migrate");
    if (!sp.active) {
        m_error = db.lastError().text();
        return false;
    }

    // Migration 1: base schema + content fingerprint column/index.
    if (version < 1) {
        const QStringList v1 = {
            "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)",
            "CREATE TABLE IF NOT EXISTS folders (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, added_at INTEGER NOT NULL)",
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
            " fingerprint TEXT NOT NULL DEFAULT '')",
            "CREATE INDEX IF NOT EXISTS books_work ON books (work_key)",
            "CREATE TABLE IF NOT EXISTS chapter_lengths (book_id INTEGER NOT NULL, chapter INTEGER NOT NULL,"
            " length INTEGER NOT NULL, PRIMARY KEY (book_id, chapter))",
            "CREATE TABLE IF NOT EXISTS annotations ("
            " id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, kind TEXT NOT NULL,"
            " chapter INTEGER NOT NULL DEFAULT 0, start INTEGER NOT NULL DEFAULT 0, finish INTEGER NOT NULL DEFAULT 0,"
            " text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '',"
            " label TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)",
            "CREATE INDEX IF NOT EXISTS annotations_book ON annotations (book_id)",
            "CREATE TABLE IF NOT EXISTS reading_days (book_id INTEGER NOT NULL, day TEXT NOT NULL,"
            " seconds INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (book_id, day))",
            "CREATE TABLE IF NOT EXISTS book_settings (book_id INTEGER NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,"
            " PRIMARY KEY (book_id, key))",
        };
        if (!execList(v1))
            return false;
        bool hasFingerprint = false;
        if (!hasColumn(QStringLiteral("books"), QStringLiteral("fingerprint"), &hasFingerprint))
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
            "CREATE TABLE IF NOT EXISTS collections ("
            " id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL COLLATE NOCASE, created_at INTEGER NOT NULL)",
            "CREATE TABLE IF NOT EXISTS book_collections ("
            " book_id INTEGER NOT NULL, collection_id INTEGER NOT NULL, added_at INTEGER NOT NULL,"
            " PRIMARY KEY (book_id, collection_id),"
            " FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,"
            " FOREIGN KEY (collection_id) REFERENCES collections(id) ON DELETE CASCADE)",
            "CREATE INDEX IF NOT EXISTS book_collections_col ON book_collections (collection_id)",
            "CREATE INDEX IF NOT EXISTS book_collections_book ON book_collections (book_id)",
        };
        if (!execList(v2) || !setVersion(2))
            return false;
    }

    // Migration 3: resilient W3C Web Annotation context columns + cross-book recency index.
    if (version < 3) {
        bool hasPrefix = false, hasSuffix = false;
        if (!hasColumn(QStringLiteral("annotations"), QStringLiteral("prefix"), &hasPrefix) ||
            !hasColumn(QStringLiteral("annotations"), QStringLiteral("suffix"), &hasSuffix))
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
            if (!hasColumn(QStringLiteral("books"), QString::fromLatin1(column), &has))
                return false;
            if (!has && !execList({QStringLiteral("ALTER TABLE books ADD COLUMN %1 INTEGER NOT NULL DEFAULT 0")
                                       .arg(QString::fromLatin1(column))}))
                return false;
        }
        if (!setVersion(4))
            return false;
    }

    if (!sp.commit()) {
        m_error = db.lastError().text();
        return false;
    }
    return true;
}

void Library::loadCollections() {
    m_collections.clear();
    m_collectionBooks.clear();
    m_bookCollections.clear();
    QSqlQuery colQuery(database());
    colQuery.exec(QStringLiteral("SELECT id, name, created_at FROM collections ORDER BY name COLLATE NOCASE"));
    while (colQuery.next()) {
        CollectionRecord c;
        c.id = colQuery.value(0).toInt();
        c.name = colQuery.value(1).toString();
        c.createdAt = colQuery.value(2).toLongLong();
        m_collections.insert(c.id, c);
    }
    QSqlQuery linkQuery(database());
    linkQuery.exec(QStringLiteral("SELECT book_id, collection_id FROM book_collections"));
    while (linkQuery.next()) {
        const int bookId = linkQuery.value(0).toInt();
        const int colId = linkQuery.value(1).toInt();
        if (m_collections.contains(colId)) {
            m_collectionBooks[colId].insert(bookId);
            m_bookCollections[bookId].insert(colId);
        }
    }
}

void Library::syncWorksCollections() {
    if (m_worksDirty) {
        m_worksDirty = false;
        m_works->setCollections(m_collectionBooks, false);
        m_works->setBooks(m_books);
    } else {
        m_works->setCollections(m_collectionBooks);
    }
}

void Library::flushNotificationsNow() {
    if (m_notify.isActive())
        m_notify.stop();
    if (m_worksDirty) {
        m_worksDirty = false;
        m_works->setCollections(m_collectionBooks, false);
        m_works->setBooks(m_books);
    }
    const bool emitBooks = m_booksNotifyDirty;
    const bool emitContinue = m_continueNotifyDirty || emitBooks;
    const bool emitCollections = m_collectionsNotifyDirty;
    const bool emitStats = m_statsNotifyDirty;
    m_booksNotifyDirty = false;
    m_continueNotifyDirty = false;
    m_collectionsNotifyDirty = false;
    m_statsNotifyDirty = false;
    if (emitBooks)
        emit booksChanged();
    if (emitContinue)
        emit continueReadingChanged();
    if (emitCollections)
        emit collectionsChanged();
    if (emitStats)
        emit readingStatsChanged();
}

void Library::loadAudioBadges() {
    m_downloadedBooks.clear();
    m_audiobookBooks.clear();
    if (!database().isOpen())
        return;
    QSqlQuery q(database());
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
            m_downloadedBooks.insert(bookId);
        else if (key == QLatin1String("audiobook_saved"))
            m_audiobookBooks.insert(bookId);
    }
}

void Library::loadOnlineMeta() {
    m_articleSettingIds.clear();
    m_sourceUrls.clear();
    m_voiceNames.clear();
    if (!database().isOpen())
        return;
    QSqlQuery q(database());
    if (!q.exec(QStringLiteral("SELECT book_id, key, value FROM book_settings "
                               "WHERE key IN ('content_kind', 'source_url', 'voiceName')")))
        return;
    while (q.next()) {
        const int bookId = q.value(0).toInt();
        const QString key = q.value(1).toString();
        const QString val = q.value(2).toString();
        if (key == QLatin1String("content_kind")) {
            if (val.trimmed().compare(QLatin1String("article"), Qt::CaseInsensitive) == 0)
                m_articleSettingIds.insert(bookId);
        } else if (key == QLatin1String("source_url")) {
            if (!val.trimmed().isEmpty())
                m_sourceUrls.insert(bookId, val.trimmed());
        } else if (key == QLatin1String("voiceName")) {
            if (!val.isEmpty())
                m_voiceNames.insert(bookId, val);
        }
    }
}

void Library::loadHighlightCounts() {
    m_highlightCounts.clear();
    if (!database().isOpen())
        return;
    QSqlQuery q(database());
    if (!q.exec(QStringLiteral("SELECT book_id, COUNT(*) FROM annotations WHERE kind = 'highlight' GROUP BY book_id")))
        return;
    while (q.next())
        m_highlightCounts.insert(q.value(0).toInt(), q.value(1).toInt());
}

void Library::adjustHighlightCount(int bookId, int delta) {
    if (bookId <= 0 || delta == 0)
        return;
    const int next = qMax(0, m_highlightCounts.value(bookId, 0) + delta);
    if (next == 0)
        m_highlightCounts.remove(bookId);
    else
        m_highlightCounts.insert(bookId, next);
}

bool Library::isArticleRecord(const BookRecord &record) const {
    if (m_articleSettingIds.contains(record.id))
        return true;
    if (record.subjects.contains(QStringLiteral("Clothbound Article"), Qt::CaseInsensitive))
        return true;
    const QString articlesDir = m_dataDirectory + QStringLiteral("/articles");
    if (isUnder(record.path, articlesDir))
        return true;
    const QString &canonicalData = m_canonicalDataDirectory;
    if (!canonicalData.isEmpty() && canonicalData != m_dataDirectory &&
        isUnder(record.path, canonicalData + QStringLiteral("/articles")))
        return true;
    return false;
}

void Library::refreshArticles() {
    m_articles.clear();
    for (const auto &record : std::as_const(m_books)) {
        if (isArticleRecord(record))
            m_articles.insert(record.id);
    }
    if (m_works)
        m_works->setArticleSet(m_articles);
}

void Library::loadFolders() {
    m_folders.clear();
    QSqlQuery query(database());
    query.exec(QStringLiteral("SELECT path FROM folders ORDER BY added_at"));
    while (query.next())
        m_folders.append(query.value(0).toString());
}

BookRecord Library::readRecord(const QSqlQuery &query) const {
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

void Library::loadBooks() {
    m_books.clear();
    m_pathToId.clear();
    m_fingerprints.clear();
    m_fingerprintToIds.clear();
    QSqlQuery query(database());
    query.exec(QStringLiteral("SELECT %1, fingerprint FROM books").arg(bookColumns));
    while (query.next()) {
        const BookRecord record = readRecord(query);
        m_books.insert(record.id, record);
        m_pathToId.insert(record.path, record.id);
        // The fingerprint comes after every column readRecord() reads.
        const QString fp = query.value(query.record().count() - 1).toString();
        if (!fp.isEmpty()) {
            m_fingerprints.insert(record.id, fp);
            m_fingerprintToIds.insert(fp, record.id);
        }
    }
}

void Library::changed(bool statsDirty, bool collectionsDirty) {
    m_worksDirty = true;
    m_booksNotifyDirty = true;
    if (statsDirty)
        m_statsNotifyDirty = true;
    if (collectionsDirty)
        m_collectionsNotifyDirty = true;
    if (!m_notify.isActive())
        m_notify.start();
}

int Library::bookCount() const {
    int count = 0;
    for (const auto &record : m_books)
        count += record.visible() ? 1 : 0;
    return count;
}

int Library::idForPath(const QString &path) const {
    if (const int id = m_pathToId.value(path, 0))
        return id;
    const QString resolved = canonical(path);
    return resolved == path ? 0 : m_pathToId.value(resolved, 0);
}

void Library::addFolder(const QString &path) {
    if (path.trimmed().isEmpty())
        return;
    const QString folder = canonical(path);
    if (!QFileInfo(folder).isDir()) {
        emit problem(tr("%1 isn't a folder.").arg(path));
        return;
    }
    if (!m_folders.contains(folder)) {
        QSqlQuery query(database());
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO folders (path, added_at) VALUES (?, ?)"));
        query.addBindValue(folder);
        query.addBindValue(now());
        if (!exec(query))
            return;
        m_folders.append(folder);
        emit foldersChanged();
    }
    rescan();
}

void Library::addUrls(const QList<QUrl> &urls) {
    addUrlsImpl(urls, -1);
}

void Library::addUrlsToCollection(const QList<QUrl> &urls, int collectionId) {
    addUrlsImpl(urls, qMax(0, collectionId));
}

void Library::addFilesToCollection(const QStringList &paths, int collectionId) {
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &path : paths)
        if (!path.trimmed().isEmpty())
            urls.append(QUrl::fromLocalFile(path));
    addUrlsToCollection(urls, collectionId);
}

void Library::addUrlsImpl(const QList<QUrl> &urls, int explicitCollectionId) {
    const int fileTargetCol = (explicitCollectionId >= 0) ? explicitCollectionId : (m_works ? m_works->collectionId() : 0);
    const int validFileCol = (fileTargetCol > 0 && m_collections.contains(fileTargetCol)) ? fileTargetCol : 0;
    const int validFolderCol = (explicitCollectionId > 0 && m_collections.contains(explicitCollectionId)) ? explicitCollectionId : 0;

    QStringList folders, files;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = canonical(url.toLocalFile());
        const QFileInfo info(path);
        if (info.isDir()) {
            folders.append(path);
        } else if (BookFiles::isBook(path)) {
            files.append(path);
            if (const int id = idForPath(path); id && m_books[id].hidden)
                setHidden(id, false);
        }
    }
    if (folders.isEmpty() && files.isEmpty()) {
        emit problem(tr("Clothbound reads EPUB and PDF books."));
        return;
    }

    QSet<int> immediateIds;
    ImportBatch immediateBatch;
    bool needScan = false;
    for (const QString &file : std::as_const(files)) {
        const QFileInfo info(file);
        if (const int id = idForPath(file)) {
            if (validFileCol > 0)
                addToCollection(id, validFileCol);
            const BookRecord &record = m_books[id];
            if (record.size == info.size() && record.mtime == info.lastModified().toSecsSinceEpoch() && !record.missing) {
                if (!immediateIds.contains(id)) {
                    immediateIds.insert(id);
                    immediateBatch.record(id, record.title, validFileCol);
                }
                continue;
            }
        }
        if (validFileCol > 0)
            m_pendingFileCollections.insert(file, validFileCol);
        if (!m_queuedFiles.contains(file))
            m_queuedFiles.append(file);
        needScan = true;
    }

    bool foldersAdded = false;
    for (const QString &folder : std::as_const(folders)) {
        if (validFolderCol > 0) {
            m_pendingFolderCollections.insert(folder, validFolderCol);
            for (const BookRecord &record : std::as_const(m_books)) {
                if (record.visible() && isUnder(record.path, folder)) {
                    addToCollection(record.id, validFolderCol);
                    if (!immediateIds.contains(record.id)) {
                        immediateIds.insert(record.id);
                        immediateBatch.record(record.id, record.title, validFolderCol);
                    }
                }
            }
        }
        if (m_folders.contains(folder))
            continue;
        QSqlQuery query(database());
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO folders (path, added_at) VALUES (?, ?)"));
        query.addBindValue(folder);
        query.addBindValue(now());
        if (exec(query)) {
            m_folders.append(folder);
            foldersAdded = true;
        }
    }
    if (foldersAdded)
        emit foldersChanged();
    if (!folders.isEmpty())
        needScan = true;

    if (needScan) {
        if (immediateBatch.count > 0) {
            flushNotificationsNow();
            m_importBatch.merge(immediateBatch);
        }
        rescan();
    } else if (immediateBatch.count > 0) {
        flushNotificationsNow();
        emit booksAdded(immediateBatch.firstBookId, immediateBatch.count, immediateBatch.firstTitle,
                        immediateBatch.targetCollectionId);
    }
}

void Library::removeFolder(const QString &path) {
    if (path.trimmed().isEmpty())
        return;
    const QString folder = canonical(path);
    const qsizetype folderIndex = m_folders.indexOf(folder);
    if (folderIndex < 0)
        return;
    QStringList remaining = m_folders;
    remaining.removeAt(folderIndex);
    QSqlDatabase db = database();
    Savepoint sp(db, "remove_folder");
    if (!sp.active)
        return;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM folders WHERE path = ?"));
    query.addBindValue(folder);
    if (!exec(query))
        return;
    // Books leave with their folder but keep their reading state, in case
    // the folder comes back (an unplugged drive).
    QList<int> markedMissing;
    for (const auto &record : std::as_const(m_books)) {
        if (record.loose || !isUnder(record.path, folder))
            continue;
        bool elsewhere = false;
        for (const QString &other : std::as_const(remaining))
            elsewhere = elsewhere || isUnder(record.path, other);
        if (elsewhere)
            continue;
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE books SET missing = 1 WHERE id = ?"));
        update.addBindValue(record.id);
        if (!exec(update))
            return;
        markedMissing.append(record.id);
    }
    if (!sp.commit())
        return;
    m_folders = remaining;
    for (const int id : markedMissing)
        m_books[id].missing = true;
    emit foldersChanged();
    changed();
    if (m_scanning) {
        cancelScan();
        if (!m_folders.isEmpty())
            rescan();
    }
}

QVariantList Library::folderStats() const {
    QVariantList result;
    result.reserve(m_folders.size());
    for (const QString &folder : std::as_const(m_folders)) {
        int books = 0;
        for (const auto &record : std::as_const(m_books))
            if (record.visible() && isUnder(record.path, folder))
                ++books;
        const QFileInfo info(folder);
        const QString name = info.fileName().isEmpty() ? folder : info.fileName();
        result.append(QVariantMap{{QStringLiteral("path"), folder},
                                  {QStringLiteral("name"), name},
                                  {QStringLiteral("exists"), info.isDir()},
                                  {QStringLiteral("books"), books}});
    }
    return result;
}

void Library::openFolder(const QString &path) const {
    if (path.trimmed().isEmpty())
        return;
    const QFileInfo info(path);
    if (info.isDir())
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}

void Library::rescan() {
    QDir().mkpath(m_coverDirectory);
    QStringList loose = m_queuedFiles;
    QSet<QString> looseSet(loose.cbegin(), loose.cend());
    QHash<QString, QPair<qint64, qint64>> known;
    known.reserve(m_books.size());
    for (const auto &record : std::as_const(m_books)) {
        const bool coverMissing =
            !record.cover.isEmpty() && !QFileInfo::exists(m_coverDirectory + QLatin1Char('/') + record.cover);
        if (!coverMissing && (!m_fingerprints.value(record.id).isEmpty() || record.missing || !record.problem.isEmpty()))
            known.insert(record.path, {record.size, record.mtime});
        if (record.loose && !looseSet.contains(record.path)) {
            looseSet.insert(record.path);
            loose.append(record.path);
        }
    }
    m_added = m_updated = 0;
    m_scanDone = m_scanTotal = 0;
    m_scanning = true;
    emit scanningChanged();
    emit scanProgressChanged();
    m_scanner->scan(m_folders, loose, known);
}

void Library::cancelScan() {
    if (!m_scanning)
        return;
    m_scanner->cancel();
    m_flush.stop();
    flushPending();
    m_queuedFiles.clear();
    m_pendingFileCollections.clear();
    m_pendingFolderCollections.clear();
    m_importBatch.clear();
    m_scanning = false;
    emit scanningChanged();
    emit scanProgressChanged();
}

void Library::markListed(const QStringList &paths, const QStringList &folders) {
    const QSet<QString> present(paths.cbegin(), paths.cend());
    QSqlDatabase db = database();
    Savepoint sp(db, "mark_listed");
    if (!sp.active)
        return;
    QList<QPair<int, bool>> updates;
    for (const auto &record : std::as_const(m_books)) {
        bool inScope = record.loose;
        for (const QString &folder : folders)
            inScope = inScope || isUnder(record.path, folder);
        if (!inScope)
            continue;
        const bool missing = !present.contains(record.path);
        if (missing == record.missing)
            continue;
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE books SET missing = ? WHERE id = ?"));
        update.addBindValue(missing ? 1 : 0);
        update.addBindValue(record.id);
        if (!exec(update))
            return;
        updates.append({record.id, missing});
    }
    if (!sp.commit())
        return;
    for (const auto &[id, missing] : updates)
        m_books[id].missing = missing;
    if (!updates.isEmpty())
        changed();
}

bool Library::storeInDb(const ScannedBook &book, QList<StagedBook> &staged) {
    const BookInfo &info = book.info;
    const BookIdentity &id = info.identity;
    QSqlDatabase db = database();

    int existing = idForPath(info.path);
    for (const StagedBook &entry : std::as_const(staged))
        if (entry.record.path == info.path)
            existing = entry.record.id;

    // If the file was moved or renamed, relink it to the missing/moved record
    // with the same content fingerprint so annotations, progress, reading_days,
    // book_settings and passage_vectors stay attached.
    bool relinked = false;
    QString oldPath;
    if (!existing && !book.fingerprint.isEmpty()) {
        for (auto it = m_fingerprintToIds.constFind(book.fingerprint);
             it != m_fingerprintToIds.cend() && it.key() == book.fingerprint; ++it) {
            const BookRecord candidate = m_books.value(it.value());
            if (candidate.id > 0 && (candidate.missing || !QFileInfo::exists(candidate.path))) {
                bool claimed = false;
                for (const StagedBook &entry : std::as_const(staged))
                    if (entry.record.id == candidate.id)
                        claimed = true;
                if (!claimed) {
                    existing = candidate.id;
                    oldPath = candidate.path;
                    relinked = true;
                    break;
                }
            }
        }
    }

    QSqlQuery query(db);
    const QString fields = QStringLiteral(
        "format = ?, size = ?, mtime = ?, title = ?, sort_title = ?, author = ?, sort_author = ?, editor = ?, "
        "publisher = ?, year = ?, language = ?, description = ?, subjects = ?, isbn = ?, series = ?, series_index = ?, "
        "work_key = ?, chapters = ?, pages = ?, length = ?, cover = ?, problem = ?, fingerprint = ?, missing = 0");
    if (relinked)
        query.prepare(QStringLiteral("UPDATE books SET %1, path = ?, loose = ? WHERE id = ?").arg(fields));
    else if (existing)
        query.prepare(QStringLiteral("UPDATE books SET %1 WHERE id = ?").arg(fields));
    else
        query.prepare(QStringLiteral("INSERT INTO books (format, size, mtime, title, sort_title, author, sort_author, "
                                     "editor, publisher, year, language, description, subjects, isbn, series, "
                                     "series_index, work_key, chapters, pages, length, cover, problem, fingerprint, missing, "
                                     "path, added_at, loose) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)"));
    BookRecord record = existing ? m_books.value(existing) : BookRecord();
    for (const StagedBook &entry : std::as_const(staged))
        if (existing && entry.record.id == existing)
            record = entry.record;
    record.path = info.path;
    record.format = info.format;
    record.size = info.size;
    record.mtime = info.modified.toSecsSinceEpoch();
    record.title = id.title;
    record.sortTitle = id.sortTitle;
    record.author = id.author;
    record.sortAuthor = id.sortAuthor;
    record.editor = id.editor;
    record.publisher = id.publisher;
    record.year = id.year;
    record.language = info.language;
    record.description = info.description;
    record.subjects = info.subjects.join(QStringLiteral(", "));
    record.isbn = info.isbn;
    record.series = info.series;
    record.seriesIndex = info.seriesIndex;
    record.workKey = id.workKey;
    record.chapters = info.chapters;
    record.pages = info.pages;
    record.length = book.length;
    record.cover = book.coverFile;
    record.problem = info.problem;
    record.missing = false;
    if (!existing || relinked) {
        if (!existing)
            record.addedAt = now();
        bool underFolder = false;
        for (const QString &folder : std::as_const(m_folders))
            underFolder = underFolder || isUnder(record.path, folder);
        record.loose = !underFolder;
    }
    for (const QVariant &value :
         QVariantList{record.format, record.size, record.mtime, record.title, record.sortTitle, record.author,
                      record.sortAuthor, record.editor, record.publisher, record.year, record.language,
                      record.description, record.subjects, record.isbn, record.series, record.seriesIndex,
                      record.workKey, record.chapters, record.pages, record.length, record.cover, record.problem,
                      book.fingerprint})
        // A null QString binds as SQL NULL; the columns hold empty text instead.
        query.addBindValue(value.typeId() == QMetaType::QString && value.toString().isNull()
                               ? QVariant(QStringLiteral("")) : value);
    if (relinked) {
        query.addBindValue(record.path);
        query.addBindValue(record.loose ? 1 : 0);
        query.addBindValue(existing);
    } else if (existing) {
        query.addBindValue(existing);
    } else {
        query.addBindValue(record.path);
        query.addBindValue(record.addedAt);
        query.addBindValue(record.loose ? 1 : 0);
    }
    if (!exec(query))
        return false;
    record.id = existing ? existing : query.lastInsertId().toInt();
    if (!book.chapterLengths.isEmpty()) {
        qint64 total = 0;
        if (!writeChapterLengths(db, record.id, book.chapterLengths, &total))
            return false;
        if (total > 0)
            record.length = total;
    }
    staged.append({record, oldPath, book.fingerprint, existing != 0});
    return true;
}

void Library::applyStaged(const QList<StagedBook> &staged) {
    bool articlesChanged = false;
    for (const StagedBook &entry : staged) {
        if (!entry.oldPath.isEmpty() && entry.oldPath != entry.record.path)
            m_pathToId.remove(entry.oldPath);
        m_books.insert(entry.record.id, entry.record);
        m_pathToId.insert(entry.record.path, entry.record.id);
        if (!entry.fingerprint.isEmpty()) {
            const QString prevFp = m_fingerprints.value(entry.record.id);
            if (!prevFp.isEmpty() && prevFp != entry.fingerprint)
                m_fingerprintToIds.remove(prevFp, entry.record.id);
            m_fingerprints.insert(entry.record.id, entry.fingerprint);
            if (!m_fingerprintToIds.contains(entry.fingerprint, entry.record.id))
                m_fingerprintToIds.insert(entry.fingerprint, entry.record.id);
        }
        const bool isArt = isArticleRecord(entry.record);
        if (isArt && !m_articles.contains(entry.record.id)) {
            m_articles.insert(entry.record.id);
            articlesChanged = true;
        } else if (!isArt && m_articles.contains(entry.record.id)) {
            m_articles.remove(entry.record.id);
            articlesChanged = true;
        }
        entry.updated ? ++m_updated : ++m_added;
    }
    if (articlesChanged && m_works)
        m_works->setArticleSet(m_articles);
}

void Library::flushPending() {
    if (m_pending.isEmpty())
        return;
    QSqlDatabase db = database();
    Savepoint sp(db, "flush_pending");
    if (!sp.active)
        return;
    QList<StagedBook> staged;
    staged.reserve(m_pending.size());
    int failedCount = 0;
    // Isolate each book in a nested savepoint so a single failing insert/update
    // cannot roll back or silently discard the rest of the batch.
    for (const ScannedBook &book : std::as_const(m_pending)) {
        Savepoint bookSp(db, "flush_book");
        const qsizetype before = staged.size();
        if (bookSp.active && storeInDb(book, staged) && bookSp.commit())
            continue;
        staged.resize(before);
        bookSp.rollback();
        ++failedCount;
    }
    if (!sp.commit()) {
        // Outer transaction failed (e.g., transient lock/I/O): preserve m_pending
        // rather than silently losing the batch.
        return;
    }
    if (failedCount > 0)
        qWarning("Library: %d book(s) failed to store during batch flush", failedCount);
    m_pending.clear();
    if (staged.isEmpty())
        return;
    applyStaged(staged);
    changed();
    for (const StagedBook &entry : std::as_const(staged)) {
        int colId = m_pendingFileCollections.take(entry.record.path);
        if (colId <= 0 && !m_pendingFolderCollections.isEmpty()) {
            for (auto it = m_pendingFolderCollections.cbegin(); it != m_pendingFolderCollections.cend(); ++it) {
                if (isUnder(entry.record.path, it.key())) {
                    colId = it.value();
                    break;
                }
            }
        }
        if (colId > 0 && m_collections.contains(colId))
            addToCollection(entry.record.id, colId);
        if (!entry.updated || colId > 0)
            m_importBatch.record(entry.record.id, entry.record.title, colId);
    }
}

int Library::addBookFile(const QString &path) {
    const QString file = canonical(path);
    const QFileInfo info(file);
    if (!info.exists()) {
        m_error = tr("%1 doesn't exist.").arg(path);
        return 0;
    }
    if (!BookFiles::isBook(file)) {
        m_error = tr("Clothbound reads EPUB and PDF books.");
        return 0;
    }
    bool wasHidden = false;
    if (const int id = idForPath(file)) {
        if (m_books[id].hidden) {
            setHidden(id, false);
            wasHidden = true;
        }
        const BookRecord &record = m_books[id];
        if (record.size == info.size() && record.mtime == info.lastModified().toSecsSinceEpoch() && !record.missing) {
            if (wasHidden) {
                flushNotificationsNow();
                emit booksAdded(id, 1, record.title, 0);
            }
            return id;
        }
    }
    const ScannedBook scanned = Scanner::read(file, m_coverDirectory);
    QSqlDatabase db = database();
    Savepoint sp(db, "add_book");
    if (!sp.active)
        return 0;
    QList<StagedBook> staged;
    if (!storeInDb(scanned, staged) || !sp.commit())
        return 0;
    const bool isNew = !staged.isEmpty() && !staged.first().updated;
    applyStaged(staged);
    changed();
    const int id = idForPath(file);
    if (id > 0 && (isNew || wasHidden)) {
        flushNotificationsNow();
        emit booksAdded(id, 1, m_books.value(id).title, 0);
    }
    return id;
}

QVariantMap Library::toMap(const BookRecord &r) const {
    QVariantList colIds;
    const QSet<int> cols = m_bookCollections.value(r.id);
    for (const int cid : cols)
        colIds.append(cid);
    return {
        {"id", r.id},
        {"path", r.path},
        {"folder", QFileInfo(r.path).absolutePath()},
        {"format", r.format},
        {"size", double(r.size)},
        {"title", r.title},
        {"author", r.author},
        {"editor", r.editor},
        {"publisher", r.publisher},
        {"year", r.year},
        {"editionLine", r.editionLine()},
        {"language", r.language},
        {"description", r.description},
        {"subjects", r.subjects},
        {"isbn", r.isbn},
        {"series", r.series},
        {"seriesIndex", r.seriesIndex},
        {"chapters", r.chapters},
        {"pages", r.pages},
        {"length", double(r.length)},
        {"workKey", r.workKey},
        {"cover", coverUrl(r.id)},
        {"problem", r.problem},
        {"addedAt", double(r.addedAt)},
        {"openedAt", double(r.openedAt)},
        {"finishedAt", double(r.finishedAt)},
        {"finished", r.finishedAt > 0},
        {"progress", r.progress},
        {"secondsRead", double(r.secondsRead)},
        {"minutesLeft", minutesLeft(r.id)},
        {"missing", r.missing},
        {"collectionIds", colIds},
        {"wantToRead", r.wantAt > 0 && r.finishedAt == 0},
        {"listening", r.listenedAt > 0},
        {"downloaded", m_downloadedBooks.contains(r.id)},
        {"hasAudiobook", m_audiobookBooks.contains(r.id)},
        {"isArticle", m_articles.contains(r.id)},
        {"sourceUrl", m_sourceUrls.value(r.id)},
        {"chapterTitle", chapterTitleOf(r)},
        {"snippet", snippetOf(r)},
    };
}

// The chapter the reader was last in, saved with the position.
QString Library::chapterTitleOf(const BookRecord &r) {
    if (r.position.isEmpty())
        return {};
    return QJsonDocument::fromJson(r.position.toUtf8()).object().value(QStringLiteral("title")).toString();
}

// A short quote from the last-read page, saved alongside the position.
QString Library::snippetOf(const BookRecord &r) {
    if (r.position.isEmpty())
        return {};
    return QJsonDocument::fromJson(r.position.toUtf8())
        .object()
        .value(QStringLiteral("snippet"))
        .toString()
        .simplified();
}

void Library::enrichBookMap(QVariantMap &map, int id) const {
    map.insert(QStringLiteral("highlightCount"), m_highlightCounts.value(id, 0));
    map.insert(QStringLiteral("voiceName"), m_voiceNames.value(id));
}

void Library::setWantToRead(int id, bool want) {
    if (!m_books.contains(id) || (m_books[id].wantAt > 0) == want)
        return;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE books SET want_at = ? WHERE id = ?"));
    const qint64 when = want ? now() : 0;
    query.addBindValue(when);
    query.addBindValue(id);
    if (!exec(query))
        return;
    m_books[id].wantAt = when;
    emit bookChanged(id);
    changed(false, false);
}

void Library::markListened(int id) {
    if (!m_books.contains(id) || m_books[id].listenedAt > 0)
        return;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE books SET listened_at = ? WHERE id = ?"));
    const qint64 when = now();
    query.addBindValue(when);
    query.addBindValue(id);
    if (!exec(query))
        return;
    m_books[id].listenedAt = when;
    emit bookChanged(id);
    changed(false, false);
}

// How many books each shelf of the sidebar holds: visible books only, a
// work's editions counted once each as the grid shows them ungrouped.
QVariantMap Library::shelfCounts() const {
    int all = 0, reading = 0, recent = 0, want = 0, finished = 0, listening = 0, epub = 0, pdf = 0, articles = 0,
        downloaded = 0, audiobooks = 0;
    const qint64 recentCutoff = now() - WorksModel::recentWindowSecs;
    for (const auto &r : std::as_const(m_books)) {
        if (!r.visible())
            continue;
        ++all;
        const bool isArt = m_articles.contains(r.id);
        if (isArt)
            ++articles;
        if (r.format.compare(QLatin1String("epub"), Qt::CaseInsensitive) == 0 && !isArt)
            ++epub;
        else if (r.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
            ++pdf;
        if (r.addedAt > 0 && r.addedAt >= recentCutoff)
            ++recent;
        if (r.finishedAt > 0)
            ++finished;
        else if (r.openedAt > 0)
            ++reading;
        if (r.wantAt > 0 && r.finishedAt == 0)
            ++want;
        if (r.listenedAt > 0)
            ++listening;
        if (m_downloadedBooks.contains(r.id))
            ++downloaded;
        if (m_audiobookBooks.contains(r.id))
            ++audiobooks;
    }
    return {{"all", all},
            {"reading", reading},
            {"recent", recent},
            {"want", want},
            {"finished", finished},
            {"listening", listening},
            {"epub", epub},
            {"pdf", pdf},
            {"articles", articles},
            {"downloaded", downloaded},
            {"audiobooks", audiobooks}};
}

QVariantMap Library::book(int id) const {
    if (!m_books.contains(id))
        return {};
    QVariantMap map = toMap(m_books.value(id));
    enrichBookMap(map, id);
    return map;
}

QVariantList Library::allBooks() const {
    QList<BookRecord> found;
    found.reserve(m_books.size());
    for (const auto &record : std::as_const(m_books))
        if (record.visible())
            found.append(record);
    std::sort(found.begin(), found.end(), [](const BookRecord &a, const BookRecord &b) {
        if (a.openedAt != b.openedAt)
            return a.openedAt > b.openedAt;
        if (a.addedAt != b.addedAt)
            return a.addedAt > b.addedAt;
        return a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
    });
    QVariantList list;
    list.reserve(found.size());
    for (const auto &record : std::as_const(found))
        list.append(toMap(record));
    return list;
}

QVariantList Library::collections() const {
    QList<CollectionRecord> sorted = m_collections.values();
    std::sort(sorted.begin(), sorted.end(), [](const CollectionRecord &a, const CollectionRecord &b) {
        const int c = a.name.localeAwareCompare(b.name);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    QVariantList out;
    out.reserve(sorted.size());
    for (const CollectionRecord &col : std::as_const(sorted)) {
        int visibleCount = 0;
        const QSet<int> bookIds = m_collectionBooks.value(col.id);
        for (const int bookId : bookIds)
            if (m_books.contains(bookId) && m_books.value(bookId).visible())
                ++visibleCount;
        out.append(QVariantMap{{"id", col.id},
                               {"name", col.name},
                               {"count", visibleCount},
                               {"createdAt", double(col.createdAt)}});
    }
    return out;
}

int Library::createCollection(const QString &name) {
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty() || !database().isOpen())
        return 0;
    for (const CollectionRecord &existing : std::as_const(m_collections))
        if (existing.name.compare(cleaned, Qt::CaseInsensitive) == 0)
            return existing.id;
    const qint64 createdAt = now();
    QSqlQuery query(database());
    query.prepare(QStringLiteral("INSERT INTO collections (name, created_at) VALUES (?, ?)"));
    query.addBindValue(cleaned);
    query.addBindValue(createdAt);
    if (!exec(query))
        return 0;
    CollectionRecord col;
    col.id = query.lastInsertId().toInt();
    col.name = cleaned;
    col.createdAt = createdAt;
    m_collections.insert(col.id, col);
    syncWorksCollections();
    emit collectionsChanged();
    return col.id;
}

bool Library::renameCollection(int id, const QString &name) {
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty() || !m_collections.contains(id) || !database().isOpen())
        return false;
    for (const CollectionRecord &existing : std::as_const(m_collections))
        if (existing.id != id && existing.name.compare(cleaned, Qt::CaseInsensitive) == 0)
            return false;
    if (m_collections[id].name == cleaned)
        return true;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE collections SET name = ? WHERE id = ?"));
    query.addBindValue(cleaned);
    query.addBindValue(id);
    if (!exec(query))
        return false;
    m_collections[id].name = cleaned;
    emit collectionsChanged();
    return true;
}

bool Library::deleteCollection(int id) {
    if (!m_collections.contains(id) || !database().isOpen())
        return false;
    QSqlDatabase db = database();
    Savepoint sp(db, "delete_collection");
    if (!sp.active)
        return false;
    {
        QSqlQuery delLinks(db);
        delLinks.prepare(QStringLiteral("DELETE FROM book_collections WHERE collection_id = ?"));
        delLinks.addBindValue(id);
        if (!exec(delLinks))
            return false;
    }
    {
        QSqlQuery delCol(db);
        delCol.prepare(QStringLiteral("DELETE FROM collections WHERE id = ?"));
        delCol.addBindValue(id);
        if (!exec(delCol))
            return false;
    }
    if (!sp.commit())
        return false;
    const QSet<int> affected = m_collectionBooks.take(id);
    for (const int bookId : affected)
        m_bookCollections[bookId].remove(id);
    m_collections.remove(id);
    if (m_works->collectionId() == id)
        m_works->setCollectionId(0);
    syncWorksCollections();
    emit collectionsChanged();
    for (const int bookId : affected)
        emit bookChanged(bookId);
    return true;
}

bool Library::addToCollection(int bookId, int collectionId) {
    if (!m_books.contains(bookId) || !m_collections.contains(collectionId) || !database().isOpen())
        return false;
    if (m_collectionBooks.value(collectionId).contains(bookId)) {
        if (m_worksDirty)
            syncWorksCollections();
        return true;
    }
    QSqlQuery query(database());
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO book_collections (book_id, collection_id, added_at) VALUES (?, ?, ?)"));
    query.addBindValue(bookId);
    query.addBindValue(collectionId);
    query.addBindValue(now());
    if (!exec(query))
        return false;
    m_collectionBooks[collectionId].insert(bookId);
    m_bookCollections[bookId].insert(collectionId);
    syncWorksCollections();
    emit collectionsChanged();
    emit bookChanged(bookId);
    return true;
}

bool Library::removeFromCollection(int bookId, int collectionId) {
    if (!m_collections.contains(collectionId) || !database().isOpen())
        return false;
    if (!m_collectionBooks.value(collectionId).contains(bookId))
        return true;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("DELETE FROM book_collections WHERE book_id = ? AND collection_id = ?"));
    query.addBindValue(bookId);
    query.addBindValue(collectionId);
    if (!exec(query))
        return false;
    m_collectionBooks[collectionId].remove(bookId);
    m_bookCollections[bookId].remove(collectionId);
    syncWorksCollections();
    emit collectionsChanged();
    emit bookChanged(bookId);
    return true;
}

QVariantList Library::collectionsForBook(int bookId) const {
    const QSet<int> ids = m_bookCollections.value(bookId);
    QList<CollectionRecord> cols;
    cols.reserve(ids.size());
    for (const int id : ids)
        if (m_collections.contains(id))
            cols.append(m_collections.value(id));
    std::sort(cols.begin(), cols.end(), [](const CollectionRecord &a, const CollectionRecord &b) {
        const int c = a.name.localeAwareCompare(b.name);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    QVariantList out;
    out.reserve(cols.size());
    for (const CollectionRecord &c : std::as_const(cols))
        out.append(QVariantMap{{"id", c.id}, {"name", c.name}});
    return out;
}

bool Library::isInCollection(int bookId, int collectionId) const {
    return m_collectionBooks.value(collectionId).contains(bookId);
}

QVariantMap Library::readingStats() const {
    int totalBooks = 0, finishedBooks = 0, readingBooks = 0, unreadBooks = 0;
    qint64 totalSeconds = 0;
    for (const BookRecord &r : std::as_const(m_books)) {
        if (!r.visible())
            continue;
        ++totalBooks;
        totalSeconds += r.secondsRead;
        if (r.finishedAt > 0)
            ++finishedBooks;
        else if (r.openedAt > 0)
            ++readingBooks;
        else
            ++unreadBooks;
    }

    QHash<QDate, qint64> byDay;
    QList<QDate> activeDates;
    qint64 daysSecondsSum = 0;
    if (database().isOpen()) {
        QSqlQuery q(database());
        if (q.exec(QStringLiteral("SELECT day, SUM(seconds) FROM reading_days WHERE seconds > 0 GROUP BY day ORDER BY day ASC"))) {
            while (q.next()) {
                const QDate d = QDate::fromString(q.value(0).toString(), Qt::ISODate);
                const qint64 sec = q.value(1).toLongLong();
                if (d.isValid() && sec > 0) {
                    byDay.insert(d, sec);
                    activeDates.append(d);
                    daysSecondsSum += sec;
                }
            }
        }
    }
    totalSeconds = qMax(totalSeconds, daysSecondsSum);

    const QDate today = QDate::currentDate();
    const qint64 todaySeconds = byDay.value(today, 0);
    qint64 weekSeconds = 0;
    for (int i = 0; i < 7; ++i)
        weekSeconds += byDay.value(today.addDays(-i), 0);

    // Current streak: counts backwards from today if read today, else from yesterday.
    int streakDays = 0;
    QDate cursor = byDay.contains(today) ? today : today.addDays(-1);
    while (byDay.contains(cursor)) {
        ++streakDays;
        cursor = cursor.addDays(-1);
    }

    // Longest consecutive reading streak ever recorded.
    int longestStreak = 0;
    int run = 0;
    QDate prev;
    for (const QDate &d : std::as_const(activeDates)) {
        run = (prev.isValid() && prev.daysTo(d) == 1) ? (run + 1) : 1;
        longestStreak = qMax(longestStreak, run);
        prev = d;
    }

    // Last 14 days for the sparkline / activity bar chart.
    QVariantList recentDays;
    recentDays.reserve(14);
    for (int i = 13; i >= 0; --i) {
        const QDate d = today.addDays(-i);
        recentDays.append(QVariantMap{{"day", d.toString(Qt::ISODate)},
                                      {"shortLabel", QLocale().toString(d, QStringLiteral("ddd"))},
                                      {"seconds", double(byDay.value(d, 0))}});
    }

    return {
        {"totalBooks", totalBooks},
        {"finishedBooks", finishedBooks},
        {"readingBooks", readingBooks},
        {"unreadBooks", unreadBooks},
        {"totalSeconds", double(totalSeconds)},
        {"todaySeconds", double(todaySeconds)},
        {"weekSeconds", double(weekSeconds)},
        {"streakDays", streakDays},
        {"longestStreak", longestStreak},
        {"activeDays", int(activeDates.size())},
        {"charsPerMinute", m_charsPerMinute},
        {"wordsPerMinute", qMax(50, int(std::round(m_charsPerMinute / 5.0)))},
        {"recentDays", recentDays},
    };
}

QVariantMap Library::bookReadingStats(int id) const {
    const BookRecord r = m_books.value(id);
    if (r.id == 0)
        return {};
    int daysRead = 0;
    QString firstDay, lastDay;
    if (database().isOpen()) {
        QSqlQuery q(database());
        q.prepare(QStringLiteral("SELECT COUNT(*), MIN(day), MAX(day) FROM reading_days WHERE book_id = ? AND seconds > 0"));
        q.addBindValue(id);
        if (q.exec() && q.next()) {
            daysRead = q.value(0).toInt();
            firstDay = q.value(1).toString();
            lastDay = q.value(2).toString();
        }
    }
    const int totalEstimatedMinutes =
        (r.length > 0 && m_charsPerMinute > 0) ? qMax(1, int(std::ceil(r.length / m_charsPerMinute))) : 0;
    return {
        {"bookId", r.id},
        {"secondsRead", double(r.secondsRead)},
        {"daysRead", daysRead},
        {"firstDay", firstDay},
        {"lastDay", lastDay},
        {"averageSecondsPerDay", daysRead > 0 ? double(r.secondsRead) / daysRead : 0.0},
        {"minutesLeft", minutesLeft(id)},
        {"estimatedTotalMinutes", totalEstimatedMinutes},
    };
}

QVariantList Library::editions(const QString &workKey) const {
    QList<BookRecord> found;
    for (const auto &record : m_books)
        if (record.visible() && record.workKey == workKey)
            found.append(record);
    std::sort(found.begin(), found.end(), [](const BookRecord &a, const BookRecord &b) {
        if (a.year != b.year)
            return a.year > b.year;
        return a.publisher.localeAwareCompare(b.publisher) < 0;
    });
    QVariantList list;
    for (const auto &record : found)
        list.append(toMap(record));
    return list;
}

QVariantList Library::continueReading() const {
    QList<BookRecord> found;
    for (const auto &record : m_books)
        if (record.visible() && record.openedAt > 0 && record.finishedAt == 0 && record.progress < 0.995)
            found.append(record);
    const qsizetype take = qMin<qsizetype>(8, found.size());
    std::partial_sort(found.begin(), found.begin() + take, found.end(),
                      [](const BookRecord &a, const BookRecord &b) { return a.openedAt > b.openedAt; });
    QVariantList list;
    list.reserve(take);
    for (qsizetype i = 0; i < take; ++i) {
        QVariantMap map = toMap(found[i]);
        enrichBookMap(map, found[i].id);
        list.append(map);
    }
    return list;
}

void Library::setFinished(int id, bool finished) {
    if (!m_books.contains(id))
        return;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE books SET finished_at = ? WHERE id = ?"));
    const qint64 when = finished ? now() : 0;
    query.addBindValue(when);
    query.addBindValue(id);
    if (!exec(query))
        return;
    m_books[id].finishedAt = when;
    emit bookChanged(id);
    changed();
}

void Library::setHidden(int id, bool hidden) {
    if (!m_books.contains(id))
        return;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE books SET hidden = ? WHERE id = ?"));
    query.addBindValue(hidden ? 1 : 0);
    query.addBindValue(id);
    if (!exec(query))
        return;
    m_books[id].hidden = hidden;
    changed();
}

void Library::showInFolder(int id) const {
    const BookRecord record = m_books.value(id);
    if (record.path.isEmpty())
        return;
    // The file manager selects the file when it speaks FileManager1, as
    // Nautilus does; otherwise the folder just opens. Run asynchronously so
    // the UI thread never blocks waiting for D-Bus activation.
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    call.setArguments({QStringList{QUrl::fromLocalFile(record.path).toString()}, QString()});
    const QString folder = QFileInfo(record.path).absolutePath();
    const QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call, 1500);
    auto *watcher = new QDBusPendingCallWatcher(pending, const_cast<Library *>(this));
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, folder] {
        watcher->deleteLater();
        const QDBusPendingReply<> reply(*watcher);
        if (reply.isError())
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
}

QString Library::coverUrl(int id) const {
    const BookRecord record = m_books.value(id);
    if (record.cover.isEmpty())
        return {};
    return QUrl::fromLocalFile(m_coverDirectory + '/' + record.cover).toString() + QStringLiteral("?v=") +
           QString::number(record.mtime);
}

int Library::lastOpenedBook() const {
    int best = 0;
    qint64 when = 0;
    for (const auto &record : m_books)
        if (record.visible() && record.openedAt > when) {
            when = record.openedAt;
            best = record.id;
        }
    return best;
}

int Library::minutesLeft(int id) const {
    const BookRecord record = m_books.value(id);
    if (record.length <= 0 || m_charsPerMinute <= 0)
        return 0;
    return qMax(1, int(std::ceil(record.length * (1.0 - record.progress) / m_charsPerMinute)));
}

void Library::markOpened(int id) {
    if (!m_books.contains(id))
        return;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE books SET opened_at = ? WHERE id = ?"));
    const qint64 when = now();
    query.addBindValue(when);
    query.addBindValue(id);
    if (!exec(query))
        return;
    const bool firstOpen = m_books[id].openedAt == 0;
    m_books[id].openedAt = when;
    changed(firstOpen, false);
}

void Library::savePosition(int id, const QString &position, double progress) {
    if (!m_books.contains(id))
        return;
    progress = std::clamp(progress, 0.0, 1.0);
    QSqlQuery query(database());
    query.prepare(QStringLiteral("UPDATE books SET position = ?, progress = ? WHERE id = ?"));
    query.addBindValue(position);
    query.addBindValue(progress);
    query.addBindValue(id);
    if (!exec(query))
        return;
    BookRecord &rec = m_books[id];
    const double prevProgress = rec.progress;
    rec.position = position;
    rec.progress = progress;
    // Update only the changed book in WorksModel and emit targeted signals
    // instead of firing booksChanged()/collectionsChanged()/readingStatsChanged()
    // storms on every page turn.
    m_works->updateBook(rec);
    emit bookUpdated(id);
    emit bookChanged(id);
    if (rec.visible() && rec.openedAt > 0 && rec.finishedAt == 0 && (prevProgress < 0.995 || progress < 0.995)) {
        m_continueNotifyDirty = true;
        if (!m_notify.isActive())
            m_notify.start();
    }
}

void Library::addReadingTime(int id, int seconds, qint64 charsRead) {
    if (!m_books.contains(id) || seconds <= 0)
        return;
    QSqlDatabase db = database();
    Savepoint sp(db, "reading_time");
    if (!sp.active)
        return;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE books SET seconds_read = seconds_read + ? WHERE id = ?"));
    query.addBindValue(seconds);
    query.addBindValue(id);
    if (!exec(query))
        return;
    QSqlQuery day(db);
    day.prepare(QStringLiteral("INSERT INTO reading_days (book_id, day, seconds) VALUES (?, ?, ?) "
                               "ON CONFLICT (book_id, day) DO UPDATE SET seconds = seconds + excluded.seconds"));
    day.addBindValue(id);
    day.addBindValue(QDate::currentDate().toString(Qt::ISODate));
    day.addBindValue(seconds);
    if (!exec(day))
        return;

    // The reader's own pace, learned from real reading: characters passed
    // per minute, once there's enough of it to trust, within human bounds.
    double pacedSeconds = 0, pacedChars = 0;
    double newPace = m_charsPerMinute;
    bool paceUpdated = false;
    {
        QSqlQuery readTotals(db);
        if (!readTotals.exec(QStringLiteral("SELECT key, value FROM meta WHERE key IN ('paced_seconds', 'paced_chars')"))) {
            m_error = readTotals.lastError().text();
            return;
        }
        while (readTotals.next())
            (readTotals.value(0).toString() == "paced_seconds" ? pacedSeconds : pacedChars) = readTotals.value(1).toDouble();
    }
    // A jump (a search hit, the contents) isn't reading.
    if (charsRead > 0 && charsRead < qint64(seconds) * 120) {
        pacedSeconds += seconds;
        pacedChars += double(charsRead);
        QSqlQuery save(db);
        save.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES ('paced_seconds', ?), "
                                    "('paced_chars', ?), ('chars_per_minute', ?)"));
        newPace = pacedSeconds >= 600 ? std::clamp(pacedChars / pacedSeconds * 60, 400.0, 4000.0) : m_charsPerMinute;
        save.addBindValue(pacedSeconds);
        save.addBindValue(pacedChars);
        save.addBindValue(newPace);
        if (!exec(save))
            return;
        paceUpdated = newPace != m_charsPerMinute;
    }
    if (!sp.commit())
        return;
    m_books[id].secondsRead += seconds;
    emit bookChanged(id);
    emit readingStatsChanged();
    if (paceUpdated) {
        m_charsPerMinute = newPace;
        emit readingSpeedChanged();
    }
}

QString Library::bookSetting(int id, const QString &key) const {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("SELECT value FROM book_settings WHERE book_id = ? AND key = ?"));
    query.addBindValue(id);
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

void Library::setBookSetting(int id, const QString &key, const QString &value) {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO book_settings (book_id, key, value) VALUES (?, ?, ?)"));
    query.addBindValue(id);
    query.addBindValue(key);
    // An empty setting is "", never NULL (which the table refuses).
    query.addBindValue(value.isNull() ? QStringLiteral("") : value);
    if (!exec(query))
        return;
    const QString norm = value.trimmed().toLower();
    const bool truthy = !norm.isEmpty() && norm != QLatin1String("0") && norm != QLatin1String("false");
    if (key == QLatin1String("audio_downloaded")) {
        const bool had = m_downloadedBooks.contains(id);
        if (truthy != had) {
            if (truthy)
                m_downloadedBooks.insert(id);
            else
                m_downloadedBooks.remove(id);
            m_works->setAudioSets(m_downloadedBooks, m_audiobookBooks);
            emit bookChanged(id);
            changed(false, false);
        }
    } else if (key == QLatin1String("audiobook_saved")) {
        const bool had = m_audiobookBooks.contains(id);
        if (truthy != had) {
            if (truthy)
                m_audiobookBooks.insert(id);
            else
                m_audiobookBooks.remove(id);
            m_works->setAudioSets(m_downloadedBooks, m_audiobookBooks);
            emit bookChanged(id);
            changed(false, false);
        }
    } else if (key == QLatin1String("content_kind")) {
        const bool wantArticle = (norm == QLatin1String("article"));
        if (wantArticle)
            m_articleSettingIds.insert(id);
        else
            m_articleSettingIds.remove(id);
        const bool isArt = m_books.contains(id) ? isArticleRecord(m_books.value(id)) : wantArticle;
        const bool had = m_articles.contains(id);
        if (isArt != had) {
            if (isArt)
                m_articles.insert(id);
            else
                m_articles.remove(id);
            m_works->setArticleSet(m_articles);
            emit bookChanged(id);
            changed(false, false);
        }
    } else if (key == QLatin1String("source_url")) {
        const QString trimmed = value.trimmed();
        const QString prev = m_sourceUrls.value(id);
        if (trimmed != prev) {
            if (trimmed.isEmpty())
                m_sourceUrls.remove(id);
            else
                m_sourceUrls.insert(id, trimmed);
            emit bookChanged(id);
        }
    } else if (key == QLatin1String("voiceName")) {
        if (value.isEmpty())
            m_voiceNames.remove(id);
        else
            m_voiceNames.insert(id, value);
        emit bookChanged(id);
        changed(false, false);
    }
}

QList<int> Library::chapterLengths(int id) const {
    QList<int> lengths;
    QSqlQuery query(database());
    query.prepare(QStringLiteral("SELECT length FROM chapter_lengths WHERE book_id = ? ORDER BY chapter"));
    query.addBindValue(id);
    query.exec();
    while (query.next())
        lengths.append(query.value(0).toInt());
    return lengths;
}

bool Library::writeChapterLengths(QSqlDatabase &db, int id, const QList<int> &lengths, qint64 *totalOut) {
    QSqlQuery remove(db);
    remove.prepare(QStringLiteral("DELETE FROM chapter_lengths WHERE book_id = ?"));
    remove.addBindValue(id);
    if (!exec(remove))
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
            ids << id;
            chapters << i;
            values << lengths[i];
            total += lengths[i];
        }
        insert.addBindValue(ids);
        insert.addBindValue(chapters);
        insert.addBindValue(values);
        if (!insert.execBatch()) {
            m_error = insert.lastError().text();
            qWarning("Library: %s", qPrintable(m_error));
            return false;
        }
    }
    if (total > 0) {
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE books SET length = ? WHERE id = ?"));
        update.addBindValue(total);
        update.addBindValue(id);
        if (!exec(update))
            return false;
    }
    if (totalOut)
        *totalOut = total;
    return true;
}

void Library::setChapterLengths(int id, const QList<int> &lengths) {
    QSqlDatabase db = database();
    Savepoint sp(db, "chapter_lengths");
    if (!sp.active)
        return;
    qint64 total = 0;
    if (!writeChapterLengths(db, id, lengths, &total) || !sp.commit())
        return;
    if (m_books.contains(id) && total > 0 && m_books[id].length != total)
        m_books[id].length = total;
}
