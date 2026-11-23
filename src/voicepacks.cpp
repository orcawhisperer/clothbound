#include "voicepacks.h"

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
#include <QSysInfo>
#include <QtConcurrent>
#include <algorithm>

namespace {
const QString installedMarker = QStringLiteral(".dogear-installed");

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

qint64 sizeOf(const QString &directory) {
    qint64 total = 0;
    QDirIterator it(directory, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        total += it.nextFileInfo().size();
    return total;
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
    for (const Pack &pack : std::as_const(m_packs))
        if (isInstalled(pack.id))
            m_megabytes += sizeOf(QDir(m_directory).filePath(pack.dir)) / (1024.0 * 1024.0);
    if (isInstalled(m_engine.id))
        m_megabytes += sizeOf(QDir(m_directory).filePath(m_engine.dir)) / (1024.0 * 1024.0);
}

VoicePacks::~VoicePacks() {
    if (m_download)
        m_download->abort();
}

QString VoicePacks::currentArchitecture() {
    const QString arch = QSysInfo::currentCpuArchitecture();
    return arch == QStringLiteral("arm64") ? QStringLiteral("aarch64") : arch;
}

void VoicePacks::load(const QByteArray &manifest) {
    const QJsonObject root = QJsonDocument::fromJson(manifest).object();
    const QJsonObject engine = root.value("engine").toObject();
    const QJsonObject forHere = engine.value(m_architecture).toObject();
    if (!forHere.isEmpty())
        m_engine = {QStringLiteral("engine"), QStringLiteral("engine"), tr("Speech engine"),
                    QUrl(forHere.value("url").toString()), forHere.value("size").toInteger(),
                    forHere.value("sha256").toString().toLatin1(), forHere.value("dir").toString(),
                    engine.value("license").toString()};

    const QJsonObject kokoro = root.value("kokoro").toObject();
    // Kokoro comes in the build that's quickest on this processor.
    const QJsonObject variant = kokoro.value("variants").toObject().value(m_architecture).toObject();
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
    if (packId == m_engine.id && !m_engine.url.isEmpty())
        return m_engine;
    const auto it = m_packs.constFind(packId);
    if (it == m_packs.constEnd())
        return std::nullopt;
    return *it;
}

bool VoicePacks::isInstalled(const QString &packId) const {
    const auto found = pack(packId);
    return found && QFile::exists(QDir(m_directory).filePath(found->dir + '/' + installedMarker));
}

QString VoicePacks::engineLibrary() const {
    if (!engineInstalled())
        return {};
    return QDir(m_directory).filePath(m_engine.dir + QStringLiteral("/lib/libsherpa-onnx-c-api.so"));
}

const VoicePacks::Voice *VoicePacks::findVoice(const QString &id) const {
    for (const Voice &voice : m_voices)
        if (voice.id == id)
            return &voice;
    return nullptr;
}

std::optional<VoicePacks::Model> VoicePacks::model(const QString &voiceId) const {
    const Voice *voice = findVoice(voiceId);
    if (!voice || !engineInstalled() || !isInstalled(voice->pack))
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
    if (!isInstalled(voice.pack))
        bytes += m_packs.value(voice.pack).size;
    if (!engineInstalled())
        bytes += m_engine.size;
    const bool kokoro = voice.pack == QStringLiteral("kokoro-v1.0") ||
                        (!voice.pack.startsWith(QStringLiteral("piper:")) && voice.reference.isEmpty());
    return {{"id", voice.id},
            {"name", voice.name},
            {"title", voice.name},
            {"language", voice.language},
            {"languageName", QLocale(voice.language).nativeLanguageName()},
            {"gender", voice.gender},
            {"grade", voice.grade},
            {"quality", voice.quality},
            {"recommended", voice.recommended},
            {"engine", kokoro ? QStringLiteral("Kokoro") : QStringLiteral("Piper")},
            {"pack", voice.pack},
            {"kind", QStringLiteral("builtin")},
            {"installed", bytes == 0},
            {"bytes", bytes}};
}

QVariantList VoicePacks::voices(const QString &language) const {
    const QString base = baseOf(language);
    const QString region = language.section('-', 1, 1).toUpper();
    QList<const Voice *> found;
    for (const Voice &voice : m_voices)
        if (base.isEmpty() || baseOf(voice.language) == base)
            found.append(&voice);
    auto score = [](const Voice *voice) {
        return voice->quality.isEmpty() ? gradeRank(voice->grade) : 3 + 2 * qualityRank(voice->quality);
    };
    std::stable_sort(found.begin(), found.end(), [&](const Voice *a, const Voice *b) {
        const bool installedA = isInstalled(a->pack), installedB = isInstalled(b->pack);
        if (installedA != installedB)
            return installedA;
        const bool regionA = !region.isEmpty() && a->language.section('-', 1, 1) == region;
        const bool regionB = !region.isEmpty() && b->language.section('-', 1, 1) == region;
        if (regionA != regionB)
            return regionA;
        if (a->recommended != b->recommended)
            return a->recommended;
        return score(a) > score(b);
    });
    QVariantList result;
    for (const Voice *voice : found)
        result.append(toMap(*voice));
    return result;
}

QVariantMap VoicePacks::voice(const QString &id) const {
    const Voice *found = findVoice(id);
    return found ? toMap(*found) : QVariantMap();
}

QVariantMap VoicePacks::offer(const QString &language) const {
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
    const Voice *voice = findVoice(voiceId);
    if (!voice || busy())
        return;
    if (!supported()) {
        fail(tr("There's no built-in speech engine for this computer's processor (%1).").arg(m_architecture));
        return;
    }
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
    if (had > 0)
        request.setRawHeader("Range", "bytes=" + QByteArray::number(had) + '-');
    QNetworkReply *reply = m_network->get(request);
    m_download = reply;
    auto started = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, had, started] {
        if (!*started) {
            *started = true;
            // A server that ignores the range sends it all again.
            if (had > 0 && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
                m_part->resize(0);
                m_part->seek(0);
            }
        }
        m_part->write(reply->readAll());
        m_currentBytes = m_part->pos();
        emit progressChanged();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, pack] {
        reply->deleteLater();
        if (reply != m_download)
            return;
        m_download = nullptr;
        if (reply->bytesAvailable())
            m_part->write(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool complete = status == 416 || (reply->error() == QNetworkReply::NoError && m_part->size() >= pack.size);
        m_part->close();
        if (reply->error() == QNetworkReply::OperationCanceledError)
            return;
        if (!complete) {
            fail(tr("The download of %1 stopped: %2. It will pick up where it left off.")
                     .arg(pack.title, reply->errorString()));
            return;
        }
        const QString archive = archivePath(pack);
        QFile::remove(archive);
        QFile::rename(m_part->fileName(), archive);
        verifyAndUnpack(pack, archive);
    });
}

void VoicePacks::verifyAndUnpack(const Pack &pack, const QString &archive) {
    m_unpacking = true;
    m_currentBytes = pack.size;
    emit progressChanged();
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, pack, archive] {
        watcher->deleteLater();
        if (watcher->result() != pack.sha256) {
            m_unpacking = false;
            QFile::remove(archive);
            fail(tr("The download of %1 didn't match its checksum, so it was set aside. Try again.").arg(pack.title));
            return;
        }
        // Unpacked aside, then moved into place: a half-unpacked voice is never used.
        const QString aside = QDir(m_directory).filePath(QStringLiteral(".unpack-") + pack.dir);
        QDir(aside).removeRecursively();
        QDir().mkpath(aside);
        auto *tar = new QProcess(this);
        connect(tar, &QProcess::finished, this, [this, tar, pack, archive, aside](int code, QProcess::ExitStatus status) {
            tar->deleteLater();
            m_unpacking = false;
            const QString target = QDir(m_directory).filePath(pack.dir);
            if (code != 0 || status != QProcess::NormalExit || !QFileInfo::exists(aside + '/' + pack.dir)) {
                QDir(aside).removeRecursively();
                fail(tr("%1 couldn't be unpacked: %2").arg(pack.title, QString::fromLocal8Bit(tar->readAllStandardError()).trimmed()));
                return;
            }
            QDir(target).removeRecursively();
            QDir().rename(aside + '/' + pack.dir, target);
            QDir(aside).removeRecursively();
            QFile marker(target + '/' + installedMarker);
            if (marker.open(QIODevice::WriteOnly))
                marker.write(pack.id.toUtf8() + ' ' + pack.sha256 + '\n');
            marker.close();
            QFile::remove(archive);
            m_megabytes += sizeOf(target) / (1024.0 * 1024.0);
            m_doneBytes += pack.size;
            m_currentBytes = 0;
            if (!m_queue.isEmpty())
                m_queue.removeFirst();
            emit changed();
            next();
        });
        tar->start(QStringLiteral("tar"), {QStringLiteral("-xjf"), archive, QStringLiteral("-C"), aside});
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
    m_problem = message;
    emit changed();
    emit progressChanged();
    emit failed(message);
}

void VoicePacks::cancel() {
    m_queue.clear();
    m_waitingVoice.clear();
    m_currentTitle.clear();
    if (m_download)
        m_download->abort();  // what came is kept, for next time
    m_download = nullptr;
    emit progressChanged();
}

void VoicePacks::remove(const QString &packId) {
    const auto found = pack(packId);
    if (!found || found->dir.isEmpty())
        return;
    const QString target = QDir(m_directory).filePath(found->dir);
    // Only ever inside Dogear's own folder.
    if (!QFileInfo(target).canonicalFilePath().startsWith(QFileInfo(m_directory).canonicalFilePath() + '/'))
        return;
    m_megabytes = qMax(0.0, m_megabytes - sizeOf(target) / (1024.0 * 1024.0));
    QDir(target).removeRecursively();
    emit changed();
}

QVariantList VoicePacks::installedPacks() const {
    QVariantList result;
    QList<Pack> all{m_engine};
    for (const Pack &pack : std::as_const(m_packs))
        all.append(pack);
    for (const Pack &pack : std::as_const(all))
        if (!pack.url.isEmpty() && isInstalled(pack.id))
            result.append(QVariantMap{{"id", pack.id},
                                      {"title", pack.title},
                                      {"megabytes", sizeOf(QDir(m_directory).filePath(pack.dir)) / (1024.0 * 1024.0)},
                                      {"license", pack.license}});
    return result;
}
