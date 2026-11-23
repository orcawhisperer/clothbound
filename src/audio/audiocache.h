#pragma once

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

class QProcess;

// Narration already made, kept so no passage is ever paid for twice: Opus
// files (48 kbps, about 0.36 MB a minute) under ~/.cache/clothbound/audio,
// indexed in the library database, the least recently heard evicted past a
// size cap. Encoding runs in ffmpeg, off the UI thread.
class AudioCache : public QObject {
    Q_OBJECT
    Q_PROPERTY(double megabytes READ megabytes NOTIFY changed)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY changed)
    Q_PROPERTY(bool clearing READ clearing NOTIFY changed)
    Q_PROPERTY(bool ffmpegAvailable READ ffmpegAvailable CONSTANT)
    Q_PROPERTY(QString audiobookFolder READ audiobookFolder CONSTANT)

public:
    AudioCache(const QString &directory, const QString &databaseConnection, QObject *parent = nullptr,
               qint64 capBytes = 4ll * 1024 * 1024 * 1024);
    ~AudioCache() override;

    bool migrate();
    // The file holding this narration, or "" when it hasn't been made.
    QString lookup(const QString &key);
    // Read-only lookup (no used_at write/fsync), for batch export checks.
    QString lookupPath(const QString &key) const;
    // Batch read-only lookup of {file, seconds} for many keys at once.
    QHash<QString, QPair<QString, double>> clipInfoBatch(const QStringList &keys) const;
    // How long a kept clip is, in seconds (0 if it isn't kept).
    double seconds(const QString &key);
    // Removes a broken or obsolete entry from disk and the index.
    void remove(const QString &key);
    // `boundaries`: where each sentence ends, in samples, when the voice said.
    void store(const QString &key, const QByteArray &pcm, int sampleRate, const QList<qint64> &boundaries = {});
    bool isStoring(const QString &key) const;
    bool hasPendingStores() const;
    QList<qint64> boundaries(const QString &key);
    // A director's answer for a chapter, kept with the audio it shapes: a
    // chapter heard again is read the same way, and found in the cache.
    QString directions(const QString &key);
    // Reads a kept clip back as 16-bit mono PCM at `sampleRate`, off the UI
    // thread; `done` runs in `context`'s thread with ok false if it failed.
    void decode(const QString &file, int sampleRate, QObject *context,
                std::function<void(const QByteArray &pcm, bool ok)> done);
    void storeDirections(const QString &key, const QString &json);
    double megabytes() const { return m_totalBytes / (1024.0 * 1024.0); }
    int clipCount() const { return m_clipCount; }
    bool clearing() const { return m_clearing; }
    bool ffmpegAvailable() const { return !m_ffmpeg.isEmpty(); }
    QString audiobookFolder() const;
    Q_INVOKABLE void openAudiobookFolder() const;
    Q_INVOKABLE void refreshStats();
    Q_INVOKABLE void clear();

    // The cache key for a piece of narration: everything that changes how it
    // sounds, so a different voice or provider never replays the wrong audio.
    static QString keyFor(const QString &provider, const QString &model, const QString &voice, const QString &style,
                          const QString &text);

signals:
    void changed();
    void stored(const QString &key);
    void storeFailed(const QString &key);
    void storesIdle();

private:
    struct EncodeJob {
        QString key;
        QByteArray pcm;
        int sampleRate = 24000;
        double seconds = 0;
        QString joined;
    };
    struct DecodeJob {
        QString file;
        int sampleRate = 24000;
        QPointer<QObject> context;
        std::function<void(const QByteArray &pcm, bool ok)> done;
    };

    void evict();
    void syncStats();
    bool hasBoundaries() const;
    void cleanupPartials();
    void indexClip(const QString &key, const QString &name, double seconds, const QString &joined);
    void pumpEncodes();
    void pumpDecodes();

    QString m_directory;
    QString m_connection;
    qint64 m_cap;
    QString m_ffmpeg;
    qint64 m_totalBytes = 0;
    int m_clipCount = 0;
    bool m_clearing = false;
    quint64 m_clearGen = 0;
    QFuture<void> m_clearFuture;
    QList<EncodeJob> m_encodeQueue;
    QList<DecodeJob> m_decodeQueue;
    QHash<QProcess *, QString> m_encodePartials;
    QHash<QProcess *, QString> m_encodeKeys;
    QList<QPointer<QProcess>> m_decodeProcesses;
    QSet<QString> m_activeEncodeKeys;
    int m_activeEncodes = 0;
    int m_activeDecodes = 0;
    quint64 m_tempSeq = 0;
};
