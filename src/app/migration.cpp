#include "app/migration.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <unistd.h>

namespace Migration {

namespace {

bool dirHasEntries(const QString &dirPath) {
    const QDir dir(dirPath);
    if (!dir.exists())
        return false;
    return !dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System).isEmpty();
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

bool copyFileSync(const QString &srcPath, const QString &dstPath) {
    const QFileInfo srcInfo(srcPath);
    if (srcInfo.isSymLink())
        return QFile::link(srcInfo.symLinkTarget(), dstPath);
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

bool copyDirectoryRecursively(const QString &srcDir, const QString &dstDir) {
    if (!QDir().mkpath(dstDir))
        return false;
    const QDir source(srcDir);
    const QFileInfoList entries =
        source.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        const QString srcChild = entry.filePath();
        const QString dstChild = dstDir + QLatin1Char('/') + entry.fileName();
        if (entry.isDir() && !entry.isSymLink()) {
            if (!copyDirectoryRecursively(srcChild, dstChild))
                return false;
        } else {
            if (!copyFileSync(srcChild, dstChild))
                return false;
        }
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
    if (!copyDirectoryRecursively(legacyDir, staging)) {
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

}  // namespace

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

    const bool configRecognized = isLegacyConfigDir(paths.legacyConfigDir);
    const bool dataRecognized = isLegacyDataDir(paths.legacyDataDir);

    if (configRecognized && !QFileInfo::exists(paths.newConfigDir + QStringLiteral("/settings.ini")) &&
        !dirHasEntries(paths.newConfigDir)) {
        result.configMigrated = moveDirectorySafely(paths.legacyConfigDir, paths.newConfigDir);
    }

    if (dataRecognized && !QFileInfo::exists(paths.newDataDir + QStringLiteral("/library.sqlite")) &&
        !dirHasEntries(paths.newDataDir)) {
        result.dataMigrated = moveDirectorySafely(paths.legacyDataDir, paths.newDataDir);
    }

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

    return result;
}

Result runIfNeeded() { return run(defaultPaths()); }

}  // namespace Migration
