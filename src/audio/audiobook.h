#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <memory>

class AudioCache;
class Download;
class Library;
class QProcess;
class QTemporaryDir;
class Reader;

// A downloaded narration as one audiobook file (M4B): the clips joined in
// order, a chapter mark for each chapter, the title, author and cover, in
// the Music folder, for any player or phone. ffmpeg does the joining.
class Audiobook : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // Where the last one was saved.
    Q_PROPERTY(QString saved READ saved NOTIFY changed)

public:
    Audiobook(AudioCache *cache, Download *download, Reader *reader, Library *library, QObject *parent = nullptr);
    ~Audiobook() override;

    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString problem() const { return m_problem; }
    QString saved() const { return m_saved; }

    // The download's chapters, as an M4B in ~/Music/Clothbound.
    Q_INVOKABLE void save();
    Q_INVOKABLE void cancel();
    // Opens the file manager highlighting the last saved M4B file.
    Q_INVOKABLE void revealSaved() const;

    // ffmpeg's metadata file: the book and its chapters (title, start and
    // end in milliseconds), escaped as ffmpeg wants.
    struct Chapter {
        QString title;
        qint64 startMs = 0;
        qint64 endMs = 0;
    };
    static QString metadata(const QString &title, const QString &author, const QList<Chapter> &chapters);
    // ffmpeg's concat list of files.
    static QString concatList(const QStringList &files);

signals:
    void changed();

private:
    struct PrepJob {
        QString inputPath;  // source file to normalize, or empty for silence PCM
        int pauseMs = 0;    // >0 when synthesizing silence
        QString outputPath;
    };

    void runNextPrep();
    void startMux();
    void fail(const QString &problem);
    void cleanupWork();

    AudioCache *m_cache;
    Download *m_download;
    Reader *m_reader;
    Library *m_library;
    bool m_busy = false;
    double m_progress = 0;
    qint64 m_totalMs = 0;
    QString m_problem;
    QString m_saved;
    QString m_partial;
    QString m_target;
    QString m_ffmpegPath;
    QStringList m_files;
    QList<Chapter> m_chapters;
    QList<PrepJob> m_prepQueue;
    int m_prepTotal = 0;
    int m_prepDone = 0;
    bool m_cancelled = false;
    QByteArray m_stdoutBuf;
    std::unique_ptr<QTemporaryDir> m_work;
    QPointer<QProcess> m_ffmpeg;
};
