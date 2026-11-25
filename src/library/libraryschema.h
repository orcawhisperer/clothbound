#pragma once

#include "formats/bookrecord.h"

#include <QHash>
#include <QList>
#include <QMultiHash>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

class QSqlQuery;

// Nested-safe SQLite transaction guard using SAVEPOINT, rolling back
// automatically on early return or error and tracking per-thread nesting depth.
class LibrarySavepoint {
public:
    LibrarySavepoint(QSqlDatabase database, const char *label);
    ~LibrarySavepoint();

    LibrarySavepoint(const LibrarySavepoint &) = delete;
    LibrarySavepoint &operator=(const LibrarySavepoint &) = delete;

    bool isActive() const { return m_active; }
    bool commit();
    void rollback();

    // Current active savepoint nesting depth on this thread (for invariant checks/tests).
    static int activeDepth();

private:
    QSqlDatabase m_db;
    QString m_name;
    bool m_active = false;
};

struct LibraryCollectionRow {
    int id = 0;
    QString name;
    qint64 createdAt = 0;
};

struct LibrarySnapshot {
    QStringList folders;
    QHash<int, BookRecord> books;
    QHash<QString, int> pathToId;
    QHash<int, QString> fingerprints;
    QMultiHash<QString, int> fingerprintToIds;
    QHash<int, LibraryCollectionRow> collections;
    QHash<int, QSet<int>> collectionBooks;
    QHash<int, QSet<int>> bookCollections;
    QSet<int> downloadedBooks;
    QSet<int> audiobookBooks;
    QSet<int> articleSettingIds;
    QHash<int, QString> sourceUrls;
    QHash<int, QString> voiceNames;
    QHash<int, int> highlightCounts;
};

// SQLite schema management, migrations (v1..v4), connection pragmas, and
// row-level persistence helpers for library.sqlite.
namespace LibrarySchema {
// Raise with every new migration in migrate(), or existing libraries skip it.
inline constexpr int kLatestVersion = 4;

// Configures SQLite connection pragmas. When `readOnly` is true, skips WAL and
// any write-capable pragmas and enables PRAGMA query_only=ON.
bool configureConnection(QSqlDatabase &db, bool readOnly, QString *errorOut = nullptr);

// Opens and initializes (or read-only validates) a named QSQLITE connection.
bool openConnection(const QString &connectionName, const QString &dbPath, bool readOnly,
                    QString *errorOut = nullptr);
void closeConnection(const QString &connectionName);

int schemaVersion(const QSqlDatabase &db);
bool hasTable(const QSqlDatabase &db, const QString &table, bool *out, QString *errorOut = nullptr);
bool hasColumn(const QSqlDatabase &db, const QString &table, const QString &column, bool *out,
               QString *errorOut = nullptr);

// Validates a read-only database without running any DDL or WAL writes.
// Fails closed if PRAGMA user_version > kLatestVersion or the core `books` table is absent.
bool validateReadOnly(QSqlDatabase &db, QString *errorOut = nullptr);

// Applies schema creation and incremental migrations v1..v4 inside a savepoint.
// Fails closed if PRAGMA user_version > kLatestVersion.
bool migrate(QSqlDatabase &db, QString *errorOut = nullptr);

const QString &bookColumns();
BookRecord readBookRecord(const QSqlQuery &query);

void loadAll(const QSqlDatabase &db, LibrarySnapshot &out);
void loadFolders(const QSqlDatabase &db, QStringList &folders);
void loadBooks(const QSqlDatabase &db, QHash<int, BookRecord> &books, QHash<QString, int> &pathToId,
               QHash<int, QString> &fingerprints, QMultiHash<QString, int> &fingerprintToIds);
void loadCollections(const QSqlDatabase &db, QHash<int, LibraryCollectionRow> &collections,
                     QHash<int, QSet<int>> &collectionBooks, QHash<int, QSet<int>> &bookCollections);
void loadAudioBadges(const QSqlDatabase &db, QSet<int> &downloadedBooks, QSet<int> &audiobookBooks);
void loadOnlineMeta(const QSqlDatabase &db, QSet<int> &articleSettingIds, QHash<int, QString> &sourceUrls,
                    QHash<int, QString> &voiceNames);
void loadHighlightCounts(const QSqlDatabase &db, QHash<int, int> &highlightCounts);

QList<int> readChapterLengths(const QSqlDatabase &db, int bookId);
bool writeChapterLengths(QSqlDatabase &db, int bookId, const QList<int> &lengths, qint64 *totalOut = nullptr,
                         QString *errorOut = nullptr);

bool upsertBook(QSqlDatabase &db, BookRecord &record, int existingId, bool relinked, const QString &fingerprint,
                QString *errorOut = nullptr);

QString readBookSetting(const QSqlDatabase &db, int bookId, const QString &key);
bool writeBookSetting(QSqlDatabase &db, int bookId, const QString &key, const QString &value,
                      QString *errorOut = nullptr);

bool insertFolder(QSqlDatabase &db, const QString &folder, qint64 addedAt, QString *errorOut = nullptr);
bool removeFolderAndMarkMissing(QSqlDatabase &db, const QString &folder, const QList<int> &idsToMarkMissing,
                                QString *errorOut = nullptr);
bool updateMissingFlags(QSqlDatabase &db, const QList<QPair<int, bool>> &updates, QString *errorOut = nullptr);
bool setBookIntColumn(QSqlDatabase &db, int bookId, const char *column, qint64 value, QString *errorOut = nullptr);
bool updateBookPosition(QSqlDatabase &db, int bookId, const QString &position, double progress,
                        QString *errorOut = nullptr);

int insertCollection(QSqlDatabase &db, const QString &name, qint64 createdAt, QString *errorOut = nullptr);
bool renameCollection(QSqlDatabase &db, int id, const QString &name, QString *errorOut = nullptr);
bool deleteCollection(QSqlDatabase &db, int id, QString *errorOut = nullptr);
bool addBookToCollection(QSqlDatabase &db, int bookId, int collectionId, qint64 addedAt,
                         QString *errorOut = nullptr);
bool removeBookFromCollection(QSqlDatabase &db, int bookId, int collectionId, QString *errorOut = nullptr);
}  // namespace LibrarySchema
