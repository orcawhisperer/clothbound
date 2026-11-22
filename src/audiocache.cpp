#include "audiocache.h"

#include "speech.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStandardPaths>

AudioCache::AudioCache(const QString &directory, const QString &databaseConnection, QObject *parent, qint64 capBytes)
    : QObject(parent), m_directory(directory), m_connection(databaseConnection), m_cap(capBytes),
      m_ffmpeg(QStandardPaths::findExecutable(QStringLiteral("ffmpeg"))) {
    QDir().mkpath(m_directory);
    migrate();
}

bool AudioCache::migrate() {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    return query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS audio_clips (key TEXT PRIMARY KEY, file TEXT NOT NULL,"
                                     " bytes INTEGER NOT NULL, seconds REAL NOT NULL, used_at INTEGER NOT NULL)")) &&
           query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS narration_directions (key TEXT PRIMARY KEY,"
                                     " json TEXT NOT NULL, made_at INTEGER NOT NULL)"));
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
        QSqlQuery remove(db);
        remove.prepare(QStringLiteral("DELETE FROM audio_clips WHERE key = ?"));
        remove.addBindValue(key);
        remove.exec();
        return {};
    }
    QSqlQuery touch(db);
    touch.prepare(QStringLiteral("UPDATE audio_clips SET used_at = ? WHERE key = ?"));
    touch.addBindValue(QDateTime::currentSecsSinceEpoch());
    touch.addBindValue(key);
    touch.exec();
    return file;
}

void AudioCache::store(const QString &key, const QByteArray &pcm, int sampleRate) {
    if (pcm.isEmpty())
        return;
    const double seconds = double(pcm.size()) / 2.0 / sampleRate;
    auto index = [this, key, seconds](const QString &name) {
        const QString path = QDir(m_directory).filePath(name);
        QSqlQuery query(QSqlDatabase::database(m_connection));
        query.prepare(QStringLiteral("INSERT OR REPLACE INTO audio_clips (key, file, bytes, seconds, used_at) VALUES (?, ?, ?, ?, ?)"));
        query.addBindValue(key);
        query.addBindValue(name);
        query.addBindValue(QFileInfo(path).size());
        query.addBindValue(seconds);
        query.addBindValue(QDateTime::currentSecsSinceEpoch());
        query.exec();
        evict();
        emit stored(key);
        emit changed();
    };

    // Without ffmpeg the narration is kept as WAV, larger but just as good.
    if (m_ffmpeg.isEmpty()) {
        const QString name = key + QStringLiteral(".wav");
        QSaveFile file(QDir(m_directory).filePath(name));
        if (file.open(QIODevice::WriteOnly) && file.write(Speech::wav(pcm, sampleRate)) >= 0 && file.commit())
            index(name);
        return;
    }
    const QString name = key + QStringLiteral(".opus");
    const QString target = QDir(m_directory).filePath(name);
    const QString partial = target + QStringLiteral(".part");
    auto *ffmpeg = new QProcess(this);
    ffmpeg->setProgram(m_ffmpeg);
    ffmpeg->setArguments({"-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar", QString::number(sampleRate),
                          "-ac", "1", "-i", "pipe:0", "-c:a", "libopus", "-b:a", "48k", "-application", "audio",
                          "-f", "ogg", "-y", partial});
    connect(ffmpeg, &QProcess::finished, this, [ffmpeg, partial, target, name, index](int code, QProcess::ExitStatus status) {
        ffmpeg->deleteLater();
        if (code == 0 && status == QProcess::NormalExit && QFile::exists(partial)) {
            QFile::remove(target);
            if (QFile::rename(partial, target))
                index(name);
        } else {
            QFile::remove(partial);
        }
    });
    connect(ffmpeg, &QProcess::started, ffmpeg, [ffmpeg, pcm] {
        ffmpeg->write(pcm);
        ffmpeg->closeWriteChannel();
    });
    ffmpeg->start();
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
    for (const QString &key : std::as_const(gone)) {
        QSqlQuery remove(db);
        remove.prepare(QStringLiteral("DELETE FROM audio_clips WHERE key = ?"));
        remove.addBindValue(key);
        remove.exec();
    }
}

void AudioCache::clear() {
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery files(db);
    files.exec(QStringLiteral("SELECT file FROM audio_clips"));
    while (files.next())
        QFile::remove(QDir(m_directory).filePath(files.value(0).toString()));
    QSqlQuery(db).exec(QStringLiteral("DELETE FROM audio_clips"));
    QSqlQuery(db).exec(QStringLiteral("DELETE FROM narration_directions"));
    emit changed();
}
