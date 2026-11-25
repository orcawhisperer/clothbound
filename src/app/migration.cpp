#include "app/migration.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPair>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Migration {

namespace {

bool dirHasEntries(const QString &dirPath) {
    const QDir dir(dirPath);
    if (!dir.exists())
        return false;
    return !dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty();
}

bool hasLegacyVoiceSentinel(const QString &dataDir) {
    const QDir voicesDir(dataDir + QStringLiteral("/voices"));
    if (!voicesDir.exists())
        return false;
    const QStringList subdirs = voicesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &sub : subdirs) {
        if (QFileInfo::exists(voicesDir.filePath(sub + QStringLiteral("/.dogear-installed"))))
            return true;
    }
    return false;
}

QSet<QString> sqliteTableNames(const QString &dbPath) {
    QSet<QString> tables;
    if (!QFileInfo(dbPath).isFile())
        return tables;
    const QString connName = QStringLiteral("migration-probe-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
            if (q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"))) {
                while (q.next())
                    tables.insert(q.value(0).toString());
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return tables;
}

void fsyncPath(const QString &path) {
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

bool copySymlinkSafely(const QString &srcPath, const QString &dstPath, const QString &rootSrcClean,
                       const QString &rootDstClean) {
    const QByteArray nativeSrc = QFile::encodeName(srcPath);
    QByteArray buf(4096, Qt::Uninitialized);
    ssize_t len = ::readlink(nativeSrc.constData(), buf.data(), size_t(buf.size()));
    while (len == ssize_t(buf.size()) && buf.size() < 1024 * 1024) {
        buf.resize(buf.size() * 2);
        len = ::readlink(nativeSrc.constData(), buf.data(), size_t(buf.size()));
    }
    if (len < 0 || len >= ssize_t(buf.size()))
        return false;
    buf.truncate(qsizetype(len));
    QString target = QFile::decodeName(buf);
    // Preserve relative symlinks verbatim; if an absolute symlink points inside
    // the legacy tree being migrated, rewrite its prefix to the destination tree
    // so it will not dangle once the legacy tree is removed.
    if (target.startsWith(QLatin1Char('/')) && !rootSrcClean.isEmpty() && !rootDstClean.isEmpty()) {
        const QString cleanTarget = QDir::cleanPath(target);
        if (cleanTarget == rootSrcClean)
            target = rootDstClean;
        else if (cleanTarget.startsWith(rootSrcClean + QLatin1Char('/')))
            target = rootDstClean + cleanTarget.mid(rootSrcClean.size());
    }
    const QByteArray nativeTarget = QFile::encodeName(target);
    const QByteArray nativeDst = QFile::encodeName(dstPath);
    return ::symlink(nativeTarget.constData(), nativeDst.constData()) == 0;
}

bool copyFileSync(const QString &srcPath, const QString &dstPath) {
    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly))
        return false;
    QFile dst(dstPath);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    char buf[65536];
    for (;;) {
        const qint64 n = src.read(buf, sizeof(buf));
        if (n < 0)
            return false;
        if (n == 0)
            break;
        if (dst.write(buf, n) != n)
            return false;
    }
    if (!dst.flush())
        return false;
    if (dst.handle() >= 0)
        ::fsync(dst.handle());
    dst.setPermissions(src.permissions());
    dst.close();
    return true;
}

// Checkpoints WAL and creates a consistent, self-contained SQLite copy at
// `dstPath` via VACUUM INTO (or checkpoint + sync copy fallback).
// Returns true if the file was handled as a valid SQLite database (meaning
// transient -wal/-shm sidecars should be skipped).
bool copySqliteCleanly(const QString &srcPath, const QString &dstPath) {
    QFile::remove(dstPath);
    const QString connName = QStringLiteral("migration-sql-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool vacuumOk = false;
    bool checkpointOk = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(srcPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
            // Verify this is actually a readable SQLite database before writing.
            if (q.exec(QStringLiteral("SELECT 1 FROM sqlite_master LIMIT 1"))) {
                checkpointOk = q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
                QSqlQuery vac(db);
                vac.prepare(QStringLiteral("VACUUM INTO ?"));
                vac.addBindValue(dstPath);
                vacuumOk = vac.exec();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);

    if (vacuumOk) {
        QFile::setPermissions(dstPath, QFile::permissions(srcPath));
        fsyncPath(dstPath);
        return true;
    }
    QFile::remove(dstPath);
    if (checkpointOk)
        return copyFileSync(srcPath, dstPath);
    return false;
}

bool copyDirectoryTree(const QString &srcDir, const QString &dstDir, const QString &rootSrcClean,
                       const QString &rootDstClean) {
    if (!QDir().mkpath(dstDir))
        return false;
    const QDir source(srcDir);
    // Omit QDir::System so FIFOs, sockets, and device nodes are never enumerated
    // or opened (opening a FIFO would block indefinitely).
    const QFileInfoList entries = source.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden);

    // First pass: cleanly back up any SQLite databases so we know which
    // transient -wal/-shm sidecars can be skipped without tearing.
    QSet<QString> handledSqliteBases;
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (!name.endsWith(QLatin1String(".sqlite"), Qt::CaseInsensitive))
            continue;
        const QString srcChild = entry.filePath();
        struct stat st {};
        if (::lstat(QFile::encodeName(srcChild).constData(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        const QString dstChild = dstDir + QLatin1Char('/') + name;
        if (copySqliteCleanly(srcChild, dstChild))
            handledSqliteBases.insert(name);
    }

    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (handledSqliteBases.contains(name))
            continue;
        // Skip transient SQLite WAL/SHM sidecars for databases we already
        // checkpointed and copied cleanly above.
        if (name.endsWith(QLatin1String("-wal"), Qt::CaseInsensitive) ||
            name.endsWith(QLatin1String("-shm"), Qt::CaseInsensitive)) {
            const QString base = name.left(name.size() - 4);
            if (handledSqliteBases.contains(base))
                continue;
        }

        const QString srcChild = entry.filePath();
        const QString dstChild = dstDir + QLatin1Char('/') + name;
        struct stat st {};
        if (::lstat(QFile::encodeName(srcChild).constData(), &st) != 0)
            continue;
        if (S_ISLNK(st.st_mode)) {
            if (!copySymlinkSafely(srcChild, dstChild, rootSrcClean, rootDstClean))
                return false;
        } else if (S_ISDIR(st.st_mode)) {
            if (!copyDirectoryTree(srcChild, dstChild, rootSrcClean, rootDstClean))
                return false;
        } else if (S_ISREG(st.st_mode)) {
            if (!copyFileSync(srcChild, dstChild))
                return false;
        }
        // Any other special file (FIFO, socket, block/char device) is skipped.
    }
    return true;
}

// Moves `legacyDir` to `newDir` atomically when on the same filesystem, or
// via copy+fsync into a staging sibling followed by atomic rename otherwise.
bool moveDirectorySafely(const QString &legacyDir, const QString &newDir) {
    if (legacyDir.isEmpty() || newDir.isEmpty() || legacyDir == newDir)
        return false;
    const QFileInfo legacyInfo(legacyDir);
    if (!legacyInfo.exists() || !legacyInfo.isDir())
        return false;
    if (dirHasEntries(newDir))
        return false;

    const QFileInfo newInfo(newDir);
    const QString parentDir = newInfo.absolutePath();
    if (!QDir().mkpath(parentDir))
        return false;

    // Remove an empty destination directory so QDir::rename can place legacyDir directly.
    if (newInfo.exists() && newInfo.isDir() && !QDir().rmdir(newDir))
        return false;

    if (QDir().rename(legacyDir, newDir))
        return true;

    // Cross-filesystem fallback: copy into a staging sibling, sync, rename into place, then clean up source.
    const QString staging = newDir + QStringLiteral(".migrating-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
    QDir(staging).removeRecursively();
    if (!copyDirectorySafely(legacyDir, staging, newDir)) {
        QDir(staging).removeRecursively();
        return false;
    }
    if (!QDir().rename(staging, newDir)) {
        QDir(staging).removeRecursively();
        return false;
    }
    QDir(legacyDir).removeRecursively();
    return true;
}

int migrateVoiceSentinels(const QString &dataDir) {
    const QDir voicesDir(dataDir + QStringLiteral("/voices"));
    if (!voicesDir.exists())
        return 0;
    int renamed = 0;
    const QStringList subdirs = voicesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &sub : subdirs) {
        const QString legacyMarker = voicesDir.filePath(sub + QStringLiteral("/.dogear-installed"));
        const QString newMarker = voicesDir.filePath(sub + QStringLiteral("/.clothbound-installed"));
        if (!QFileInfo::exists(legacyMarker))
            continue;
        if (QFileInfo::exists(newMarker)) {
            QFile::remove(legacyMarker);
            continue;
        }
        if (QFile::rename(legacyMarker, newMarker))
            ++renamed;
    }
    return renamed;
}

QString rewritePrefix(const QString &value, const QList<QPair<QString, QString>> &prefixes) {
    for (const auto &[oldPrefix, newPrefix] : prefixes) {
        if (oldPrefix.isEmpty() || newPrefix.isEmpty() || oldPrefix == newPrefix)
            continue;
        if (value == oldPrefix)
            return newPrefix;
        if (value.startsWith(oldPrefix + QLatin1Char('/')))
            return newPrefix + value.mid(oldPrefix.size());
    }
    return value;
}

bool rewriteLibraryDbPaths(const QString &dbPath, const QList<QPair<QString, QString>> &prefixes) {
    if (!QFileInfo(dbPath).isFile() || prefixes.isEmpty())
        return true;
    const QString connName = QStringLiteral("migration-rewrite-lib-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
            QSet<QString> tables;
            {
                QSqlQuery tq(db);
                if (tq.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"))) {
                    while (tq.next())
                        tables.insert(tq.value(0).toString());
                }
            }
            if (db.transaction()) {
                ok = true;
                auto rewriteTable = [&](const QString &tableName) -> bool {
                    if (!tables.contains(tableName))
                        return true;
                    QList<QPair<qint64, QString>> updates;
                    {
                        QSqlQuery sel(db);
                        if (!sel.exec(QStringLiteral("SELECT id, path FROM %1").arg(tableName)))
                            return false;
                        while (sel.next()) {
                            const qint64 id = sel.value(0).toLongLong();
                            const QString path = sel.value(1).toString();
                            const QString rewritten = rewritePrefix(path, prefixes);
                            if (rewritten != path)
                                updates.append({id, rewritten});
                        }
                    }
                    if (updates.isEmpty())
                        return true;
                    QSqlQuery upd(db);
                    upd.prepare(QStringLiteral("UPDATE %1 SET path = ? WHERE id = ?").arg(tableName));
                    for (const auto &[id, newPath] : std::as_const(updates)) {
                        upd.addBindValue(newPath);
                        upd.addBindValue(id);
                        if (!upd.exec())
                            return false;
                    }
                    return true;
                };
                if (!rewriteTable(QStringLiteral("books")) || !rewriteTable(QStringLiteral("folders")))
                    ok = false;
                if (ok)
                    ok = db.commit();
                else
                    db.rollback();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return ok;
}

bool rewriteIndexDbPaths(const QString &dbPath, const QList<QPair<QString, QString>> &prefixes) {
    if (!QFileInfo(dbPath).isFile() || prefixes.isEmpty())
        return true;
    const QString connName = QStringLiteral("migration-rewrite-idx-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA busy_timeout = 4000"));
            bool hasIndexed = false;
            {
                QSqlQuery tq(db);
                if (tq.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='indexed'")) &&
                    tq.next())
                    hasIndexed = true;
            }
            if (!hasIndexed) {
                ok = true;
            } else if (db.transaction()) {
                ok = true;
                QList<QPair<qint64, QString>> updates;
                {
                    QSqlQuery sel(db);
                    if (!sel.exec(QStringLiteral("SELECT book, path FROM indexed"))) {
                        ok = false;
                    } else {
                        while (sel.next()) {
                            const qint64 book = sel.value(0).toLongLong();
                            const QString path = sel.value(1).toString();
                            const QString rewritten = rewritePrefix(path, prefixes);
                            if (rewritten != path)
                                updates.append({book, rewritten});
                        }
                    }
                }
                if (ok && !updates.isEmpty()) {
                    QSqlQuery upd(db);
                    upd.prepare(QStringLiteral("UPDATE indexed SET path = ? WHERE book = ?"));
                    for (const auto &[book, newPath] : std::as_const(updates)) {
                        upd.addBindValue(newPath);
                        upd.addBindValue(book);
                        if (!upd.exec()) {
                            ok = false;
                            break;
                        }
                    }
                }
                if (ok)
                    ok = db.commit();
                else
                    db.rollback();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return ok;
}

QString migrationLockPath(const Paths &paths) {
    const QByteArray key =
        (QDir::cleanPath(paths.newConfigDir) + QLatin1Char('|') + QDir::cleanPath(paths.newDataDir) +
         QLatin1Char('|') + QDir::cleanPath(paths.newCacheDir))
            .toUtf8();
    const QByteArray digest = QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16);
    return QDir::tempPath() + QStringLiteral("/clothbound-migration-") + QString::fromLatin1(digest) +
           QStringLiteral(".lock");
}

}  // namespace

bool copyDirectorySafely(const QString &srcDir, const QString &dstDir, const QString &finalDstDir) {
    if (srcDir.isEmpty() || dstDir.isEmpty())
        return false;
    const QString rootSrcClean = QDir::cleanPath(QFileInfo(srcDir).absoluteFilePath());
    const QString rootDstClean =
        QDir::cleanPath(QFileInfo(finalDstDir.isEmpty() ? dstDir : finalDstDir).absoluteFilePath());
    return copyDirectoryTree(srcDir, dstDir, rootSrcClean, rootDstClean);
}

Paths defaultPaths() {
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return {
        cfg.isEmpty() ? QString() : cfg + QStringLiteral("/dogear"),
        cfg.isEmpty() ? QString() : cfg + QStringLiteral("/clothbound"),
        data.isEmpty() ? QString() : data + QStringLiteral("/dogear"),
        data.isEmpty() ? QString() : data + QStringLiteral("/clothbound"),
        cache.isEmpty() ? QString() : cache + QStringLiteral("/dogear"),
        cache.isEmpty() ? QString() : cache + QStringLiteral("/clothbound"),
    };
}

bool isLegacyConfigDir(const QString &configDir) {
    if (configDir.isEmpty())
        return false;
    const QString iniPath = configDir + QStringLiteral("/settings.ini");
    if (!QFileInfo(iniPath).isFile())
        return false;
    const QSettings settings(iniPath, QSettings::IniFormat);
    const QStringList groups = settings.childGroups();
    static const QSet<QString> knownGroups{
        QStringLiteral("reading"),    QStringLiteral("onboarding"), QStringLiteral("ai"),
        QStringLiteral("appearance"), QStringLiteral("typography"), QStringLiteral("listen"),
        QStringLiteral("library"),    QStringLiteral("companion"),  QStringLiteral("narration"),
        QStringLiteral("local"),
    };
    for (const QString &group : groups) {
        if (knownGroups.contains(group))
            return true;
    }
    return false;
}

bool isLegacyDataDir(const QString &dataDir) {
    if (dataDir.isEmpty())
        return false;
    const QString dbPath = dataDir + QStringLiteral("/library.sqlite");
    if (QFileInfo(dbPath).isFile()) {
        const QSet<QString> tables = sqliteTableNames(dbPath);
        const bool ourCoreSchema =
            tables.contains(QStringLiteral("books")) && tables.contains(QStringLiteral("annotations")) &&
            (tables.contains(QStringLiteral("chapter_lengths")) || tables.contains(QStringLiteral("reading_days")) ||
             tables.contains(QStringLiteral("book_settings")) || tables.contains(QStringLiteral("ai_usage")) ||
             tables.contains(QStringLiteral("companion_threads")) || tables.contains(QStringLiteral("audio_clips")));
        const bool ourAltSchema =
            tables.contains(QStringLiteral("marks")) &&
            (tables.contains(QStringLiteral("ai_usage")) || tables.contains(QStringLiteral("companion_messages")) ||
             tables.contains(QStringLiteral("companion_threads")));
        return ourCoreSchema || ourAltSchema;
    }
    return hasLegacyVoiceSentinel(dataDir);
}

bool isLegacyCacheDir(const QString &cacheDir) {
    if (cacheDir.isEmpty())
        return false;
    const QString indexPath = cacheDir + QStringLiteral("/index.sqlite");
    if (QFileInfo(indexPath).isFile()) {
        const QSet<QString> tables = sqliteTableNames(indexPath);
        if (tables.contains(QStringLiteral("chapter_text")) && tables.contains(QStringLiteral("indexed")))
            return true;
    }
    return false;
}

Result run(const Paths &paths) {
    Result result;

    // Serialize concurrent GUI/CLI/MCP startups so only one process migrates.
    QLockFile lock(migrationLockPath(paths));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(10000))
        return result;

    const bool configRecognized = isLegacyConfigDir(paths.legacyConfigDir);
    const bool dataRecognized = isLegacyDataDir(paths.legacyDataDir);

    // Record legacy data dir paths before moving so we can rewrite both logical
    // and canonicalized absolute prefixes stored inside SQLite.
    const QString legacyDataClean =
        paths.legacyDataDir.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(paths.legacyDataDir).absoluteFilePath());
    const QString legacyDataCanon =
        paths.legacyDataDir.isEmpty() ? QString() : QFileInfo(paths.legacyDataDir).canonicalFilePath();

    if (configRecognized && !QFileInfo::exists(paths.newConfigDir + QStringLiteral("/settings.ini")) &&
        !dirHasEntries(paths.newConfigDir)) {
        result.configMigrated = moveDirectorySafely(paths.legacyConfigDir, paths.newConfigDir);
    }

    if (dataRecognized && !QFileInfo::exists(paths.newDataDir + QStringLiteral("/library.sqlite")) &&
        !dirHasEntries(paths.newDataDir)) {
        result.dataMigrated = moveDirectorySafely(paths.legacyDataDir, paths.newDataDir);
    }

    QList<QPair<QString, QString>> dataPrefixes;
    if (!paths.legacyDataDir.isEmpty() && !paths.newDataDir.isEmpty()) {
        const QString newDataClean = QDir::cleanPath(QFileInfo(paths.newDataDir).absoluteFilePath());
        const QString newDataCanon = QFileInfo(paths.newDataDir).canonicalFilePath();
        if (!legacyDataClean.isEmpty() && !newDataClean.isEmpty() && legacyDataClean != newDataClean)
            dataPrefixes.append({legacyDataClean, newDataClean});
        if (!legacyDataCanon.isEmpty()) {
            const QString canonSrc = QDir::cleanPath(legacyDataCanon);
            const QString canonDst = !newDataCanon.isEmpty() ? QDir::cleanPath(newDataCanon) : newDataClean;
            if (!canonSrc.isEmpty() && !canonDst.isEmpty() && canonSrc != canonDst &&
                !dataPrefixes.contains({canonSrc, canonDst}))
                dataPrefixes.append({canonSrc, canonDst});
        }
    }

    // Rewrite stored absolute paths inside library.sqlite (saved web articles
    // under .../articles/*.epub, OPDS books under .../downloads/..., and any
    // watched folders under the legacy data dir) so they are never orphaned.
    if (result.dataMigrated && !paths.newDataDir.isEmpty() && !dataPrefixes.isEmpty())
        rewriteLibraryDbPaths(paths.newDataDir + QStringLiteral("/library.sqlite"), dataPrefixes);

    // Rename any legacy voice-pack sentinels inside the Clothbound data folder
    // (both right after migration and if any were copied manually).
    if (!paths.newDataDir.isEmpty())
        result.voiceSentinelsRenamed = migrateVoiceSentinels(paths.newDataDir);

    // Migrate cache only when either the cache's own FTS index matches our
    // schema, or we verified legacy config/data belong to our reader and the
    // legacy cache holds our expected cache buckets.
    const bool hasKnownCacheEntries =
        QFileInfo::exists(paths.legacyCacheDir + QStringLiteral("/covers")) ||
        QFileInfo::exists(paths.legacyCacheDir + QStringLiteral("/audio")) ||
        QFileInfo::exists(paths.legacyCacheDir + QStringLiteral("/index.sqlite")) ||
        QFileInfo::exists(paths.legacyCacheDir + QStringLiteral("/voices.json"));
    const bool cacheRecognized =
        isLegacyCacheDir(paths.legacyCacheDir) || ((configRecognized || dataRecognized) && hasKnownCacheEntries);

    if (cacheRecognized && !dirHasEntries(paths.newCacheDir))
        result.cacheMigrated = moveDirectorySafely(paths.legacyCacheDir, paths.newCacheDir);

    // Keep FTS index paths aligned with migrated data-dir paths so indexed
    // articles/OPDS books are not needlessly re-indexed or dropped.
    if ((result.dataMigrated || result.cacheMigrated) && !paths.newCacheDir.isEmpty() && !dataPrefixes.isEmpty())
        rewriteIndexDbPaths(paths.newCacheDir + QStringLiteral("/index.sqlite"), dataPrefixes);

    return result;
}

Result runIfNeeded() { return run(defaultPaths()); }

}  // namespace Migration
