#include "library/libraryindex.h"

#include "formats/epubbook.h"
#include "formats/pdfbook.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

namespace {
QString defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/index.sqlite");
}

void applyPragmas(QSqlDatabase &db) {
    QSqlQuery pragma(db);
    // Set busy_timeout first so WAL configuration and concurrent writes wait
    // instead of failing immediately with SQLITE_BUSY.
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA cache_size = -16000"));
    pragma.exec(QStringLiteral("PRAGMA mmap_size = 67108864"));
}

// One connection per thread and purpose, closed when done.
struct Connection {
    QString name;
    QSqlDatabase db;
    explicit Connection(const QString &path) : name(QUuid::createUuid().toString()) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open())
            applyPragmas(db);
    }
    ~Connection() {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
};

bool createSchema(QSqlDatabase &db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS chapter_text USING fts5("
                               "body, book UNINDEXED, chapter UNINDEXED, tokenize = 'unicode61 remove_diacritics 2')")) ||
        !q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS indexed (book INTEGER PRIMARY KEY, mtime INTEGER NOT NULL, "
                               "path TEXT NOT NULL)")) ||
        !q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS chapter_rows (book INTEGER NOT NULL, chapter INTEGER NOT NULL, "
                               "fts_rowid INTEGER NOT NULL, PRIMARY KEY (book, chapter))"))) {
        return false;
    }
    // One-time backfill for pre-existing index.sqlite databases created before
    // chapter_rows existed, giving O(log N) indexed deletes and passage lookups.
    QSqlQuery hasRows(db);
    QSqlQuery hasIndexed(db);
    if (hasRows.exec(QStringLiteral("SELECT 1 FROM chapter_rows LIMIT 1")) && !hasRows.next() &&
        hasIndexed.exec(QStringLiteral("SELECT 1 FROM indexed LIMIT 1")) && hasIndexed.next()) {
        if (db.transaction()) {
            QSqlQuery backfill(db);
            if (backfill.exec(QStringLiteral("INSERT OR REPLACE INTO chapter_rows (book, chapter, fts_rowid) "
                                             "SELECT book, chapter, rowid FROM chapter_text"))) {
                db.commit();
            } else {
                db.rollback();
            }
        }
    }
    return true;
}

bool deleteBookChapters(QSqlDatabase &db, int bookId) {
    QSqlQuery check(db);
    check.prepare(QStringLiteral("SELECT 1 FROM chapter_rows WHERE book = ? LIMIT 1"));
    check.addBindValue(bookId);
    if (!check.exec())
        return false;
    if (check.next()) {
        QSqlQuery delFts(db);
        delFts.prepare(QStringLiteral(
            "DELETE FROM chapter_text WHERE rowid IN (SELECT fts_rowid FROM chapter_rows WHERE book = ?)"));
        delFts.addBindValue(bookId);
        if (!delFts.exec())
            return false;
        QSqlQuery delMap(db);
        delMap.prepare(QStringLiteral("DELETE FROM chapter_rows WHERE book = ?"));
        delMap.addBindValue(bookId);
        return delMap.exec();
    }
    // Fallback for legacy or manually inserted rows without chapter_rows entries.
    QSqlQuery delFts(db);
    delFts.prepare(QStringLiteral("DELETE FROM chapter_text WHERE book = ?"));
    delFts.addBindValue(bookId);
    return delFts.exec();
}
}  // namespace

LibraryIndex::LibraryIndex(const QString &path, QObject *parent)
    : QObject(parent),
      m_path(path.isEmpty() ? defaultPath() : path),
      m_cancel(std::make_shared<std::atomic<bool>>(false)) {
    m_pool.setMaxThreadCount(1);
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    m_connection = QStringLiteral("libraryindex-") + QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_path);
    // Without FTS5 in SQLite, search across books says it isn't available
    // rather than quietly finding nothing.
    if (db.open()) {
        applyPragmas(db);
        m_available = createSchema(db);
    }
}

LibraryIndex::~LibraryIndex() {
    cancel();
    m_pool.waitForDone();
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
}

bool LibraryIndex::ensureDatabase() const {
    if (m_path.isEmpty())
        return m_available;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (m_available && db.isOpen() && QFileInfo::exists(m_path))
        return true;
    // Self-heal if ~/.cache/clothbound (and index.sqlite) was deleted while running:
    // reopen a fresh file instead of remaining attached to an unlinked inode.
    if (db.isOpen())
        db.close();
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    if (!db.isValid())
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_path);
    m_available = false;
    if (db.open()) {
        applyPragmas(db);
        m_available = createSchema(db);
    }
    return m_available;
}

int LibraryIndex::bookCount() const {
    if (!ensureDatabase())
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    return q.exec(QStringLiteral("SELECT count(*) FROM indexed")) && q.next() ? q.value(0).toInt() : 0;
}

double LibraryIndex::megabytes() const {
    qint64 bytes = 0;
    for (const QString &suffix : {QString(), QStringLiteral("-wal")})
        bytes += QFileInfo(m_path + suffix).size();
    return bytes / (1024.0 * 1024.0);
}

QString LibraryIndex::matchExpression(const QString &query) {
    static const QRegularExpression words(QStringLiteral("[\\p{L}\\p{N}][\\p{L}\\p{N}\\p{M}'’]*"));
    QStringList terms;
    auto it = words.globalMatch(query);
    while (it.hasNext())
        terms.append(it.next().captured(0).replace(QLatin1Char('"'), QString()));
    if (terms.isEmpty())
        return {};
    QStringList quoted;
    for (int i = 0; i < terms.size(); ++i)
        quoted.append(QLatin1Char('"') + terms[i] + QLatin1Char('"') + (i == terms.size() - 1 ? QStringLiteral("*") : QString()));
    return quoted.join(QLatin1Char(' '));
}

bool LibraryIndex::indexBook(const QString &databasePath, const Source &book, const std::atomic<bool> *cancel) {
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (cancelled())
        return false;
    QStringList chapters;
    if (book.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(book.path);
        if (!pdf.open())
            return false;
        for (int page = 0; page < pdf.pageCount() && !cancelled(); ++page)
            chapters.append(pdf.pageText(page));
    } else {
        EpubBook epub(book.path);
        if (!epub.open())
            return false;
        for (int chapter = 0; chapter < int(epub.spine().size()) && !cancelled(); ++chapter)
            chapters.append(epub.chapterText(chapter));
    }
    if (cancelled())
        return false;
    Connection c(databasePath);
    if (!c.db.isOpen() || !createSchema(c.db) || !c.db.transaction())
        return false;
    if (!deleteBookChapters(c.db, book.id)) {
        c.db.rollback();
        return false;
    }
    QSqlQuery insFts(c.db);
    insFts.prepare(QStringLiteral("INSERT INTO chapter_text (body, book, chapter) VALUES (?, ?, ?)"));
    QSqlQuery insMap(c.db);
    insMap.prepare(QStringLiteral("INSERT OR REPLACE INTO chapter_rows (book, chapter, fts_rowid) VALUES (?, ?, ?)"));
    for (int i = 0; i < chapters.size(); ++i) {
        if (cancelled()) {
            c.db.rollback();
            return false;
        }
        if (chapters[i].trimmed().isEmpty())
            continue;
        insFts.addBindValue(chapters[i]);
        insFts.addBindValue(book.id);
        insFts.addBindValue(i);
        if (!insFts.exec()) {
            c.db.rollback();
            return false;
        }
        const qint64 rowId = insFts.lastInsertId().toLongLong();
        insMap.addBindValue(book.id);
        insMap.addBindValue(i);
        insMap.addBindValue(rowId);
        if (!insMap.exec()) {
            c.db.rollback();
            return false;
        }
    }
    QSqlQuery insIdx(c.db);
    insIdx.prepare(QStringLiteral("INSERT OR REPLACE INTO indexed (book, mtime, path) VALUES (?, ?, ?)"));
    insIdx.addBindValue(book.id);
    insIdx.addBindValue(book.mtime);
    insIdx.addBindValue(book.path);
    if (cancelled() || !insIdx.exec()) {
        c.db.rollback();
        return false;
    }
    return c.db.commit();
}

void LibraryIndex::update(const QList<Source> &books) {
    if (!ensureDatabase())
        return;
    m_last = books;
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QHash<int, QPair<qint64, QString>> have;
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("SELECT book, mtime, path FROM indexed")))
            return;
        while (q.next())
            have.insert(q.value(0).toInt(), {q.value(1).toLongLong(), q.value(2).toString()});
    }
    // Books gone from the library leave the index in a single batched transaction.
    QSet<int> listed;
    listed.reserve(books.size());
    for (const Source &book : books)
        listed.insert(book.id);
    QList<int> removedIds;
    for (auto it = have.cbegin(); it != have.cend(); ++it) {
        if (!listed.contains(it.key()))
            removedIds.append(it.key());
    }
    if (!removedIds.isEmpty() && db.transaction()) {
        bool ok = true;
        QSqlQuery delIndexed(db);
        delIndexed.prepare(QStringLiteral("DELETE FROM indexed WHERE book = ?"));
        for (const int removedId : std::as_const(removedIds)) {
            if (!deleteBookChapters(db, removedId)) {
                ok = false;
                break;
            }
            delIndexed.addBindValue(removedId);
            if (!delIndexed.exec()) {
                ok = false;
                break;
            }
        }
        if (ok)
            db.commit();
        else
            db.rollback();
    }
    // New or changed books join the queue in O(1) per book.
    for (const Source &book : books) {
        const auto found = have.constFind(book.id);
        const bool fresh = found != have.cend() && found->first == book.mtime && found->second == book.path;
        const bool inFlight =
            m_indexing && m_activeBookId == book.id && m_activeMtime == book.mtime && m_activePath == book.path;
        if (!fresh && !m_queuedIds.contains(book.id) && !inFlight) {
            m_queue.append(book);
            m_queuedIds.insert(book.id);
        }
    }
    const int remaining = int(m_queue.size() - m_queueHead);
    if (!m_indexing && remaining > 0) {
        m_done = 0;
        m_total = remaining;
        m_indexing = true;
        ++m_generation;
        m_cancel = std::make_shared<std::atomic<bool>>(false);
        emit progressChanged();
        next();
    } else if (m_indexing) {
        m_total = m_done + remaining + 1;
        emit progressChanged();
    } else {
        emit progressChanged();
    }
}

void LibraryIndex::next() {
    if (m_queueHead >= m_queue.size() || !m_cancel || m_cancel->load(std::memory_order_relaxed)) {
        m_queue.clear();
        m_queuedIds.clear();
        m_queueHead = 0;
        m_activeBookId = 0;
        m_activeMtime = 0;
        m_activePath.clear();
        m_indexing = false;
        emit progressChanged();
        return;
    }
    const Source book = m_queue[m_queueHead++];
    m_queuedIds.remove(book.id);
    if (m_queueHead >= m_queue.size()) {
        m_queue.clear();
        m_queueHead = 0;
    }
    m_activeBookId = book.id;
    m_activeMtime = book.mtime;
    m_activePath = book.path;

    const QString path = m_path;
    const auto cancelToken = m_cancel;
    const quint64 gen = m_generation;
    const QPointer<LibraryIndex> self(this);
    m_pool.start([self, path, book, cancelToken, gen] {
        // Gently: one book at a time, at low priority, never in the reader's way.
        QThread::currentThread()->setPriority(QThread::LowPriority);
        if (!cancelToken->load(std::memory_order_relaxed))
            indexBook(path, book, cancelToken.get());
        QMetaObject::invokeMethod(
            self,
            [self, gen, cancelToken] {
                if (!self || gen != self->m_generation || cancelToken->load(std::memory_order_relaxed))
                    return;
                self->m_activeBookId = 0;
                self->m_activeMtime = 0;
                self->m_activePath.clear();
                ++self->m_done;
                emit self->progressChanged();
                self->next();
            },
            Qt::QueuedConnection);
    });
}

void LibraryIndex::cancel() {
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_queue.clear();
    m_queuedIds.clear();
    m_queueHead = 0;
    m_activeBookId = 0;
    m_activeMtime = 0;
    m_activePath.clear();
    if (m_indexing) {
        m_indexing = false;
        emit progressChanged();
    }
}

void LibraryIndex::rebuild() {
    if (!ensureDatabase())
        return;
    cancel();
    // Wait for any cancelled in-flight worker on the dedicated 1-thread pool so
    // it cannot commit stale rows after we wipe the tables or spawn a dual chain.
    m_pool.waitForDone();
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (db.transaction()) {
        QSqlQuery q(db);
        const bool ok = q.exec(QStringLiteral("DELETE FROM chapter_text")) &&
                        q.exec(QStringLiteral("DELETE FROM chapter_rows")) &&
                        q.exec(QStringLiteral("DELETE FROM indexed"));
        if (ok)
            db.commit();
        else
            db.rollback();
    }
    update(m_last);
}

QVariantList LibraryIndex::search(const QString &query, int limit, int bookId) const {
    QVariantList results;
    const QString match = matchExpression(query);
    if (!ensureDatabase() || match.isEmpty())
        return results;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    if (bookId > 0) {
        q.prepare(QStringLiteral("SELECT book, chapter, snippet(chapter_text, 0, '‹', '›', '…', 14), body "
                                 "FROM chapter_text WHERE chapter_text MATCH ? AND book = ? ORDER BY rank LIMIT ?"));
        q.addBindValue(match);
        q.addBindValue(bookId);
        q.addBindValue(qMax(1, limit));
    } else {
        q.prepare(QStringLiteral("SELECT book, chapter, snippet(chapter_text, 0, '‹', '›', '…', 14), body "
                                 "FROM chapter_text WHERE chapter_text MATCH ? ORDER BY rank LIMIT ?"));
        q.addBindValue(match);
        q.addBindValue(qMax(1, limit));
    }
    if (!q.exec())
        return results;
    // The first word searched for, to find where in the chapter to open.
    static const QRegularExpression firstWord(QStringLiteral("[\\p{L}\\p{N}]+"));
    const QString first = firstWord.match(query).captured(0);
    while (q.next()) {
        const QString body = q.value(3).toString();
        qint64 offset = first.isEmpty() ? 0 : body.indexOf(first, 0, Qt::CaseInsensitive);
        if (offset < 0) {
            // Accents differ ("cafe" found "café"): look for the marked words instead.
            const QString snippet = q.value(2).toString();
            const qsizetype a = snippet.indexOf(QStringLiteral("‹")), b = snippet.indexOf(QStringLiteral("›"));
            offset = a >= 0 && b > a ? qMax<qint64>(0, body.indexOf(snippet.mid(a + 1, b - a - 1), 0, Qt::CaseInsensitive)) : 0;
        }
        results.append(QVariantMap{{"bookId", q.value(0).toInt()},
                                   {"chapter", q.value(1).toInt()},
                                   {"offset", double(offset)},
                                   {"snippet", q.value(2).toString().simplified()}});
    }
    return results;
}

QString LibraryIndex::passage(int bookId, int chapter, qint64 start, qint64 end) const {
    if (!ensureDatabase() || end <= start)
        return {};
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    // Indexed O(log N) lookup via chapter_rows -> FTS5 rowid B-tree lookup.
    {
        QSqlQuery rowQ(db);
        rowQ.prepare(QStringLiteral("SELECT fts_rowid FROM chapter_rows WHERE book = ? AND chapter = ?"));
        rowQ.addBindValue(bookId);
        rowQ.addBindValue(chapter);
        if (rowQ.exec() && rowQ.next()) {
            const qint64 rowId = rowQ.value(0).toLongLong();
            QSqlQuery textQ(db);
            textQ.prepare(QStringLiteral("SELECT substr(body, ?, ?) FROM chapter_text WHERE rowid = ?"));
            textQ.addBindValue(start + 1);
            textQ.addBindValue(end - start);
            textQ.addBindValue(rowId);
            return textQ.exec() && textQ.next() ? textQ.value(0).toString().simplified() : QString();
        }
    }
    // Fallback for rows inserted directly into chapter_text without chapter_rows.
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT substr(body, ?, ?) FROM chapter_text WHERE book = ? AND chapter = ?"));
    q.addBindValue(start + 1);
    q.addBindValue(end - start);
    q.addBindValue(bookId);
    q.addBindValue(chapter);
    return q.exec() && q.next() ? q.value(0).toString().simplified() : QString();
}
