#include "libraryindex.h"

#include "epubbook.h"
#include "pdfbook.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
QString defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/index.sqlite");
}

// One connection per thread and purpose, closed when done.
struct Connection {
    QString name;
    QSqlDatabase db;
    explicit Connection(const QString &path) : name(QUuid::createUuid().toString()) {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.open();
        QSqlQuery(db).exec(QStringLiteral("PRAGMA journal_mode = WAL"));
        QSqlQuery(db).exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
    }
    ~Connection() {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
};

bool createSchema(QSqlDatabase &db) {
    QSqlQuery q(db);
    return q.exec(QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS chapter_text USING fts5("
                                 "body, book UNINDEXED, chapter UNINDEXED, tokenize = 'unicode61 remove_diacritics 2')")) &&
           q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS indexed (book INTEGER PRIMARY KEY, mtime INTEGER NOT NULL, "
                                 "path TEXT NOT NULL)"));
}
}  // namespace

LibraryIndex::LibraryIndex(const QString &path, QObject *parent)
    : QObject(parent), m_path(path.isEmpty() ? defaultPath() : path),
      m_cancel(std::make_shared<QAtomicInteger<bool>>(false)) {
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    m_connection = QStringLiteral("libraryindex-") + QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_path);
    // Without FTS5 in SQLite, search across books says it isn't available
    // rather than quietly finding nothing.
    m_available = db.open() && createSchema(db);
}

LibraryIndex::~LibraryIndex() {
    cancel();
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
}

int LibraryIndex::bookCount() const {
    if (!m_available)
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

bool LibraryIndex::indexBook(const QString &databasePath, const Source &book, const QAtomicInteger<bool> *cancel) {
    QStringList chapters;
    if (book.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(book.path);
        if (!pdf.open())
            return false;
        for (int page = 0; page < pdf.pageCount() && !(cancel && cancel->loadRelaxed()); ++page)
            chapters.append(pdf.pageText(page));
    } else {
        EpubBook epub(book.path);
        if (!epub.open())
            return false;
        for (int chapter = 0; chapter < int(epub.spine().size()) && !(cancel && cancel->loadRelaxed()); ++chapter)
            chapters.append(epub.chapterText(chapter));
    }
    if (cancel && cancel->loadRelaxed())
        return false;
    Connection c(databasePath);
    if (!c.db.isOpen() || !createSchema(c.db))
        return false;
    c.db.transaction();
    QSqlQuery q(c.db);
    q.prepare(QStringLiteral("DELETE FROM chapter_text WHERE book = ?"));
    q.addBindValue(book.id);
    q.exec();
    q.prepare(QStringLiteral("INSERT INTO chapter_text (body, book, chapter) VALUES (?, ?, ?)"));
    for (int i = 0; i < chapters.size(); ++i) {
        if (chapters[i].trimmed().isEmpty())
            continue;
        q.addBindValue(chapters[i]);
        q.addBindValue(book.id);
        q.addBindValue(i);
        q.exec();
    }
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO indexed (book, mtime, path) VALUES (?, ?, ?)"));
    q.addBindValue(book.id);
    q.addBindValue(book.mtime);
    q.addBindValue(book.path);
    q.exec();
    return c.db.commit();
}

void LibraryIndex::update(const QList<Source> &books) {
    if (!m_available)
        return;
    m_last = books;
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QHash<int, QPair<qint64, QString>> have;
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("SELECT book, mtime, path FROM indexed"));
        while (q.next())
            have.insert(q.value(0).toInt(), {q.value(1).toLongLong(), q.value(2).toString()});
    }
    // Books gone from the library leave the index.
    QSet<int> listed;
    for (const Source &book : books)
        listed.insert(book.id);
    for (auto it = have.cbegin(); it != have.cend(); ++it) {
        if (listed.contains(it.key()))
            continue;
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM chapter_text WHERE book = ?"));
        q.addBindValue(it.key());
        q.exec();
        q.prepare(QStringLiteral("DELETE FROM indexed WHERE book = ?"));
        q.addBindValue(it.key());
        q.exec();
    }
    // New or changed books join the queue.
    for (const Source &book : books) {
        const auto found = have.constFind(book.id);
        const bool fresh = found != have.cend() && found->first == book.mtime && found->second == book.path;
        const bool queued = std::any_of(m_queue.cbegin(), m_queue.cend(), [&](const Source &s) { return s.id == book.id; });
        if (!fresh && !queued)
            m_queue.append(book);
    }
    if (!m_indexing && !m_queue.isEmpty()) {
        m_done = 0;
        m_total = int(m_queue.size());
        m_indexing = true;
        m_cancel->storeRelaxed(false);
        emit progressChanged();
        next();
    } else if (m_indexing) {
        m_total = m_done + int(m_queue.size()) + 1;
        emit progressChanged();
    } else {
        emit progressChanged();
    }
}

void LibraryIndex::next() {
    if (m_queue.isEmpty() || m_cancel->loadRelaxed()) {
        m_queue.clear();
        m_indexing = false;
        emit progressChanged();
        return;
    }
    const Source book = m_queue.takeFirst();
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        watcher->deleteLater();
        ++m_done;
        emit progressChanged();
        next();
    });
    const QString path = m_path;
    const auto cancel = m_cancel;
    watcher->setFuture(QtConcurrent::run([path, book, cancel] {
        // Gently: one book at a time, at low priority, never in the reader's way.
        QThread::currentThread()->setPriority(QThread::LowPriority);
        return indexBook(path, book, cancel.get());
    }));
}

void LibraryIndex::cancel() {
    m_cancel->storeRelaxed(true);
    m_queue.clear();
}

void LibraryIndex::rebuild() {
    if (!m_available)
        return;
    cancel();
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM chapter_text"));
    q.exec(QStringLiteral("DELETE FROM indexed"));
    m_indexing = false;
    update(m_last);
}

QVariantList LibraryIndex::search(const QString &query, int limit) const {
    QVariantList results;
    const QString match = matchExpression(query);
    if (!m_available || match.isEmpty())
        return results;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("SELECT book, chapter, snippet(chapter_text, 0, '‹', '›', '…', 14), body "
                             "FROM chapter_text WHERE chapter_text MATCH ? ORDER BY rank LIMIT ?"));
    q.addBindValue(match);
    q.addBindValue(qMax(1, limit));
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
