#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class AudioCache;
class Download;
class Library;
class QProcess;
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

    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString problem() const { return m_problem; }
    QString saved() const { return m_saved; }

    // The download's chapters, as an M4B in ~/Music/Dogear.
    Q_INVOKABLE void save();
    Q_INVOKABLE void cancel();

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
    void fail(const QString &problem);

    AudioCache *m_cache;
    Download *m_download;
    Reader *m_reader;
    Library *m_library;
    bool m_busy = false;
    double m_progress = 0;
    qint64 m_totalMs = 0;
    QString m_problem;
    QString m_saved;
    QPointer<QProcess> m_ffmpeg;
};
