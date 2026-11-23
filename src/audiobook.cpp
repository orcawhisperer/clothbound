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
#include <cstdio>

namespace {

// ffmpeg's metadata escapes '=', ';', '#', '\' and line ends with a backslash.
QString escaped(const QString &value) {
    QString out;
    out.reserve(value.size() + 8);
    for (const QChar c : value) {
        if (c == QLatin1Char('\r'))
            continue;
        if (c == QLatin1Char('=') || c == QLatin1Char(';') || c == QLatin1Char('#') || c == QLatin1Char('\\') ||
            c == QLatin1Char('\n'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

bool atomicReplace(const QString &source, const QString &target) {
    const QByteArray src = QFile::encodeName(source);
    const QByteArray dst = QFile::encodeName(target);
    if (std::rename(src.constData(), dst.constData()) == 0)
        return true;
    QFile::remove(target);
    return QFile::rename(source, target);
}

}  // namespace

Audiobook::Audiobook(AudioCache *cache, Download *download, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_cache(cache), m_download(download), m_reader(reader), m_library(library) {
    if (m_reader) {
        connect(m_reader, &Reader::bookChanged, this, [this] {
            if (m_busy)
                return;
            if (!m_saved.isEmpty() || !m_problem.isEmpty() || m_progress != 0) {
                m_saved.clear();
                m_problem.clear();
                m_progress = 0;
                emit changed();
            }
        });
    }
}

Audiobook::~Audiobook() {
    if (m_ffmpeg) {
        m_ffmpeg->disconnect(this);
        if (m_ffmpeg->state() != QProcess::NotRunning) {
            m_ffmpeg->kill();
            m_ffmpeg->waitForFinished(1000);
        }
    }
    cleanupWork();
}

void Audiobook::cleanupWork() {
    m_work.reset();
    if (!m_partial.isEmpty()) {
        QFile::remove(m_partial);
        m_partial.clear();
    }
    m_stdoutBuf.clear();
}

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
    for (QString file : files) {
        file.remove(QLatin1Char('\r'));
        file.remove(QLatin1Char('\n'));
        out += QStringLiteral("file '%1'\n").arg(file.replace(QLatin1String("'"), QLatin1String("'\\''")));
    }
    return out;
}

void Audiobook::fail(const QString &problem) {
    cleanupWork();
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
    // The clips in order, and where each chapter begins — looked up in one
    // read-only batch query with zero per-clip database writes.
    const QMap<int, QStringList> planned = m_download->plannedKeys();
    QStringList allKeys;
    for (auto it = planned.cbegin(); it != planned.cend(); ++it)
        allKeys.append(it.value());
    const auto clipInfo = m_cache->clipInfoBatch(allKeys);

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
            const auto found = clipInfo.constFind(key);
            if (found == clipInfo.constEnd() || found->first.isEmpty()) {
                fail(tr("Some of the narration is no longer kept. Download it again, then save."));
                return;
            }
            files.append(found->first);
            at += qint64(found->second * 1000);
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

    m_work = std::make_unique<QTemporaryDir>();
    if (!m_work->isValid()) {
        fail(tr("The audiobook couldn't be prepared."));
        return;
    }
    QFile list(m_work->filePath(QStringLiteral("clips.txt")));
    QFile meta(m_work->filePath(QStringLiteral("metadata.txt")));
    const QByteArray listData = concatList(files).toUtf8();
    const QByteArray metaData = metadata(m_reader->title(), m_reader->author(), chapters).toUtf8();
    if (!list.open(QIODevice::WriteOnly) || list.write(listData) != listData.size() || !list.flush() ||
        !meta.open(QIODevice::WriteOnly) || meta.write(metaData) != metaData.size() || !meta.flush()) {
        fail(tr("The audiobook couldn't be prepared."));
        return;
    }
    list.close();
    meta.close();

    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty())
        music = QDir::homePath() + QStringLiteral("/Music");
    const QString folder = music + QStringLiteral("/Dogear");
    if (!QDir().mkpath(folder)) {
        fail(tr("Couldn't create the folder %1.").arg(folder));
        return;
    }
    static const QRegularExpression badChars(QStringLiteral("[/\\\\:*?\"<>|\\x00-\\x1f\\x7f]"));
    QString name = m_reader->title().left(80);
    name.replace(badChars, QStringLiteral(" "));
    name = name.simplified();
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    name = name.simplified();
    if (name.isEmpty())
        name = tr("Audiobook");
    const QString target = QDir(folder).filePath(name + QStringLiteral(".m4b"));
    const QString partial = target + QStringLiteral(".part.m4b");
    m_partial = partial;
    m_stdoutBuf.clear();

    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
                          "-f", "concat", "-safe", "0", "-i", list.fileName(), "-i", meta.fileName()};
    const QString coverName = m_library->record(m_reader->bookId()).cover;
    const QString cover = coverName.isEmpty() ? QString() : QDir(m_library->coverDirectory()).filePath(coverName);
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
        // Line-buffer stdout so partial "out_time_us=" lines are never split.
        m_stdoutBuf += process->readAllStandardOutput();
        bool updated = false;
        int nl = -1;
        while ((nl = m_stdoutBuf.indexOf('\n')) >= 0) {
            const QByteArray line = m_stdoutBuf.left(nl).trimmed();
            m_stdoutBuf.remove(0, nl + 1);
            if (line.startsWith("out_time_us=")) {
                bool ok = false;
                const qint64 us = line.mid(12).toLongLong(&ok);
                if (ok && us >= 0 && m_totalMs > 0) {
                    const double next = qBound(0.0, (us / 1000.0) / double(m_totalMs), 1.0);
                    if (next > m_progress) {
                        m_progress = next;
                        updated = true;
                    }
                }
            }
        }
        if (updated)
            emit changed();
    });
    auto handled = std::make_shared<bool>(false);
    connect(process, &QProcess::errorOccurred, this, [this, process, handled](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *handled)
            return;
        *handled = true;
        process->deleteLater();
        if (m_ffmpeg == process)
            m_ffmpeg = nullptr;
        fail(tr("ffmpeg couldn't be started."));
    });
    connect(process, &QProcess::finished, this, [this, process, partial, target, handled](int code, QProcess::ExitStatus status) {
        if (*handled)
            return;
        *handled = true;
        process->deleteLater();
        if (m_ffmpeg == process)
            m_ffmpeg = nullptr;
        if (code != 0 || status != QProcess::NormalExit) {
            fail(status == QProcess::CrashExit ? tr("Saving the audiobook was stopped.")
                                               : tr("ffmpeg couldn't make the audiobook: %1")
                                                     .arg(QString::fromUtf8(process->readAllStandardError()).trimmed().left(200)));
            return;
        }
        if (!atomicReplace(partial, target)) {
            fail(tr("Couldn't move the audiobook into %1.").arg(target));
            return;
        }
        m_partial.clear();
        m_work.reset();
        m_stdoutBuf.clear();
        m_saved = target;
        m_progress = 1;
        m_busy = false;
        emit changed();
    });
    process->start();
}
