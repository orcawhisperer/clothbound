#include "audio/voicepacks.h"

#include "audio/sherpaonnx.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSet>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <memory>
#include <sys/stat.h>

namespace {
const QString installedMarker = QStringLiteral(".clothbound-installed");
// Legacy sentinel accepted as a read fallback so pre-rebrand voice packs remain usable.
const QString legacyInstalledMarker = QStringLiteral(".dogear-installed");
constexpr int downloadTimeoutMs = 60000;

int gradeRank(const QString &grade) {
    static const QStringList order{"F+", "D-", "D", "D+", "C-", "C", "C+", "B-", "B", "B+", "A-", "A"};
    const int at = int(order.indexOf(grade));
    return at < 0 ? 5 : at;  // ungraded counts as C
}

int qualityRank(const QString &quality) {
    static const QStringList order{"x_low", "low", "medium", "high"};
    return int(qMax<qsizetype>(0, order.indexOf(quality)));
}

QString baseOf(const QString &language) { return language.section('-', 0, 0).section('_', 0, 0).toLower(); }

QString regionOf(const QString &language) {
    const QString norm = QString(language).replace('_', '-');
    return norm.section('-', 1, 1).toUpper();
}

qint64 sizeOf(const QString &directory) {
    qint64 total = 0;
    QDirIterator it(directory, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        total += it.nextFileInfo().size();
    return total;
}

QString curatedVoiceNote(const QString &voiceId) {
    const QString key = voiceId.section(QLatin1Char(':'), -1).toLower();
    static const QHash<QString, QString> notes{
        {QStringLiteral("bf_emma"), QStringLiteral("warm, measured")},
        {QStringLiteral("bm_george"), QStringLiteral("low, unhurried")},
        {QStringLiteral("bm_lewis"), QStringLiteral("bright")},
        {QStringLiteral("bm_fable"), QStringLiteral("storyteller")},
        {QStringLiteral("bf_isabella"), QStringLiteral("poised")},
        {QStringLiteral("af_heart"), QStringLiteral("clear, friendly")},
        {QStringLiteral("af_bella"), QStringLiteral("expressive, warm")},
        {QStringLiteral("am_michael"), QStringLiteral("steady")},
        {QStringLiteral("am_fenrir"), QStringLiteral("deep, resonant")},
        {QStringLiteral("af_nicole"), QStringLiteral("soft, intimate")},
        {QStringLiteral("af_sarah"), QStringLiteral("crisp, calm")},
        {QStringLiteral("af_sky"), QStringLiteral("light, breezy")},
        {QStringLiteral("bria"), QStringLiteral("natural, warm")},
        {QStringLiteral("loona"), QStringLiteral("bright, crisp")},
    };
    return notes.value(key);
}
}

VoicePacks::VoicePacks(const QString &directory, QObject *parent, const QString &architecture, const QByteArray &manifest)
    : QObject(parent), m_directory(directory), m_architecture(architecture.isEmpty() ? currentArchitecture() : architecture),
      m_network(new QNetworkAccessManager(this)) {
    QByteArray json = manifest;
    if (json.isEmpty()) {
        QFile file(QStringLiteral(":/voices/packs.json"));
        if (file.open(QIODevice::ReadOnly))
            json = file.readAll();
    }
    load(json);
    QDir().mkpath(m_directory);
    cleanupOrphanedStaging();
    scanInstalledPacks();
    // Seed estimates immediately from the manifest so callers never block on
    // recursive directory walks during startup, then refine off the UI thread.
    for (const Pack &pack : std::as_const(m_packs))
        if (isInstalledCached(pack.id))
            m_packMegabytes.insert(pack.id, pack.size / (1024.0 * 1024.0));
    if (!m_engine.url.isEmpty() && isInstalledCached(m_engine.id))
        m_packMegabytes.insert(m_engine.id, m_engine.size / (1024.0 * 1024.0));
    recomputeMegabytes();
    if (!m_packMegabytes.isEmpty())
        measureInstalledAsync();
}

VoicePacks::~VoicePacks() {
    ++m_generation;
    ++m_measureGeneration;
    if (m_download)
        m_download->abort();
    if (m_tar)
        m_dyingTars.append(m_tar);
    for (const QPointer<QProcess> &proc : std::as_const(m_dyingTars)) {
        if (!proc)
            continue;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(1000);
        }
    }
    m_dyingTars.clear();
    m_tar = nullptr;
    if (m_activeStaging) {
        QDir(m_activeStaging->path()).removeRecursively();
        m_activeStaging.reset();
    }
    if (m_part)
        m_part->close();
    if (m_measureFuture.isRunning())
        m_measureFuture.waitForFinished();
    if (m_trashFuture.isRunning())
        m_trashFuture.waitForFinished();
    for (const QString &path : std::as_const(m_pendingTrash))
        QDir(path).removeRecursively();
    m_pendingTrash.clear();
}

VoicePacks::DirStamp VoicePacks::readDirStamp() const {
    struct stat st {};
    const QByteArray native = QFile::encodeName(m_directory);
    if (::stat(native.constData(), &st) != 0)
        return {};
    DirStamp stamp;
    stamp.valid = true;
#ifdef Q_OS_MACOS
    stamp.mtimeSec = qint64(st.st_mtimespec.tv_sec);
    stamp.mtimeNsec = qint64(st.st_mtimespec.tv_nsec);
#else
    stamp.mtimeSec = qint64(st.st_mtim.tv_sec);
    stamp.mtimeNsec = qint64(st.st_mtim.tv_nsec);
#endif
    stamp.nlink = quint64(st.st_nlink);
    stamp.ino = quint64(st.st_ino);
    return stamp;
}

void VoicePacks::scanInstalledPacks() const {
    m_installedPacks.clear();
    const QDir base(m_directory);
    auto check = [&](const Pack &p) {
        if (p.dir.isEmpty())
            return;
        if (QFile::exists(base.filePath(p.dir + QLatin1Char('/') + installedMarker)) ||
            QFile::exists(base.filePath(p.dir + QLatin1Char('/') + legacyInstalledMarker)))
            m_installedPacks.insert(p.id);
    };
    if (!m_engine.url.isEmpty())
        check(m_engine);
    for (const Pack &p : std::as_const(m_packs))
        check(p);
    m_dirStamp = readDirStamp();
}

void VoicePacks::syncInstalledIfDirChanged() const {
    const DirStamp now = readDirStamp();
    if (!m_dirStamp.valid || now != m_dirStamp)
        scanInstalledPacks();
}

void VoicePacks::cleanupOrphanedStaging() {
    const QDir dir(m_directory);
    const QStringList orphans = dir.entryList(
        {QStringLiteral(".unpack-*"), QStringLiteral(".trash-*"), QStringLiteral(".backup-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &name : orphans)
        scheduleTrashRemoval(dir.filePath(name));
}

void VoicePacks::scheduleTrashRemoval(const QString &path) {
    if (path.isEmpty())
        return;
    m_pendingTrash.append(path);
    pumpTrash();
}

void VoicePacks::pumpTrash() {
    if (m_trashFuture.isRunning() || m_pendingTrash.isEmpty())
        return;
    const QStringList batch = std::exchange(m_pendingTrash, {});
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
        watcher->deleteLater();
        if (!m_pendingTrash.isEmpty())
            pumpTrash();
    });
    m_trashFuture = QtConcurrent::run([batch] {
        for (const QString &path : batch)
            QDir(path).removeRecursively();
    });
    watcher->setFuture(m_trashFuture);
}

QString VoicePacks::engineVersion() const { return QString::fromLatin1(SherpaOnnx::version); }

int VoicePacks::installedVoiceCount() const {
    Q_ASSERT(QThread::currentThread() == thread());
    syncInstalledIfDirChanged();
    if (!isInstalledCached(m_engine.id))
        return 0;
    int count = 0;
    for (const Voice &voice : m_voices)
        if (isInstalledCached(voice.pack))
            ++count;
    return count;
}

int VoicePacks::voiceCountForPack(const QString &packId) const {
    int count = 0;
    for (const Voice &voice : m_voices)
        if (voice.pack == packId)
            ++count;
    return count;
}

void VoicePacks::recomputeMegabytes() {
    double total = 0;
    for (auto it = m_packMegabytes.cbegin(); it != m_packMegabytes.cend(); ++it)
        total += it.value();
    m_megabytes = total;
}

void VoicePacks::measureInstalledAsync() {
    syncInstalledIfDirChanged();
    QList<QPair<QString, QString>> targets;
    if (!m_engine.url.isEmpty() && isInstalledCached(m_engine.id))
        targets.append({m_engine.id, QDir(m_directory).filePath(m_engine.dir)});
    for (const Pack &pack : std::as_const(m_packs))
        if (isInstalledCached(pack.id))
            targets.append({pack.id, QDir(m_directory).filePath(pack.dir)});
    if (targets.isEmpty())
        return;
    const quint64 gen = ++m_measureGeneration;
    auto *watcher = new QFutureWatcher<QHash<QString, double>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, double>>::finished, this, [this, watcher, gen] {
        const QHash<QString, double> measured = watcher->result();
        watcher->deleteLater();
        if (gen != m_measureGeneration)
            return;
        syncInstalledIfDirChanged();
        for (auto it = measured.cbegin(); it != measured.cend(); ++it)
            if (isInstalledCached(it.key()))
                m_packMegabytes.insert(it.key(), it.value());
        recomputeMegabytes();
        emit changed();
    });
    m_measureFuture = QtConcurrent::run([targets] {
        QHash<QString, double> sizes;
        for (const auto &[id, dir] : targets)
            sizes.insert(id, sizeOf(dir) / (1024.0 * 1024.0));
        return sizes;
    });
    watcher->setFuture(m_measureFuture);
}

QString VoicePacks::currentArchitecture() {
    const QString arch = QSysInfo::currentCpuArchitecture();
    return arch == QStringLiteral("arm64") ? QStringLiteral("aarch64") : arch;
}

QString VoicePacks::enginePlatformKey(const QString &architecture, bool isMacOs) {
    const QString norm = architecture == QLatin1String("arm64") ? QStringLiteral("aarch64") : architecture;
    if (norm == QLatin1String("macos-universal2") || norm == QLatin1String("osx-universal2"))
        return QStringLiteral("macos-universal2");
    if (isMacOs && (norm == QLatin1String("aarch64") || norm == QLatin1String("x86_64")))
        return QStringLiteral("macos-universal2");
    return norm;
}

QString VoicePacks::kokoroVariantKey(const QString &architecture) {
    if (architecture == QLatin1String("arm64"))
        return QStringLiteral("aarch64");
    if (architecture == QLatin1String("macos-universal2") || architecture == QLatin1String("osx-universal2"))
        return currentArchitecture();
    return architecture;
}

QString VoicePacks::engineLibraryFileName(const QString &enginePlatform, bool isMacOs) {
    const bool useDylib = enginePlatform == QLatin1String("macos-universal2") ||
                          enginePlatform == QLatin1String("osx-universal2") ||
                          (enginePlatform.isEmpty() && isMacOs);
    return useDylib ? QStringLiteral("libsherpa-onnx-c-api.dylib") : QStringLiteral("libsherpa-onnx-c-api.so");
}

QStringList VoicePacks::tarExtractArguments(const QString &archive, const QString &destination, bool isMacOs) {
    if (isMacOs) {
        // macOS /usr/bin/tar is bsdtar (libarchive), which rejects GNU-only flags
        // like --no-same-permissions. -o drops ownership restoration, and omitting
        // -p applies the process umask cleanly without GNU extensions.
        return {QStringLiteral("-x"), QStringLiteral("-j"), QStringLiteral("-o"),
                QStringLiteral("-f"), archive, QStringLiteral("-C"), destination};
    }
    return {QStringLiteral("--no-same-owner"), QStringLiteral("--no-same-permissions"),
            QStringLiteral("-xjf"), archive, QStringLiteral("-C"), destination, QStringLiteral("--")};
}

bool VoicePacks::isSafeExtractedTree(const QString &rootDir) {
    const QString canonicalRoot = QFileInfo(rootDir).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return false;
    const QString prefix = canonicalRoot.endsWith(QLatin1Char('/')) ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    QDirIterator it(canonicalRoot, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi = it.nextFileInfo();
        if (fi.isSymLink()) {
            const QString target = fi.symLinkTarget();
            const QString canonicalTarget = QFileInfo(target).canonicalFilePath();
            if (canonicalTarget.isEmpty() || (canonicalTarget != canonicalRoot && !canonicalTarget.startsWith(prefix)))
                return false;
        }
    }
    return true;
}

QString VoicePacks::accentFor(const QString &language, const QString &voiceId) {
    if (voiceId.contains(QLatin1String("alba"), Qt::CaseInsensitive) ||
        language.contains(QLatin1String("scotland"), Qt::CaseInsensitive))
        return tr("Scottish");
    const QString norm = QString(language).replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
    if (norm == QLatin1String("en-gb"))
        return tr("British");
    if (norm == QLatin1String("en-us"))
        return tr("American");
    if (norm == QLatin1String("en-au"))
        return tr("Australian");
    if (norm == QLatin1String("en-ie"))
        return tr("Irish");
    if (norm == QLatin1String("en-in"))
        return tr("Indian");
    if (norm == QLatin1String("en-ca"))
        return tr("Canadian");
    if (norm == QLatin1String("pt-br"))
        return tr("Brazilian");
    if (norm == QLatin1String("pt-pt"))
        return tr("Portuguese");
    if (norm == QLatin1String("es-mx"))
        return tr("Mexican");
    if (norm == QLatin1String("es-es"))
        return tr("Spanish");
    if (norm == QLatin1String("fr-ca"))
        return tr("Canadian French");
    const QString native = QLocale(language).nativeLanguageName();
    return native.isEmpty() ? language : native;
}

void VoicePacks::load(const QByteArray &manifest) {
    const QJsonObject root = QJsonDocument::fromJson(manifest).object();
    const QJsonObject engine = root.value("engine").toObject();
    const QString requestedPlatform = enginePlatformKey(m_architecture);
    QJsonObject forHere = engine.value(requestedPlatform).toObject();
    if (!forHere.isEmpty()) {
        m_enginePlatform = requestedPlatform;
    } else if (requestedPlatform != m_architecture) {
        forHere = engine.value(m_architecture).toObject();
        if (!forHere.isEmpty())
            m_enginePlatform = m_architecture;
    }
    if (!forHere.isEmpty())
        m_engine = {QStringLiteral("engine"), QStringLiteral("engine"), tr("Speech engine"),
                    QUrl(forHere.value("url").toString()), forHere.value("size").toInteger(),
                    forHere.value("sha256").toString().toLatin1(), forHere.value("dir").toString(),
                    engine.value("license").toString()};

    const QJsonObject kokoro = root.value("kokoro").toObject();
    // Kokoro comes in the build that's quickest on this processor.
    const QJsonObject variants = kokoro.value("variants").toObject();
    QJsonObject variant = variants.value(kokoroVariantKey(m_architecture)).toObject();
    if (variant.isEmpty() && (m_architecture == QLatin1String("macos-universal2") ||
                              m_architecture == QLatin1String("osx-universal2")))
        variant = variants.value(QStringLiteral("aarch64")).toObject();
    if (!kokoro.isEmpty() && !variant.isEmpty()) {
        const Pack pack{kokoro.value("id").toString(), QStringLiteral("kokoro"), kokoro.value("title").toString(),
                        QUrl(variant.value("url").toString()), variant.value("size").toInteger(),
                        variant.value("sha256").toString().toLatin1(), variant.value("dir").toString(),
                        kokoro.value("license").toString()};
        m_kokoroModel = variant.value("model").toString();
        m_packs.insert(pack.id, pack);
        const QJsonObject languages = kokoro.value("languages").toObject();
        for (auto it = languages.begin(); it != languages.end(); ++it) {
            const QJsonObject setup = it.value().toObject();
            m_kokoroLanguages.insert(it.key(), {setup.value("lexicon").toString(), setup.value("lang").toString(),
                                                setup.value("dict").toString()});
        }
        for (const auto &value : kokoro.value("voices").toArray()) {
            const QJsonObject v = value.toObject();
            Voice voice;
            voice.id = QStringLiteral("builtin:kokoro:") + v.value("id").toString();
            voice.pack = pack.id;
            voice.name = v.value("name").toString();
            voice.language = v.value("language").toString();
            voice.gender = v.value("gender").toString();
            voice.grade = v.value("grade").toString();
            voice.recommended = v.value("recommended").toBool();
            voice.speaker = v.value("speaker").toInt();
            m_voices.append(voice);
        }
    }

    // Pocket TTS: English voices made like a short recording.
    const QJsonObject pocket = root.value("pocket").toObject();
    if (!pocket.isEmpty()) {
        const Pack pack{pocket.value("id").toString(), QStringLiteral("pocket"), pocket.value("title").toString(),
                        QUrl(pocket.value("url").toString()), pocket.value("size").toInteger(),
                        pocket.value("sha256").toString().toLatin1(), pocket.value("dir").toString(),
                        pocket.value("license").toString()};
        m_packs.insert(pack.id, pack);
        for (const auto &value : pocket.value("voices").toArray()) {
            const QJsonObject v = value.toObject();
            Voice voice;
            voice.id = QStringLiteral("builtin:pocket:") + v.value("id").toString();
            voice.pack = pack.id;
            voice.name = v.value("name").toString();
            voice.language = v.value("language").toString();
            voice.gender = v.value("gender").toString();
            voice.recommended = v.value("recommended").toBool();
            voice.reference = v.value("reference").toString();
            m_voices.append(voice);
        }
    }

    const QJsonObject piper = root.value("piper").toObject();
    const QString piperLicense = piper.value("license").toString();
    for (const auto &value : piper.value("voices").toArray()) {
        const QJsonObject v = value.toObject();
        const QString language = v.value("language").toString();
        const Pack pack{QStringLiteral("piper:") + v.value("id").toString(), QStringLiteral("piper"),
                        QStringLiteral("%1 · %2").arg(v.value("name").toString(), QLocale(language).nativeLanguageName()),
                        QUrl(v.value("url").toString()), v.value("size").toInteger(),
                        v.value("sha256").toString().toLatin1(), v.value("dir").toString(), piperLicense};
        m_packs.insert(pack.id, pack);
        Voice voice;
        voice.id = QStringLiteral("builtin:piper:") + v.value("id").toString();
        voice.pack = pack.id;
        voice.name = v.value("name").toString();
        voice.language = language;
        voice.quality = v.value("quality").toString();
        voice.recommended = v.value("recommended").toBool();
        voice.model = v.value("model").toString();
        m_voices.append(voice);
    }
}

std::optional<VoicePacks::Pack> VoicePacks::pack(const QString &packId) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (packId == m_engine.id && !m_engine.url.isEmpty())
        return m_engine;
    const auto it = m_packs.constFind(packId);
    if (it == m_packs.constEnd())
        return std::nullopt;
    return *it;
}

bool VoicePacks::isInstalled(const QString &packId) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (packId.isEmpty())
        return false;
    syncInstalledIfDirChanged();
    return isInstalledCached(packId);
}

QString VoicePacks::engineLibrary() const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!engineInstalled())
        return {};
    const QDir libDir(QDir(m_directory).filePath(m_engine.dir + QStringLiteral("/lib")));
    const QString primary = libDir.filePath(engineLibraryFileName(m_enginePlatform));
#ifdef Q_OS_MACOS
    if (QFileInfo::exists(primary))
        return primary;
    const QString dylib = libDir.filePath(QStringLiteral("libsherpa-onnx-c-api.dylib"));
    if (QFileInfo::exists(dylib))
        return dylib;
#endif
    return primary;
}

const VoicePacks::Voice *VoicePacks::findVoice(const QString &id) const {
    for (const Voice &voice : m_voices)
        if (voice.id == id)
            return &voice;
    return nullptr;
}

std::optional<VoicePacks::Model> VoicePacks::model(const QString &voiceId) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const Voice *voice = findVoice(voiceId);
    if (!voice)
        return std::nullopt;
    syncInstalledIfDirChanged();
    if (!isInstalledCached(m_engine.id) || !isInstalledCached(voice->pack))
        return std::nullopt;
    const QString dir = QDir(m_directory).filePath(m_packs.value(voice->pack).dir);
    auto path = [&dir](const QString &file) { return file.isEmpty() ? QString() : dir + '/' + file; };
    Model model;
    if (!voice->reference.isEmpty()) {
        model.kind = QStringLiteral("pocket");
        model.key = voice->pack;
        model.dir = dir;
        model.reference = path(voice->reference);
        return model;
    }
    if (voice->pack.startsWith(QStringLiteral("piper:"))) {
        model.kind = QStringLiteral("piper");
        model.key = voice->id;
        model.model = path(voice->model);
        model.tokens = path(QStringLiteral("tokens.txt"));
        model.dataDir = path(QStringLiteral("espeak-ng-data"));
        return model;
    }
    if (!m_kokoroLanguages.contains(voice->language))
        return std::nullopt;
    const LanguageSetup setup = m_kokoroLanguages.value(voice->language);
    model.kind = QStringLiteral("kokoro");
    // One model serves every voice of a language; another language loads
    // with its own pronunciation.
    model.key = QStringLiteral("kokoro:") + voice->language;
    model.model = path(m_kokoroModel);
    model.voices = path(QStringLiteral("voices.bin"));
    model.tokens = path(QStringLiteral("tokens.txt"));
    model.dataDir = path(QStringLiteral("espeak-ng-data"));
    model.lexicon = path(setup.lexicon);
    model.lang = setup.lang;
    model.dictDir = path(setup.dict);
    model.speaker = voice->speaker;
    return model;
}

QVariantMap VoicePacks::toMap(const Voice &voice) const {
    qint64 bytes = 0;
    if (!isInstalledCached(voice.pack))
        bytes += m_packs.value(voice.pack).size;
    if (!isInstalledCached(m_engine.id))
        bytes += m_engine.size;
    const bool kokoro = voice.pack == QStringLiteral("kokoro-v1.0") ||
                        (!voice.pack.startsWith(QStringLiteral("piper:")) && voice.reference.isEmpty());
    QString note = curatedVoiceNote(voice.id);
    if (note.isEmpty()) {
        if (!voice.grade.isEmpty())
            note = tr("grade %1").arg(voice.grade);
        else if (!voice.quality.isEmpty())
            note = voice.quality == QLatin1String("high") ? tr("high quality")
                 : voice.quality == QLatin1String("medium") ? tr("medium quality")
                 : tr("low quality");
    }
    return {{"id", voice.id},
            {"name", voice.name},
            {"title", voice.name},
            {"language", voice.language},
            {"languageName", QLocale(voice.language).nativeLanguageName()},
            {"accent", accentFor(voice.language, voice.id)},
            {"note", note},
            {"gender", voice.gender},
            {"grade", voice.grade},
            {"quality", voice.quality},
            {"recommended", voice.recommended},
            {"suggested", voice.recommended},
            {"engine", !voice.reference.isEmpty() ? QStringLiteral("Pocket TTS") : kokoro ? QStringLiteral("Kokoro") : QStringLiteral("Piper")},
            {"pack", voice.pack},
            {"kind", QStringLiteral("builtin")},
            {"installed", bytes == 0},
            {"bytes", bytes}};
}

QVariantList VoicePacks::voices(const QString &language) const {
    Q_ASSERT(QThread::currentThread() == thread());
    syncInstalledIfDirChanged();
    const QString base = baseOf(language);
    const QString region = regionOf(language);
    QList<const Voice *> found;
    for (const Voice &voice : m_voices)
        if (base.isEmpty() || baseOf(voice.language) == base)
            found.append(&voice);
    auto score = [](const Voice *voice) {
        return voice->quality.isEmpty() ? gradeRank(voice->grade) : 3 + 2 * qualityRank(voice->quality);
    };
    std::stable_sort(found.begin(), found.end(), [&](const Voice *a, const Voice *b) {
        const bool installedA = isInstalledCached(a->pack), installedB = isInstalledCached(b->pack);
        if (installedA != installedB)
            return installedA;
        const bool regionA = !region.isEmpty() && regionOf(a->language) == region;
        const bool regionB = !region.isEmpty() && regionOf(b->language) == region;
        if (regionA != regionB)
            return regionA;
        if (a->recommended != b->recommended)
            return a->recommended;
        return score(a) > score(b);
    });
    QVariantList result;
    result.reserve(found.size());
    for (const Voice *voice : found)
        result.append(toMap(*voice));
    return result;
}

QVariantMap VoicePacks::voice(const QString &id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const Voice *found = findVoice(id);
    if (!found)
        return {};
    syncInstalledIfDirChanged();
    return toMap(*found);
}

QVariantList VoicePacks::searchVoices(const QString &query, const QString &language, bool installedOnly) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const QStringList words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QVariantList baseList = voices(language);
    if (words.isEmpty() && !installedOnly)
        return baseList;
    QVariantList result;
    result.reserve(baseList.size());
    for (const QVariant &value : baseList) {
        const QVariantMap map = value.toMap();
        if (installedOnly && !map.value(QStringLiteral("installed")).toBool())
            continue;
        if (!words.isEmpty()) {
            const QString hay = QStringList{map.value(QStringLiteral("name")).toString(),
                                            map.value(QStringLiteral("language")).toString(),
                                            map.value(QStringLiteral("languageName")).toString(),
                                            map.value(QStringLiteral("accent")).toString(),
                                            map.value(QStringLiteral("note")).toString(),
                                            map.value(QStringLiteral("engine")).toString(),
                                            map.value(QStringLiteral("gender")).toString(),
                                            map.value(QStringLiteral("grade")).toString(),
                                            map.value(QStringLiteral("quality")).toString()}
                                    .join(QLatin1Char(' '))
                                    .toLower();
            const bool match = std::all_of(words.cbegin(), words.cend(),
                                           [&hay](const QString &word) { return hay.contains(word); });
            if (!match)
                continue;
        }
        result.append(map);
    }
    return result;
}

QVariantList VoicePacks::languages() const {
    Q_ASSERT(QThread::currentThread() == thread());
    syncInstalledIfDirChanged();
    const bool engineHere = isInstalledCached(m_engine.id);
    struct LangInfo {
        QString code;
        QString name;
        int count = 0;
        int installed = 0;
    };
    QHash<QString, LangInfo> byBase;
    for (const Voice &voice : m_voices) {
        const QString base = baseOf(voice.language);
        if (base.isEmpty())
            continue;
        LangInfo &info = byBase[base];
        if (info.code.isEmpty()) {
            info.code = base;
            const QString native = QLocale(voice.language).nativeLanguageName();
            info.name = native.isEmpty() ? base : native;
        }
        ++info.count;
        if (engineHere && isInstalledCached(voice.pack))
            ++info.installed;
    }
    QList<LangInfo> list = byBase.values();
    std::sort(list.begin(), list.end(), [](const LangInfo &a, const LangInfo &b) {
        if ((a.installed > 0) != (b.installed > 0))
            return a.installed > 0;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    QVariantList result;
    result.reserve(list.size());
    for (const LangInfo &info : std::as_const(list))
        result.append(QVariantMap{{QStringLiteral("code"), info.code},
                                  {QStringLiteral("name"), info.name},
                                  {QStringLiteral("count"), info.count},
                                  {QStringLiteral("installed"), info.installed}});
    return result;
}

QVariantMap VoicePacks::offer(const QString &language) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const QVariantList candidates = voices(language);
    if (candidates.isEmpty())
        return {};
    // The first is installed, or else the best to download.
    const QVariantMap best = candidates.first().toMap();
    QStringList titles;
    if (!engineInstalled())
        titles.append(m_engine.title);
    if (!isInstalled(best.value("pack").toString()))
        titles.append(m_packs.value(best.value("pack").toString()).title);
    return {{"voice", best}, {"bytes", best.value("bytes")}, {"titles", titles}};
}

void VoicePacks::installVoice(const QString &voiceId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const Voice *voice = findVoice(voiceId);
    if (!voice || busy())
        return;
    if (!supported()) {
        fail(tr("There's no built-in speech engine for this computer's processor (%1).").arg(m_architecture));
        return;
    }
    ++m_generation;
    m_queue.clear();
    if (!engineInstalled())
        m_queue.append(m_engine);
    if (!isInstalled(voice->pack))
        m_queue.append(m_packs.value(voice->pack));
    m_waitingVoice = voiceId;
    m_batchBytes = 0;
    for (const Pack &pack : std::as_const(m_queue))
        m_batchBytes += pack.size;
    m_doneBytes = 0;
    m_currentBytes = 0;
    m_problem.clear();
    emit changed();
    next();
}

void VoicePacks::installPack(const QString &packId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto found = pack(packId);
    if (!found || busy())
        return;
    if (!supported()) {
        fail(tr("There's no built-in speech engine for this computer's processor (%1).").arg(m_architecture));
        return;
    }
    ++m_generation;
    m_queue.clear();
    if (!engineInstalled())
        m_queue.append(m_engine);
    if (found->id != m_engine.id && !isInstalled(found->id))
        m_queue.append(*found);
    if (m_queue.isEmpty())
        return;
    m_waitingVoice.clear();
    m_batchBytes = 0;
    for (const Pack &queued : std::as_const(m_queue))
        m_batchBytes += queued.size;
    m_doneBytes = 0;
    m_currentBytes = 0;
    m_problem.clear();
    emit changed();
    next();
}

double VoicePacks::progress() const {
    if (m_batchBytes <= 0)
        return 0;
    return qMin(1.0, double(m_doneBytes + m_currentBytes) / double(m_batchBytes));
}

QString VoicePacks::archivePath(const Pack &pack) const {
    return QDir(m_directory).filePath(QStringLiteral(".downloads/") + pack.url.fileName());
}

void VoicePacks::next() {
    if (m_queue.isEmpty()) {
        m_currentTitle.clear();
        const QString voice = std::exchange(m_waitingVoice, QString());
        emit progressChanged();
        if (!voice.isEmpty() && model(voice))
            emit ready(voice);
        return;
    }
    const Pack pack = m_queue.first();
    m_currentTitle = pack.title;
    m_currentBytes = 0;
    emit progressChanged();
    if (QFile::exists(archivePath(pack)))
        verifyAndUnpack(pack, archivePath(pack));
    else
        download(pack);
}

void VoicePacks::download(const Pack &pack) {
    QDir().mkpath(QFileInfo(archivePath(pack)).absolutePath());
    delete m_part;
    m_part = new QFile(archivePath(pack) + QStringLiteral(".part"), this);
    if (!m_part->open(QIODevice::ReadWrite)) {
        fail(tr("Couldn't save the download of %1 in %2.").arg(pack.title, m_directory));
        return;
    }
    // What came before is kept, and the rest asked for.
    const qint64 had = m_part->size();
    m_part->seek(had);
    QNetworkRequest request(pack.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(downloadTimeoutMs);
    if (had > 0)
        request.setRawHeader("Range", "bytes=" + QByteArray::number(had) + '-');
    QNetworkReply *reply = m_network->get(request);
    m_download = reply;
    const quint64 gen = m_generation;
    auto started = std::make_shared<bool>(false);
    auto writeFailed = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, pack, had, gen, started, writeFailed] {
        if (gen != m_generation || reply != m_download || !m_part || !m_part->isOpen() || *writeFailed)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400)
            return;
        if (!*started) {
            *started = true;
            // A server that ignores the range sends it all again.
            if (had > 0 && status == 200) {
                m_part->resize(0);
                m_part->seek(0);
            }
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty() && m_part->write(chunk) != chunk.size()) {
            *writeFailed = true;
            m_part->close();
            reply->abort();
            fail(tr("Couldn't write %1: %2").arg(pack.title, m_part->errorString()));
            return;
        }
        m_currentBytes = m_part->pos();
        emit progressChanged();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, pack, had, gen, started, writeFailed] {
        reply->deleteLater();
        if (gen != m_generation || reply != m_download)
            return;
        m_download = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!*writeFailed && status < 400 && reply->bytesAvailable() > 0 && m_part && m_part->isOpen()) {
            if (!*started) {
                *started = true;
                if (had > 0 && status == 200) {
                    m_part->resize(0);
                    m_part->seek(0);
                }
            }
            const QByteArray chunk = reply->readAll();
            if (!chunk.isEmpty() && m_part->write(chunk) != chunk.size()) {
                *writeFailed = true;
                m_part->close();
                fail(tr("Couldn't write %1: %2").arg(pack.title, m_part->errorString()));
                return;
            }
        }
        const qint64 partSize = m_part ? m_part->size() : 0;
        const QString partName = m_part ? m_part->fileName() : QString();
        if (m_part)
            m_part->close();
        if (*writeFailed || reply->error() == QNetworkReply::OperationCanceledError)
            return;
        if (status == 416 && partSize < pack.size) {
            QFile::remove(partName);
            fail(tr("The download of %1 couldn't be resumed. Try again.").arg(pack.title));
            return;
        }
        const bool complete = (status == 416 && partSize >= pack.size) ||
                              (reply->error() == QNetworkReply::NoError && status < 400 && partSize >= pack.size);
        if (!complete) {
            fail(tr("The download of %1 stopped: %2. It will pick up where it left off.")
                     .arg(pack.title, reply->errorString()));
            return;
        }
        const QString archive = archivePath(pack);
        QFile::remove(archive);
        if (!QFile::rename(partName, archive)) {
            fail(tr("Couldn't move the download of %1 into place.").arg(pack.title));
            return;
        }
        verifyAndUnpack(pack, archive);
    });
}

void VoicePacks::verifyAndUnpack(const Pack &pack, const QString &archive) {
    m_unpacking = true;
    m_currentBytes = pack.size;
    emit progressChanged();
    const quint64 gen = m_generation;
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, pack, archive, gen] {
        watcher->deleteLater();
        if (gen != m_generation)
            return;
        if (watcher->result() != pack.sha256) {
            m_unpacking = false;
            QFile::remove(archive);
            fail(tr("The download of %1 didn't match its checksum, so it was set aside. Try again.").arg(pack.title));
            return;
        }
        // Unpacked into a unique per-attempt staging directory inside m_directory so
        // cancel() + fast retry can never delete a live retry's extraction dir.
        auto staging = std::make_shared<QTemporaryDir>(
            QDir(m_directory).filePath(QStringLiteral(".unpack-") + pack.dir + QStringLiteral("-XXXXXX")));
        if (!staging->isValid()) {
            m_unpacking = false;
            fail(tr("%1 couldn't be unpacked: couldn't create staging directory.").arg(pack.title));
            return;
        }
        staging->setAutoRemove(false);
        const QString aside = staging->path();
        m_activeStaging = staging;
        m_dirStamp = readDirStamp();

        auto *tar = new QProcess(this);
        m_tar = tar;
        auto handled = std::make_shared<bool>(false);
        connect(tar, &QProcess::errorOccurred, this,
                [this, tar, pack, aside, staging, gen, handled](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart || *handled)
                        return;
                    *handled = true;
                    if (m_tar == tar)
                        m_tar = nullptr;
                    m_dyingTars.removeAll(tar);
                    if (m_activeStaging == staging)
                        m_activeStaging.reset();
                    tar->deleteLater();
                    scheduleTrashRemoval(aside);
                    if (gen != m_generation)
                        return;
                    m_unpacking = false;
                    fail(tr("%1 couldn't be unpacked: tar couldn't be started.").arg(pack.title));
                });
        connect(tar, &QProcess::finished, this,
                [this, tar, pack, archive, aside, staging, gen, handled](int code, QProcess::ExitStatus status) {
                    if (*handled)
                        return;
                    *handled = true;
                    if (m_tar == tar)
                        m_tar = nullptr;
                    m_dyingTars.removeAll(tar);
                    if (m_activeStaging == staging)
                        m_activeStaging.reset();
                    tar->deleteLater();
                    if (gen != m_generation) {
                        scheduleTrashRemoval(aside);
                        return;
                    }
                    m_unpacking = false;
                    const QString extracted = QDir(aside).filePath(pack.dir);
                    const QString target = QDir(m_directory).filePath(pack.dir);
                    const QString asideCanon = QFileInfo(aside).canonicalFilePath();
                    const QString extractedCanon = QFileInfo(extracted).canonicalFilePath();
                    const bool safeTree = !asideCanon.isEmpty() &&
                                          extractedCanon.startsWith(asideCanon + QLatin1Char('/')) &&
                                          isSafeExtractedTree(extracted);
                    if (code != 0 || status != QProcess::NormalExit || !QFileInfo::exists(extracted) || !safeTree) {
                        scheduleTrashRemoval(aside);
                        const QString errText = QString::fromLocal8Bit(tar->readAllStandardError()).trimmed();
                        fail(tr("%1 couldn't be unpacked: %2")
                                 .arg(pack.title, errText.isEmpty() ? QStringLiteral("invalid archive layout") : errText));
                        return;
                    }
                    // Write the sentinel before swapping into place so target is never half-installed.
                    QFile marker(extracted + QLatin1Char('/') + installedMarker);
                    const QByteArray markerData = pack.id.toUtf8() + ' ' + pack.sha256 + '\n';
                    if (!marker.open(QIODevice::WriteOnly) || marker.write(markerData) != markerData.size()) {
                        marker.close();
                        scheduleTrashRemoval(aside);
                        fail(tr("Couldn't finish installing %1.").arg(pack.title));
                        return;
                    }
                    marker.close();
                    QFile::remove(extracted + QLatin1Char('/') + legacyInstalledMarker);

                    QString backup;
                    if (QFileInfo::exists(target)) {
                        emit aboutToRemove(pack.id);
                        backup = QDir(m_directory)
                                     .filePath(QStringLiteral(".backup-%1-%2-%3")
                                                   .arg(pack.dir)
                                                   .arg(QCoreApplication::applicationPid())
                                                   .arg(++m_trashSeq));
                        if (!QDir().rename(target, backup)) {
                            scheduleTrashRemoval(aside);
                            fail(tr("%1 couldn't be moved into place.").arg(pack.title));
                            return;
                        }
                    }
                    if (!QDir().rename(extracted, target)) {
                        if (!backup.isEmpty())
                            QDir().rename(backup, target);
                        m_dirStamp = readDirStamp();
                        scheduleTrashRemoval(aside);
                        fail(tr("%1 couldn't be moved into place.").arg(pack.title));
                        return;
                    }
                    m_installedPacks.insert(pack.id);
                    m_dirStamp = readDirStamp();
                    if (!backup.isEmpty())
                        scheduleTrashRemoval(backup);
                    scheduleTrashRemoval(aside);
                    QFile::remove(archive);
                    m_packMegabytes.insert(pack.id, pack.size / (1024.0 * 1024.0));
                    recomputeMegabytes();
                    measureInstalledAsync();
                    m_doneBytes += pack.size;
                    m_currentBytes = 0;
                    if (!m_queue.isEmpty())
                        m_queue.removeFirst();
                    emit changed();
                    next();
                });
        tar->start(QStringLiteral("tar"), tarExtractArguments(archive, aside));
    });
    watcher->setFuture(QtConcurrent::run([archive] {
        QFile file(archive);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (file.open(QIODevice::ReadOnly))
            hash.addData(&file);
        return hash.result().toHex();
    }));
}

void VoicePacks::fail(const QString &message) {
    m_queue.clear();
    m_waitingVoice.clear();
    m_currentTitle.clear();
    m_unpacking = false;
    m_problem = message;
    emit changed();
    emit progressChanged();
    emit failed(message);
}

void VoicePacks::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_generation;
    m_queue.clear();
    m_waitingVoice.clear();
    m_currentTitle.clear();
    m_unpacking = false;
    if (m_download)
        m_download->abort();  // what came is kept, for next time
    m_download = nullptr;
    if (m_part) {
        m_part->close();
        delete m_part;
        m_part = nullptr;
    }
    if (m_tar) {
        m_dyingTars.append(m_tar);
        m_tar->kill();
        m_tar = nullptr;
    }
    emit progressChanged();
}

void VoicePacks::remove(const QString &packId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (busy())
        return;
    const auto found = pack(packId);
    if (!found || found->dir.isEmpty())
        return;
    const QString target = QDir(m_directory).filePath(found->dir);
    // Only ever inside Clothbound's own folder.
    if (!QFileInfo(target).canonicalFilePath().startsWith(QFileInfo(m_directory).canonicalFilePath() + '/'))
        return;
    emit aboutToRemove(packId);
    m_installedPacks.remove(packId);
    m_packMegabytes.remove(packId);
    recomputeMegabytes();
    const QString trashDir = QDir(m_directory)
                                 .filePath(QStringLiteral(".trash-%1-%2-%3")
                                               .arg(found->dir)
                                               .arg(QCoreApplication::applicationPid())
                                               .arg(++m_trashSeq));
    if (QDir().rename(target, trashDir)) {
        scheduleTrashRemoval(trashDir);
    } else {
        scheduleTrashRemoval(target);
    }
    m_dirStamp = readDirStamp();
    const QString archive = archivePath(*found);
    QFile::remove(archive);
    QFile::remove(archive + QStringLiteral(".part"));
    emit changed();
}

QVariantList VoicePacks::installedPacks() const {
    Q_ASSERT(QThread::currentThread() == thread());
    syncInstalledIfDirChanged();
    QVariantList result;
    QList<Pack> all;
    if (!m_engine.url.isEmpty())
        all.append(m_engine);
    for (const Pack &pack : std::as_const(m_packs))
        all.append(pack);
    std::stable_sort(all.begin(), all.end(), [](const Pack &a, const Pack &b) {
        auto rank = [](const QString &k) {
            return k == QLatin1String("engine") ? 0 : k == QLatin1String("kokoro") ? 1 : k == QLatin1String("pocket") ? 2 : 3;
        };
        if (rank(a.kind) != rank(b.kind))
            return rank(a.kind) < rank(b.kind);
        return a.title.localeAwareCompare(b.title) < 0;
    });
    for (const Pack &pack : std::as_const(all))
        if (!pack.url.isEmpty() && isInstalledCached(pack.id))
            result.append(QVariantMap{{"id", pack.id},
                                      {"kind", pack.kind},
                                      {"title", pack.title},
                                      {"megabytes", m_packMegabytes.value(pack.id, pack.size / (1024.0 * 1024.0))},
                                      {"license", pack.license},
                                      {"voiceCount", voiceCountForPack(pack.id)}});
    return result;
}

QVariantList VoicePacks::catalogPacks() const {
    Q_ASSERT(QThread::currentThread() == thread());
    syncInstalledIfDirChanged();
    QVariantList result;
    const bool engineHere = isInstalledCached(m_engine.id);
    const qint64 engineExtra = engineHere ? 0 : m_engine.size;
    QList<Pack> featured;
    for (const Pack &pack : std::as_const(m_packs))
        if (pack.kind == QLatin1String("kokoro") || pack.kind == QLatin1String("pocket"))
            featured.append(pack);
    std::stable_sort(featured.begin(), featured.end(), [](const Pack &a, const Pack &b) {
        return a.kind == QLatin1String("kokoro") && b.kind != QLatin1String("kokoro");
    });
    for (const Pack &pack : std::as_const(featured)) {
        const bool packHere = isInstalledCached(pack.id);
        const bool installed = engineHere && packHere;
        QSet<QString> langs;
        int voices = 0;
        for (const Voice &v : m_voices) {
            if (v.pack != pack.id)
                continue;
            ++voices;
            langs.insert(baseOf(v.language));
        }
        result.append(QVariantMap{{"id", pack.id},
                                  {"kind", pack.kind},
                                  {"title", pack.title},
                                  {"installed", installed},
                                  {"packInstalled", packHere},
                                  {"bytes", installed ? 0 : (packHere ? 0 : pack.size) + engineExtra},
                                  {"license", pack.license},
                                  {"voiceCount", voices},
                                  {"languageCount", int(langs.size())}});
    }
    return result;
}
