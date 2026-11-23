#include "audiobook.h"

#include "audiocache.h"
#include "download.h"
#include "library.h"
#include "reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

// ffmpeg's metadata escapes '=', ';', '#', '\' and line ends with a backslash.
QString escaped(const QString &value) {
    QString out;
    out.reserve(value.size() + 8);
    for (const QChar c : value) {
        if (c == QLatin1Char('=') || c == QLatin1Char(';') || c == QLatin1Char('#') || c == QLatin1Char('\\') ||
            c == QLatin1Char('\n'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

}  // namespace

Audiobook::Audiobook(AudioCache *cache, Download *download, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_cache(cache), m_download(download), m_reader(reader), m_library(library) {}

QString Audiobook::metadata(const QString &title, const QString &author, const QList<Chapter> &chapters) {
    QString out = QStringLiteral(";FFMETADATA1\ntitle=%1\nalbum=%1\nartist=%2\nalbum_artist=%2\ngenre=Audiobook\n")
                      .arg(escaped(title), escaped(author));
    for (const Chapter &chapter : chapters)
        out += QStringLiteral("\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=%1\nEND=%2\ntitle=%3\n")
                   .arg(chapter.startMs)
                   .arg(chapter.endMs)
                   .arg(escaped(chapter.title));
    return out;
}

QString Audiobook::concatList(const QStringList &files) {
    QString out = QStringLiteral("ffconcat version 1.0\n");
    for (QString file : files)
        out += QStringLiteral("file '%1'\n").arg(file.replace(QLatin1String("'"), QLatin1String("'\\''")));
    return out;
}

void Audiobook::fail(const QString &problem) {
    m_problem = problem;
    m_busy = false;
    emit changed();
}

void Audiobook::cancel() {
    if (m_ffmpeg)
        m_ffmpeg->kill();
}

void Audiobook::save() {
    if (m_busy)
        return;
    m_problem.clear();
    m_saved.clear();
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        fail(tr("Saving an audiobook needs ffmpeg: install it with omarchy-pkg-add ffmpeg."));
        return;
    }
    // The clips in order, and where each chapter begins.
    const QMap<int, QStringList> planned = m_download->plannedKeys();
    QStringList files;
    QList<Chapter> chapters;
    qint64 at = 0;
    for (auto it = planned.cbegin(); it != planned.cend(); ++it) {
        Chapter chapter;
        chapter.title = m_reader->titleOfChapter(it.key());
        if (chapter.title.isEmpty())
            chapter.title = tr("Chapter %1").arg(it.key() + 1);
        chapter.startMs = at;
        for (const QString &key : it.value()) {
            const QString file = m_cache->lookup(key);
            if (file.isEmpty()) {
                fail(tr("Some of the narration is no longer kept. Download it again, then save."));
                return;
            }
            files.append(file);
            at += qint64(m_cache->seconds(key) * 1000);
        }
        chapter.endMs = at;
        // Chapters the same as the one before (the contents point twice) are one.
        if (!chapters.isEmpty() && chapters.last().title == chapter.title)
            chapters.last().endMs = chapter.endMs;
        else if (chapter.endMs > chapter.startMs)
            chapters.append(chapter);
    }
    if (files.isEmpty()) {
        fail(tr("Download the narration first."));
        return;
    }
    m_totalMs = at;

    auto *work = new QTemporaryDir();
    QFile list(work->filePath(QStringLiteral("clips.txt")));
    QFile meta(work->filePath(QStringLiteral("metadata.txt")));
    if (!list.open(QIODevice::WriteOnly) || !meta.open(QIODevice::WriteOnly)) {
        delete work;
        fail(tr("The audiobook couldn't be prepared."));
        return;
    }
    list.write(concatList(files).toUtf8());
    list.close();
    meta.write(metadata(m_reader->title(), m_reader->author(), chapters).toUtf8());
    meta.close();

    const QString folder = QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Dogear");
    QDir().mkpath(folder);
    QString name = m_reader->title().left(80);
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral(" "));
    const QString target = QDir(folder).filePath(name.simplified() + QStringLiteral(".m4b"));
    const QString partial = target + QStringLiteral(".part.m4b");

    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
                          "-f", "concat", "-safe", "0", "-i", list.fileName(), "-i", meta.fileName()};
    const QString cover = m_library->record(m_reader->bookId()).cover;
    const bool hasCover = !cover.isEmpty() && QFileInfo::exists(cover);
    if (hasCover)
        arguments << "-i" << cover;
    arguments << "-map" << "0:a" << "-map_metadata" << "1" << "-map_chapters" << "1";
    if (hasCover)
        arguments << "-map" << "2:v" << "-c:v" << "mjpeg" << "-disposition:v" << "attached_pic";
    arguments << "-c:a" << "aac" << "-b:a" << "64k" << "-ac" << "1" << "-movflags" << "+faststart" << "-y" << partial;

    m_busy = true;
    m_progress = 0;
    emit changed();
    auto *process = new QProcess(this);
    m_ffmpeg = process;
    process->setProgram(ffmpeg);
    process->setArguments(arguments);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        // "out_time_ms=12345678" lines (microseconds, despite the name).
        const QString text = QString::fromUtf8(process->readAllStandardOutput());
        static const QRegularExpression time(QStringLiteral("out_time_us=(\\d+)"));
        auto it = time.globalMatch(text);
        while (it.hasNext()) {
            const double ms = it.next().captured(1).toDouble() / 1000.0;
            m_progress = m_totalMs > 0 ? qBound(0.0, ms / double(m_totalMs), 1.0) : 0;
        }
        emit changed();
    });
    connect(process, &QProcess::finished, this, [this, process, work, partial, target](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        delete work;
        if (code != 0 || status != QProcess::NormalExit) {
            QFile::remove(partial);
            fail(status == QProcess::CrashExit ? tr("Saving the audiobook was stopped.")
                                               : tr("ffmpeg couldn't make the audiobook: %1")
                                                     .arg(QString::fromUtf8(process->readAllStandardError()).trimmed().left(200)));
            return;
        }
        QFile::remove(target);
        QFile::rename(partial, target);
        m_saved = target;
        m_progress = 1;
        m_busy = false;
        emit changed();
    });
    process->start();
}
