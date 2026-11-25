#include "ai/companionstore.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>

CompanionStore::CompanionStore(const QString &databaseConnection)
    : m_connection(databaseConnection), m_ownerThread(QThread::currentThread()) {}

QVariantList CompanionStore::citationsToVariant(const QList<Citation> &citations) {
    QVariantList out;
    out.reserve(citations.size());
    for (const Citation &cite : citations) {
        QVariantMap map{{QStringLiteral("chapter"), cite.chapter},
                        {QStringLiteral("offset"), cite.offset},
                        {QStringLiteral("label"), cite.label},
                        {QStringLiteral("excerpt"), cite.excerpt}};
        if (cite.bookId > 0)
            map.insert(QStringLiteral("bookId"), cite.bookId);
        if (!cite.bookTitle.isEmpty())
            map.insert(QStringLiteral("bookTitle"), cite.bookTitle);
        out.append(map);
    }
    return out;
}

QJsonArray CompanionStore::citationsToJson(const QList<Citation> &citations) {
    QJsonArray out;
    for (const Citation &cite : citations) {
        QJsonObject obj{{QStringLiteral("chapter"), cite.chapter},
                        {QStringLiteral("offset"), cite.offset},
                        {QStringLiteral("label"), cite.label},
                        {QStringLiteral("excerpt"), cite.excerpt}};
        if (cite.bookId > 0)
            obj.insert(QStringLiteral("bookId"), cite.bookId);
        if (!cite.bookTitle.isEmpty())
            obj.insert(QStringLiteral("bookTitle"), cite.bookTitle);
        out.append(obj);
    }
    return out;
}

QList<CompanionStore::Citation> CompanionStore::citationsFromJson(const QByteArray &json) {
    QList<Citation> out;
    for (const QJsonValue &val : QJsonDocument::fromJson(json).array()) {
        const QJsonObject obj = val.toObject();
        Citation cite;
        cite.bookId = obj.value(QStringLiteral("bookId")).toInt();
        cite.bookTitle = obj.value(QStringLiteral("bookTitle")).toString();
        cite.chapter = obj.value(QStringLiteral("chapter")).toInt();
        cite.offset = obj.value(QStringLiteral("offset")).toInteger();
        cite.label = obj.value(QStringLiteral("label")).toString();
        cite.excerpt = obj.value(QStringLiteral("excerpt")).toString();
        if (!cite.label.isEmpty() || !cite.excerpt.isEmpty())
            out.append(cite);
    }
    return out;
}

void CompanionStore::ensureTables() {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_threads ("
        " id INTEGER PRIMARY KEY,"
        " book_id INTEGER NOT NULL,"
        " seq INTEGER NOT NULL,"
        " from_user INTEGER NOT NULL,"
        " text TEXT NOT NULL,"
        " sources TEXT NOT NULL DEFAULT '[]',"
        " provenance TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT 0,"
        " snip_url TEXT NOT NULL DEFAULT '',"
        " chapter INTEGER NOT NULL DEFAULT -1,"
        " offset INTEGER NOT NULL DEFAULT -1,"
        " created_at INTEGER NOT NULL)"));
    bool hasProvenance = false;
    bool hasIsLocal = false;
    bool hasSnipUrl = false;
    bool hasChapter = false;
    bool hasOffset = false;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(companion_threads)"))) {
        while (info.next()) {
            const QString col = info.value(1).toString();
            if (col == QLatin1String("provenance"))
                hasProvenance = true;
            else if (col == QLatin1String("is_local"))
                hasIsLocal = true;
            else if (col == QLatin1String("snip_url"))
                hasSnipUrl = true;
            else if (col == QLatin1String("chapter"))
                hasChapter = true;
            else if (col == QLatin1String("offset"))
                hasOffset = true;
        }
    }
    if (!hasProvenance)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN provenance TEXT NOT NULL DEFAULT ''"));
    if (!hasIsLocal)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN is_local INTEGER NOT NULL DEFAULT 0"));
    if (!hasSnipUrl)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN snip_url TEXT NOT NULL DEFAULT ''"));
    if (!hasChapter)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN chapter INTEGER NOT NULL DEFAULT -1"));
    if (!hasOffset)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN offset INTEGER NOT NULL DEFAULT -1"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS companion_threads_book ON companion_threads (book_id, seq)"));

    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_whoswho ("
        " book_id INTEGER NOT NULL,"
        " mode TEXT NOT NULL DEFAULT 'fiction',"
        " chapter INTEGER NOT NULL,"
        " offset INTEGER NOT NULL,"
        " page_start INTEGER NOT NULL DEFAULT 0,"
        " where_label TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT -1,"
        " people TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " PRIMARY KEY (book_id, mode, chapter, offset))"));
    bool whoswhoHasLocal = false;
    bool whoswhoHasMode = false;
    QSqlQuery wInfo(db);
    if (wInfo.exec(QStringLiteral("PRAGMA table_info(companion_whoswho)"))) {
        while (wInfo.next()) {
            const QString col = wInfo.value(1).toString();
            if (col == QLatin1String("is_local"))
                whoswhoHasLocal = true;
            else if (col == QLatin1String("mode"))
                whoswhoHasMode = true;
        }
    }
    if (!whoswhoHasLocal)
        query.exec(QStringLiteral("ALTER TABLE companion_whoswho ADD COLUMN is_local INTEGER NOT NULL DEFAULT -1"));
    if (!whoswhoHasMode) {
        const bool inTx = db.transaction();
        const bool ok =
            query.exec(QStringLiteral(
                "CREATE TABLE companion_whoswho_v2 ("
                " book_id INTEGER NOT NULL,"
                " mode TEXT NOT NULL DEFAULT 'fiction',"
                " chapter INTEGER NOT NULL,"
                " offset INTEGER NOT NULL,"
                " page_start INTEGER NOT NULL DEFAULT 0,"
                " where_label TEXT NOT NULL DEFAULT '',"
                " is_local INTEGER NOT NULL DEFAULT -1,"
                " people TEXT NOT NULL,"
                " updated_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, mode, chapter, offset))")) &&
            query.exec(QStringLiteral(
                "INSERT INTO companion_whoswho_v2 "
                "(book_id, mode, chapter, offset, page_start, where_label, is_local, people, updated_at) "
                "SELECT book_id, 'fiction', chapter, offset, page_start, where_label, is_local, people, updated_at "
                "FROM companion_whoswho")) &&
            query.exec(QStringLiteral("DROP TABLE companion_whoswho")) &&
            query.exec(QStringLiteral("ALTER TABLE companion_whoswho_v2 RENAME TO companion_whoswho"));
        if (inTx)
            ok ? db.commit() : db.rollback();
    }
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS companion_whoswho_book ON companion_whoswho (book_id, mode, chapter DESC, offset DESC)"));

    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_recaps ("
        " book_id INTEGER NOT NULL,"
        " mode TEXT NOT NULL DEFAULT 'fiction',"
        " chapter INTEGER NOT NULL,"
        " offset INTEGER NOT NULL,"
        " page_start INTEGER NOT NULL DEFAULT 0,"
        " where_label TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT 0,"
        " text TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " PRIMARY KEY (book_id, mode, chapter, offset))"));
    bool recapsHasMode = false;
    QSqlQuery rInfo(db);
    if (rInfo.exec(QStringLiteral("PRAGMA table_info(companion_recaps)"))) {
        while (rInfo.next())
            if (rInfo.value(1).toString() == QLatin1String("mode"))
                recapsHasMode = true;
    }
    if (!recapsHasMode) {
        const bool inTx = db.transaction();
        const bool ok =
            query.exec(QStringLiteral(
                "CREATE TABLE companion_recaps_v2 ("
                " book_id INTEGER NOT NULL,"
                " mode TEXT NOT NULL DEFAULT 'fiction',"
                " chapter INTEGER NOT NULL,"
                " offset INTEGER NOT NULL,"
                " page_start INTEGER NOT NULL DEFAULT 0,"
                " where_label TEXT NOT NULL DEFAULT '',"
                " is_local INTEGER NOT NULL DEFAULT 0,"
                " text TEXT NOT NULL,"
                " updated_at INTEGER NOT NULL,"
                " PRIMARY KEY (book_id, mode, chapter, offset))")) &&
            query.exec(QStringLiteral(
                "INSERT INTO companion_recaps_v2 "
                "(book_id, mode, chapter, offset, page_start, where_label, is_local, text, updated_at) "
                "SELECT book_id, 'fiction', chapter, offset, page_start, where_label, is_local, text, updated_at "
                "FROM companion_recaps")) &&
            query.exec(QStringLiteral("DROP TABLE companion_recaps")) &&
            query.exec(QStringLiteral("ALTER TABLE companion_recaps_v2 RENAME TO companion_recaps"));
        if (inTx)
            ok ? db.commit() : db.rollback();
    }
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS companion_recaps_book ON companion_recaps (book_id, mode, chapter DESC, offset DESC)"));

    // Clean up companion data automatically when a book is deleted from `books`.
    QSqlQuery hasBooks(db);
    if (hasBooks.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='books'")) && hasBooks.next()) {
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS companion_cleanup_book AFTER DELETE ON books BEGIN "
            "DELETE FROM companion_threads WHERE book_id = OLD.id; "
            "DELETE FROM companion_whoswho WHERE book_id = OLD.id; "
            "END"));
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS companion_cleanup_recaps AFTER DELETE ON books BEGIN "
            "DELETE FROM companion_recaps WHERE book_id = OLD.id; "
            "END"));
    }
}

void CompanionStore::ensurePictureTable() {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS illustrations (id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, "
        "chapter INTEGER NOT NULL, offset INTEGER NOT NULL, caption TEXT NOT NULL, prompt TEXT NOT NULL, "
        "file TEXT NOT NULL, created INTEGER NOT NULL, style TEXT NOT NULL DEFAULT 'auto', "
        "passage TEXT NOT NULL DEFAULT '')"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS illustrations_book ON illustrations (book_id)"));
    bool hasStyle = false, hasPassage = false;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(illustrations)"))) {
        while (info.next()) {
            const QString col = info.value(1).toString();
            if (col == QLatin1String("style"))
                hasStyle = true;
            else if (col == QLatin1String("passage"))
                hasPassage = true;
        }
    }
    if (!hasStyle)
        query.exec(QStringLiteral("ALTER TABLE illustrations ADD COLUMN style TEXT NOT NULL DEFAULT 'auto'"));
    if (!hasPassage)
        query.exec(QStringLiteral("ALTER TABLE illustrations ADD COLUMN passage TEXT NOT NULL DEFAULT ''"));
}

QString CompanionStore::loadReadingMode(int bookId) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0)
        return {};
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return {};
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM book_settings WHERE book_id = ? AND key = 'companion_mode'"));
    q.addBindValue(bookId);
    if (!q.exec() || !q.next())
        return {};
    const QString val = q.value(0).toString().trimmed().toLower();
    if (val == QLatin1String("study") || val == QLatin1String("fiction"))
        return val;
    return {};
}

void CompanionStore::saveReadingMode(int bookId, const QString &mode) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO book_settings (book_id, key, value) VALUES (?, 'companion_mode', ?)"));
    q.addBindValue(bookId);
    q.addBindValue(mode);
    q.exec();
}

QList<CompanionStore::ShelfBook> CompanionStore::shelfBooksFor(int bookId, QString *labelOut) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    QList<ShelfBook> out;
    if (labelOut)
        labelOut->clear();
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return out;

    // 1. Prefer the current book's collection(s) when it belongs to one.
    if (bookId > 0) {
        QSqlQuery colQ(db);
        colQ.prepare(QStringLiteral(
            "SELECT c.id, c.name FROM collections c "
            "JOIN book_collections bc ON bc.collection_id = c.id "
            "WHERE bc.book_id = ? ORDER BY c.name COLLATE NOCASE"));
        colQ.addBindValue(bookId);
        QList<int> colIds;
        QStringList colNames;
        if (colQ.exec()) {
            while (colQ.next()) {
                colIds.append(colQ.value(0).toInt());
                colNames.append(colQ.value(1).toString());
            }
        }
        if (!colIds.isEmpty()) {
            QStringList placeholders;
            for (int i = 0; i < colIds.size(); ++i)
                placeholders.append(QStringLiteral("?"));
            QSqlQuery booksQ(db);
            booksQ.prepare(QStringLiteral(
                               "SELECT DISTINCT b.id, b.title, b.format, b.path "
                               "FROM books b "
                               "JOIN book_collections bc ON bc.book_id = b.id "
                               "WHERE bc.collection_id IN (%1) AND b.missing = 0 AND b.hidden = 0 "
                               "ORDER BY b.opened_at DESC, b.id ASC")
                               .arg(placeholders.join(QLatin1Char(','))));
            for (int cid : colIds)
                booksQ.addBindValue(cid);
            if (booksQ.exec()) {
                while (booksQ.next())
                    out.append({booksQ.value(0).toInt(), booksQ.value(1).toString(), booksQ.value(2).toString(),
                                booksQ.value(3).toString()});
            }
            if (!out.isEmpty()) {
                if (labelOut)
                    *labelOut = colNames.first();
                return out;
            }
        }
    }

    // 2. Otherwise, check sibling books in the same folder.
    QList<ShelfBook> allVisible;
    QString currentFolder;
    {
        QSqlQuery allQ(db);
        if (allQ.exec(QStringLiteral(
                "SELECT id, title, format, path FROM books "
                "WHERE missing = 0 AND hidden = 0 ORDER BY opened_at DESC, id ASC"))) {
            while (allQ.next()) {
                ShelfBook b{allQ.value(0).toInt(), allQ.value(1).toString(), allQ.value(2).toString(),
                            allQ.value(3).toString()};
                if (b.id == bookId && !b.path.isEmpty())
                    currentFolder = QFileInfo(b.path).absolutePath();
                allVisible.append(std::move(b));
            }
        }
    }
    if (!currentFolder.isEmpty()) {
        QList<ShelfBook> sameFolder;
        for (const ShelfBook &b : std::as_const(allVisible)) {
            if (!b.path.isEmpty() && QFileInfo(b.path).absolutePath() == currentFolder)
                sameFolder.append(b);
        }
        if (sameFolder.size() >= 2) {
            if (labelOut)
                *labelOut = QFileInfo(currentFolder).fileName();
            return sameFolder;
        }
    }

    // 3. Fallback: whole library.
    return allVisible;
}

QList<CompanionStore::Turn> CompanionStore::loadConversation(int bookId) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    QList<Turn> out;
    if (bookId <= 0)
        return out;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return out;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT from_user, text, sources, provenance, is_local, snip_url, chapter, offset FROM companion_threads "
        "WHERE book_id = ? ORDER BY seq ASC, id ASC"));
    query.addBindValue(bookId);
    if (!query.exec())
        return out;
    while (query.next()) {
        Turn turn;
        turn.fromUser = query.value(0).toInt() != 0;
        turn.text = query.value(1).toString();
        turn.sources = citationsFromJson(query.value(2).toString().toUtf8());
        turn.provenance = query.value(3).toString();
        turn.local = query.value(4).toInt() != 0;
        const QString savedSnip = query.value(5).toString();
        if (turn.fromUser && savedSnip.startsWith(QLatin1String("data:image/png;base64,")))
            turn.snipDataUrl = savedSnip;
        turn.chapter = query.value(6).toInt();
        turn.offset = query.value(7).toLongLong();
        out.append(std::move(turn));
    }
    return out;
}

bool CompanionStore::saveConversation(int bookId, const QList<Turn> &turns) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    const bool inTx = db.transaction();
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
    del.addBindValue(bookId);
    bool ok = del.exec();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < turns.size() && ok; ++i) {
        const Turn &turn = turns[i];
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO companion_threads "
            "(book_id, seq, from_user, text, sources, provenance, is_local, snip_url, chapter, offset, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        ins.addBindValue(bookId);
        ins.addBindValue(i);
        ins.addBindValue(turn.fromUser ? 1 : 0);
        ins.addBindValue(turn.text.isNull() ? QStringLiteral("") : turn.text);
        ins.addBindValue(QString::fromUtf8(QJsonDocument(citationsToJson(turn.sources)).toJson(QJsonDocument::Compact)));
        ins.addBindValue(turn.provenance.isNull() ? QStringLiteral("") : turn.provenance);
        ins.addBindValue(turn.local ? 1 : 0);
        ins.addBindValue(turn.fromUser && !turn.snipDataUrl.isEmpty() ? turn.snipDataUrl : QStringLiteral(""));
        ins.addBindValue(turn.chapter);
        ins.addBindValue(turn.offset);
        ins.addBindValue(now);
        ok = ins.exec();
    }
    if (inTx)
        ok = ok ? db.commit() : (db.rollback(), false);
    return ok;
}

void CompanionStore::clearConversation(int bookId) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
    del.addBindValue(bookId);
    del.exec();
}

bool CompanionStore::loadWhosWhoSnapshot(int bookId, const QString &mode, bool wholeStudy, int chapter, qint64 pageEnd,
                                         qint64 pageStart, bool requireSameChapter, WhosWhoSnapshot *out) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0 || !out)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    if (wholeStudy) {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, people FROM companion_whoswho "
            "WHERE book_id = ? AND mode = ? AND chapter = -1 ORDER BY updated_at DESC LIMIT 1"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    } else {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, people FROM companion_whoswho "
            "WHERE book_id = ? AND mode = ? AND chapter >= 0 ORDER BY chapter DESC, offset DESC"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    }
    if (!query.exec())
        return false;
    while (query.next()) {
        const int savedChapter = query.value(0).toInt();
        const qint64 savedOffset = query.value(1).toLongLong();
        const qint64 savedStart = query.value(2).toLongLong();
        if (!wholeStudy) {
            // Strictly spoiler-safe: never restore a checkpoint from ahead of the reader.
            if (savedChapter > chapter)
                continue;
            if (requireSameChapter && savedChapter != chapter)
                continue;
            if (savedChapter == chapter) {
                if (pageEnd > 0) {
                    if (savedOffset > pageEnd)
                        continue;
                } else if (savedStart > pageStart) {
                    continue;
                }
            }
        }
        const QJsonArray arr = QJsonDocument::fromJson(query.value(5).toString().toUtf8()).array();
        QVariantList loaded;
        loaded.reserve(arr.size());
        for (const QJsonValue &val : arr)
            if (val.isObject())
                loaded.append(val.toObject().toVariantMap());
        out->people = std::move(loaded);
        out->chapter = savedChapter;
        out->offset = savedOffset;
        out->whereLabel = query.value(3).toString();
        const int savedLocal = query.value(4).toInt();
        out->isLocal = savedLocal >= 0
                           ? (savedLocal != 0)
                           : out->whereLabel.contains(QLatin1String("this computer"), Qt::CaseInsensitive);
        return true;
    }
    return false;
}

bool CompanionStore::saveWhosWhoSnapshot(int bookId, const QString &mode, int chapter, qint64 offset, qint64 pageStart,
                                         const QString &whereLabel, bool isLocal, const QVariantList &people) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    QJsonArray arr;
    for (const QVariant &v : people)
        arr.append(QJsonObject::fromVariantMap(v.toMap()));
    const QString json = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO companion_whoswho "
        "(book_id, mode, chapter, offset, page_start, where_label, is_local, people, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(bookId);
    ins.addBindValue(mode);
    ins.addBindValue(chapter);
    ins.addBindValue(offset);
    ins.addBindValue(pageStart);
    ins.addBindValue(whereLabel);
    ins.addBindValue(isLocal ? 1 : 0);
    ins.addBindValue(json);
    ins.addBindValue(now);
    if (!ins.exec())
        return false;
    // Keep the 12 most recent checkpoints per (book, mode).
    QSqlQuery prune(db);
    prune.prepare(QStringLiteral(
        "DELETE FROM companion_whoswho WHERE book_id = ? AND mode = ? AND rowid NOT IN ("
        "SELECT rowid FROM companion_whoswho WHERE book_id = ? AND mode = ? ORDER BY updated_at DESC LIMIT 12)"));
    prune.addBindValue(bookId);
    prune.addBindValue(mode);
    prune.addBindValue(bookId);
    prune.addBindValue(mode);
    prune.exec();
    return true;
}

bool CompanionStore::loadRecapSnapshot(int bookId, const QString &mode, bool wholeStudy, int chapter, qint64 pageEnd,
                                       qint64 pageStart, bool requireSameChapter, RecapSnapshot *out) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0 || !out)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    if (wholeStudy) {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, text FROM companion_recaps "
            "WHERE book_id = ? AND mode = ? AND chapter = -1 ORDER BY updated_at DESC LIMIT 1"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    } else {
        query.prepare(QStringLiteral(
            "SELECT chapter, offset, page_start, where_label, is_local, text FROM companion_recaps "
            "WHERE book_id = ? AND mode = ? AND chapter >= 0 ORDER BY chapter DESC, offset DESC"));
        query.addBindValue(bookId);
        query.addBindValue(mode);
    }
    if (!query.exec())
        return false;
    while (query.next()) {
        const int savedChapter = query.value(0).toInt();
        const qint64 savedOffset = query.value(1).toLongLong();
        const qint64 savedStart = query.value(2).toLongLong();
        if (!wholeStudy) {
            // Strictly spoiler-safe: never restore a recap from ahead of the reader.
            if (savedChapter > chapter)
                continue;
            if (requireSameChapter && savedChapter != chapter)
                continue;
            if (savedChapter == chapter) {
                if (pageEnd > 0) {
                    if (savedOffset > pageEnd)
                        continue;
                } else if (savedStart > pageStart) {
                    continue;
                }
            }
        }
        const QString savedText = query.value(5).toString();
        if (savedText.trimmed().isEmpty())
            continue;
        out->text = savedText;
        out->chapter = savedChapter;
        out->offset = savedOffset;
        out->whereLabel = query.value(3).toString();
        out->isLocal = query.value(4).toInt() != 0;
        return true;
    }
    return false;
}

bool CompanionStore::saveRecapSnapshot(int bookId, const QString &mode, int chapter, qint64 offset, qint64 pageStart,
                                       const QString &whereLabel, bool isLocal, const QString &text) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0 || text.trimmed().isEmpty())
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO companion_recaps "
        "(book_id, mode, chapter, offset, page_start, where_label, is_local, text, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(bookId);
    ins.addBindValue(mode);
    ins.addBindValue(chapter);
    ins.addBindValue(offset);
    ins.addBindValue(pageStart);
    ins.addBindValue(whereLabel);
    ins.addBindValue(isLocal ? 1 : 0);
    ins.addBindValue(text);
    ins.addBindValue(now);
    if (!ins.exec())
        return false;
    QSqlQuery prune(db);
    prune.prepare(QStringLiteral(
        "DELETE FROM companion_recaps WHERE book_id = ? AND mode = ? AND rowid NOT IN ("
        "SELECT rowid FROM companion_recaps WHERE book_id = ? AND mode = ? ORDER BY updated_at DESC LIMIT 12)"));
    prune.addBindValue(bookId);
    prune.addBindValue(mode);
    prune.addBindValue(bookId);
    prune.addBindValue(mode);
    prune.exec();
    return true;
}

void CompanionStore::clearBookHistory(int bookId) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery q1(db);
    q1.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
    q1.addBindValue(bookId);
    q1.exec();
    QSqlQuery q2(db);
    q2.prepare(QStringLiteral("DELETE FROM companion_whoswho WHERE book_id = ?"));
    q2.addBindValue(bookId);
    q2.exec();
    QSqlQuery q3(db);
    q3.prepare(QStringLiteral("DELETE FROM companion_recaps WHERE book_id = ?"));
    q3.addBindValue(bookId);
    q3.exec();
}

QList<CompanionStore::PictureRecord> CompanionStore::loadPictures(int bookId) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    QList<PictureRecord> out;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return out;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, book_id, chapter, offset, caption, prompt, file, created, style, passage "
        "FROM illustrations WHERE book_id = ? ORDER BY created DESC, id DESC"));
    query.addBindValue(bookId);
    if (!query.exec())
        return out;
    while (query.next()) {
        PictureRecord rec;
        rec.id = query.value(0).toInt();
        rec.bookId = query.value(1).toInt();
        rec.chapter = query.value(2).toInt();
        rec.offset = query.value(3).toLongLong();
        rec.caption = query.value(4).toString();
        rec.prompt = query.value(5).toString();
        rec.file = query.value(6).toString();
        rec.created = query.value(7).toLongLong();
        rec.style = query.value(8).toString();
        rec.passage = query.value(9).toString();
        out.append(std::move(rec));
    }
    return out;
}

int CompanionStore::insertPicture(const PictureRecord &record) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return 0;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO illustrations (book_id, chapter, offset, caption, prompt, file, created, style, passage) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(record.bookId);
    query.addBindValue(record.chapter);
    query.addBindValue(record.offset);
    query.addBindValue(record.caption);
    query.addBindValue(record.prompt);
    query.addBindValue(record.file);
    query.addBindValue(record.created > 0 ? record.created : QDateTime::currentSecsSinceEpoch());
    query.addBindValue(record.style);
    query.addBindValue(record.passage);
    return query.exec() ? query.lastInsertId().toInt() : 0;
}

bool CompanionStore::findPicture(int id, int bookId, PictureRecord *out) const {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (id <= 0 || !out)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    if (bookId > 0) {
        query.prepare(QStringLiteral(
            "SELECT id, book_id, chapter, offset, caption, prompt, file, created, style, passage "
            "FROM illustrations WHERE id = ? AND book_id = ?"));
        query.addBindValue(id);
        query.addBindValue(bookId);
    } else {
        query.prepare(QStringLiteral(
            "SELECT id, book_id, chapter, offset, caption, prompt, file, created, style, passage "
            "FROM illustrations WHERE id = ?"));
        query.addBindValue(id);
    }
    if (!query.exec() || !query.next())
        return false;
    out->id = query.value(0).toInt();
    out->bookId = query.value(1).toInt();
    out->chapter = query.value(2).toInt();
    out->offset = query.value(3).toLongLong();
    out->caption = query.value(4).toString();
    out->prompt = query.value(5).toString();
    out->file = query.value(6).toString();
    out->created = query.value(7).toLongLong();
    out->style = query.value(8).toString();
    out->passage = query.value(9).toString();
    return true;
}

QString CompanionStore::deletePicture(int id) {
    Q_ASSERT(QThread::currentThread() == m_ownerThread);
    if (id <= 0)
        return {};
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return {};
    QString file;
    QSqlQuery find(db);
    find.prepare(QStringLiteral("SELECT file FROM illustrations WHERE id = ?"));
    find.addBindValue(id);
    if (find.exec() && find.next())
        file = find.value(0).toString();
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM illustrations WHERE id = ?"));
    del.addBindValue(id);
    del.exec();
    return file;
}

bool CompanionStore::isTurnVisible(const Turn &turn, bool filterSpoiler, int readerChapter, qint64 readerPageEnd) {
    if (!filterSpoiler || turn.chapter < 0)
        return true;
    if (turn.chapter > readerChapter)
        return false;
    if (turn.chapter == readerChapter && readerPageEnd > 0 && turn.offset > 0 && turn.offset > readerPageEnd)
        return false;
    return true;
}

QList<CompanionStore::Turn> CompanionStore::filterVisibleTurns(const QList<Turn> &turns, bool filterSpoiler,
                                                               int readerChapter, qint64 readerPageEnd) {
    if (!filterSpoiler)
        return turns;
    QList<Turn> kept;
    kept.reserve(turns.size());
    for (const Turn &turn : turns)
        if (isTurnVisible(turn, true, readerChapter, readerPageEnd))
            kept.append(turn);
    return kept;
}

QVariantList CompanionStore::conversationToVariant(const QList<Turn> &turns, bool filterSpoiler, int readerChapter,
                                                   qint64 readerPageEnd) {
    QVariantList out;
    out.reserve(turns.size());
    for (const Turn &turn : turns) {
        if (!isTurnVisible(turn, filterSpoiler, readerChapter, readerPageEnd))
            continue;
        QVariantMap entry{{QStringLiteral("fromUser"), turn.fromUser},
                          {QStringLiteral("text"), turn.text},
                          {QStringLiteral("sources"), citationsToVariant(turn.sources)},
                          {QStringLiteral("provenance"), turn.provenance},
                          {QStringLiteral("local"), turn.local}};
        if (turn.fromUser && !turn.snipDataUrl.isEmpty())
            entry.insert(QStringLiteral("snipUrl"), turn.snipDataUrl);
        out.append(entry);
    }
    return out;
}

QList<Ai::Message> CompanionStore::buildHistoryMessages(const QList<Turn> &turns, qint64 budget, bool filterSpoiler,
                                                        int readerChapter, qint64 readerPageEnd) {
    QList<Ai::Message> selected;
    for (qsizetype i = turns.size() - 1; i >= 0; --i) {
        const Turn &msg = turns[i];
        if (!isTurnVisible(msg, filterSpoiler, readerChapter, readerPageEnd))
            continue;
        if (!selected.isEmpty() && budget - msg.text.size() < 0)
            break;
        budget -= msg.text.size();
        selected.prepend({msg.fromUser, msg.text});
    }
    if (!selected.isEmpty() && !selected.first().fromUser)
        selected.removeFirst();
    QList<Ai::Message> normalized;
    for (const Ai::Message &msg : selected) {
        if (!normalized.isEmpty() && normalized.last().fromUser == msg.fromUser)
            normalized.last().text += QStringLiteral("\n\n") + msg.text;
        else
            normalized.append(msg);
    }
    return normalized;
}
