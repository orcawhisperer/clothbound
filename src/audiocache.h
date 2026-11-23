#pragma once

#include <QObject>
#include <functional>
#include <QString>

// Narration already made, kept so no passage is ever paid for twice: Opus
// files (48 kbps, about 0.36 MB a minute) under ~/.cache/dogear/audio,
// indexed in the library database, the least recently heard evicted past a
// size cap. Encoding runs in ffmpeg, off the UI thread.
class AudioCache : public QObject {
    Q_OBJECT
    Q_PROPERTY(double megabytes READ megabytes NOTIFY changed)

public:
    AudioCache(const QString &directory, const QString &databaseConnection, QObject *parent = nullptr,
               qint64 capBytes = 4ll * 1024 * 1024 * 1024);

    bool migrate();
    // The file holding this narration, or "" when it hasn't been made.
    QString lookup(const QString &key);
    // How long a kept clip is, in seconds (0 if it isn't kept).
    double seconds(const QString &key);
    // `boundaries`: where each sentence ends, in samples, when the voice said.
    void store(const QString &key, const QByteArray &pcm, int sampleRate, const QList<qint64> &boundaries = {});
    QList<qint64> boundaries(const QString &key);
    // A director's answer for a chapter, kept with the audio it shapes: a
    // chapter heard again is read the same way, and found in the cache.
    QString directions(const QString &key);
    // Reads a kept clip back as 16-bit mono PCM at `sampleRate`, off the UI
    // thread; `done` runs in `context`'s thread with ok false if it failed.
    void decode(const QString &file, int sampleRate, QObject *context,
                std::function<void(const QByteArray &pcm, bool ok)> done);
    void storeDirections(const QString &key, const QString &json);
    double megabytes() const;
    Q_INVOKABLE void clear();

    // The cache key for a piece of narration: everything that changes how it
    // sounds, so a different voice or provider never replays the wrong audio.
    static QString keyFor(const QString &provider, const QString &model, const QString &voice, const QString &style,
                          const QString &text);

signals:
    void changed();
    void stored(const QString &key);

private:
    void evict();
    bool hasBoundaries() const;

    QString m_directory;
    QString m_connection;
    qint64 m_cap;
    QString m_ffmpeg;
};
