#include "mcp/libraryqueryservice.h"

#include "ai/aiservices.h"
#include "ai/localmodels.h"
#include "ai/passageindex.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "library/librarymodel.h"
#include "library/marks.h"
#include "mcp/mcpprotocol.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QPair>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

QString cleanSnippet(QString snippet) {
    snippet.remove(QChar(0x2039));  // ‹
    snippet.remove(QChar(0x203A));  // ›
    return snippet.simplified();
}

QStringList significantQueryTerms(const QString &query) {
    static const QSet<QString> stop = {
        QStringLiteral("a"),     QStringLiteral("an"),    QStringLiteral("and"),   QStringLiteral("are"),
        QStringLiteral("as"),    QStringLiteral("at"),    QStringLiteral("be"),    QStringLiteral("but"),
        QStringLiteral("by"),    QStringLiteral("did"),   QStringLiteral("do"),    QStringLiteral("does"),
        QStringLiteral("for"),   QStringLiteral("from"),  QStringLiteral("had"),   QStringLiteral("has"),
        QStringLiteral("have"),  QStringLiteral("he"),    QStringLiteral("her"),   QStringLiteral("him"),
        QStringLiteral("his"),   QStringLiteral("how"),   QStringLiteral("if"),    QStringLiteral("in"),
        QStringLiteral("into"),  QStringLiteral("is"),    QStringLiteral("it"),    QStringLiteral("me"),
        QStringLiteral("my"),    QStringLiteral("no"),    QStringLiteral("not"),   QStringLiteral("of"),
        QStringLiteral("on"),    QStringLiteral("or"),    QStringLiteral("she"),   QStringLiteral("so"),
        QStringLiteral("that"),  QStringLiteral("the"),   QStringLiteral("their"), QStringLiteral("there"),
        QStringLiteral("they"),  QStringLiteral("this"),  QStringLiteral("to"),    QStringLiteral("up"),
        QStringLiteral("us"),    QStringLiteral("was"),   QStringLiteral("we"),    QStringLiteral("were"),
        QStringLiteral("what"),  QStringLiteral("when"),  QStringLiteral("where"), QStringLiteral("which"),
        QStringLiteral("who"),   QStringLiteral("why"),   QStringLiteral("with"),  QStringLiteral("you"),
        QStringLiteral("about"),
    };
    static const QRegularExpression wordRe(QStringLiteral("[\\p{L}\\p{N}][\\p{L}\\p{N}\\p{M}'’]*"));
    QStringList all;
    QStringList kept;
    auto it = wordRe.globalMatch(query.toLower());
    while (it.hasNext()) {
        const QString w = it.next().captured(0);
        if (!all.contains(w))
            all.append(w);
        if (w.size() >= 2 && !stop.contains(w) && !kept.contains(w))
            kept.append(w);
    }
    return kept.isEmpty() ? all : kept;
}

QString epubChapterTitle(const EpubBook &epub, int spineIndex) {
    QString best;
    for (const EpubTocEntry &entry : epub.toc()) {
        if (entry.spine == spineIndex && !entry.title.isEmpty())
            return entry.title;
        if (entry.spine >= 0 && entry.spine < spineIndex && !entry.title.isEmpty())
            best = entry.title;
    }
    return best;
}

QString pdfSectionTitle(const PdfBook &pdf, int pageIndex) {
    QString best;
    for (const PdfOutlineEntry &entry : pdf.outline()) {
        if (entry.page <= pageIndex && !entry.title.isEmpty())
            best = entry.title;
        else if (entry.page > pageIndex)
            break;
    }
    return best;
}

QJsonArray collectionNamesFromList(const QVariantList &cols) {
    QJsonArray names;
    for (const QVariant &v : cols) {
        const QString name = v.toMap().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            names.append(name);
    }
    return names;
}

bool databasePathHasPassageVectorsReadOnly(const QString &dbPath) {
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath))
        return false;
    bool hasVectors = false;
    const QString connName = QStringLiteral("mcp-pv-ro-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db;
        if (LibraryQueryService::openSqliteReadOnly(dbPath, connName, &db)) {
            Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='passage_vectors'")) &&
                q.next()) {
                QSqlQuery c(db);
                hasVectors = c.exec(QStringLiteral("SELECT 1 FROM passage_vectors LIMIT 1")) && c.next();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return hasVectors;
}

bool databaseHasPassageVectorsReadOnly(const QSqlDatabase &existingDb) {
    const QString dbPath = existingDb.databaseName();
    if (!dbPath.isEmpty() && dbPath != QLatin1String(":memory:"))
        return databasePathHasPassageVectorsReadOnly(dbPath);
    if (!existingDb.isOpen())
        return false;
    QSqlQuery q(existingDb);
    if (!q.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='passage_vectors'")) ||
        !q.next())
        return false;
    QSqlQuery c(existingDb);
    return c.exec(QStringLiteral("SELECT 1 FROM passage_vectors LIMIT 1")) && c.next();
}

QHash<int, QPair<qint64, QString>> readIndexedMapReadOnly(const QString &dbPath) {
    QHash<int, QPair<qint64, QString>> indexed;
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath))
        return indexed;
    const QString connName = QStringLiteral("mcp-idx-ro-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db;
        if (LibraryQueryService::openSqliteReadOnly(dbPath, connName, &db)) {
            Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT book, mtime, path FROM indexed"))) {
                while (q.next())
                    indexed.insert(q.value(0).toInt(), {q.value(1).toLongLong(), q.value(2).toString()});
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return indexed;
}

QVariantList searchFtsReadOnly(const QString &dbPath, const QString &query, int limit, int bookId) {
    QVariantList results;
    const QString match = LibraryIndex::matchExpression(query);
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath) || match.isEmpty())
        return results;
    const QString connName = QStringLiteral("mcp-fts-ro-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db;
        if (LibraryQueryService::openSqliteReadOnly(dbPath, connName, &db)) {
            Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
            QSqlQuery q(db);
            if (bookId > 0) {
                q.prepare(QStringLiteral(
                    "SELECT book, chapter, snippet(chapter_text, 0, '‹', '›', '…', 14), body "
                    "FROM chapter_text WHERE chapter_text MATCH ? AND book = ? ORDER BY rank LIMIT ?"));
                q.addBindValue(match);
                q.addBindValue(bookId);
                q.addBindValue(qMax(1, limit));
            } else {
                q.prepare(QStringLiteral(
                    "SELECT book, chapter, snippet(chapter_text, 0, '‹', '›', '…', 14), body "
                    "FROM chapter_text WHERE chapter_text MATCH ? ORDER BY rank LIMIT ?"));
                q.addBindValue(match);
                q.addBindValue(qMax(1, limit));
            }
            if (q.exec()) {
                static const QRegularExpression firstWord(QStringLiteral("[\\p{L}\\p{N}]+"));
                const QString first = firstWord.match(query).captured(0);
                while (q.next()) {
                    const QString body = q.value(3).toString();
                    qint64 offset = first.isEmpty() ? 0 : body.indexOf(first, 0, Qt::CaseInsensitive);
                    if (offset < 0) {
                        const QString snippet = q.value(2).toString();
                        const qsizetype a = snippet.indexOf(QStringLiteral("‹"));
                        const qsizetype b = snippet.indexOf(QStringLiteral("›"));
                        offset = a >= 0 && b > a
                                     ? qMax<qint64>(0, body.indexOf(snippet.mid(a + 1, b - a - 1), 0, Qt::CaseInsensitive))
                                     : 0;
                    }
                    results.append(QVariantMap{{QStringLiteral("bookId"), q.value(0).toInt()},
                                               {QStringLiteral("chapter"), q.value(1).toInt()},
                                               {QStringLiteral("offset"), double(offset)},
                                               {QStringLiteral("snippet"), q.value(2).toString().simplified()}});
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return results;
}

QString readPassageFromIndexReadOnly(const QString &dbPath, int bookId, int chapter, qint64 start, qint64 end) {
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath) || end <= start)
        return {};
    QString out;
    const QString connName = QStringLiteral("mcp-psg-ro-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db;
        if (LibraryQueryService::openSqliteReadOnly(dbPath, connName, &db)) {
            Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
            bool found = false;
            {
                QSqlQuery fast(db);
                if (fast.prepare(QStringLiteral(
                        "SELECT substr(c.body, ?, ?) FROM chapter_rows r "
                        "JOIN chapter_text c ON c.rowid = r.fts_rowid "
                        "WHERE r.book = ? AND r.chapter = ? LIMIT 1"))) {
                    fast.addBindValue(start + 1);
                    fast.addBindValue(end - start);
                    fast.addBindValue(bookId);
                    fast.addBindValue(chapter);
                    if (fast.exec() && fast.next()) {
                        out = fast.value(0).toString().simplified();
                        found = true;
                    }
                }
            }
            if (!found) {
                QSqlQuery q(db);
                if (q.prepare(QStringLiteral(
                        "SELECT substr(body, ?, ?) FROM chapter_text WHERE book = ? AND chapter = ? LIMIT 1"))) {
                    q.addBindValue(start + 1);
                    q.addBindValue(end - start);
                    q.addBindValue(bookId);
                    q.addBindValue(chapter);
                    if (q.exec() && q.next())
                        out = q.value(0).toString().simplified();
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return out;
}

QString makeLexicalSnippet(const QString &body, qsizetype pos, qsizetype matchLen) {
    const qsizetype start = qMax<qsizetype>(0, pos - 60);
    const qsizetype end = qMin<qsizetype>(body.size(), pos + matchLen + 80);
    QString raw = body.mid(start, end - start).simplified();
    if (start > 0)
        raw.prepend(QStringLiteral("… "));
    if (end < body.size())
        raw.append(QStringLiteral(" …"));
    return raw;
}

QVariantList searchUnindexedBooksLexical(const QList<BookRecord> &candidateBooks,
                                         const QHash<int, QPair<qint64, QString>> &indexedMap,
                                         const QString &query,
                                         int limit) {
    QVariantList out;
    if (candidateBooks.isEmpty() || limit <= 0)
        return out;
    static const QRegularExpression wordRe(QStringLiteral("[\\p{L}\\p{N}][\\p{L}\\p{N}\\p{M}'’]*"));
    QStringList terms;
    auto it = wordRe.globalMatch(query);
    while (it.hasNext()) {
        const QString t = it.next().captured(0);
        if (!t.isEmpty() && !terms.contains(t, Qt::CaseInsensitive))
            terms.append(t);
    }
    if (terms.isEmpty())
        return out;

    auto isIndexedFresh = [&indexedMap](const BookRecord &r) {
        const auto found = indexedMap.constFind(r.id);
        return found != indexedMap.cend() && found->first == r.mtime && found->second == r.path;
    };

    QList<BookRecord> toScan;
    for (const BookRecord &rec : candidateBooks) {
        if (!rec.visible() || rec.path.isEmpty() || !QFileInfo::exists(rec.path) || isIndexedFresh(rec))
            continue;
        toScan.append(rec);
    }

    QElapsedTimer budget;
    budget.start();
    int booksScanned = 0;
    constexpr int kMaxFallbackBooks = 25;
    constexpr qint64 kMaxFallbackMs = 3000;

    auto scanChapter = [&](int bId, int ch, const QString &text) {
        if (text.isEmpty() || out.size() >= limit)
            return;
        qsizetype firstPos = -1;
        qsizetype firstLen = 0;
        for (const QString &term : terms) {
            const qsizetype pos = text.indexOf(term, 0, Qt::CaseInsensitive);
            if (pos < 0)
                return;
            if (firstPos < 0 || pos < firstPos) {
                firstPos = pos;
                firstLen = term.size();
            }
        }
        out.append(QVariantMap{{QStringLiteral("bookId"), bId},
                               {QStringLiteral("chapter"), ch},
                               {QStringLiteral("offset"), double(qMax<qsizetype>(0, firstPos))},
                               {QStringLiteral("snippet"), makeLexicalSnippet(text, qMax<qsizetype>(0, firstPos), firstLen)}});
    };

    for (const BookRecord &rec : toScan) {
        if (out.size() >= limit || booksScanned >= kMaxFallbackBooks || budget.elapsed() >= kMaxFallbackMs)
            break;
        ++booksScanned;
        if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
            PdfBook pdf(rec.path);
            if (!pdf.open())
                continue;
            const int pages = qMin(pdf.pageCount(), 500);
            for (int p = 0; p < pages && out.size() < limit && budget.elapsed() < kMaxFallbackMs; ++p)
                scanChapter(rec.id, p, pdf.pageText(p));
        } else {
            EpubBook epub(rec.path);
            if (!epub.open() || epub.isDrmProtected())
                continue;
            const int chapters = qMin(int(epub.spine().size()), 500);
            for (int ch = 0; ch < chapters && out.size() < limit && budget.elapsed() < kMaxFallbackMs; ++ch)
                scanChapter(rec.id, ch, epub.chapterText(ch));
        }
    }
    return out;
}

QString formatBlockquote(const QString &raw) {
    QString quote = raw;
    quote.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    quote.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = quote.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    for (const QString &line : lines)
        out.append(line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line);
    return out.join(QLatin1Char('\n'));
}

QString appendMarksMarkdownSection(const QString &title, const QString &author, const QList<QJsonObject> &marks) {
    QString out;
    out += QStringLiteral("# %1\n").arg(title);
    if (!author.isEmpty())
        out += QStringLiteral("\n*%1*\n").arg(author);
    QString place;
    for (const QJsonObject &m : marks) {
        const QString label = m.value(QStringLiteral("label")).toString();
        if (label != place && !label.isEmpty()) {
            place = label;
            out += QStringLiteral("\n## %1\n").arg(place);
        }
        const QString kind = m.value(QStringLiteral("kind")).toString();
        const QString quote = m.value(QStringLiteral("quote")).toString();
        const QString note = m.value(QStringLiteral("note")).toString();
        if (kind == QLatin1String("bookmark")) {
            out += QStringLiteral("\n- \U0001F516 %1\n").arg(quote);
        } else if (!quote.isEmpty()) {
            out += QLatin1Char('\n') + formatBlockquote(quote) + QLatin1Char('\n');
        }
        if (!note.isEmpty())
            out += QStringLiteral("\n%1\n").arg(note);
    }
    return out;
}

}  // namespace

QJsonObject LibraryQueryService::ReadPassageResult::toJson() const {
    return QJsonObject{
        {QStringLiteral("book_id"), book.id},
        {QStringLiteral("title"), book.title},
        {QStringLiteral("author"), book.author},
        {QStringLiteral("format"), book.format},
        {QStringLiteral("chapter"), chapter},
        {QStringLiteral("chapter_title"), chapterTitle},
        {QStringLiteral("page"), page > 0 ? QJsonValue(page) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("page_end"), pageEnd > 0 ? QJsonValue(pageEnd) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("offset"), double(offset)},
        {QStringLiteral("total_chars"), double(totalChars)},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("citation"), citation},
        {QStringLiteral("text"), text},
    };
}

LibraryQueryService::LibraryQueryService(Library *library,
                                         LibraryIndex *index,
                                         AiServices *ai,
                                         const Options &options)
    : m_library(library),
      m_index(index),
      m_ai(ai),
      m_indexPath(options.indexPath),
      m_libraryPath(options.libraryPath) {}

LibraryQueryService::~LibraryQueryService() {
    m_passageIndex.reset();
    if (!m_roLibraryConn.isEmpty()) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_roLibraryConn, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_roLibraryConn);
        m_roLibraryConn.clear();
        m_roLibraryOpen = false;
    }
}

QString LibraryQueryService::defaultSettingsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
}

QString LibraryQueryService::defaultIndexFile() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/index.sqlite");
}

QString LibraryQueryService::defaultLibraryFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/library.sqlite");
}

bool LibraryQueryService::openSqliteReadOnly(const QString &dbPath,
                                             const QString &connName,
                                             QSqlDatabase *outDb,
                                             QString *errorOut) {
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Database file not found: %1").arg(dbPath);
        return false;
    }

    auto tryOpen = [&](const QString &name, const QString &opts) -> bool {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(name);
        db.setConnectOptions(opts);
        Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
        if (!db.open()) {
            if (errorOut)
                *errorOut = db.lastError().text();
            return false;
        }
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
        pragma.exec(QStringLiteral("PRAGMA query_only=ON"));
        QSqlQuery probe(db);
        if (!probe.exec(QStringLiteral("SELECT 1 FROM sqlite_master LIMIT 1"))) {
            if (errorOut)
                *errorOut = probe.lastError().text();
            db.close();
            return false;
        }
        if (outDb)
            *outDb = db;
        return true;
    };

    // When the DB file or its directory has no write bits (e.g. 0444/0555) and no
    // active non-empty -wal sidecar exists, open immutable first so even privileged
    // root callers (Docker/CI with CAP_DAC_OVERRIDE) never create -wal/-shm sidecars.
    const QFileInfo dbInfo(dbPath);
    const QFileInfo dirInfo(dbInfo.absolutePath());
    const QFileInfo walInfo(dbPath + QStringLiteral("-wal"));
    const bool hasWalData = walInfo.exists() && walInfo.size() > 0;
    const auto writeBits = QFileDevice::WriteOwner | QFileDevice::WriteGroup | QFileDevice::WriteOther;
    const bool lockedReadOnly = ((dbInfo.permissions() & writeBits) == 0) ||
                                ((dirInfo.permissions() & writeBits) == 0);
    if (lockedReadOnly && !hasWalData) {
        const QString uri =
            QUrl::fromLocalFile(dbInfo.absoluteFilePath()).toString(QUrl::FullyEncoded) + QStringLiteral("?mode=ro&immutable=1");
        if (tryOpen(uri, QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI")))
            return true;
        QSqlDatabase::removeDatabase(connName);
    }

    if (tryOpen(dbPath, QStringLiteral("QSQLITE_OPEN_READONLY")))
        return true;
    QSqlDatabase::removeDatabase(connName);

    // Fallback for WAL-mode SQLite files in non-writable directories (no active -wal sidecar).
    if (dbInfo.isReadable() && !hasWalData) {
        const QString absPath = dbInfo.absoluteFilePath();
        const QString uri =
            QUrl::fromLocalFile(absPath).toString(QUrl::FullyEncoded) + QStringLiteral("?mode=ro&immutable=1");
        if (tryOpen(uri, QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI")))
            return true;
        QSqlDatabase::removeDatabase(connName);
    }
    return false;
}

bool LibraryQueryService::openReadOnlyLibrary(QString *errorOut) {
    if (m_library)
        return true;
    if (m_roLibraryOpen)
        return ensureReadOnlyLibraryLoaded();
    const QString path = m_libraryPath.isEmpty() ? defaultLibraryFile() : m_libraryPath;
    m_roLibraryConn = QStringLiteral("mcp-lib-ro-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    if (!openSqliteReadOnly(path, m_roLibraryConn, &db, errorOut)) {
        m_roLibraryConn.clear();
        m_roLibraryOpen = false;
        return false;
    }
    Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));
    m_libraryPath = path;
    m_roLibraryOpen = true;
    m_roDataVersion = -1;
    m_roSnapshotLoaded = false;
    return ensureReadOnlyLibraryLoaded();
}

bool LibraryQueryService::ensureReadOnlyLibraryLoaded() const {
    if (m_library)
        return true;
    if (!m_roLibraryOpen && !m_libraryPath.isEmpty()) {
        m_roLibraryConn = QStringLiteral("mcp-lib-ro-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSqlDatabase db;
        if (!openSqliteReadOnly(m_libraryPath, m_roLibraryConn, &db)) {
            m_roLibraryConn.clear();
            return false;
        }
        m_roLibraryOpen = true;
        m_roDataVersion = -1;
        m_roSnapshotLoaded = false;
    }
    if (!m_roLibraryOpen)
        return false;
    Q_ASSERT(m_library == nullptr);
    QSqlDatabase db = QSqlDatabase::database(m_roLibraryConn, false);
    if (!db.isOpen())
        return false;
    Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));

    qint64 dataVersion = -1;
    {
        QSqlQuery dv(db);
        if (dv.exec(QStringLiteral("PRAGMA data_version")) && dv.next())
            dataVersion = dv.value(0).toLongLong();
    }
    if (m_roSnapshotLoaded && dataVersion >= 0 && dataVersion == m_roDataVersion)
        return true;

    m_roBooks.clear();
    m_roCollections.clear();
    m_roCollectionNames.clear();
    m_roBookCollections.clear();
    m_chapterTitleCache.clear();

    bool hasWantAt = false;
    bool hasListenedAt = false;
    {
        QSqlQuery cols(db);
        if (cols.exec(QStringLiteral("PRAGMA table_info(books)"))) {
            while (cols.next()) {
                const QString name = cols.value(1).toString();
                if (name == QLatin1String("want_at"))
                    hasWantAt = true;
                else if (name == QLatin1String("listened_at"))
                    hasListenedAt = true;
            }
        }
    }

    const QString sql = QStringLiteral(
                            "SELECT id, path, format, size, mtime, title, sort_title, author, sort_author, editor, "
                            "publisher, year, language, description, subjects, isbn, series, series_index, work_key, "
                            "chapters, pages, length, cover, problem, added_at, opened_at, finished_at, progress, "
                            "position, seconds_read, missing, hidden, loose%1%2 FROM books")
                            .arg(hasWantAt ? QStringLiteral(", want_at") : QStringLiteral(", 0"),
                                 hasListenedAt ? QStringLiteral(", listened_at") : QStringLiteral(", 0"));
    QSqlQuery q(db);
    if (q.exec(sql)) {
        while (q.next()) {
            BookRecord r;
            int i = 0;
            r.id = q.value(i++).toInt();
            r.path = q.value(i++).toString();
            r.format = q.value(i++).toString();
            r.size = q.value(i++).toLongLong();
            r.mtime = q.value(i++).toLongLong();
            r.title = q.value(i++).toString();
            r.sortTitle = q.value(i++).toString();
            r.author = q.value(i++).toString();
            r.sortAuthor = q.value(i++).toString();
            r.editor = q.value(i++).toString();
            r.publisher = q.value(i++).toString();
            r.year = q.value(i++).toInt();
            r.language = q.value(i++).toString();
            r.description = q.value(i++).toString();
            r.subjects = q.value(i++).toString();
            r.isbn = q.value(i++).toString();
            r.series = q.value(i++).toString();
            r.seriesIndex = q.value(i++).toDouble();
            r.workKey = q.value(i++).toString();
            r.chapters = q.value(i++).toInt();
            r.pages = q.value(i++).toInt();
            r.length = q.value(i++).toLongLong();
            r.cover = q.value(i++).toString();
            r.problem = q.value(i++).toString();
            r.addedAt = q.value(i++).toLongLong();
            r.openedAt = q.value(i++).toLongLong();
            r.finishedAt = q.value(i++).toLongLong();
            r.progress = q.value(i++).toDouble();
            r.position = q.value(i++).toString();
            r.secondsRead = q.value(i++).toLongLong();
            r.missing = q.value(i++).toBool();
            r.hidden = q.value(i++).toBool();
            r.loose = q.value(i++).toBool();
            r.wantAt = q.value(i++).toLongLong();
            r.listenedAt = q.value(i++).toLongLong();
            m_roBooks.insert(r.id, std::move(r));
        }
    }

    QSqlQuery hasColTable(db);
    if (hasColTable.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='collections'")) &&
        hasColTable.next()) {
        QSqlQuery cq(db);
        if (cq.exec(QStringLiteral("SELECT id, name FROM collections ORDER BY name COLLATE NOCASE"))) {
            while (cq.next()) {
                const int id = cq.value(0).toInt();
                const QString name = cq.value(1).toString();
                m_roCollections.append({id, name});
                m_roCollectionNames.insert(id, name);
            }
        }
        QSqlQuery lq(db);
        if (lq.exec(QStringLiteral("SELECT book_id, collection_id FROM book_collections"))) {
            while (lq.next()) {
                const int bId = lq.value(0).toInt();
                const int cId = lq.value(1).toInt();
                if (m_roCollectionNames.contains(cId))
                    m_roBookCollections[bId].insert(cId);
            }
        }
    }

    m_roDataVersion = dataVersion;
    m_roSnapshotLoaded = true;
    return true;
}

bool LibraryQueryService::hasLibraryData() const {
    return m_library != nullptr || ensureReadOnlyLibraryLoaded();
}

QString LibraryQueryService::libraryDatabasePath() const {
    if (m_library)
        return m_library->database().databaseName();
    return m_libraryPath;
}

QString LibraryQueryService::resolvedIndexPath() const {
    return m_indexPath.isEmpty() ? defaultIndexFile() : m_indexPath;
}

bool LibraryQueryService::hasPassageVectors() const {
    if (m_library)
        return m_library->database().isOpen() && databaseHasPassageVectorsReadOnly(m_library->database());
    if (!ensureReadOnlyLibraryLoaded())
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_roLibraryConn, false);
    if (!db.isOpen())
        return false;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='passage_vectors'")) ||
        !q.next())
        return false;
    QSqlQuery c(db);
    return c.exec(QStringLiteral("SELECT 1 FROM passage_vectors LIMIT 1")) && c.next();
}

QList<BookRecord> LibraryQueryService::visibleRecords() const {
    if (m_library)
        return m_library->visibleRecords();
    if (!ensureReadOnlyLibraryLoaded())
        return {};
    QList<BookRecord> list;
    for (const BookRecord &r : std::as_const(m_roBooks))
        if (r.visible())
            list.append(r);
    return list;
}

BookRecord LibraryQueryService::bookRecord(int bookId) const {
    if (m_library)
        return m_library->record(bookId);
    if (!ensureReadOnlyLibraryLoaded())
        return {};
    return m_roBooks.value(bookId);
}

QVariantList LibraryQueryService::allCollections() const {
    if (m_library)
        return m_library->collections();
    if (!ensureReadOnlyLibraryLoaded())
        return {};
    QVariantList out;
    out.reserve(m_roCollections.size());
    for (const ReadOnlyCollection &c : std::as_const(m_roCollections))
        out.append(QVariantMap{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}});
    return out;
}

QVariantList LibraryQueryService::collectionsForBook(int bookId) const {
    if (m_library)
        return m_library->collectionsForBook(bookId);
    if (!ensureReadOnlyLibraryLoaded())
        return {};
    QVariantList out;
    const QSet<int> cols = m_roBookCollections.value(bookId);
    for (const ReadOnlyCollection &c : std::as_const(m_roCollections)) {
        if (cols.contains(c.id))
            out.append(QVariantMap{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}});
    }
    return out;
}

bool LibraryQueryService::isBookInCollection(int bookId, int colId) const {
    if (m_library)
        return m_library->isInCollection(bookId, colId);
    if (!ensureReadOnlyLibraryLoaded())
        return false;
    return m_roBookCollections.value(bookId).contains(colId);
}

int LibraryQueryService::resolveCollectionId(const QVariantList &collections, const QString &collectionArg) {
    const QString trimmed = collectionArg.trimmed();
    if (trimmed.isEmpty())
        return 0;
    bool okNum = false;
    const int asId = trimmed.toInt(&okNum);
    for (const QVariant &v : collections) {
        const QVariantMap m = v.toMap();
        const int id = m.value(QStringLiteral("id")).toInt();
        const QString name = m.value(QStringLiteral("name")).toString();
        if (name.compare(trimmed, Qt::CaseInsensitive) == 0)
            return id;
        if (okNum && asId > 0 && id == asId)
            return id;
    }
    return -1;
}

QList<BookRecord> LibraryQueryService::matchBooks(const QString &selector) const {
    const QString wanted = selector.trimmed();
    if (wanted.isEmpty() || !hasLibraryData())
        return {};

    bool okId = false;
    const int numericId = wanted.toInt(&okId);
    if (okId && numericId > 0) {
        const BookRecord rec = bookRecord(numericId);
        if (rec.visible())
            return {rec};
    }

    const QList<BookRecord> books = visibleRecords();
    const QFileInfo fi(wanted);
    if (fi.isFile()) {
        const QString canonical = fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
        for (const BookRecord &book : books) {
            const QFileInfo bookInfo(book.path);
            const QString bookCanonical =
                bookInfo.canonicalFilePath().isEmpty() ? bookInfo.absoluteFilePath() : bookInfo.canonicalFilePath();
            if (bookCanonical == canonical)
                return {book};
        }
    }

    QList<BookRecord> found;
    for (const BookRecord &book : books) {
        if ((book.title + QLatin1Char(' ') + book.author).contains(wanted, Qt::CaseInsensitive))
            found.append(book);
    }
    std::sort(found.begin(), found.end(), [](const BookRecord &a, const BookRecord &b) {
        return a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
    });
    return found;
}

QString LibraryQueryService::chapterTitleFor(const BookRecord &rec, int chapter) const {
    if (rec.id <= 0 || rec.format.compare(QLatin1String("epub"), Qt::CaseInsensitive) != 0)
        return {};
    auto it = m_chapterTitleCache.constFind(rec.id);
    if (it == m_chapterTitleCache.cend() || it->mtime != rec.mtime || it->path != rec.path) {
        CachedChapterTitles entry;
        entry.mtime = rec.mtime;
        entry.path = rec.path;
        EpubBook epub(rec.path);
        if (epub.open() && !epub.isDrmProtected()) {
            for (const EpubTocEntry &tocEntry : epub.toc()) {
                if (tocEntry.spine >= 0 && !tocEntry.title.isEmpty() && !entry.titles.contains(tocEntry.spine))
                    entry.titles.insert(tocEntry.spine, tocEntry.title);
            }
        }
        it = m_chapterTitleCache.insert(rec.id, std::move(entry));
    }
    return it->titles.value(chapter);
}

QString LibraryQueryService::extractPassageText(int bookId, int chapter, qint64 start, qint64 end) const {
    if (end > start) {
        const QString fromIndex = readPassageFromIndexReadOnly(resolvedIndexPath(), bookId, chapter, start, end);
        if (!fromIndex.isEmpty())
            return fromIndex;
        if (m_indexPath.isEmpty() && m_index && m_index->available()) {
            const QString fromInjected = m_index->passage(bookId, chapter, start, end);
            if (!fromInjected.isEmpty())
                return fromInjected;
        }
    }
    if (!hasLibraryData())
        return {};
    const BookRecord rec = bookRecord(bookId);
    if (!rec.visible() || rec.path.isEmpty())
        return {};
    QString full;
    if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(rec.path);
        if (pdf.open())
            full = pdf.pageText(chapter);
    } else {
        EpubBook epub(rec.path);
        if (epub.open() && !epub.isDrmProtected())
            full = epub.chapterText(chapter);
    }
    if (full.isEmpty())
        return {};
    const qsizetype s = std::clamp<qsizetype>(qsizetype(start), 0, full.size());
    const qsizetype len = end > start ? qMin<qsizetype>(qsizetype(end - start), full.size() - s)
                                      : qMin<qsizetype>(1100, full.size() - s);
    return full.mid(s, len).trimmed();
}

QJsonObject LibraryQueryService::listBooks(const QString &query,
                                           const QString &format,
                                           const QString &collection,
                                           int limit) const {
    QJsonArray out;
    int totalMatched = 0;
    if (!hasLibraryData())
        return QJsonObject{{QStringLiteral("books"), out}, {QStringLiteral("count"), 0}, {QStringLiteral("total"), 0}};

    const int colId = collection.trimmed().isEmpty() ? 0 : resolveCollectionId(allCollections(), collection);
    const QString fmt = format.trimmed().toLower();
    const QStringList tokens = WorksModel::searchable(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QList<BookRecord> books = visibleRecords();
    std::sort(books.begin(), books.end(), [](const BookRecord &a, const BookRecord &b) {
        if (a.openedAt != b.openedAt)
            return a.openedAt > b.openedAt;
        if (a.addedAt != b.addedAt)
            return a.addedAt > b.addedAt;
        return a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
    });

    if (colId >= 0) {
        for (const BookRecord &book : std::as_const(books)) {
            if (!fmt.isEmpty() && book.format.compare(fmt, Qt::CaseInsensitive) != 0)
                continue;
            if (colId > 0 && !isBookInCollection(book.id, colId))
                continue;
            if (!tokens.isEmpty()) {
                const QString hay = WorksModel::searchable(
                    book.title + QLatin1Char(' ') + book.author + QLatin1Char(' ') + book.series + QLatin1Char(' ') +
                    book.subjects + QLatin1Char(' ') + book.publisher + QLatin1Char(' ') + book.isbn);
                bool matchesAll = true;
                for (const QString &tok : tokens) {
                    if (!hay.contains(tok)) {
                        matchesAll = false;
                        break;
                    }
                }
                if (!matchesAll)
                    continue;
            }
            ++totalMatched;
            if (out.size() >= limit)
                continue;

            QJsonObject item{
                {QStringLiteral("id"), book.id},
                {QStringLiteral("title"), book.title},
                {QStringLiteral("author"), book.author},
                {QStringLiteral("format"), book.format},
                {QStringLiteral("progress"), book.progress},
                {QStringLiteral("pages"), book.pages},
                {QStringLiteral("chapters"), book.chapters},
                {QStringLiteral("collections"), collectionNamesFromList(collectionsForBook(book.id))},
                {QStringLiteral("filePath"), book.path},
                {QStringLiteral("finished"), book.finishedAt > 0},
            };
            if (!book.series.isEmpty())
                item.insert(QStringLiteral("series"), book.series);
            if (book.year > 0)
                item.insert(QStringLiteral("year"), book.year);
            out.append(item);
        }
    }

    return QJsonObject{
        {QStringLiteral("books"), out},
        {QStringLiteral("count"), int(out.size())},
        {QStringLiteral("total"), totalMatched},
        {QStringLiteral("truncated"), totalMatched > int(out.size())},
    };
}

QJsonObject LibraryQueryService::searchLibrary(const QString &query, int bookId, int limit, QString *errorOut) const {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Missing required parameter 'query'.");
        return {};
    }
    if (bookId > 0 && (!hasLibraryData() || !bookRecord(bookId).visible())) {
        if (errorOut)
            *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
        return {};
    }
    limit = qBound(1, limit, 50);

    const QString idxPath = resolvedIndexPath();
    const QHash<int, QPair<qint64, QString>> indexedMap = readIndexedMapReadOnly(idxPath);
    QVariantList hits = searchFtsReadOnly(idxPath, trimmed, limit, bookId);
    if (hits.isEmpty() && m_indexPath.isEmpty() && m_index && m_index->available())
        hits = m_index->search(trimmed, limit, bookId);
    if (hits.size() < limit && hasLibraryData()) {
        QList<BookRecord> candidates;
        if (bookId > 0)
            candidates.append(bookRecord(bookId));
        else
            candidates = visibleRecords();
        const QVariantList fallbackHits =
            searchUnindexedBooksLexical(candidates, indexedMap, trimmed, limit - int(hits.size()));
        hits.append(fallbackHits);
    }

    QJsonArray results;
    QSet<QPair<int, int>> seenResults;
    for (const QVariant &v : hits) {
        const QVariantMap hit = v.toMap();
        const int hitBookId = hit.value(QStringLiteral("bookId")).toInt();
        const BookRecord rec = hasLibraryData() ? bookRecord(hitBookId) : BookRecord{};
        if (!rec.visible())
            continue;
        const int chapter = hit.value(QStringLiteral("chapter")).toInt();
        if (seenResults.contains({rec.id, chapter}))
            continue;
        seenResults.insert({rec.id, chapter});
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        QJsonObject item{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("format"), rec.format},
            {QStringLiteral("chapter"), chapter},
            {QStringLiteral("offset"), hit.value(QStringLiteral("offset")).toDouble()},
            {QStringLiteral("snippet"), cleanSnippet(hit.value(QStringLiteral("snippet")).toString())},
        };
        if (isPdf) {
            item.insert(QStringLiteral("page"), chapter + 1);
        } else {
            const QString chTitle = chapterTitleFor(rec, chapter);
            if (!chTitle.isEmpty())
                item.insert(QStringLiteral("chapter_title"), chTitle);
        }
        results.append(item);
        if (results.size() >= limit)
            break;
    }

    return QJsonObject{
        {QStringLiteral("query"), trimmed},
        {QStringLiteral("count"), int(results.size())},
        {QStringLiteral("results"), results},
    };
}

QJsonObject LibraryQueryService::searchPassages(const QString &query,
                                                int bookId,
                                                const QString &collection,
                                                int limit,
                                                QString *errorOut,
                                                bool *hadContentOut) const {
    if (hadContentOut)
        *hadContentOut = false;

    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Missing required parameter 'query'.");
        return {};
    }
    if (bookId > 0 && (!hasLibraryData() || !bookRecord(bookId).visible())) {
        if (errorOut)
            *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
        return {};
    }
    limit = qBound(1, limit, 25);

    const QString colTrimmed = collection.trimmed();
    int colId = 0;
    if (!colTrimmed.isEmpty()) {
        if (!hasLibraryData()) {
            if (errorOut)
                *errorOut = QStringLiteral("Library is not available.");
            return {};
        }
        colId = resolveCollectionId(allCollections(), colTrimmed);
        if (colId < 0) {
            return QJsonObject{
                {QStringLiteral("query"), trimmed},
                {QStringLiteral("mode"), QStringLiteral("lexical")},
                {QStringLiteral("count"), 0},
                {QStringLiteral("passages"), QJsonArray{}},
            };
        }
    }

    QSet<int> allowedBooks;
    if (hasLibraryData()) {
        if (bookId > 0) {
            if (colId == 0 || isBookInCollection(bookId, colId))
                allowedBooks.insert(bookId);
        } else {
            for (const BookRecord &rec : visibleRecords()) {
                if (colId > 0 && !isBookInCollection(rec.id, colId))
                    continue;
                allowedBooks.insert(rec.id);
            }
        }
    }

    if (allowedBooks.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("query"), trimmed},
            {QStringLiteral("mode"), QStringLiteral("lexical")},
            {QStringLiteral("count"), 0},
            {QStringLiteral("passages"), QJsonArray{}},
        };
    }

    struct CitedPassage {
        int bookId = 0;
        int chapter = 0;
        qint64 start = 0;
        qint64 end = 0;
        QString text;
        double score = 0.0;
        QString matchKind;
    };

    QList<CitedPassage> chosen;
    QSet<QPair<int, int>> seenChapters;
    bool usedSemantic = false;
    bool usedLexical = false;

    // 1. Try strictly local, read-only semantic search when local loopback embeddings exist.
    //    MCP mode NEVER invokes cloud providers (Gemini) or writes ai_usage / DDL.
    if (hasPassageVectors()) {
        QList<PassageIndex::LibraryHit> semHits;
        const int askCount = qMin(60, limit * 2);
        if (m_localEmbedder && m_localEmbedder->embedder() && m_localEmbedder->isLocal()) {
            const QString dbPath = libraryDatabasePath();
            const QString modelKey = QStringLiteral("local/") + m_localEmbedder->embeddingModelName();
            if (!dbPath.isEmpty() && dbPath != QLatin1String(":memory:")) {
                Ai::Reply *reply = m_localEmbedder->embedder()->embed({trimmed}, true);
                QEventLoop loop;
                QTimer timeout;
                timeout.setSingleShot(true);
                QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
                QObject::connect(reply, &Ai::Reply::finished, &loop, &QEventLoop::quit);
                if (!reply->isFinished()) {
                    timeout.start(10000);
                    loop.exec();
                }
                if (!reply->isFinished())
                    reply->abort();
                if (!reply->error() && !reply->vectors().isEmpty()) {
                    semHits = PassageIndex::rankLibrary(dbPath, modelKey, reply->vectors().first(), askCount,
                                                        PassageIndex::minSemanticScore, allowedBooks);
                }
                reply->deleteLater();
            }
        } else if (m_ai && m_library) {
            Ai::Provider *embProvider = m_ai->embedderProvider();
            if (embProvider && embProvider->id() == QLatin1String("local") && embProvider->isLocal()) {
                Q_ASSERT(embProvider->isLocal());
                if (!m_passageIndex) {
                    m_passageIndex =
                        std::make_unique<PassageIndex>(m_ai, m_library->database().connectionName(), nullptr, true);
                }
                if (m_passageIndex->booksPrepared() > 0) {
                    bool finished = false;
                    QEventLoop loop;
                    QTimer timeout;
                    timeout.setSingleShot(true);
                    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
                    m_passageIndex->searchLibrary(
                        trimmed, askCount,
                        [&](const QList<PassageIndex::LibraryHit> &hits) {
                            semHits = hits;
                            finished = true;
                            loop.quit();
                        },
                        allowedBooks);
                    if (!finished) {
                        timeout.start(10000);
                        loop.exec();
                    }
                    if (!finished)
                        m_passageIndex->cancel();
                }
            }
        }
        for (const PassageIndex::LibraryHit &h : std::as_const(semHits)) {
            if (!allowedBooks.contains(h.bookId))
                continue;
            const QString text = extractPassageText(h.bookId, h.chapter, h.start, h.end);
            if (text.isEmpty())
                continue;
            seenChapters.insert({h.bookId, h.chapter});
            chosen.append({h.bookId, h.chapter, h.start, h.end, text, h.score, QStringLiteral("semantic")});
            usedSemantic = true;
            if (chosen.size() >= limit)
                break;
        }
    }

    // 2. Automatic read-only FTS5 + BM25 lexical fallback (or hybrid completion when semantic hits < limit).
    if (chosen.size() < limit) {
        const QString idxPath = resolvedIndexPath();
        QList<QPair<int, int>> candidates;
        QSet<QPair<int, int>> seenCand;
        auto addCandidate = [&](int bId, int ch) {
            if (!allowedBooks.contains(bId))
                return;
            if (seenChapters.contains({bId, ch}) || seenCand.contains({bId, ch}))
                return;
            seenCand.insert({bId, ch});
            candidates.append({bId, ch});
        };

        if (QFileInfo::exists(idxPath)) {
            const int ftsLimit = qMin(60, qMax(24, limit * 4));
            const QVariantList direct = searchFtsReadOnly(idxPath, trimmed, ftsLimit, bookId);
            for (const QVariant &v : direct) {
                const QVariantMap m = v.toMap();
                addCandidate(m.value(QStringLiteral("bookId")).toInt(), m.value(QStringLiteral("chapter")).toInt());
            }
            if (candidates.size() < limit) {
                const QStringList terms = significantQueryTerms(trimmed);
                if (terms.size() > 1) {
                    const QVariantList sig = searchFtsReadOnly(idxPath, terms.join(QLatin1Char(' ')), ftsLimit, bookId);
                    for (const QVariant &v : sig) {
                        const QVariantMap m = v.toMap();
                        addCandidate(m.value(QStringLiteral("bookId")).toInt(),
                                     m.value(QStringLiteral("chapter")).toInt());
                    }
                }
                if (candidates.size() < limit) {
                    for (const QString &term : terms) {
                        const QVariantList perTerm = searchFtsReadOnly(idxPath, term, qMin(20, ftsLimit), bookId);
                        for (const QVariant &v : perTerm) {
                            const QVariantMap m = v.toMap();
                            addCandidate(m.value(QStringLiteral("bookId")).toInt(),
                                         m.value(QStringLiteral("chapter")).toInt());
                        }
                        if (candidates.size() >= 32)
                            break;
                    }
                }
            }
        }

        struct CandidatePassage {
            int bookId = 0;
            PassageIndex::Passage passage;
        };
        QList<CandidatePassage> pool;
        QList<PassageIndex::Passage> bm25Input;

        auto appendBookChapters = [&](const BookRecord &rec, const QList<int> &chaptersToRead) {
            if (!rec.visible() || rec.path.isEmpty() || !QFileInfo::exists(rec.path))
                return;
            if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
                PdfBook pdf(rec.path);
                if (!pdf.open())
                    return;
                const int total = pdf.pageCount();
                if (chaptersToRead.isEmpty()) {
                    const int cap = qMin(total, 80);
                    for (int p = 0; p < cap; ++p) {
                        const QString text = pdf.pageText(p);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(p, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                } else {
                    for (int p : chaptersToRead) {
                        if (p < 0 || p >= total)
                            continue;
                        const QString text = pdf.pageText(p);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(p, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                }
            } else {
                EpubBook epub(rec.path);
                if (!epub.open() || epub.isDrmProtected())
                    return;
                const int total = int(epub.spine().size());
                if (chaptersToRead.isEmpty()) {
                    const int cap = qMin(total, 80);
                    for (int ch = 0; ch < cap; ++ch) {
                        const QString text = epub.chapterText(ch);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(ch, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                } else {
                    for (int ch : chaptersToRead) {
                        if (ch < 0 || ch >= total)
                            continue;
                        const QString text = epub.chapterText(ch);
                        if (text.trimmed().isEmpty())
                            continue;
                        for (const PassageIndex::Passage &psg : PassageIndex::split(ch, text, 1100)) {
                            pool.append({rec.id, psg});
                            bm25Input.append(psg);
                        }
                    }
                }
            }
        };

        if (!candidates.isEmpty()) {
            QHash<int, QList<int>> byBook;
            const int maxCand = qMin(int(candidates.size()), 24);
            for (int i = 0; i < maxCand; ++i)
                byBook[candidates[i].first].append(candidates[i].second);
            for (auto it = byBook.cbegin(); it != byBook.cend(); ++it)
                appendBookChapters(bookRecord(it.key()), it.value());
        } else if (allowedBooks.size() <= 8) {
            for (int allowedId : std::as_const(allowedBooks))
                appendBookChapters(bookRecord(allowedId), {});
        }

        if (!bm25Input.isEmpty()) {
            QList<PassageIndex::Passage> tagged = bm25Input;
            for (int i = 0; i < tagged.size(); ++i)
                tagged[i].chapter = i;
            const int needed = limit - int(chosen.size());
            const QList<PassageIndex::Hit> bmHits = PassageIndex::byWords(tagged, trimmed, needed * 3);
            for (const PassageIndex::Hit &h : bmHits) {
                const int poolIdx = h.passage.chapter;
                if (poolIdx < 0 || poolIdx >= pool.size())
                    continue;
                const CandidatePassage &cp = pool[poolIdx];
                if (seenChapters.contains({cp.bookId, cp.passage.chapter}) && allowedBooks.size() > 1)
                    continue;
                seenChapters.insert({cp.bookId, cp.passage.chapter});
                chosen.append({cp.bookId, cp.passage.chapter, cp.passage.start, cp.passage.end, cp.passage.text,
                               h.lexical > 0 ? h.lexical : h.score, QStringLiteral("lexical")});
                usedLexical = true;
                if (chosen.size() >= limit)
                    break;
            }
        }
    }

    QJsonArray passagesJson;
    for (const CitedPassage &p : std::as_const(chosen)) {
        const BookRecord rec = bookRecord(p.bookId);
        if (!rec.visible())
            continue;
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        passagesJson.append(QJsonObject{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("format"), rec.format},
            {QStringLiteral("chapter"), p.chapter},
            {QStringLiteral("page"), isPdf ? QJsonValue(p.chapter + 1) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("start"), double(p.start)},
            {QStringLiteral("end"), double(p.end)},
            {QStringLiteral("text"), p.text},
            {QStringLiteral("score"), std::round(p.score * 10000.0) / 10000.0},
            {QStringLiteral("match"), p.matchKind},
        });
    }

    if (hadContentOut)
        *hadContentOut = true;

    const QString mode = (usedSemantic && usedLexical) ? QStringLiteral("hybrid")
                         : usedSemantic                ? QStringLiteral("semantic")
                                                       : QStringLiteral("lexical");

    return QJsonObject{
        {QStringLiteral("query"), trimmed},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("count"), int(passagesJson.size())},
        {QStringLiteral("passages"), passagesJson},
    };
}

QJsonObject LibraryQueryService::bookOutline(int bookId, QString *errorOut) const {
    if (!hasLibraryData() || bookId <= 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
        return {};
    }
    const BookRecord rec = bookRecord(bookId);
    if (!rec.visible()) {
        if (errorOut)
            *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
        return {};
    }
    if (!QFileInfo::exists(rec.path)) {
        if (errorOut)
            *errorOut = QStringLiteral("Book file is missing on disk: %1").arg(rec.path);
        return {};
    }

    QJsonObject out{
        {QStringLiteral("book_id"), rec.id},
        {QStringLiteral("id"), rec.id},
        {QStringLiteral("title"), rec.title},
        {QStringLiteral("author"), rec.author},
        {QStringLiteral("format"), rec.format},
        {QStringLiteral("progress"), rec.progress},
        {QStringLiteral("pages"), rec.pages},
        {QStringLiteral("chapters"), rec.chapters},
        {QStringLiteral("collections"), collectionNamesFromList(collectionsForBook(rec.id))},
        {QStringLiteral("filePath"), rec.path},
    };
    if (!rec.editor.isEmpty())
        out.insert(QStringLiteral("editor"), rec.editor);
    if (!rec.publisher.isEmpty())
        out.insert(QStringLiteral("publisher"), rec.publisher);
    if (rec.year > 0)
        out.insert(QStringLiteral("year"), rec.year);
    if (!rec.language.isEmpty())
        out.insert(QStringLiteral("language"), rec.language);
    if (!rec.series.isEmpty())
        out.insert(QStringLiteral("series"), rec.series);
    if (!rec.description.isEmpty())
        out.insert(QStringLiteral("description"), rec.description);

    QJsonArray toc;
    QJsonArray pageMap;

    if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
        PdfBook pdf(rec.path);
        if (!pdf.open()) {
            if (errorOut)
                *errorOut = pdf.errorString();
            return {};
        }
        const int pageCount = pdf.pageCount();
        out.insert(QStringLiteral("pages"), pageCount);
        const QList<PdfOutlineEntry> outline = pdf.outline();
        out.insert(QStringLiteral("chapters"), int(outline.size()));
        for (const PdfOutlineEntry &entry : outline) {
            toc.append(QJsonObject{
                {QStringLiteral("title"), entry.title},
                {QStringLiteral("depth"), entry.depth},
                {QStringLiteral("page"), entry.page + 1},
            });
        }
        constexpr int kMaxPageMap = 500;
        bool hasCustomLabels = false;
        for (int p = 0; p < qMin(pageCount, kMaxPageMap); ++p) {
            const QString label = pdf.pageLabel(p).trimmed();
            if (!label.isEmpty() && label != QString::number(p + 1)) {
                hasCustomLabels = true;
                break;
            }
        }
        if (hasCustomLabels) {
            for (int p = 0; p < qMin(pageCount, kMaxPageMap); ++p) {
                const QString label = pdf.pageLabel(p).trimmed();
                if (!label.isEmpty())
                    pageMap.append(QJsonObject{{QStringLiteral("page"), p + 1}, {QStringLiteral("label"), label}});
            }
        }
    } else {
        EpubBook epub(rec.path);
        if (!epub.open()) {
            if (errorOut)
                *errorOut = epub.errorString();
            return {};
        }
        if (epub.isDrmProtected()) {
            if (errorOut)
                *errorOut = QStringLiteral("This book is protected by DRM.");
            return {};
        }
        const int spineCount = int(epub.spine().size());
        out.insert(QStringLiteral("chapters"), spineCount);
        if (!epub.pageList().isEmpty())
            out.insert(QStringLiteral("pages"), int(epub.pageList().size()));

        QHash<int, QString> spineTitle;
        for (const EpubTocEntry &entry : epub.toc()) {
            toc.append(QJsonObject{
                {QStringLiteral("title"), entry.title},
                {QStringLiteral("depth"), entry.depth},
                {QStringLiteral("chapter"), entry.spine},
            });
            if (entry.spine >= 0 && !entry.title.isEmpty() && !spineTitle.contains(entry.spine))
                spineTitle.insert(entry.spine, entry.title);
        }

        QJsonArray spineJson;
        for (int i = 0; i < spineCount; ++i) {
            const EpubSpineItem &item = epub.spine()[i];
            QJsonObject entry{
                {QStringLiteral("chapter"), i},
                {QStringLiteral("linear"), item.linear},
            };
            if (spineTitle.contains(i))
                entry.insert(QStringLiteral("title"), spineTitle.value(i));
            spineJson.append(entry);
        }
        out.insert(QStringLiteral("spine"), spineJson);

        constexpr int kMaxPageMap = 2000;
        const auto &pageList = epub.pageList();
        for (int i = 0; i < qMin(int(pageList.size()), kMaxPageMap); ++i) {
            const EpubPageTarget &pt = pageList[i];
            pageMap.append(QJsonObject{
                {QStringLiteral("label"), pt.label},
                {QStringLiteral("chapter"), pt.spine},
            });
        }
    }

    out.insert(QStringLiteral("toc"), toc);
    if (!pageMap.isEmpty())
        out.insert(QStringLiteral("page_map"), pageMap);
    return out;
}

LibraryQueryService::ReadPassageResult LibraryQueryService::readPassage(const ReadPassageRequest &req) const {
    ReadPassageResult res;
    if (!hasLibraryData()) {
        res.error = QStringLiteral("Library is not available.");
        return res;
    }
    const BookRecord rec = bookRecord(req.bookId);
    if (!rec.visible()) {
        res.error = QStringLiteral("Book %1 not found in the library.").arg(req.bookId);
        return res;
    }
    if (!QFileInfo::exists(rec.path)) {
        res.error = QStringLiteral("Book file is missing on disk: %1").arg(rec.path);
        return res;
    }

    const int maxChars = qBound(McpProtocol::kMinReadMaxChars, req.maxChars, McpProtocol::kHardCapReadMaxChars);
    const int offsetArg = qMax(0, req.offset);
    const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;

    QString rawText;
    QString citationLocation;
    QString sectionTitle;
    int resolvedChapter = 0;
    int resolvedPage = 0;
    int resolvedPageEnd = 0;

    if (isPdf) {
        PdfBook pdf(rec.path);
        if (!pdf.open()) {
            res.error = pdf.errorString();
            return res;
        }
        const int totalPages = pdf.pageCount();
        if (totalPages <= 0) {
            res.error = QStringLiteral("This PDF has no pages.");
            return res;
        }

        int startPage = 1;
        if (req.hasPage)
            startPage = req.page;
        else if (req.hasChapter)
            startPage = req.chapter + 1;

        if (startPage < 1 || startPage > totalPages) {
            res.error = QStringLiteral("Page %1 is out of range (valid pages: 1–%2).").arg(startPage).arg(totalPages);
            return res;
        }

        int endPage = req.hasPageEnd ? req.pageEnd : startPage;
        if (endPage < startPage) {
            res.error =
                QStringLiteral("'page_end' (%1) must be >= starting page (%2).").arg(endPage).arg(startPage);
            return res;
        }
        endPage = qMin(endPage, totalPages);
        endPage = qMin(endPage, startPage + 49);

        resolvedChapter = startPage - 1;
        resolvedPage = startPage;
        resolvedPageEnd = endPage;
        sectionTitle = pdfSectionTitle(pdf, startPage - 1);

        QStringList pieces;
        qsizetype accumulated = 0;
        const qsizetype stopCollectingAt = qsizetype(offsetArg) + qsizetype(maxChars) + 256;
        for (int p = startPage; p <= endPage; ++p) {
            const QString pageText = pdf.pageText(p - 1);
            if (endPage > startPage) {
                const QString label = pdf.pageLabel(p - 1).trimmed();
                const QString header = (!label.isEmpty() && label != QString::number(p))
                                           ? QStringLiteral("--- Page %1 (%2) ---\n").arg(QString::number(p), label)
                                           : QStringLiteral("--- Page %1 ---\n").arg(p);
                pieces.append(header + pageText);
                accumulated += header.size() + pageText.size() + 2;
            } else {
                pieces.append(pageText);
                accumulated += pageText.size();
            }
            if (accumulated >= stopCollectingAt) {
                resolvedPageEnd = p;
                break;
            }
        }
        rawText = pieces.join(QStringLiteral("\n\n"));

        const QString printedLabel = pdf.pageLabel(startPage - 1).trimmed();
        if (resolvedPageEnd > startPage) {
            citationLocation = QStringLiteral("pages %1–%2 of %3").arg(startPage).arg(resolvedPageEnd).arg(totalPages);
        } else if (!printedLabel.isEmpty() && printedLabel != QString::number(startPage)) {
            citationLocation = QStringLiteral("page %1 (printed %2) of %3")
                                   .arg(QString::number(startPage), printedLabel, QString::number(totalPages));
        } else {
            citationLocation = QStringLiteral("page %1 of %2").arg(startPage).arg(totalPages);
        }
        if (!sectionTitle.isEmpty())
            citationLocation += QStringLiteral(" · ") + sectionTitle;
    } else {
        EpubBook epub(rec.path);
        if (!epub.open()) {
            res.error = epub.errorString();
            return res;
        }
        if (epub.isDrmProtected()) {
            res.error = QStringLiteral("This book is protected by DRM.");
            return res;
        }
        const int spineCount = int(epub.spine().size());
        if (spineCount <= 0) {
            res.error = QStringLiteral("This EPUB has no readable chapters.");
            return res;
        }

        int spineIdx = 0;
        if (req.hasChapter) {
            spineIdx = req.chapter;
        } else if (req.hasPage) {
            int matchedSpine = -1;
            const QString targetLabel = QString::number(req.page);
            for (const EpubPageTarget &pt : epub.pageList()) {
                if (pt.label.trimmed() == targetLabel && pt.spine >= 0) {
                    matchedSpine = pt.spine;
                    break;
                }
            }
            if (matchedSpine >= 0) {
                spineIdx = matchedSpine;
                resolvedPage = req.page;
            } else if (req.page >= 1 && req.page <= spineCount) {
                spineIdx = req.page - 1;
            } else {
                res.error = QStringLiteral("Page %1 could not be mapped to a chapter in this EPUB (chapters: 0–%2).")
                                .arg(req.page)
                                .arg(spineCount - 1);
                return res;
            }
        }

        if (spineIdx < 0 || spineIdx >= spineCount) {
            res.error = QStringLiteral("Chapter %1 is out of range (valid chapters: 0–%2).")
                            .arg(spineIdx)
                            .arg(spineCount - 1);
            return res;
        }

        resolvedChapter = spineIdx;
        sectionTitle = epubChapterTitle(epub, spineIdx);
        rawText = epub.chapterText(spineIdx);

        citationLocation = QStringLiteral("chapter %1/%2").arg(spineIdx).arg(spineCount - 1);
        if (resolvedPage > 0)
            citationLocation += QStringLiteral(" (print p. %1)").arg(resolvedPage);
        if (!sectionTitle.isEmpty())
            citationLocation += QStringLiteral(" · ") + sectionTitle;
    }

    const qsizetype totalChars = rawText.size();
    const qsizetype clampedOffset = std::clamp<qsizetype>(qsizetype(offsetArg), 0, totalChars);
    const QString slice = rawText.mid(clampedOffset, maxChars);
    const bool truncated = (clampedOffset + slice.size()) < totalChars;

    const QString authorPart = rec.author.isEmpty() ? QString() : QStringLiteral(" — ") + rec.author;
    const QString citation = QStringLiteral("[Citation: %1%2 | book_id=%3 | %4]")
                                 .arg(rec.title, authorPart, QString::number(rec.id), citationLocation);

    res.ok = true;
    res.book = rec;
    res.chapter = resolvedChapter;
    res.chapterTitle = sectionTitle;
    res.page = resolvedPage;
    res.pageEnd = resolvedPageEnd;
    res.offset = clampedOffset;
    res.totalChars = totalChars;
    res.truncated = truncated;
    res.citation = citation;
    res.text = slice;
    return res;
}

QJsonObject LibraryQueryService::listMarksReadOnly(int bookId,
                                                   const QString &query,
                                                   int limit,
                                                   QString *errorOut) const {
    if (!ensureReadOnlyLibraryLoaded()) {
        if (errorOut)
            *errorOut = QStringLiteral("Library is not available.");
        return {};
    }
    QSqlDatabase db = QSqlDatabase::database(m_roLibraryConn, false);
    if (!db.isOpen()) {
        if (errorOut)
            *errorOut = QStringLiteral("Library is not available.");
        return {};
    }
    Q_ASSERT(db.connectOptions().contains(QLatin1String("QSQLITE_OPEN_READONLY")));

    const QStringList tokens = WorksModel::searchable(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (bookId > 0) {
        const BookRecord rec = bookRecord(bookId);
        if (!rec.visible()) {
            if (errorOut)
                *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
            return {};
        }
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        QSqlQuery sql(db);
        sql.prepare(QStringLiteral(
            "SELECT id, kind, chapter, start, finish, text, note, color, label, created_at "
            "FROM annotations WHERE book_id = ? ORDER BY chapter ASC, start ASC, id ASC"));
        sql.addBindValue(bookId);
        QJsonArray arr;
        if (sql.exec()) {
            while (sql.next() && arr.size() < limit) {
                const QString quote = sql.value(5).toString();
                const QString note = sql.value(6).toString();
                const QString label = sql.value(8).toString();
                if (!tokens.isEmpty()) {
                    const QString hay = WorksModel::searchable(quote + QLatin1Char(' ') + note + QLatin1Char(' ') +
                                                               label + QLatin1Char(' ') + rec.title + QLatin1Char(' ') +
                                                               rec.author);
                    bool ok = true;
                    for (const QString &tok : tokens) {
                        if (!hay.contains(tok)) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok)
                        continue;
                }
                const int chapter = sql.value(2).toInt();
                arr.append(QJsonObject{
                    {QStringLiteral("id"), sql.value(0).toInt()},
                    {QStringLiteral("book_id"), rec.id},
                    {QStringLiteral("book_title"), rec.title},
                    {QStringLiteral("book_author"), rec.author},
                    {QStringLiteral("kind"), sql.value(1).toString()},
                    {QStringLiteral("chapter"), chapter},
                    {QStringLiteral("page"), isPdf ? QJsonValue(chapter + 1) : QJsonValue(QJsonValue::Null)},
                    {QStringLiteral("label"), label},
                    {QStringLiteral("quote"), quote},
                    {QStringLiteral("note"), note},
                    {QStringLiteral("color"), sql.value(7).toString()},
                    {QStringLiteral("start"), double(sql.value(3).toLongLong())},
                    {QStringLiteral("end"), double(sql.value(4).toLongLong())},
                    {QStringLiteral("created"),
                     QDateTime::fromSecsSinceEpoch(sql.value(9).toLongLong()).toString(Qt::ISODate)},
                });
            }
        }
        return QJsonObject{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("book_title"), rec.title},
            {QStringLiteral("count"), int(arr.size())},
            {QStringLiteral("marks"), arr},
        };
    }

    QSqlQuery sql(db);
    sql.prepare(QStringLiteral(
        "SELECT a.id, a.book_id, a.kind, a.chapter, a.start, a.finish, a.text, a.note, a.color, a.label, "
        "a.created_at, a.updated_at, b.title, b.author, b.format "
        "FROM annotations a "
        "JOIN books b ON b.id = a.book_id "
        "WHERE b.hidden = 0 AND b.missing = 0 "
        "ORDER BY a.updated_at DESC, a.id DESC"));

    struct ScoredMark {
        int score = 0;
        qint64 updatedAt = 0;
        QJsonObject entry;
    };
    QList<ScoredMark> hits;
    const QString trimmed = query.trimmed();
    if (sql.exec()) {
        while (sql.next()) {
            const int id = sql.value(0).toInt();
            const int bId = sql.value(1).toInt();
            const QString kind = sql.value(2).toString();
            const int chapter = sql.value(3).toInt();
            const qint64 start = sql.value(4).toLongLong();
            const qint64 end = sql.value(5).toLongLong();
            const QString quote = sql.value(6).toString();
            const QString note = sql.value(7).toString();
            const QString color = sql.value(8).toString();
            const QString label = sql.value(9).toString();
            const qint64 createdAt = sql.value(10).toLongLong();
            const qint64 updatedAt = sql.value(11).toLongLong();
            const QString bookTitle = sql.value(12).toString();
            const QString bookAuthor = sql.value(13).toString();
            const bool isPdf = sql.value(14).toString().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;

            int score = 0;
            if (!tokens.isEmpty()) {
                const QString hayNote = note.isEmpty() ? QString() : WorksModel::searchable(note);
                const QString hayText = quote.isEmpty() ? QString() : WorksModel::searchable(quote);
                const QString hayMeta =
                    WorksModel::searchable(label + QLatin1Char(' ') + bookTitle + QLatin1Char(' ') + bookAuthor);
                bool ok = true;
                for (const QString &tok : tokens) {
                    const bool inNote = !hayNote.isEmpty() && hayNote.contains(tok);
                    const bool inText = !hayText.isEmpty() && hayText.contains(tok);
                    const bool inMeta = !hayMeta.isEmpty() && hayMeta.contains(tok);
                    if (!inNote && !inText && !inMeta) {
                        ok = false;
                        break;
                    }
                    if (inNote)
                        score += 30;
                    if (inText)
                        score += 20;
                    if (inMeta)
                        score += 10;
                }
                if (!ok)
                    continue;
                if (!trimmed.isEmpty()) {
                    if (note.contains(trimmed, Qt::CaseInsensitive))
                        score += 40;
                    if (quote.contains(trimmed, Qt::CaseInsensitive))
                        score += 30;
                }
            }
            hits.append({score, updatedAt,
                         QJsonObject{
                             {QStringLiteral("id"), id},
                             {QStringLiteral("book_id"), bId},
                             {QStringLiteral("book_title"), bookTitle},
                             {QStringLiteral("book_author"), bookAuthor},
                             {QStringLiteral("kind"), kind},
                             {QStringLiteral("chapter"), chapter},
                             {QStringLiteral("page"), isPdf ? QJsonValue(chapter + 1) : QJsonValue(QJsonValue::Null)},
                             {QStringLiteral("label"), label},
                             {QStringLiteral("quote"), quote},
                             {QStringLiteral("note"), note},
                             {QStringLiteral("color"), color},
                             {QStringLiteral("start"), double(start)},
                             {QStringLiteral("end"), double(end)},
                             {QStringLiteral("created"), QDateTime::fromSecsSinceEpoch(createdAt).toString(Qt::ISODate)},
                         }});
            if (tokens.isEmpty() && hits.size() >= limit)
                break;
        }
    }
    if (!tokens.isEmpty()) {
        std::sort(hits.begin(), hits.end(), [](const ScoredMark &a, const ScoredMark &b) {
            return a.score != b.score ? a.score > b.score : a.updatedAt > b.updatedAt;
        });
    }
    QJsonArray arr;
    const int n = qMin(limit, int(hits.size()));
    for (int i = 0; i < n; ++i)
        arr.append(hits[i].entry);
    return QJsonObject{
        {QStringLiteral("count"), int(arr.size())},
        {QStringLiteral("marks"), arr},
    };
}

QJsonObject LibraryQueryService::listMarks(int bookId, const QString &query, int limit, QString *errorOut) const {
    if (!m_library)
        return listMarksReadOnly(bookId, query, limit, errorOut);

    if (bookId > 0) {
        const BookRecord rec = m_library->record(bookId);
        if (!rec.visible()) {
            if (errorOut)
                *errorOut = QStringLiteral("Book %1 not found in the library.").arg(bookId);
            return {};
        }

        Marks marks(m_library);
        marks.setBook(bookId);
        const QStringList tokens = WorksModel::searchable(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;

        QJsonArray arr;
        for (int row = 0; row < marks.count() && arr.size() < limit; ++row) {
            const int id = marks.data(marks.index(row), Marks::IdRole).toInt();
            const QVariantMap m = marks.mark(id);
            const QString quote = m.value(QStringLiteral("text")).toString();
            const QString note = m.value(QStringLiteral("note")).toString();
            const QString label = m.value(QStringLiteral("label")).toString();
            if (!tokens.isEmpty()) {
                const QString hay =
                    WorksModel::searchable(quote + QLatin1Char(' ') + note + QLatin1Char(' ') + label + QLatin1Char(' ') +
                                           rec.title + QLatin1Char(' ') + rec.author);
                bool ok = true;
                for (const QString &tok : tokens) {
                    if (!hay.contains(tok)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    continue;
            }
            const int chapter = m.value(QStringLiteral("chapter")).toInt();
            arr.append(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("book_id"), rec.id},
                {QStringLiteral("book_title"), rec.title},
                {QStringLiteral("book_author"), rec.author},
                {QStringLiteral("kind"), m.value(QStringLiteral("kind")).toString()},
                {QStringLiteral("chapter"), chapter},
                {QStringLiteral("page"), isPdf ? QJsonValue(chapter + 1) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("label"), label},
                {QStringLiteral("quote"), quote},
                {QStringLiteral("note"), note},
                {QStringLiteral("color"), m.value(QStringLiteral("color")).toString()},
                {QStringLiteral("start"), double(m.value(QStringLiteral("start")).toLongLong())},
                {QStringLiteral("end"), double(m.value(QStringLiteral("end")).toLongLong())},
                {QStringLiteral("created"), m.value(QStringLiteral("created")).toDateTime().toString(Qt::ISODate)},
            });
        }

        return QJsonObject{
            {QStringLiteral("book_id"), rec.id},
            {QStringLiteral("book_title"), rec.title},
            {QStringLiteral("count"), int(arr.size())},
            {QStringLiteral("marks"), arr},
        };
    }

    Marks marks(m_library);
    const QVariantList found = marks.searchAll(query, limit);
    QJsonArray arr;
    for (const QVariant &v : found) {
        const QVariantMap m = v.toMap();
        const int bId = m.value(QStringLiteral("bookId")).toInt();
        const BookRecord rec = m_library->record(bId);
        const bool isPdf = rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        const int chapter = m.value(QStringLiteral("chapter")).toInt();
        arr.append(QJsonObject{
            {QStringLiteral("id"), m.value(QStringLiteral("id")).toInt()},
            {QStringLiteral("book_id"), bId},
            {QStringLiteral("book_title"), m.value(QStringLiteral("bookTitle")).toString()},
            {QStringLiteral("book_author"), m.value(QStringLiteral("bookAuthor")).toString()},
            {QStringLiteral("kind"), m.value(QStringLiteral("kind")).toString()},
            {QStringLiteral("chapter"), chapter},
            {QStringLiteral("page"), isPdf ? QJsonValue(chapter + 1) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("label"), m.value(QStringLiteral("label")).toString()},
            {QStringLiteral("quote"), m.value(QStringLiteral("text")).toString()},
            {QStringLiteral("note"), m.value(QStringLiteral("note")).toString()},
            {QStringLiteral("color"), m.value(QStringLiteral("color")).toString()},
            {QStringLiteral("start"), double(m.value(QStringLiteral("start")).toLongLong())},
            {QStringLiteral("end"), double(m.value(QStringLiteral("end")).toLongLong())},
            {QStringLiteral("created"), m.value(QStringLiteral("created")).toDateTime().toString(Qt::ISODate)},
        });
    }

    return QJsonObject{
        {QStringLiteral("count"), int(arr.size())},
        {QStringLiteral("marks"), arr},
    };
}

QString LibraryQueryService::formatMarksMarkdown(int bookId,
                                                 const QString &query,
                                                 int limit,
                                                 QString *errorOut,
                                                 int *markCountOut) const {
    QString err;
    const QJsonObject payload = listMarks(bookId, query, limit, &err);
    if (!err.isEmpty()) {
        if (errorOut)
            *errorOut = err;
        if (markCountOut)
            *markCountOut = 0;
        return {};
    }

    const QJsonArray marks = payload.value(QStringLiteral("marks")).toArray();
    if (markCountOut)
        *markCountOut = int(marks.size());
    if (marks.isEmpty())
        return {};

    if (bookId > 0) {
        const BookRecord rec = bookRecord(bookId);
        QList<QJsonObject> list;
        list.reserve(marks.size());
        for (const QJsonValue &v : marks)
            list.append(v.toObject());
        return appendMarksMarkdownSection(rec.title, rec.author, list);
    }

    // Group cross-book marks by book_id while preserving order of first appearance.
    QList<int> bookOrder;
    QHash<int, QList<QJsonObject>> grouped;
    QHash<int, QPair<QString, QString>> meta;
    for (const QJsonValue &v : marks) {
        const QJsonObject obj = v.toObject();
        const int bId = obj.value(QStringLiteral("book_id")).toInt();
        if (!grouped.contains(bId)) {
            bookOrder.append(bId);
            meta.insert(bId, {obj.value(QStringLiteral("book_title")).toString(),
                              obj.value(QStringLiteral("book_author")).toString()});
        }
        grouped[bId].append(obj);
    }

    QStringList sections;
    sections.reserve(bookOrder.size());
    for (int bId : std::as_const(bookOrder)) {
        const auto m = meta.value(bId);
        sections.append(appendMarksMarkdownSection(m.first, m.second, grouped.value(bId)));
    }
    return sections.join(QLatin1Char('\n'));
}
