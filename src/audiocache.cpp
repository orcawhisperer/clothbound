#include "audiocache.h"

#include "speech.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <cstdio>
#include <memory>

namespace {
constexpr int maxConcurrentEncodes = 3;
constexpr int maxConcurrentDecodes = 4;

bool atomicReplace(const QString &source, const QString &target) {
    const QByteArray src = QFile::encodeName(source);
    const QByteArray dst = QFile::encodeName(target);
    if (std::rename(src.constData(), dst.constData()) == 0)
        return true;
    QFile::remove(target);
    return QFile::rename(source, target);
}
}

AudioCache::AudioCache(const QString &directory, const QString &databaseConnection, QObject *parent, qint64 capBytes)
    : QObject(parent), m_directory(directory), m_connection(databaseConnection), m_cap(capBytes),
      m_ffmpeg(QStandardPaths::findExecutable(QStringLiteral("ffmpeg"))) {
    QDir().mkpath(m_directory);
    cleanupPartials();
    migrate();
}

AudioCache::~AudioCache() {
    m_encodeQueue.clear();
    m_decodeQueue.clear();
    const auto encodeProcs = m_encodePartials.keys();
    for (QProcess *proc : encodeProcs) {
        const QString partial = m_encodePartials.take(proc);
        m_encodeKeys.remove(proc);
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(1000);
        }
        QFile::remove(partial);
    }
    for (const QPointer<QProcess> &proc : std::as_const(m_decodeProcesses)) {
        if (!proc)
            continue;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(1000);
        }
    }
    m_decodeProcesses.clear();
}

void AudioCache::cleanupPartials() {
    QSet<QString> active;
    for (const QString &path : std::as_const(m_encodePartials))
        active.insert(path);
    QDir dir(m_directory);
    const QStringList partials = dir.entryList({QStringLiteral("*.part"), QStringLiteral("*.part.*")}, QDir::Files);
    for (const QString &name : partials) {
        const QString path = dir.filePath(name);
        if (!active.contains(path))
            QFile::remove(path);
    }
}

bool AudioCache::migrate() {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    return query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS audio_clips (key TEXT PRIMARY KEY, file TEXT NOT NULL,"
                                     " bytes INTEGER NOT NULL, seconds REAL NOT NULL, used_at INTEGER NOT NULL)")) &&
           query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS narration_directions (key TEXT PRIMARY KEY,"
                                     " json TEXT NOT NULL, made_at INTEGER NOT NULL)")) &&
           // Added later: where each sentence of a clip ends, when known.
           (hasBoundaries() || query.exec(QStringLiteral("ALTER TABLE audio_clips ADD COLUMN boundaries TEXT NOT NULL DEFAULT ''"))) &&
           query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS audio_clips_used_at ON audio_clips (used_at)"));
}

bool AudioCache::hasBoundaries() const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.exec(QStringLiteral("PRAGMA table_info(audio_clips)"));
    while (query.next())
        if (query.value(1).toString() == QStringLiteral("boundaries"))
            return true;
    return false;
}

QList<qint64> AudioCache::boundaries(const QString &key) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT boundaries FROM audio_clips WHERE key = ?"));
    query.addBindValue(key);
    QList<qint64> result;
    if (query.exec() && query.next())
        for (const QString &part : query.value(0).toString().split(',', Qt::SkipEmptyParts))
            result.append(part.toLongLong());
    return result;
}

QString AudioCache::directions(const QString &key) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT json FROM narration_directions WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

void AudioCache::storeDirections(const QString &key, const QString &json) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO narration_directions (key, json, made_at) VALUES (?, ?, ?)"));
    query.addBindValue(key);
    query.addBindValue(json);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

QString AudioCache::keyFor(const QString &provider, const QString &model, const QString &voice, const QString &style,
                           const QString &text) {
    const QString material = provider + '\n' + model + '\n' + voice + '\n' + style + '\n' + text;
    return QString::fromLatin1(QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString AudioCache::lookup(const QString &key) {
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT file FROM audio_clips WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next())
        return {};
    const QString file = QDir(m_directory).filePath(query.value(0).toString());
    if (!QFileInfo::exists(file)) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM audio_clips WHERE key = ?"));
        del.addBindValue(key);
        del.exec();
        return {};
    }
    QSqlQuery touch(db);
    touch.prepare(QStringLiteral("UPDATE audio_clips SET used_at = ? WHERE key = ?"));
    touch.addBindValue(QDateTime::currentSecsSinceEpoch());
    touch.addBindValue(key);
    touch.exec();
    return file;
}

QString AudioCache::lookupPath(const QString &key) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT file FROM audio_clips WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next())
        return {};
    const QString file = QDir(m_directory).filePath(query.value(0).toString());
    return QFileInfo::exists(file) ? file : QString();
}

QHash<QString, QPair<QString, double>> AudioCache::clipInfoBatch(const QStringList &keys) const {
    QHash<QString, QPair<QString, double>> result;
    if (keys.isEmpty())
        return result;
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    const QDir dir(m_directory);
    constexpr int chunkSize = 400;
    for (int offset = 0; offset < keys.size(); offset += chunkSize) {
        const int count = qMin(chunkSize, int(keys.size()) - offset);
        QStringList placeholders;
        placeholders.reserve(count);
        for (int i = 0; i < count; ++i)
            placeholders.append(QStringLiteral("?"));
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT key, file, seconds FROM audio_clips WHERE key IN (") +
                      placeholders.join(',') + QLatin1Char(')'));
        for (int i = 0; i < count; ++i)
            query.addBindValue(keys.at(offset + i));
        if (!query.exec())
            continue;
        while (query.next()) {
            const QString file = dir.filePath(query.value(1).toString());
            if (QFileInfo::exists(file))
                result.insert(query.value(0).toString(), {file, query.value(2).toDouble()});
        }
    }
    return result;
}

void AudioCache::remove(const QString &key) {
    if (key.isEmpty())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT file FROM audio_clips WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next())
        QFile::remove(QDir(m_directory).filePath(query.value(0).toString()));
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM audio_clips WHERE key = ?"));
    del.addBindValue(key);
    del.exec();
    emit changed();
}

bool AudioCache::isStoring(const QString &key) const {
    if (m_activeEncodeKeys.contains(key))
        return true;
    for (const EncodeJob &job : m_encodeQueue)
        if (job.key == key)
            return true;
    return false;
}

bool AudioCache::hasPendingStores() const { return m_activeEncodes > 0 || !m_encodeQueue.isEmpty(); }

void AudioCache::indexClip(const QString &key, const QString &name, double seconds, const QString &joined) {
    const QString path = QDir(m_directory).filePath(name);
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO audio_clips (key, file, bytes, seconds, used_at, boundaries)"
                                 " VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(key);
    query.addBindValue(name);
    query.addBindValue(QFileInfo(path).size());
    query.addBindValue(seconds);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(joined);
    query.exec();
    evict();
    emit stored(key);
    emit changed();
}

void AudioCache::store(const QString &key, const QByteArray &pcm, int sampleRate, const QList<qint64> &boundaries) {
    if (pcm.isEmpty() || sampleRate <= 0)
        return;
    const double seconds = double(pcm.size()) / 2.0 / sampleRate;
    QStringList marks;
    for (qint64 boundary : boundaries)
        marks.append(QString::number(boundary));
    const QString joined = marks.isEmpty() ? QStringLiteral("") : marks.join(',');

    // Without ffmpeg the narration is kept as WAV, larger but just as good.
    if (m_ffmpeg.isEmpty()) {
        const QString name = key + QStringLiteral(".wav");
        QSaveFile file(QDir(m_directory).filePath(name));
        if (file.open(QIODevice::WriteOnly) && file.write(Speech::wav(pcm, sampleRate)) >= 0 && file.commit())
            indexClip(key, name, seconds, joined);
        else
            emit storeFailed(key);
        if (!hasPendingStores())
            emit storesIdle();
        return;
    }

    for (int i = int(m_encodeQueue.size()) - 1; i >= 0; --i)
        if (m_encodeQueue[i].key == key)
            m_encodeQueue.removeAt(i);
    m_encodeQueue.append({key, pcm, sampleRate, seconds, joined});
    pumpEncodes();
}

void AudioCache::pumpEncodes() {
    while (m_activeEncodes < maxConcurrentEncodes && !m_encodeQueue.isEmpty()) {
        int pick = -1;
        for (int i = 0; i < m_encodeQueue.size(); ++i) {
            if (!m_activeEncodeKeys.contains(m_encodeQueue[i].key)) {
                pick = i;
                break;
            }
        }
        if (pick < 0)
            break;
        EncodeJob job = m_encodeQueue.takeAt(pick);
        ++m_activeEncodes;
        m_activeEncodeKeys.insert(job.key);

        const QString name = job.key + QStringLiteral(".opus");
        const QString target = QDir(m_directory).filePath(name);
        const QString partial = QStringLiteral("%1.part.%2.%3")
                                    .arg(target)
                                    .arg(QCoreApplication::applicationPid())
                                    .arg(++m_tempSeq);

        auto *ffmpeg = new QProcess(this);
        m_encodePartials.insert(ffmpeg, partial);
        m_encodeKeys.insert(ffmpeg, job.key);
        ffmpeg->setProgram(m_ffmpeg);
        ffmpeg->setArguments({"-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar", QString::number(job.sampleRate),
                              "-ac", "1", "-i", "pipe:0", "-c:a", "libopus", "-b:a", "48k", "-application", "audio",
                              "-f", "ogg", "-y", partial});

        auto handled = std::make_shared<bool>(false);
        const QString key = job.key;
        const double secs = job.seconds;
        const QString joined = job.joined;
        auto finish = [this, ffmpeg, partial, target, name, key, secs, joined, handled](bool ok) {
            if (*handled)
                return;
            *handled = true;
            m_encodePartials.remove(ffmpeg);
            m_encodeKeys.remove(ffmpeg);
            m_activeEncodeKeys.remove(key);
            --m_activeEncodes;
            ffmpeg->deleteLater();
            if (ok && QFile::exists(partial)) {
                if (atomicReplace(partial, target)) {
                    indexClip(key, name, secs, joined);
                } else {
                    QFile::remove(partial);
                    emit storeFailed(key);
                }
            } else {
                QFile::remove(partial);
                emit storeFailed(key);
            }
            pumpEncodes();
            if (!hasPendingStores())
                emit storesIdle();
        };

        connect(ffmpeg, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
            finish(code == 0 && status == QProcess::NormalExit);
        });
        connect(ffmpeg, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false);
        });
        QByteArray pcm = std::move(job.pcm);
        connect(ffmpeg, &QProcess::started, ffmpeg, [ffmpeg, pcm = std::move(pcm)] {
            ffmpeg->write(pcm);
            ffmpeg->closeWriteChannel();
        });
        ffmpeg->start();
    }
}

void AudioCache::decode(const QString &file, int sampleRate, QObject *context,
                        std::function<void(const QByteArray &, bool)> done) {
    QObject *target = context ? context : this;
    QPointer<QObject> guard(target);
    auto defer = [target, guard, done](const QByteArray &pcm, bool ok) {
        QMetaObject::invokeMethod(
            target,
            [guard, done, pcm, ok] {
                if (guard)
                    done(pcm, ok);
            },
            Qt::QueuedConnection);
    };
    if (sampleRate <= 0) {
        defer({}, false);
        return;
    }
    if (file.endsWith(QStringLiteral(".wav"))) {
        QFile wav(file);
        QByteArray bytes = wav.open(QIODevice::ReadOnly) ? wav.readAll() : QByteArray();
        // A WAV this cache wrote: a 44-byte header, then the samples.
        const bool ok = bytes.size() > 44;
        defer(ok ? bytes.mid(44) : QByteArray(), ok);
        return;
    }
    if (m_ffmpeg.isEmpty()) {
        defer({}, false);
        return;
    }
    m_decodeQueue.append({file, sampleRate, QPointer<QObject>(target), std::move(done)});
    pumpDecodes();
}

void AudioCache::pumpDecodes() {
    while (m_activeDecodes < maxConcurrentDecodes && !m_decodeQueue.isEmpty()) {
        DecodeJob job = m_decodeQueue.takeFirst();
        if (job.context.isNull())
            continue;
        ++m_activeDecodes;
        auto *ffmpeg = new QProcess(this);
        m_decodeProcesses.append(ffmpeg);

        auto handled = std::make_shared<bool>(false);
        auto finish = [this, ffmpeg, guard = job.context, done = std::move(job.done), handled](bool ok, QByteArray pcm) {
            if (*handled)
                return;
            *handled = true;
            m_decodeProcesses.removeAll(ffmpeg);
            --m_activeDecodes;
            ffmpeg->deleteLater();
            if (guard)
                done(ok ? pcm : QByteArray(), ok);
            pumpDecodes();
        };

        connect(ffmpeg, &QProcess::finished, this, [ffmpeg, finish](int code, QProcess::ExitStatus status) {
            const bool exitedOk = code == 0 && status == QProcess::NormalExit;
            QByteArray pcm = exitedOk ? ffmpeg->readAllStandardOutput() : QByteArray();
            finish(exitedOk && !pcm.isEmpty(), std::move(pcm));
        });
        connect(ffmpeg, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false, {});
        });
        ffmpeg->start(m_ffmpeg, {"-hide_banner", "-loglevel", "error", "-i", job.file, "-f", "s16le", "-ac", "1", "-ar",
                                 QString::number(job.sampleRate), "pipe:1"});
    }
}

double AudioCache::megabytes() const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.exec(QStringLiteral("SELECT COALESCE(SUM(bytes), 0) FROM audio_clips"));
    return query.next() ? query.value(0).toDouble() / (1024.0 * 1024.0) : 0;
}

void AudioCache::evict() {
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery total(db);
    total.exec(QStringLiteral("SELECT COALESCE(SUM(bytes), 0) FROM audio_clips"));
    qint64 bytes = total.next() ? total.value(0).toLongLong() : 0;
    if (bytes <= m_cap)
        return;
    // Down to nine tenths of the cap, so eviction doesn't run on every clip.
    QSqlQuery oldest(db);
    oldest.exec(QStringLiteral("SELECT key, file, bytes FROM audio_clips ORDER BY used_at ASC"));
    QStringList gone;
    while (bytes > m_cap * 9 / 10 && oldest.next()) {
        QFile::remove(QDir(m_directory).filePath(oldest.value(1).toString()));
        bytes -= oldest.value(2).toLongLong();
        gone.append(oldest.value(0).toString());
    }
    if (gone.isEmpty())
        return;
    const bool inTx = db.transaction();
    QSqlQuery remove(db);
    remove.prepare(QStringLiteral("DELETE FROM audio_clips WHERE key = ?"));
    for (const QString &key : std::as_const(gone)) {
        remove.addBindValue(key);
        remove.exec();
    }
    if (inTx)
        db.commit();
}

void AudioCache::clear() {
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery files(db);
    files.exec(QStringLiteral("SELECT file FROM audio_clips"));
    while (files.next())
        QFile::remove(QDir(m_directory).filePath(files.value(0).toString()));
    const bool inTx = db.transaction();
    QSqlQuery(db).exec(QStringLiteral("DELETE FROM audio_clips"));
    QSqlQuery(db).exec(QStringLiteral("DELETE FROM narration_directions"));
    if (inTx)
        db.commit();
    cleanupPartials();
    emit changed();
}

double AudioCache::seconds(const QString &key) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT seconds FROM audio_clips WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toDouble() : 0;
}
