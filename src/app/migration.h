#pragma once

#include <QString>

namespace Migration {

struct Paths {
    QString legacyConfigDir;
    QString newConfigDir;
    QString legacyDataDir;
    QString newDataDir;
    QString legacyCacheDir;
    QString newCacheDir;
};

struct Result {
    bool configMigrated = false;
    bool dataMigrated = false;
    bool cacheMigrated = false;
    int voiceSentinelsRenamed = 0;
};

// Default XDG legacy ("dogear") and Clothbound ("clothbound") directories.
Paths defaultPaths();

// Fingerprint guards verifying a legacy "dogear" directory actually belongs to
// our reader (and not an unrelated tool such as ninepointlabs/dogear).
bool isLegacyConfigDir(const QString &configDir);
bool isLegacyDataDir(const QString &dataDir);
bool isLegacyCacheDir(const QString &cacheDir);

// One-time non-destructive migration from legacy XDG directories into
// Clothbound's directories. Safe to call on every startup; never overwrites
// existing Clothbound data.
Result runIfNeeded();
Result run(const Paths &paths);

}  // namespace Migration
