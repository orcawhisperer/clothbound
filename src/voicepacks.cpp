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
#include <memory>

namespace {
const QString installedMarker = QStringLiteral(".dogear-installed");
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
    ++m_generation;
    if (m_download)
        m_download->abort();
    if (m_tar) {
        m_tar->disconnect(this);
        if (m_tar->state() != QProcess::NotRunning) {
            m_tar->kill();
            m_tar->waitForFinished(1000);
        }
    }
    if (m_part)
        m_part->close();
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
            {"engine", !voice.reference.isEmpty() ? QStringLiteral("Pocket TTS") : kokoro ? QStringLiteral("Kokoro") : QStringLiteral("Piper")},
            {"pack", voice.pack},
            {"kind", QStringLiteral("builtin")},
            {"installed", bytes == 0},
            {"bytes", bytes}};
}

QVariantList VoicePacks::voices(const QString &language) const {
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
        const bool installedA = isInstalled(a->pack), installedB = isInstalled(b->pack);
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, pack, gen, writeFailed] {
        reply->deleteLater();
        if (gen != m_generation || reply != m_download)
            return;
        m_download = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!*writeFailed && status < 400 && reply->bytesAvailable() > 0 && m_part && m_part->isOpen()) {
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
        // Unpacked aside, then moved into place: a half-unpacked voice is never used.
        const QString aside = QDir(m_directory).filePath(QStringLiteral(".unpack-") + pack.dir);
        QDir(aside).removeRecursively();
        QDir().mkpath(aside);
        auto *tar = new QProcess(this);
        m_tar = tar;
        auto handled = std::make_shared<bool>(false);
        connect(tar, &QProcess::errorOccurred, this, [this, tar, pack, aside, gen, handled](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || *handled)
                return;
            *handled = true;
            if (m_tar == tar)
                m_tar = nullptr;
            tar->deleteLater();
            QDir(aside).removeRecursively();
            if (gen != m_generation)
                return;
            m_unpacking = false;
            fail(tr("%1 couldn't be unpacked: tar couldn't be started.").arg(pack.title));
        });
        connect(tar, &QProcess::finished, this,
                [this, tar, pack, archive, aside, gen, handled](int code, QProcess::ExitStatus status) {
                    if (*handled)
                        return;
                    *handled = true;
                    if (m_tar == tar)
                        m_tar = nullptr;
                    tar->deleteLater();
                    if (gen != m_generation) {
                        QDir(aside).removeRecursively();
                        return;
                    }
                    m_unpacking = false;
                    const QString target = QDir(m_directory).filePath(pack.dir);
                    if (code != 0 || status != QProcess::NormalExit || !QFileInfo::exists(aside + '/' + pack.dir)) {
                        QDir(aside).removeRecursively();
                        fail(tr("%1 couldn't be unpacked: %2")
                                 .arg(pack.title, QString::fromLocal8Bit(tar->readAllStandardError()).trimmed()));
                        return;
                    }
                    QDir(target).removeRecursively();
                    if (!QDir().rename(aside + '/' + pack.dir, target)) {
                        QDir(aside).removeRecursively();
                        fail(tr("%1 couldn't be moved into place.").arg(pack.title));
                        return;
                    }
                    QDir(aside).removeRecursively();
                    QFile marker(target + '/' + installedMarker);
                    const QByteArray markerData = pack.id.toUtf8() + ' ' + pack.sha256 + '\n';
                    if (!marker.open(QIODevice::WriteOnly) || marker.write(markerData) != markerData.size()) {
                        marker.close();
                        QDir(target).removeRecursively();
                        fail(tr("Couldn't finish installing %1.").arg(pack.title));
                        return;
                    }
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
    m_unpacking = false;
    m_problem = message;
    emit changed();
    emit progressChanged();
    emit failed(message);
}

void VoicePacks::cancel() {
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
        m_tar->kill();
        m_tar = nullptr;
    }
    emit progressChanged();
}

void VoicePacks::remove(const QString &packId) {
    if (busy())
        return;
    const auto found = pack(packId);
    if (!found || found->dir.isEmpty())
        return;
    const QString target = QDir(m_directory).filePath(found->dir);
    // Only ever inside Dogear's own folder.
    if (!QFileInfo(target).canonicalFilePath().startsWith(QFileInfo(m_directory).canonicalFilePath() + '/'))
        return;
    emit aboutToRemove(packId);
    m_megabytes = qMax(0.0, m_megabytes - sizeOf(target) / (1024.0 * 1024.0));
    QDir(target).removeRecursively();
    const QString archive = archivePath(*found);
    QFile::remove(archive);
    QFile::remove(archive + QStringLiteral(".part"));
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
