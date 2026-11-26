#include "audio/audiobook.h"

#include "audio/audiocache.h"
#include "audio/download.h"
#include "library/library.h"
#include "reader/reader.h"
#include "audio/speech.h"

#include <QtGlobal>

#if !defined(CLOTHBOUND_HAS_DBUS) && defined(Q_OS_LINUX) && !defined(CLOTHBOUND_NO_DBUS)
#define CLOTHBOUND_HAS_DBUS 1
#endif

#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace {
constexpr int silenceSampleRate = 24000;

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

// Reads {channels, sampleRate} from a standard PCM WAV header; returns {0, 0} if non-standard.
QPair<int, int> wavFormat(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {0, 0};
    char hdr[36];
    if (f.read(hdr, sizeof(hdr)) < 28)
        return {0, 0};
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0 ||
        std::memcmp(hdr + 12, "fmt ", 4) != 0)
        return {0, 0};
    const quint16 audioFmt = qFromLittleEndian<quint16>(hdr + 20);
    const quint16 channels = qFromLittleEndian<quint16>(hdr + 22);
    const quint32 rate = qFromLittleEndian<quint32>(hdr + 24);
    if (audioFmt != 1 || channels == 0 || rate == 0)
        return {0, 0};
    return {int(channels), int(rate)};
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
    m_target.clear();
    m_exportBookId = 0;
    m_exportTitle.clear();
    m_exportAuthor.clear();
    m_exportCoverPath.clear();
    m_files.clear();
    m_chapters.clear();
    m_prepQueue.clear();
    m_prepTotal = 0;
    m_prepDone = 0;
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

QStringList Audiobook::macExecutableSearchDirs() {
    return {
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/local/bin"),
    };
}

QString Audiobook::findExecutableInPaths(const QString &name, const QStringList &fallbackDirs, bool searchSystemPath) {
    if (name.isEmpty())
        return {};
    if (searchSystemPath) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }
    if (!fallbackDirs.isEmpty())
        return QStandardPaths::findExecutable(name, fallbackDirs);
    return {};
}

QString Audiobook::findFfmpeg() {
#ifdef Q_OS_MACOS
    return findExecutableInPaths(QStringLiteral("ffmpeg"), macExecutableSearchDirs(), true);
#else
    return findExecutableInPaths(QStringLiteral("ffmpeg"), {}, true);
#endif
}

void Audiobook::fail(const QString &problem) {
    cleanupWork();
    m_problem = problem;
    m_busy = false;
    emit changed();
}

void Audiobook::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_cancelled = true;
    m_prepQueue.clear();
    if (m_ffmpeg)
        m_ffmpeg->kill();
}

void Audiobook::revealSaved() const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_saved.isEmpty() || !QFileInfo::exists(m_saved))
        return;
    const QString savedPath = m_saved;
    const QString folderPath = QFileInfo(savedPath).absolutePath();
#if defined(CLOTHBOUND_HAS_DBUS) && CLOTHBOUND_HAS_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                           QStringLiteral("/org/freedesktop/FileManager1"),
                                                           QStringLiteral("org.freedesktop.FileManager1"),
                                                           QStringLiteral("ShowItems"));
        call.setArguments({QStringList{QUrl::fromLocalFile(savedPath).toString()}, QString()});
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), const_cast<Audiobook *>(this));
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, folderPath] {
            const QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError())
                QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
        });
        return;
    }
#elif defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), savedPath}))
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
}

void Audiobook::save() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_busy)
        return;
    m_problem.clear();
    m_saved.clear();
    m_cancelled = false;
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty()) {
#ifdef Q_OS_MACOS
        fail(QStringLiteral("Saving an audiobook needs ffmpeg: install it with brew install ffmpeg."));
#else
        fail(tr("Saving an audiobook needs ffmpeg: install it with omarchy-pkg-add ffmpeg."));
#endif
        return;
    }
    // Snapshot book identity and metadata before any async prep runs so
    // switching books mid-export never corrupts the M4B or marks the wrong book.
    const int exportBookId = m_reader ? m_reader->bookId() : 0;
    const QString exportTitle = m_reader ? m_reader->title() : QString();
    const QString exportAuthor = m_reader ? m_reader->author() : QString();
    QString exportCoverPath;
    if (m_library && exportBookId > 0) {
        const QString coverName = m_library->record(exportBookId).cover;
        if (!coverName.isEmpty())
            exportCoverPath = QDir(m_library->coverDirectory()).filePath(coverName);
    }

    // The clips in order, and where each chapter begins — looked up in one
    // read-only batch query with zero per-clip database writes.
    const QMap<int, QStringList> planned = m_download->plannedKeys();
    QStringList allKeys;
    for (auto it = planned.cbegin(); it != planned.cend(); ++it)
        for (const QString &key : it.value())
            if (!key.startsWith(QLatin1String("pause:")))
                allKeys.append(key);
    const auto clipInfo = m_cache->clipInfoBatch(allKeys);

    struct Entry {
        QString file;
        int pauseMs = 0;
    };
    QList<Entry> entries;
    QList<Chapter> chapters;
    double atSec = 0.0;
    bool hasOpus = false;
    bool hasNonOpus = false;
    bool wavUniform = true;
    QPair<int, int> commonWavFmt{0, 0};

    for (auto it = planned.cbegin(); it != planned.cend(); ++it) {
        Chapter chapter;
        chapter.title = m_reader->titleOfChapter(it.key());
        if (chapter.title.isEmpty())
            chapter.title = tr("Chapter %1").arg(it.key() + 1);
        chapter.startMs = qint64(std::llround(atSec * 1000.0));
        for (const QString &key : it.value()) {
            if (key.startsWith(QLatin1String("pause:"))) {
                const int ms = qBound(100, key.mid(6).toInt(), 10000);
                entries.append({{}, ms});
                atSec += ms / 1000.0;
                continue;
            }
            const auto found = clipInfo.constFind(key);
            if (found == clipInfo.constEnd() || found->first.isEmpty()) {
                fail(tr("Some of the narration is no longer kept. Download it again, then save."));
                return;
            }
            const QString &path = found->first;
            if (path.endsWith(QLatin1String(".opus"), Qt::CaseInsensitive)) {
                hasOpus = true;
            } else {
                hasNonOpus = true;
                if (path.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)) {
                    const auto fmt = wavFormat(path);
                    if (fmt.first <= 0 || fmt.second <= 0)
                        wavUniform = false;
                    else if (commonWavFmt.first == 0)
                        commonWavFmt = fmt;
                    else if (fmt != commonWavFmt)
                        wavUniform = false;
                } else {
                    wavUniform = false;
                }
            }
            entries.append({path, 0});
            atSec += found->second;
        }
        chapter.endMs = qint64(std::llround(atSec * 1000.0));
        // Chapters the same as the one before (the contents point twice) are one.
        if (!chapters.isEmpty() && chapters.last().title == chapter.title)
            chapters.last().endMs = chapter.endMs;
        else if (chapter.endMs > chapter.startMs)
            chapters.append(chapter);
    }
    if (entries.isEmpty() || allKeys.isEmpty()) {
        fail(tr("Download the narration first."));
        return;
    }
    m_totalMs = qint64(std::llround(atSec * 1000.0));

    m_work = std::make_unique<QTemporaryDir>();
    if (!m_work->isValid()) {
        fail(tr("The audiobook couldn't be prepared."));
        return;
    }

    // Decide uniform concat target format:
    // - If every real clip is a uniform mono WAV, synthesize silence as matching WAV.
    // - Otherwise normalize non-Opus clips (when mixed .wav/.opus) and silence pauses
    //   into uniform 48k mono Opus files in m_work so ffmpeg's concat demuxer is lossless
    //   and never chokes on mismatched stream parameters.
    const bool allUniformMonoWav = !hasOpus && hasNonOpus && wavUniform && commonWavFmt.first == 1 && commonWavFmt.second > 0;
    QStringList files;
    files.reserve(entries.size());
    QList<PrepJob> prepJobs;
    QHash<int, QString> pauseFiles;
    int normSeq = 0;

    for (const Entry &entry : std::as_const(entries)) {
        if (entry.pauseMs > 0) {
            QString pausePath = pauseFiles.value(entry.pauseMs);
            if (pausePath.isEmpty()) {
                if (allUniformMonoWav) {
                    pausePath = m_work->filePath(QStringLiteral("pause-%1.wav").arg(entry.pauseMs));
                    const qint64 samples = qint64(entry.pauseMs) * commonWavFmt.second / 1000;
                    const QByteArray wavData = Speech::wav(QByteArray(int(samples * 2), '\0'), commonWavFmt.second);
                    QFile f(pausePath);
                    if (!f.open(QIODevice::WriteOnly) || f.write(wavData) != wavData.size() || !f.flush()) {
                        fail(tr("The audiobook couldn't be prepared."));
                        return;
                    }
                } else {
                    pausePath = m_work->filePath(QStringLiteral("pause-%1.opus").arg(entry.pauseMs));
                    prepJobs.append({{}, entry.pauseMs, pausePath});
                }
                pauseFiles.insert(entry.pauseMs, pausePath);
            }
            files.append(pausePath);
            continue;
        }
        if (allUniformMonoWav || entry.file.endsWith(QLatin1String(".opus"), Qt::CaseInsensitive)) {
            files.append(entry.file);
        } else {
            const QString normPath = m_work->filePath(QStringLiteral("norm-%1.opus").arg(++normSeq));
            prepJobs.append({entry.file, 0, normPath});
            files.append(normPath);
        }
    }

    QString music = qEnvironmentVariable("CLOTHBOUND_MUSIC_DIR").trimmed();
    if (music.isEmpty())
        music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty())
        music = QDir::homePath() + QStringLiteral("/Music");
    const QString folder = music + QStringLiteral("/Clothbound");
    if (!QDir().mkpath(folder)) {
        fail(tr("Couldn't create the folder %1.").arg(folder));
        return;
    }
    static const QRegularExpression badChars(QStringLiteral("[/\\\\:*?\"<>|\\x00-\\x1f\\x7f]"));
    QString name = exportTitle.left(80);
    name.replace(badChars, QStringLiteral(" "));
    name = name.simplified();
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    name = name.simplified();
    if (name.isEmpty())
        name = tr("Audiobook");

    // Append chapter range on partial exports (e.g. "Book (Ch 3–5).m4b") so
    // exporting one chapter or a subset never clobbers a full-book .m4b.
    const QVariantList spine = m_reader->manifest().value(QStringLiteral("spine")).toList();
    QList<int> allLinear;
    QList<int> allLinearRaw;
    for (int i = 0; i < spine.size(); ++i) {
        if (spine[i].toMap().value(QStringLiteral("linear")).toBool()) {
            allLinearRaw.append(i);
            if (m_reader->charactersIn(i) > 0)
                allLinear.append(i);
        }
    }
    const QList<int> &bookChapters = !allLinear.isEmpty() ? allLinear : allLinearRaw;
    const QList<int> exportedChapters = planned.keys();
    if (!bookChapters.isEmpty() && !exportedChapters.isEmpty() && exportedChapters.size() < bookChapters.size()) {
        const int firstIdx = int(bookChapters.indexOf(exportedChapters.first()));
        const int lastIdx = int(bookChapters.indexOf(exportedChapters.last()));
        const int firstNum = firstIdx >= 0 ? firstIdx + 1 : exportedChapters.first() + 1;
        const int lastNum = lastIdx >= 0 ? lastIdx + 1 : exportedChapters.last() + 1;
        if (firstNum == lastNum)
            name += QStringLiteral(" (Ch %1)").arg(firstNum);
        else
            name += QStringLiteral(" (Ch %1\u2013%2)").arg(firstNum).arg(lastNum);
    }

    const QString target = QDir(folder).filePath(name + QStringLiteral(".m4b"));
    const QString partial = target + QStringLiteral(".part.m4b");
    m_ffmpegPath = ffmpeg;
    m_target = target;
    m_partial = partial;
    m_exportBookId = exportBookId;
    m_exportTitle = exportTitle;
    m_exportAuthor = exportAuthor;
    m_exportCoverPath = exportCoverPath;
    m_files = std::move(files);
    m_chapters = std::move(chapters);
    m_prepQueue = std::move(prepJobs);
    m_prepTotal = int(m_prepQueue.size());
    m_prepDone = 0;
    m_stdoutBuf.clear();

    m_busy = true;
    m_progress = 0;
    emit changed();

    if (!m_prepQueue.isEmpty())
        runNextPrep();
    else
        startMux();
}

void Audiobook::runNextPrep() {
    if (m_cancelled) {
        fail(tr("Saving the audiobook was stopped."));
        return;
    }
    if (m_prepQueue.isEmpty()) {
        startMux();
        return;
    }
    const PrepJob job = m_prepQueue.takeFirst();
    auto *process = new QProcess(this);
    m_ffmpeg = process;
    process->setProgram(m_ffmpegPath);
    QByteArray silencePcm;
    if (job.pauseMs > 0) {
        const qint64 samples = qint64(job.pauseMs) * silenceSampleRate / 1000;
        silencePcm = QByteArray(int(samples * 2), '\0');
        process->setArguments({"-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar",
                               QString::number(silenceSampleRate), "-ac", "1", "-i", "pipe:0", "-c:a", "libopus",
                               "-b:a", "48k", "-application", "audio", "-f", "ogg", "-y", job.outputPath});
    } else {
        process->setArguments({"-hide_banner", "-loglevel", "error", "-i", job.inputPath, "-ac", "1", "-c:a",
                               "libopus", "-b:a", "48k", "-application", "audio", "-f", "ogg", "-y", job.outputPath});
    }

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
    connect(process, &QProcess::finished, this, [this, process, handled](int code, QProcess::ExitStatus status) {
        if (*handled)
            return;
        *handled = true;
        process->deleteLater();
        if (m_ffmpeg == process)
            m_ffmpeg = nullptr;
        if (m_cancelled || status != QProcess::NormalExit) {
            fail(tr("Saving the audiobook was stopped."));
            return;
        }
        if (code != 0) {
            fail(tr("ffmpeg couldn't prepare a clip: %1")
                     .arg(QString::fromUtf8(process->readAllStandardError()).trimmed().left(200)));
            return;
        }
        ++m_prepDone;
        if (m_prepTotal > 0) {
            m_progress = qBound(0.0, 0.15 * double(m_prepDone) / double(m_prepTotal), 0.15);
            emit changed();
        }
        runNextPrep();
    });
    if (!silencePcm.isEmpty()) {
        connect(process, &QProcess::started, process, [process, pcm = std::move(silencePcm)] {
            process->write(pcm);
            process->closeWriteChannel();
        });
    }
    process->start();
}

void Audiobook::startMux() {
    if (m_cancelled || !m_work || !m_work->isValid()) {
        fail(tr("Saving the audiobook was stopped."));
        return;
    }
    QFile list(m_work->filePath(QStringLiteral("clips.txt")));
    QFile meta(m_work->filePath(QStringLiteral("metadata.txt")));
    const QByteArray listData = concatList(m_files).toUtf8();
    const QByteArray metaData = metadata(m_exportTitle, m_exportAuthor, m_chapters).toUtf8();
    if (!list.open(QIODevice::WriteOnly) || list.write(listData) != listData.size() || !list.flush() ||
        !meta.open(QIODevice::WriteOnly) || meta.write(metaData) != metaData.size() || !meta.flush()) {
        fail(tr("The audiobook couldn't be prepared."));
        return;
    }
    list.close();
    meta.close();

    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
                          "-f", "concat", "-safe", "0", "-i", list.fileName(), "-i", meta.fileName()};
    const QString &cover = m_exportCoverPath;
    const bool hasCover = !cover.isEmpty() && QFileInfo::exists(cover);
    if (hasCover)
        arguments << "-i" << cover;
    arguments << "-map" << "0:a" << "-map_metadata" << "1" << "-map_chapters" << "1";
    if (hasCover)
        arguments << "-map" << "2:v" << "-c:v" << "mjpeg" << "-pix_fmt" << "yuvj420p" << "-disposition:v"
                  << "attached_pic";
    arguments << "-c:a" << "aac" << "-b:a" << "64k" << "-ac" << "1" << "-movflags" << "+faststart" << "-y" << m_partial;

    const double baseProgress = m_prepTotal > 0 ? 0.15 : 0.0;
    const double spanProgress = 1.0 - baseProgress;
    const QString partial = m_partial;
    const QString target = m_target;
    const int exportBookId = m_exportBookId;

    auto *process = new QProcess(this);
    m_ffmpeg = process;
    process->setProgram(m_ffmpegPath);
    process->setArguments(arguments);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, baseProgress, spanProgress] {
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
                    const double muxFrac = qBound(0.0, (us / 1000.0) / double(m_totalMs), 1.0);
                    const double next = baseProgress + spanProgress * muxFrac;
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
    connect(process, &QProcess::finished, this,
            [this, process, partial, target, exportBookId, handled](int code, QProcess::ExitStatus status) {
        if (*handled)
            return;
        *handled = true;
        process->deleteLater();
        if (m_ffmpeg == process)
            m_ffmpeg = nullptr;
        if (m_cancelled || code != 0 || status != QProcess::NormalExit) {
            fail((m_cancelled || status == QProcess::CrashExit)
                     ? tr("Saving the audiobook was stopped.")
                     : tr("ffmpeg couldn't make the audiobook: %1")
                           .arg(QString::fromUtf8(process->readAllStandardError()).trimmed().left(200)));
            return;
        }
        if (!Speech::atomicReplace(partial, target)) {
            fail(tr("Couldn't move the audiobook into %1.").arg(target));
            return;
        }
        m_partial.clear();
        cleanupWork();
        m_saved = target;
        if (m_library && exportBookId > 0)
            m_library->setBookSetting(exportBookId, QStringLiteral("audiobook_saved"), QStringLiteral("1"));
        m_progress = 1;
        m_busy = false;
        emit changed();
    });
    process->start();
}
