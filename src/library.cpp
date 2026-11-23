#include "library.h"

#include "librarymodel.h"
#include "scanner.h"

#include <QCoreApplication>
#include <QDate>
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
    "added_at, opened_at, finished_at, progress, position, seconds_read, missing, hidden, loose");

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
    // Many changes in a burst (a scan, a page turn) become one refresh.
    m_notify.setSingleShot(true);
    m_notify.setInterval(60);
    connect(&m_notify, &QTimer::timeout, this, [this] {
        if (m_worksDirty) {
            m_worksDirty = false;
            m_works->setBooks(m_books);
        }
        emit booksChanged();
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
        m_scanning = false;
        emit scanningChanged();
        emit scanFinished(m_added, m_updated);
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
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_dataDirectory + QStringLiteral("/library.sqlite"));
    if (!db.open()) {
        m_error = db.lastError().text();
        return false;
    }
    QSqlQuery pragma(db);
    // Set busy_timeout first so journal_mode=WAL waits rather than failing with
    // SQLITE_BUSY when another Dogear instance or the CLI holds the database.
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    if (!migrate())
        return false;
    loadFolders();
    loadBooks();
    QSqlQuery speed(db);
    speed.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'chars_per_minute'"));
    if (speed.next() && speed.value(0).toDouble() > 0)
        m_charsPerMinute = speed.value(0).toDouble();
    m_works->setBooks(m_books);
    return true;
}

bool Library::migrate() {
    QSqlDatabase db = database();
    const QStringList schema = {
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
        "INSERT OR IGNORE INTO meta (key, value) VALUES ('schema', '1')",
    };
    Savepoint sp(db, "migrate");
    if (!sp.active) {
        m_error = db.lastError().text();
        return false;
    }
    for (const QString &statement : schema) {
        QSqlQuery query(db);
        if (!query.exec(statement)) {
            m_error = query.lastError().text();
            return false;
        }
    }
    // Upgrade existing databases without a content fingerprint column.
    bool hasFingerprint = false;
    {
        QSqlQuery cols(db);
        if (!cols.exec(QStringLiteral("PRAGMA table_info(books)"))) {
            m_error = cols.lastError().text();
            return false;
        }
        while (cols.next())
            if (cols.value(1).toString() == QLatin1String("fingerprint")) {
                hasFingerprint = true;
                break;
            }
    }
    if (!hasFingerprint) {
        QSqlQuery alter(db);
        if (!alter.exec(QStringLiteral("ALTER TABLE books ADD COLUMN fingerprint TEXT NOT NULL DEFAULT ''"))) {
            m_error = alter.lastError().text();
            return false;
        }
    }
    {
        QSqlQuery idx(db);
        if (!idx.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS books_fingerprint ON books (fingerprint)"))) {
            m_error = idx.lastError().text();
            return false;
        }
    }
    if (!sp.commit()) {
        m_error = db.lastError().text();
        return false;
    }
    return true;
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
    return r;
}

void Library::loadBooks() {
    m_books.clear();
    m_pathToId.clear();
    m_fingerprints.clear();
    QSqlQuery query(database());
    query.exec(QStringLiteral("SELECT %1, fingerprint FROM books").arg(bookColumns));
    while (query.next()) {
        const BookRecord record = readRecord(query);
        m_books.insert(record.id, record);
        m_pathToId.insert(record.path, record.id);
        const QString fp = query.value(33).toString();
        if (!fp.isEmpty())
            m_fingerprints.insert(record.id, fp);
    }
}

void Library::changed() {
    m_worksDirty = true;
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
        emit problem(tr("Dogear reads EPUB and PDF books."));
        return;
    }
    bool needScan = false;
    for (const QString &file : std::as_const(files)) {
        const QFileInfo info(file);
        if (const int id = idForPath(file)) {
            const BookRecord &record = m_books[id];
            if (record.size == info.size() && record.mtime == info.lastModified().toSecsSinceEpoch() && !record.missing)
                continue;
        }
        if (!m_queuedFiles.contains(file))
            m_queuedFiles.append(file);
        needScan = true;
    }
    bool foldersAdded = false;
    for (const QString &folder : std::as_const(folders)) {
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
    if (needScan)
        rescan();
}

void Library::removeFolder(const QString &path) {
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
}

void Library::rescan() {
    QStringList loose = m_queuedFiles;
    QHash<QString, QPair<qint64, qint64>> known;
    for (const auto &record : std::as_const(m_books)) {
        if (!m_fingerprints.value(record.id).isEmpty() || record.missing || !record.problem.isEmpty())
            known.insert(record.path, {record.size, record.mtime});
        if (record.loose && !loose.contains(record.path))
            loose.append(record.path);
    }
    m_added = m_updated = 0;
    m_scanDone = m_scanTotal = 0;
    m_scanning = true;
    emit scanningChanged();
    emit scanProgressChanged();
    m_scanner->scan(m_folders, loose, known);
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
        for (auto it = m_fingerprints.cbegin(); it != m_fingerprints.cend(); ++it) {
            if (it.value() != book.fingerprint)
                continue;
            const BookRecord candidate = m_books.value(it.key());
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
    for (const StagedBook &entry : staged) {
        if (!entry.oldPath.isEmpty() && entry.oldPath != entry.record.path)
            m_pathToId.remove(entry.oldPath);
        m_books.insert(entry.record.id, entry.record);
        m_pathToId.insert(entry.record.path, entry.record.id);
        if (!entry.fingerprint.isEmpty())
            m_fingerprints.insert(entry.record.id, entry.fingerprint);
        entry.updated ? ++m_updated : ++m_added;
    }
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
    bool ok = true;
    for (const ScannedBook &book : std::as_const(m_pending))
        if (!storeInDb(book, staged)) {
            ok = false;
            break;
        }
    if (!ok || !sp.commit()) {
        m_pending.clear();
        return;
    }
    applyStaged(staged);
    m_pending.clear();
    changed();
}

int Library::addBookFile(const QString &path) {
    const QString file = canonical(path);
    const QFileInfo info(file);
    if (!info.exists()) {
        m_error = tr("%1 doesn't exist.").arg(path);
        return 0;
    }
    if (!BookFiles::isBook(file)) {
        m_error = tr("Dogear reads EPUB and PDF books.");
        return 0;
    }
    if (const int id = idForPath(file)) {
        if (m_books[id].hidden)
            setHidden(id, false);
        const BookRecord &record = m_books[id];
        if (record.size == info.size() && record.mtime == info.lastModified().toSecsSinceEpoch() && !record.missing)
            return id;
    }
    const ScannedBook scanned = Scanner::read(file, m_coverDirectory);
    QSqlDatabase db = database();
    Savepoint sp(db, "add_book");
    if (!sp.active)
        return 0;
    QList<StagedBook> staged;
    if (!storeInDb(scanned, staged) || !sp.commit())
        return 0;
    applyStaged(staged);
    changed();
    return idForPath(file);
}

QVariantMap Library::toMap(const BookRecord &r) const {
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
        {"chapters", r.chapters},
        {"pages", r.pages},
        {"length", double(r.length)},
        {"workKey", r.workKey},
        {"cover", coverUrl(r.id)},
        {"problem", r.problem},
        {"addedAt", double(r.addedAt)},
        {"openedAt", double(r.openedAt)},
        {"finished", r.finishedAt > 0},
        {"progress", r.progress},
        {"secondsRead", double(r.secondsRead)},
        {"minutesLeft", minutesLeft(r.id)},
        {"missing", r.missing},
    };
}

QVariantMap Library::book(int id) const {
    return m_books.contains(id) ? toMap(m_books.value(id)) : QVariantMap();
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
    std::sort(found.begin(), found.end(),
              [](const BookRecord &a, const BookRecord &b) { return a.openedAt > b.openedAt; });
    QVariantList list;
    for (const auto &record : found.mid(0, 8))
        list.append(toMap(record));
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
    m_books[id].openedAt = when;
    changed();
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
    m_books[id].position = position;
    m_books[id].progress = progress;
    // Update the single work row in place instead of rebuilding the whole grid
    // on every page turn.
    m_works->updateBook(m_books[id]);
    emit bookChanged(id);
    if (!m_notify.isActive())
        m_notify.start();
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
    exec(query);
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
