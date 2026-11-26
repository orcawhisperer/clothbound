#include "reader/reader.h"

#include "formats/bookscheme.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "library/library.h"
#include "reader/readerimageprovider.h"

#if !defined(CLOTHBOUND_NO_MULTIMEDIA)
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>
#endif
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
QString typographySettingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini");
}

#if !defined(CLOTHBOUND_NO_MULTIMEDIA)
bool sfxSuppressedEnvironment() {
    if (qEnvironmentVariableIsSet("CLOTHBOUND_MUTE") || qEnvironmentVariableIsSet("DOGEAR_MUTE"))
        return true;
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return true;
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") || platform == QLatin1String("minimal");
}
#endif

// Ensures a shared PdfBook (and its thread-affine QPdfDocument) is always
// destroyed on the main/UI thread even when a worker outlives Reader::close().
struct MainThreadPdfRelease {
    std::shared_ptr<PdfBook> handle;
    ~MainThreadPdfRelease() {
        if (!handle)
            return;
        if (QCoreApplication *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
            auto keep = std::move(handle);
            QMetaObject::invokeMethod(
                app,
                [b = std::move(keep), app]() mutable {
                    Q_ASSERT(QThread::currentThread() == app->thread());
                    b.reset();
                },
                Qt::QueuedConnection);
        }
    }
};

QString imageExtensionForMime(const QString &mime) {
    const QString lower = mime.trimmed().toLower();
    if (lower == QLatin1String("image/png"))
        return QStringLiteral(".png");
    if (lower == QLatin1String("image/jpeg") || lower == QLatin1String("image/jpg"))
        return QStringLiteral(".jpg");
    if (lower == QLatin1String("image/webp"))
        return QStringLiteral(".webp");
    if (lower == QLatin1String("image/gif"))
        return QStringLiteral(".gif");
    if (lower == QLatin1String("image/svg+xml"))
        return QStringLiteral(".svg");
    if (lower == QLatin1String("image/bmp"))
        return QStringLiteral(".bmp");
    if (lower == QLatin1String("image/tiff"))
        return QStringLiteral(".tiff");
    return QStringLiteral(".img");
}

QVariantMap buildPeekDestinationMap(const PdfBook &pdf, const QRectF &pdfCrop, int destPage, double destY) {
    if (destPage < 0 || destPage >= pdf.pageCount())
        return {};
    constexpr double stripH = 0.38;
    const double topY = (destY < 0.0 || !std::isfinite(destY))
                            ? 0.0
                            : std::clamp(std::clamp(destY, 0.0, 1.0) - stripH * 0.5, 0.0, 1.0 - stripH);

    // Frame horizontally around the book's detected text bounds (with comfortable breathing room)
    // so tiny citations and figure labels read clearly inside the Quick-Look card.
    double normX = 0.0;
    double normW = 1.0;
    if (pdfCrop.width() >= 0.4 && pdfCrop.width() < 0.98) {
        normX = qMax(0.0, pdfCrop.x() - 0.02);
        normW = qMin(1.0 - normX, pdfCrop.width() + 0.04);
    }

    const QByteArray png = pdf.renderRegionPng(destPage, normX, topY, normW, stripH, 840);
    const QString imageDataUrl =
        png.isEmpty() ? QString() : QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());

    QString excerpt = pdf.pageTextInBand(destPage, topY, qMin(1.0, topY + stripH), 260);
    if (excerpt.isEmpty())
        excerpt = pdf.pageText(destPage);
    excerpt.replace(QString(QChar(0xFFFE)) + QLatin1String("\r\n"), QString())
        .replace(QLatin1String("\r\n"), QLatin1String(" "));
    excerpt = excerpt.simplified();
    if (excerpt.size() > 240) {
        const qsizetype cut = excerpt.lastIndexOf(QLatin1Char(' '), 240);
        excerpt = excerpt.left(cut > 140 ? cut : 240).trimmed() + QChar(0x2026);
    }

    const QString rawLabel = pdf.pageLabel(destPage).trimmed();
    const QString pageLabel = rawLabel.isEmpty() ? QString::number(destPage + 1) : rawLabel;
    return QVariantMap{
        {QStringLiteral("page"), destPage},
        {QStringLiteral("pageLabel"), pageLabel},
        {QStringLiteral("imageDataUrl"), imageDataUrl},
        {QStringLiteral("excerpt"), excerpt},
    };
}
}

#if !defined(CLOTHBOUND_NO_MULTIMEDIA)
// Tactile page-turn sound engine backed by Clothbound's single signature Public
// Domain hardcover page-flip WAV clip (:/audio/sfx/page_turn.wav), with minimal
// RIFF/WAVE PCM decoding, linear resampling for any audio output device format,
// gentler backward gain (0.82x), rapid-repeat throttling, automatic idle stream
// release, and silent headless/offscreen safety.
struct Reader::PageTurnSfx {
    static constexpr double kBackwardGain = 0.82;
    static constexpr qint64 kMinIntervalMs = 85;
    static constexpr int kIdleReleaseMs = 2000;

    explicit PageTurnSfx(QObject *owner) : m_owner(owner) {
        m_idleTimer.setSingleShot(true);
        m_idleTimer.setInterval(kIdleReleaseMs);
        QObject::connect(&m_idleTimer, &QTimer::timeout, m_owner, [this] { releaseSink(); });
    }

    ~PageTurnSfx() {
        m_idleTimer.stop();
        releaseSink();
    }

    void play(int direction) {
        if (sfxSuppressedEnvironment())
            return;
        if (m_throttle.isValid() && m_throttle.elapsed() < kMinIntervalMs)
            return;
        ensureSink();
        if (!m_sink)
            return;

        QByteArray *clip = clipFor(direction >= 0);
        if (!clip || clip->isEmpty())
            return;

        m_throttle.restart();
        m_idleTimer.stop();
        m_sink->reset();
        if (m_buffer.isOpen())
            m_buffer.close();
        m_buffer.setBuffer(clip);
        if (!m_buffer.open(QIODevice::ReadOnly))
            return;
        m_sink->start(&m_buffer);
        m_idleTimer.start();
    }

    // Hermetic test entry point: exercises throttle + WAV PCM decode/resample without opening an audio sink.
    bool playDryRun(int direction, QByteArray *pcmOut = nullptr) {
        if (m_throttle.isValid() && m_throttle.elapsed() < kMinIntervalMs)
            return false;
        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        prepareClips(fmt);
        const QByteArray *clip = clipFor(direction >= 0);
        if (!clip || clip->isEmpty())
            return false;
        m_throttle.restart();
        if (pcmOut)
            *pcmOut = *clip;
        return true;
    }

private:
    struct DecodedWav {
        int sampleRate = 0;
        QVector<float> mono;
    };

    static DecodedWav decodeWavPcm(const QByteArray &bytes) {
        DecodedWav out;
        if (bytes.size() < 44)
            return out;
        const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
        const auto readU16 = [p](qsizetype off) -> uint16_t {
            return uint16_t(p[off]) | (uint16_t(p[off + 1]) << 8);
        };
        const auto readU32 = [p](qsizetype off) -> uint32_t {
            return uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8) | (uint32_t(p[off + 2]) << 16) |
                   (uint32_t(p[off + 3]) << 24);
        };
        if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0)
            return out;

        uint16_t audioFormat = 0;
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        bool hasFmt = false;
        qsizetype dataOffset = -1;
        qsizetype dataSize = 0;

        qsizetype pos = 12;
        while (pos + 8 <= bytes.size()) {
            const uint32_t chunkSize = readU32(pos + 4);
            const qsizetype payload = pos + 8;
            const qsizetype avail = qMin<qsizetype>(qsizetype(chunkSize), bytes.size() - payload);
            if (std::memcmp(p + pos, "fmt ", 4) == 0 && avail >= 16) {
                audioFormat = readU16(payload);
                channels = readU16(payload + 2);
                sampleRate = readU32(payload + 4);
                bitsPerSample = readU16(payload + 14);
                hasFmt = true;
            } else if (std::memcmp(p + pos, "data", 4) == 0 && avail > 0) {
                dataOffset = payload;
                dataSize = avail;
            }
            const qsizetype step = qsizetype(chunkSize) + qsizetype(chunkSize & 1u);
            if (payload > bytes.size() - step)
                break;
            pos = payload + step;
        }

        if (!hasFmt || audioFormat != 1 || channels < 1 || channels > 8 || bitsPerSample != 16 || sampleRate < 8000 ||
            sampleRate > 192000 || dataOffset < 0)
            return out;

        const qsizetype frameBytes = qsizetype(channels) * 2;
        const qsizetype frames = dataSize / frameBytes;
        if (frames <= 0)
            return out;

        out.sampleRate = int(sampleRate);
        out.mono.resize(frames);
        const unsigned char *src = p + dataOffset;
        const float invChannels = 1.0f / float(channels);
        for (qsizetype i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (uint16_t c = 0; c < channels; ++c) {
                const qsizetype off = (i * qsizetype(channels) + qsizetype(c)) * 2;
                const int16_t sample = int16_t(uint16_t(src[off]) | (uint16_t(src[off + 1]) << 8));
                acc += float(sample) / 32768.0f;
            }
            out.mono[i] = acc * invChannels;
        }
        return out;
    }

    static QByteArray readClipBytes() {
        const QString rel = QStringLiteral("audio/sfx/page_turn.wav");
        const QStringList candidates = {
            QStringLiteral(":/") + rel,
            QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()).filePath(QStringLiteral("../") + rel),
            QCoreApplication::applicationDirPath() + QStringLiteral("/../../src/") + rel,
        };
        for (const QString &path : candidates) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly))
                return file.readAll();
        }
        return {};
    }

    static QByteArray renderClip(const DecodedWav &wav, const QAudioFormat &fmt, double gain) {
        if (wav.mono.isEmpty() || wav.sampleRate <= 0)
            return {};
        const int dstRate = qBound(8000, fmt.sampleRate(), 192000);
        const int channels = qBound(1, fmt.channelCount(), 8);
        const int bytesPerSample = fmt.bytesPerSample();
        if (bytesPerSample <= 0)
            return {};

        const qsizetype srcCount = wav.mono.size();
        const qsizetype outFrames =
            (dstRate == wav.sampleRate)
                ? srcCount
                : qMax<qsizetype>(1, qsizetype(std::llround(double(srcCount) * double(dstRate) / double(wav.sampleRate))));
        const double rateRatio = double(wav.sampleRate) / double(dstRate);

        QByteArray pcm;
        pcm.resize(outFrames * channels * bytesPerSample);
        char *dst = pcm.data();

        for (qsizetype i = 0; i < outFrames; ++i) {
            double sample = 0.0;
            if (dstRate == wav.sampleRate) {
                sample = double(wav.mono[i]);
            } else {
                const double srcPos = double(i) * rateRatio;
                const qsizetype lo = qBound<qsizetype>(0, qsizetype(std::floor(srcPos)), srcCount - 1);
                const qsizetype hi = qMin<qsizetype>(lo + 1, srcCount - 1);
                const double frac = srcPos - double(lo);
                sample = double(wav.mono[lo]) * (1.0 - frac) + double(wav.mono[hi]) * frac;
            }
            const double v = std::clamp(sample * gain, -1.0, 1.0);
            for (int c = 0; c < channels; ++c) {
                switch (fmt.sampleFormat()) {
                case QAudioFormat::Int16: {
                    const qint16 s = qint16(std::clamp(std::lround(v * 32767.0), -32768L, 32767L));
                    std::memcpy(dst, &s, sizeof(s));
                    dst += sizeof(s);
                    break;
                }
                case QAudioFormat::Float: {
                    const float s = float(v);
                    std::memcpy(dst, &s, sizeof(s));
                    dst += sizeof(s);
                    break;
                }
                case QAudioFormat::Int32: {
                    const qint32 s = qint32(std::clamp(std::llround(v * 2147483647.0), -2147483648LL, 2147483647LL));
                    std::memcpy(dst, &s, sizeof(s));
                    dst += sizeof(s);
                    break;
                }
                case QAudioFormat::UInt8: {
                    const quint8 s = quint8(std::clamp(std::lround((v * 0.5 + 0.5) * 255.0), 0L, 255L));
                    std::memcpy(dst, &s, sizeof(s));
                    dst += sizeof(s);
                    break;
                }
                default:
                    return {};
                }
            }
        }
        return pcm;
    }

    QByteArray *clipFor(bool forward) {
        QByteArray &clip = forward ? m_forwardClip : m_backwardClip;
        return clip.isEmpty() ? nullptr : &clip;
    }

    void ensureSourceLoaded() {
        if (m_sourceLoaded)
            return;
        m_sourceLoaded = true;
        m_source = decodeWavPcm(readClipBytes());
    }

    void prepareClips(const QAudioFormat &fmt) {
        ensureSourceLoaded();
        if (m_preparedFormat == fmt && !m_forwardClip.isEmpty())
            return;
        m_preparedFormat = fmt;
        m_forwardClip = renderClip(m_source, fmt, 1.0);
        m_backwardClip = renderClip(m_source, fmt, kBackwardGain);
    }

    void ensureSink() {
        if (m_sink)
            return;
        if (!m_devices) {
            m_devices = std::make_unique<QMediaDevices>();
            QObject::connect(m_devices.get(), &QMediaDevices::audioOutputsChanged, m_owner, [this] { handleOutputsChanged(); });
        }
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            return;
        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(fmt))
            fmt = device.preferredFormat();
        if (!fmt.isValid() || fmt.sampleRate() <= 0 || fmt.channelCount() <= 0 || fmt.bytesPerSample() <= 0)
            return;
        prepareClips(fmt);
        if (m_forwardClip.isEmpty())
            return;
        m_outputDevice = device;
        m_sink = new QAudioSink(device, fmt, m_owner);
        m_sink->setBufferSize(qMax(1024, fmt.bytesForDuration(60000)));
        QObject::connect(m_sink, &QAudioSink::stateChanged, m_owner, [this](QAudio::State state) {
            if (!m_sink)
                return;
            if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
                releaseSink();
                return;
            }
            if (state == QAudio::IdleState)
                m_idleTimer.start();
        });
    }

    void releaseSink() {
        m_idleTimer.stop();
        if (m_sink) {
            m_sink->disconnect(m_owner);
            m_sink->stop();
            delete m_sink;
            m_sink = nullptr;
        }
        if (m_buffer.isOpen())
            m_buffer.close();
    }

    void handleOutputsChanged() {
        if (!m_sink)
            return;
        const QAudioDevice current = QMediaDevices::defaultAudioOutput();
        if (current != m_outputDevice || m_sink->error() != QAudio::NoError)
            releaseSink();
    }

    QObject *m_owner;
    std::unique_ptr<QMediaDevices> m_devices;
    QAudioDevice m_outputDevice;
    QAudioSink *m_sink = nullptr;
    QBuffer m_buffer;
    bool m_sourceLoaded = false;
    DecodedWav m_source;
    QAudioFormat m_preparedFormat;
    QByteArray m_forwardClip;
    QByteArray m_backwardClip;
    QElapsedTimer m_throttle;
    QTimer m_idleTimer;
};
#else
struct Reader::PageTurnSfx {};
#endif

Reader::Reader(Library *library, BookScheme *scheme, QObject *parent)
    : QObject(parent), m_library(library), m_scheme(scheme) {
    QSettings settings(typographySettingsPath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("typography"));
    m_fontWeight = qBound(300, settings.value(QStringLiteral("fontWeight"), 400).toInt(), 700);
    m_paragraphSpacing = qBound(0.0, settings.value(QStringLiteral("paragraphSpacing"), 0.0).toDouble(), 1.2);
    settings.endGroup();

    // Positions are saved shortly after the reader settles on a page, so a
    // burst of page turns writes once.
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Reader::savePosition);
    m_timeFlush.setInterval(60000);
    connect(&m_timeFlush, &QTimer::timeout, this, &Reader::flushReadingTime);
    if (m_library)
        connect(m_library, &Library::bookPositionChanged, this, &Reader::handleExternalPositionChanged);
}

Reader::~Reader() { close(); }

bool Reader::openBook(int bookId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_library) {
        m_error = tr("That book isn't in the library.");
        return false;
    }
    const BookRecord record = m_library->record(bookId);
    if (!record.id) {
        m_error = tr("That book isn't in the library.");
        return false;
    }
    if (record.format == QStringLiteral("pdf"))
        return openPdf(bookId);
    if (record.format != QStringLiteral("epub")) {
        m_error = tr("Clothbound reads EPUB and PDF books.");
        return false;
    }
    auto book = std::make_shared<EpubBook>(record.path);
    if (!book->open()) {
        m_error = book->errorString();
        return false;
    }
    if (book->isDrmProtected()) {
        m_error = tr("This book is protected by DRM, so it can't be opened.");
        return false;
    }
    close();

    m_book = book;
    m_token = m_scheme ? m_scheme->add(book) : QUuid::createUuid().toString(QUuid::Id128);
    ReaderImageProvider::add(m_token, book);
    m_bookId = bookId;
    ++m_session;
    m_activeSession->store(m_session, std::memory_order_relaxed);
    m_format = record.format;
    m_title = record.title;
    m_author = record.author;
    m_language = book->metadata().language.isEmpty() ? record.language : book->metadata().language;
    m_detectedDoi = PdfBook::extractDoi(record.isbn + QLatin1Char(' ') + record.description);
    m_detectedArxiv = PdfBook::extractArxivId(record.description);

    const auto &spine = book->spine();
    QVariantList spineList;
    m_linear.clear();
    m_firstLinear = -1;
    m_lastLinear = -1;
    for (int i = 0; i < spine.size(); ++i) {
        spineList.append(QVariantMap{
            {"href", QString::fromUtf8(QUrl::toPercentEncoding(spine[i].path, "/"))},
            {"linear", spine[i].linear},
        });
        m_linear.append(spine[i].linear);
        if (spine[i].linear) {
            if (m_firstLinear < 0)
                m_firstLinear = i;
            m_lastLinear = i;
        }
    }
    if (m_firstLinear < 0)
        m_firstLinear = 0;
    if (m_lastLinear < 0)
        m_lastLinear = qMax(0, int(spine.size()) - 1);

    // Each chapter is titled by the first contents entry that points into
    // it; chapters the contents skip carry the title before them.
    m_chapterTitles = QStringList(spine.size(), QString());
    m_contents.clear();
    m_contentsChapter.clear();
    for (const auto &entry : book->toc()) {
        m_contents.append(QVariantMap{
            {"title", entry.title}, {"depth", entry.depth}, {"chapter", entry.spine}, {"fragment", entry.fragment}});
        m_contentsChapter.append(entry.spine);
        if (entry.spine >= 0 && entry.spine < m_chapterTitles.size() && m_chapterTitles[entry.spine].isEmpty())
            m_chapterTitles[entry.spine] = entry.title;
    }
    for (int i = 1; i < m_chapterTitles.size(); ++i)
        if (m_chapterTitles[i].isEmpty())
            m_chapterTitles[i] = m_chapterTitles[i - 1];

    QVariantList pageTargets;
    m_pageTargets.clear();
    for (const auto &target : book->pageList()) {
        if (target.spine < 0)
            continue;
        pageTargets.append(QVariantMap{{"chapter", target.spine}, {"label", target.label}, {"fragment", target.fragment}});
        m_pageTargets.append({target.spine, target.label});
    }
    m_hasPageList = !m_pageTargets.isEmpty();

    m_manifest = QVariantMap{
        {"base", BookScheme::baseUrl(m_token)},
        {"spine", spineList},
        {"pageTargets", pageTargets},
        {"language", m_language},
        {"rtl", book->isRightToLeft()},
        {"title", m_title},
        {"session", double(m_session)},
    };

    m_lengths = m_library->chapterLengths(bookId);
    if (m_lengths.size() != spine.size()) {
        m_lengths.clear();
        measureChapters();
    }

    // Where the reader left off, or the first page.
    m_startPosition = QVariantMap{{"chapter", 0}, {"offset", 0}};
    const QJsonObject saved = QJsonDocument::fromJson(record.position.toUtf8()).object();
    if (saved.contains("chapter")) {
        const int chapter = saved.value("chapter").toInt();
        if (chapter >= 0 && chapter < spine.size())
            m_startPosition = QVariantMap{{"chapter", chapter},
                                          {"offset", saved.value("offset").toDouble()},
                                          {"snippet", saved.value("snippet").toString()}};
    }
    m_chapter = m_startPosition.value("chapter").toInt();
    m_offset = m_startPosition.value("offset").toLongLong();
    m_pageEnd = 0;
    m_snippet.clear();
    m_relocationSeq = 0;
    m_page = 0;
    m_pages = 1;
    m_visibleColumns = 1;
    m_visiblePageEnd = -1;
    m_progress = record.progress;
    m_pageLabel.clear();
    m_atEnd = false;
    m_rtl = book->isRightToLeft();
    m_canGoBack = false;
    m_canGoForward = false;
    m_backLabel.clear();
    loadBookAppearance(bookId);

    m_previousOpenedAt = record.openedAt;
    updateBookPages();
    m_library->markOpened(bookId);
    m_lastGlobal = -1;
    m_sinceTurn.start();
    m_timeFlush.start();
    emit bookChanged();
    emit appearanceChanged();
    emit locationChanged();
    return true;
}

void Reader::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId)
        return;
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        savePosition();
    }
    flushReadingTime();
    m_timeFlush.stop();
    ++m_session;
    m_activeSession->store(m_session, std::memory_order_relaxed);
    m_latestPeekReqId = 0;
    if (m_scheme && !m_token.isEmpty())
        m_scheme->remove(m_token);
    ReaderImageProvider::remove(m_token);
    PdfPages::remove(m_token);
    m_lightboxDir.reset();
    m_book.reset();
    m_pdf.reset();
    m_pdfCrop = QRectF(0, 0, 1, 1);
    m_token.clear();
    m_bookId = 0;
    m_format.clear();
    m_detectedDoi.clear();
    m_detectedArxiv.clear();
    m_manifest.clear();
    m_contents.clear();
    m_chapterTitles.clear();
    m_lengths.clear();
    m_pageTargets.clear();
    m_hasPageList = false;
    m_firstLinear = 0;
    m_lastLinear = 0;
    m_chapter = 0;
    m_offset = 0;
    m_pageEnd = 0;
    m_snippet.clear();
    m_relocationSeq = 0;
    m_page = 0;
    m_pages = 1;
    m_bookPageStart = 1;
    m_bookPageEnd = 1;
    m_bookPages = 1;
    m_visibleColumns = 1;
    m_visiblePageEnd = -1;
    m_previousOpenedAt = 0;
    m_progress = 0;
    m_pageLabel.clear();
    m_atEnd = false;
    m_rtl = false;
    m_canGoBack = false;
    m_canGoForward = false;
    m_backLabel.clear();
    m_bookAppearanceOverride = false;
    m_bookAppearance.clear();
    emit bookChanged();
    emit appearanceChanged();
    emit locationChanged();
}

void Reader::updateBookPages() {
    if (m_pdf) {
        m_bookPages = qMax(1, chapterCount());
        m_bookPageStart = qBound(1, m_chapter + 1, m_bookPages);
        const int endIndex = m_visiblePageEnd >= m_chapter ? m_visiblePageEnd : m_chapter;
        m_bookPageEnd = qBound(m_bookPageStart, endIndex + 1, m_bookPages);
        return;
    }
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0) {
        m_bookPages = qMax(1, m_pages);
        m_bookPageStart = qBound(1, m_page + 1, m_bookPages);
        m_bookPageEnd = m_bookPageStart;
        return;
    }
    constexpr double kCharsPerBookPage = 1650.0;
    m_bookPages = qMax(1, int(std::ceil(double(total) / kCharsPerBookPage)));
    if (m_atEnd) {
        m_bookPageEnd = m_bookPages;
        m_bookPageStart = (m_visibleColumns > 1 && m_bookPages > 1) ? (m_bookPages - 1) : m_bookPages;
        return;
    }
    const qint64 startGlobal = globalOffset(m_chapter, m_offset);
    int startPage = qBound(1, int(std::floor(double(startGlobal) / kCharsPerBookPage)) + 1, m_bookPages);
    int endPage = startPage;
    if (m_visibleColumns > 1 && m_bookPages > 1) {
        if (startPage >= m_bookPages) {
            startPage = m_bookPages - 1;
            endPage = m_bookPages;
        } else {
            endPage = startPage + 1;
        }
    }
    m_bookPageStart = startPage;
    m_bookPageEnd = endPage;
}

void Reader::measureChapters() {
    // Books added outside a scan haven't had their chapters measured yet;
    // the reader can start meanwhile, with progress by chapters.
    auto book = m_book;
    const int bookId = m_bookId;
    auto *watcher = new QFutureWatcher<QList<int>>(this);
    connect(watcher, &QFutureWatcher<QList<int>>::finished, this, [this, watcher, bookId] {
        watcher->deleteLater();
        const QList<int> lengths = watcher->result();
        if (m_library)
            m_library->setChapterLengths(bookId, lengths);
        if (bookId != m_bookId)
            return;
        m_lengths = lengths;
        m_progress = fractionAt(m_chapter, m_offset);
        updateBookPages();
        emit lengthsChanged();
        emit locationChanged();
    });
    watcher->setFuture(QtConcurrent::run([book] {
        QList<int> lengths;
        for (int i = 0; i < book->spine().size(); ++i)
            lengths.append(int(book->chapterText(i).size()));
        return lengths;
    }));
}

qint64 Reader::globalOffset(int chapter, qint64 offset) const {
    qint64 before = 0;
    for (int i = 0; i < chapter && i < m_lengths.size(); ++i)
        before += m_lengths[i];
    const qint64 length = m_lengths.value(chapter, 0);
    return before + qBound<qint64>(0, offset, length > 0 ? length : offset);
}

double Reader::fractionAt(int chapter, qint64 offset) const {
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0) {
        const int count = qMax(1, chapterCount());
        return qBound(0.0, (chapter + double(m_page) / qMax(1, m_pages)) / count, 1.0);
    }
    return qBound(0.0, double(globalOffset(chapter, offset)) / double(total), 1.0);
}

QVariantMap Reader::targetForFraction(double fraction) const {
    fraction = qBound(0.0, fraction, 1.0);
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0) {
        const int chapter = qMin(chapterCount() - 1, int(fraction * chapterCount()));
        return {{"chapter", qMax(0, chapter)}, {"offset", 0}};
    }
    qint64 wanted = qint64(fraction * total), before = 0;
    for (int i = 0; i < m_lengths.size(); ++i) {
        if (wanted < before + m_lengths[i] || i == m_lengths.size() - 1)
            return {{"chapter", i}, {"offset", double(qMax<qint64>(0, wanted - before))}};
        before += m_lengths[i];
    }
    return {{"chapter", 0}, {"offset", 0}};
}

QVariantMap Reader::targetForPageLabel(const QString &label) const {
    const QString wanted = label.trimmed();
    for (const auto &[chapter, pageLabel] : m_pageTargets)
        if (pageLabel.compare(wanted, Qt::CaseInsensitive) == 0)
            return {{"chapter", chapter}, {"pageLabel", pageLabel}};
    // A number the page list skips lands on the nearest page before it.
    bool ok = false;
    const int number = wanted.toInt(&ok);
    if (!ok)
        return {};
    int bestChapter = -1, bestNumber = -1;
    QString bestLabel;
    for (const auto &[chapter, pageLabel] : m_pageTargets) {
        bool isNumber = false;
        const int n = pageLabel.toInt(&isNumber);
        if (isNumber && n <= number && n > bestNumber) {
            bestNumber = n;
            bestChapter = chapter;
            bestLabel = pageLabel;
        }
    }
    if (bestChapter < 0)
        return {};
    return {{"chapter", bestChapter}, {"pageLabel", bestLabel}};
}

QVariantList Reader::chapterMarks() const {
    QVariantList marks;
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0)
        return marks;
    qint64 before = 0;
    for (int i = 0; i < m_lengths.size(); ++i) {
        // Only chapters the contents name, and not too close together.
        const double at = double(before) / double(total);
        if (i > 0 && m_contentsChapter.contains(i) && (marks.isEmpty() || at - marks.last().toDouble() > 0.004))
            marks.append(at);
        before += m_lengths[i];
    }
    return marks;
}

int Reader::contentsIndex() const {
    int found = -1;
    for (int i = 0; i < m_contentsChapter.size(); ++i)
        if (m_contentsChapter[i] >= 0 && m_contentsChapter[i] <= m_chapter)
            found = i;
    return found;
}

int Reader::minutesLeftInChapter() const {
    if (m_atEnd || !m_library)
        return 0;
    const double pace = m_library->charsPerMinute();
    const qint64 length = m_lengths.value(m_chapter, 0);
    if (length <= 0 || pace <= 0)
        return 0;
    const qint64 remaining = length - qMin(m_offset, length);
    if (remaining <= 0)
        return 0;
    return qMax(1, int(std::ceil(remaining / pace)));
}

int Reader::minutesLeftInBook() const {
    if (m_atEnd || !m_library)
        return 0;
    const double pace = m_library->charsPerMinute();
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0 || pace <= 0)
        return 0;
    const qint64 remaining = total - globalOffset(m_chapter, m_offset);
    if (remaining <= 0)
        return 0;
    return qMax(1, int(std::ceil(remaining / pace)));
}

void Reader::setFontWeight(int weight) {
    Q_ASSERT(QThread::currentThread() == thread());
    weight = qBound(300, weight, 700);
    if (weight == m_fontWeight)
        return;
    m_fontWeight = weight;
    QSettings settings(typographySettingsPath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("typography"));
    settings.setValue(QStringLiteral("fontWeight"), m_fontWeight);
    settings.endGroup();
    emit appearanceChanged();
}

void Reader::setParagraphSpacing(double spacing) {
    Q_ASSERT(QThread::currentThread() == thread());
    spacing = std::round(qBound(0.0, spacing, 1.2) * 100.0) / 100.0;
    if (qFuzzyCompare(spacing + 1.0, m_paragraphSpacing + 1.0))
        return;
    m_paragraphSpacing = spacing;
    QSettings settings(typographySettingsPath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("typography"));
    settings.setValue(QStringLiteral("paragraphSpacing"), m_paragraphSpacing);
    settings.endGroup();
    emit appearanceChanged();
}

void Reader::loadBookAppearance(int bookId) {
    m_bookAppearance.clear();
    m_bookAppearanceOverride = false;
    if (!bookId || !m_library)
        return;
    const QString raw = m_library->bookSetting(bookId, QStringLiteral("appearance"));
    if (raw.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject())
        return;
    const QVariantMap map = doc.object().toVariantMap();
    if (map.isEmpty())
        return;
    m_bookAppearance = map;
    m_bookAppearanceOverride = true;
}

void Reader::setBookAppearanceOverride(bool enabled) {
    if (!enabled) {
        clearBookAppearance();
        return;
    }
    if (!m_bookId || m_bookAppearanceOverride)
        return;
    if (m_bookAppearance.isEmpty()) {
        m_bookAppearance = QVariantMap{
            {QStringLiteral("fontWeight"), m_fontWeight},
            {QStringLiteral("paragraphSpacing"), m_paragraphSpacing},
        };
    }
    saveBookAppearance(m_bookAppearance);
}

void Reader::saveBookAppearance(const QVariantMap &appearance) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId || !m_library)
        return;
    QVariantMap cleaned = appearance;
    if (cleaned.contains(QStringLiteral("fontWeight")))
        cleaned[QStringLiteral("fontWeight")] = qBound(300, cleaned.value(QStringLiteral("fontWeight")).toInt(), 700);
    if (cleaned.contains(QStringLiteral("paragraphSpacing")))
        cleaned[QStringLiteral("paragraphSpacing")] =
            std::round(qBound(0.0, cleaned.value(QStringLiteral("paragraphSpacing")).toDouble(), 1.2) * 100.0) / 100.0;
    m_bookAppearance = cleaned;
    m_bookAppearanceOverride = true;
    const QString json = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(cleaned)).toJson(QJsonDocument::Compact));
    m_library->setBookSetting(m_bookId, QStringLiteral("appearance"), json);
    emit appearanceChanged();
}

void Reader::clearBookAppearance() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId || !m_library)
        return;
    const bool had = m_bookAppearanceOverride || !m_bookAppearance.isEmpty();
    m_bookAppearance.clear();
    m_bookAppearanceOverride = false;
    m_library->setBookSetting(m_bookId, QStringLiteral("appearance"), QString());
    if (had)
        emit appearanceChanged();
}

QString Reader::formatBackLabel(int chapter, const QString &pageLabel) const {
    if (!pageLabel.trimmed().isEmpty())
        return tr("p. %1").arg(pageLabel.trimmed());
    const QString title = titleOfChapter(chapter).trimmed();
    if (!title.isEmpty())
        return title;
    if (chapter >= 0)
        return m_pdf ? tr("p. %1").arg(chapter + 1) : tr("Ch. %1").arg(chapter + 1);
    return {};
}

void Reader::relocated(const QVariantMap &location) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId)
        return;
    if (location.contains(QStringLiteral("session")) && location.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    m_chapter = location.value("chapter").toInt();
    m_offset = qMax<qint64>(0, location.value("offset").toLongLong());
    m_snippet = location.value("snippet").toString();
    m_pageEnd = qMax(m_offset, location.value("end", m_offset).toLongLong());
    m_page = location.value("page").toInt();
    m_pages = qMax(1, location.value("pages").toInt());
    m_visibleColumns = qBound(1, location.value(QStringLiteral("columns"), m_visibleColumns).toInt(), 2);
    m_visiblePageEnd = location.contains(QStringLiteral("visiblePageEnd"))
                           ? location.value(QStringLiteral("visiblePageEnd")).toInt()
                           : -1;
    m_pageLabel = location.value("pageLabel").toString();
    if (location.contains(QStringLiteral("rtl")))
        m_rtl = location.value(QStringLiteral("rtl")).toBool();
    if (location.contains(QStringLiteral("canGoBack")))
        m_canGoBack = location.value(QStringLiteral("canGoBack")).toBool();
    if (location.contains(QStringLiteral("canGoForward")))
        m_canGoForward = location.value(QStringLiteral("canGoForward")).toBool();
    if (m_canGoBack && location.contains(QStringLiteral("backChapter"))) {
        m_backLabel = formatBackLabel(location.value(QStringLiteral("backChapter")).toInt(),
                                      location.value(QStringLiteral("backPageLabel")).toString());
    } else if (!m_canGoBack) {
        m_backLabel.clear();
    }
    const bool jumped = location.value(QStringLiteral("jumped")).toBool();
    // Keep startPosition current so a WebEngine crash reload resumes here.
    m_startPosition = QVariantMap{{"chapter", m_chapter}, {"offset", double(m_offset)}, {"snippet", m_snippet}};
    const bool nearEnd = m_pdf ? (m_chapter >= qMax(0, chapterCount() - 2)) : (m_chapter >= m_lastLinear);
    m_atEnd = location.value("atEnd").toBool() && nearEnd;
    m_progress = m_atEnd ? 1.0 : fractionAt(m_chapter, m_offset);
    updateBookPages();
    ++m_relocationSeq;

    // Reading pace comes from steady forward reading: the time spent on a
    // page and how much text it held. Jumps, quick flips, and automated
    // narration turns don't count toward visual reading WPM.
    const qint64 global = globalOffset(m_chapter, m_offset);
    const qint64 seconds = m_sinceTurn.restart() / 1000;
    const bool automated = location.value(QStringLiteral("automated")).toBool();
    if (!automated && m_lastGlobal >= 0 && seconds >= 2 && seconds <= 600) {
        const qint64 delta = global - m_lastGlobal;
        m_pendingSeconds += int(seconds);
        if (delta > 0 && delta < 20000)
            m_pendingChars += delta;
    }
    m_lastGlobal = global;
    if (!m_applyingExternalPosition)
        m_saveTimer.start();
    emit locationChanged();
    if (jumped && m_canGoBack && !m_backLabel.isEmpty())
        emit jumpRecorded(m_backLabel);
}

void Reader::flushPosition() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId)
        return;
    if (m_saveTimer.isActive())
        m_saveTimer.stop();
    savePosition();
}

void Reader::savePosition() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId || !m_library || m_applyingExternalPosition)
        return;
    // The chapter's name rides along, for the library's Continue card.
    const QJsonObject position{{"chapter", m_chapter}, {"offset", double(m_offset)}, {"snippet", m_snippet},
                               {"title", chapterTitle()}};
    m_library->savePosition(m_bookId, QString::fromUtf8(QJsonDocument(position).toJson(QJsonDocument::Compact)),
                            m_progress, sessionSource());
    if (m_atEnd && m_library->record(m_bookId).finishedAt == 0)
        m_library->setFinished(m_bookId, true);
}

void Reader::handleExternalPositionChanged(int bookId, int chapter, double progress, QObject *sourceSession) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (bookId <= 0 || bookId != m_bookId || !m_library)
        return;
    if (sourceSession == this || (m_sessionOwner && sourceSession == m_sessionOwner))
        return;
    if (m_applyingExternalPosition)
        return;

    const BookRecord record = m_library->record(bookId);
    int nextChapter = chapter;
    qint64 nextOffset = m_offset;
    QString nextSnippet = m_snippet;
    if (!record.position.isEmpty()) {
        const QJsonObject saved = QJsonDocument::fromJson(record.position.toUtf8()).object();
        if (saved.contains(QStringLiteral("chapter")))
            nextChapter = saved.value(QStringLiteral("chapter")).toInt(nextChapter);
        if (saved.contains(QStringLiteral("offset")))
            nextOffset = qMax<qint64>(0, saved.value(QStringLiteral("offset")).toInteger());
        if (saved.contains(QStringLiteral("snippet")))
            nextSnippet = saved.value(QStringLiteral("snippet")).toString();
    }
    const int count = chapterCount();
    if (count > 0)
        nextChapter = qBound(0, nextChapter, count - 1);
    else
        nextChapter = qMax(0, nextChapter);
    const double nextProgress = std::clamp(progress, 0.0, 1.0);
    if (nextChapter == m_chapter && nextOffset == m_offset &&
        qFuzzyCompare(nextProgress + 1.0, m_progress + 1.0)) {
        return;
    }

    m_applyingExternalPosition = true;
    m_saveTimer.stop();
    m_chapter = nextChapter;
    m_offset = nextOffset;
    m_snippet = nextSnippet;
    m_progress = nextProgress;
    m_startPosition = QVariantMap{{"chapter", m_chapter}, {"offset", double(m_offset)}, {"snippet", m_snippet}};
    const bool nearEnd = m_pdf ? (m_chapter >= qMax(0, count - 2)) : (m_chapter >= m_lastLinear);
    m_atEnd = nextProgress >= 0.999 && nearEnd;
    updateBookPages();
    m_lastGlobal = globalOffset(m_chapter, m_offset);
    emit locationChanged();
    emit externalPositionSynced(m_chapter, m_offset, m_progress);
    m_applyingExternalPosition = false;
}

void Reader::flushReadingTime() {
    if (!m_bookId || !m_library || m_pendingSeconds <= 0)
        return;
    m_library->addReadingTime(m_bookId, m_pendingSeconds, m_pendingChars);
    m_pendingSeconds = 0;
    m_pendingChars = 0;
}

void Reader::surfaceLoaded() { emit surfaceReady(); }
void Reader::tapped(const QString &zone) { emit tap(zone); }

void Reader::openExternal(const QString &url) {
    const QUrl link(url);
    if (link.scheme() == QStringLiteral("http") || link.scheme() == QStringLiteral("https") ||
        link.scheme() == QStringLiteral("mailto"))
        emit externalLink(url);
}

void Reader::keyPressed(const QString &key, int modifiers) { emit this->key(key, modifiers); }

void Reader::playPageTurn(int direction) {
#if !defined(CLOTHBOUND_NO_MULTIMEDIA)
    if (sfxSuppressedEnvironment())
        return;
    if (!m_sfx)
        m_sfx = std::make_unique<PageTurnSfx>(this);
    m_sfx->play(direction);
#else
    Q_UNUSED(direction);
#endif
}

bool Reader::playPageTurnDryRun(int direction, QByteArray *pcmOut) {
#if !defined(CLOTHBOUND_NO_MULTIMEDIA)
    if (!m_sfx)
        m_sfx = std::make_unique<PageTurnSfx>(this);
    return m_sfx->playDryRun(direction, pcmOut);
#else
    Q_UNUSED(direction);
    Q_UNUSED(pcmOut);
    return false;
#endif
}

void Reader::notifyPageTurn(int direction) { emit pageTurnRequested(direction >= 0 ? 1 : -1); }

void Reader::speakableReadyFor(qint64 session, int chapter, const QString &blocks) {
    if (session != m_session || !m_bookId)
        return;
    emit speakable(chapter, blocks);
}

void Reader::selectionChanged(const QVariantMap &selection) {
    if (selection.contains(QStringLiteral("session")) && selection.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    emit selected(selection);
}

void Reader::markClicked(int id, const QVariantMap &rect) {
    if (rect.contains(QStringLiteral("session")) && rect.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    emit markChosen(id, rect);
}

void Reader::imageClicked(const QVariantMap &info) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_bookId)
        return;
    if (info.contains(QStringLiteral("session")) && info.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    const QString caption = info.value(QStringLiteral("caption")).toString().trimmed();
    const QString dataUrl = info.value(QStringLiteral("dataUrl")).toString();
    constexpr qsizetype maxDataUrlChars = 28 * 1024 * 1024;
    if (dataUrl.startsWith(QLatin1String("data:image/"), Qt::CaseInsensitive) && dataUrl.size() <= maxDataUrlChars) {
        emit imageChosen(dataUrl, caption);
        return;
    }
    const QString urlString = info.value(QStringLiteral("url")).toString();
    if (urlString.startsWith(QLatin1String("data:image/"), Qt::CaseInsensitive) && urlString.size() <= maxDataUrlChars) {
        emit imageChosen(urlString, caption);
        return;
    }
    if (!m_book)
        return;
    const QUrl url(urlString);
    if (url.scheme() != QLatin1String("clothbound") || url.host() != QLatin1String("local"))
        return;
    const QString path = url.path(QUrl::FullyDecoded);
    const QString prefix = QStringLiteral("/book/%1/").arg(m_token);
    if (!path.startsWith(prefix))
        return;
    const QString entry = path.mid(prefix.size());
    if (entry.isEmpty())
        return;

    // Fast zero-base64 async path once ReaderImageProvider ("bookimg") is registered on the QML engine.
    if (ReaderImageProvider::isRegistered()) {
        QString quickMime = m_book->mediaType(entry);
        if (quickMime.isEmpty())
            quickMime = EpubBook::mediaTypeForPath(entry);
        if (!quickMime.isEmpty() && !quickMime.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
            return;
        emit imageChosen(ReaderImageProvider::imageUrl(m_token, entry), caption);
        return;
    }

    // Async off-GUI-thread fallback (eliminates synchronous 20 MB ZIP inflate and
    // 27 MB base64 encoding on the GUI thread even before "bookimg" is wired in main.cpp).
    if (!m_lightboxDir || !m_lightboxDir->isValid())
        m_lightboxDir = std::make_unique<QTemporaryDir>();
    const QString tempDirPath = (m_lightboxDir && m_lightboxDir->isValid()) ? m_lightboxDir->path() : QString();
    const qint64 imgSeq = ++m_lightboxSeq;
    const qint64 session = m_session;
    auto book = m_book;
    auto activeSession = m_activeSession;

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, session, caption] {
        Q_ASSERT(QThread::currentThread() == thread());
        watcher->deleteLater();
        const QString source = watcher->result();
        if (source.isEmpty() || session != m_session || !m_bookId)
            return;
        emit imageChosen(source, caption);
    });
    watcher->setFuture(QtConcurrent::run([book, activeSession, session, entry, tempDirPath, imgSeq]() -> QString {
        if (activeSession->load(std::memory_order_relaxed) != session)
            return {};
        QString resolvedEntry = entry;
        QString mime, err;
        QByteArray bytes = book->resource(resolvedEntry, &mime, &err);
        if (bytes.isEmpty() && !err.isEmpty()) {
            const QString normalized = EpubBook::resolve(QString(), resolvedEntry);
            if (!normalized.isEmpty() && normalized != resolvedEntry) {
                err.clear();
                bytes = book->resource(normalized, &mime, &err);
                if (!bytes.isEmpty())
                    resolvedEntry = normalized;
            }
        }
        if (activeSession->load(std::memory_order_relaxed) != session)
            return {};
        if (bytes.isEmpty() || bytes.size() > ReaderImageProvider::kMaxResourceBytes)
            return {};
        if (mime.isEmpty())
            mime = EpubBook::mediaTypeForPath(resolvedEntry);
        if (!mime.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
            return {};

        if (!tempDirPath.isEmpty()) {
            const QString outPath = tempDirPath + QStringLiteral("/img-%1%2").arg(imgSeq).arg(imageExtensionForMime(mime));
            QFile out(outPath);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(bytes) == bytes.size()) {
                out.close();
                return QUrl::fromLocalFile(outPath).toString();
            }
        }
        return QStringLiteral("data:%1;base64,%2").arg(mime, QString::fromLatin1(bytes.toBase64()));
    }));
}

void Reader::listenFromSession(qint64 session, int chapter, qint64 offset) {
    if (session != m_session || !m_bookId)
        return;
    emit listenRequested(chapter, offset);
}

void Reader::log(const QString &message) {
    if (qEnvironmentVariableIsSet("CLOTHBOUND_DEBUG") || qEnvironmentVariableIsSet("DOGEAR_DEBUG"))
        fprintf(stderr, "reader: %s\n", qPrintable(message));
}

QString Reader::passageFrom(int chapter, int maxChars) const {
    if (m_pdf) {
        QString text;
        for (int page = qMax(0, chapter); page < m_pdf->pageCount() && text.size() < maxChars; ++page) {
            const QString words = m_pdf->pageText(page).simplified();
            if (words.size() > 600)
                text += words.left(maxChars - text.size()) + QStringLiteral("\n\n");
        }
        return text.trimmed();
    }
    if (!m_book)
        return {};
    QString text;
    const QVariantList spine = m_manifest.value(QStringLiteral("spine")).toList();
    for (int i = qMax(0, chapter); i < spine.size() && text.size() < maxChars; ++i) {
        const QString words = m_book->chapterText(i).simplified();
        if (words.size() < 1200)
            continue;
        text += words.left(maxChars - text.size()) + QStringLiteral("\n\n");
    }
    return text.trimmed();
}

QString Reader::bookPath() const { return m_book ? m_book->filePath() : m_pdf ? m_pdf->path() : QString(); }

void Reader::copyText(const QString &text) const {
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

// --- PDFs ----------------------------------------------------------------------

bool Reader::openPdf(int bookId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_library)
        return false;
    const BookRecord record = m_library->record(bookId);
    auto book = std::make_shared<PdfBook>(record.path);
    if (!book->open()) {
        m_error = book->errorString();
        return false;
    }
    const int pages = book->pageCount();
    if (pages <= 0) {
        m_error = tr("This PDF has no pages.");
        return false;
    }
    close();
    m_pdf = book;
    m_bookId = bookId;
    ++m_session;
    m_activeSession->store(m_session, std::memory_order_relaxed);
    const qint64 session = m_session;
    m_token = QStringLiteral("p%1-%2").arg(bookId).arg(QUuid::createUuid().toString(QUuid::Id128));
    PdfPages::add(m_token, book);
    m_format = record.format;
    m_title = record.title;
    m_author = record.author;
    m_language = record.language;
    m_detectedDoi = book->detectedDoi();
    m_detectedArxiv = book->detectedArxiv();

    // Each page is titled by the outline entry it falls under.
    m_chapterTitles = QStringList(pages, QString());
    m_contents.clear();
    m_contentsChapter.clear();
    for (const PdfOutlineEntry &entry : book->outline()) {
        m_contents.append(QVariantMap{{"title", entry.title}, {"depth", entry.depth}, {"chapter", entry.page}, {"fragment", QString()}});
        m_contentsChapter.append(entry.page);
        if (entry.page >= 0 && entry.page < pages && m_chapterTitles[entry.page].isEmpty())
            m_chapterTitles[entry.page] = entry.title;
    }
    for (int i = 1; i < pages; ++i)
        if (m_chapterTitles[i].isEmpty())
            m_chapterTitles[i] = m_chapterTitles[i - 1];
    QVariantList spine;
    m_pageTargets.clear();
    m_pageTargets.reserve(pages);
    for (int i = 0; i < pages; ++i) {
        spine.append(QVariantMap{{"linear", true}});
        const QString label = book->pageLabel(i).trimmed();
        m_pageTargets.append({i, label.isEmpty() ? QString::number(i + 1) : label});
    }
    m_linear = QList<bool>(pages, true);
    m_firstLinear = 0;
    m_lastLinear = pages - 1;
    m_hasPageList = !m_pageTargets.isEmpty();
    m_manifest = QVariantMap{{"format", "pdf"}, {"spine", spine}, {"pages", pages}, {"title", m_title},
                             {"language", m_language}, {"session", double(m_session)}};

    // Progress by text: each page's length, measured once and kept.
    m_lengths = m_library->chapterLengths(bookId);
    const bool needLengths = (m_lengths.size() != pages);
    if (needLengths)
        m_lengths = QList<int>(pages, 1500);

    // The margins to cut: restored from book_settings when the file hasn't
    // changed, or measured once in the background and saved.
    m_pdfCrop = QRectF(0, 0, 1, 1);
    bool cropCached = false;
    const qint64 mtime = record.mtime;
    const QString savedCrop = m_library->bookSetting(bookId, QStringLiteral("pdf_crop"));
    if (!savedCrop.isEmpty()) {
        const QStringList parts = savedCrop.split(QLatin1Char(':'));
        if (parts.size() == 2 && parts[0].toLongLong() == mtime) {
            const QStringList nums = parts[1].split(QLatin1Char(','));
            if (nums.size() == 4) {
                bool ok0 = false, ok1 = false, ok2 = false, ok3 = false;
                const double cx = nums[0].toDouble(&ok0), cy = nums[1].toDouble(&ok1);
                const double cw = nums[2].toDouble(&ok2), ch = nums[3].toDouble(&ok3);
                if (ok0 && ok1 && ok2 && ok3 && cx >= 0.0 && cy >= 0.0 && cw >= 0.4 && ch >= 0.4 && cx + cw <= 1.001 &&
                    cy + ch <= 1.001) {
                    m_pdfCrop = QRectF(cx, cy, qMin(cw, 1.0 - cx), qMin(ch, 1.0 - cy));
                    cropCached = true;
                }
            }
        }
    }
    const bool needCrop = !cropCached;
    // Run margin detection and page-length measurement in ONE background task
    // reusing the already-open `book` (`m_pdf`, mutex-guarded) instead of
    // opening two redundant concurrent `PdfBook` handles on initial load.
    if (needCrop || needLengths) {
        struct PdfInitChunk {
            enum class Kind { Crop, Lengths } kind = Kind::Crop;
            QRectF crop{0, 0, 1, 1};
            QList<int> lengths;
        };
        auto *watcher = new QFutureWatcher<PdfInitChunk>(this);
        connect(watcher, &QFutureWatcher<PdfInitChunk>::resultReadyAt, this, [this, watcher, bookId, mtime, session](int index) {
            const PdfInitChunk chunk = watcher->resultAt(index);
            if (chunk.kind == PdfInitChunk::Kind::Crop) {
                if (m_library) {
                    m_library->setBookSetting(
                        bookId, QStringLiteral("pdf_crop"),
                        QStringLiteral("%1:%2,%3,%4,%5")
                            .arg(mtime)
                            .arg(chunk.crop.x(), 0, 'f', 4)
                            .arg(chunk.crop.y(), 0, 'f', 4)
                            .arg(chunk.crop.width(), 0, 'f', 4)
                            .arg(chunk.crop.height(), 0, 'f', 4));
                }
                if (bookId != m_bookId || session != m_session)
                    return;
                m_pdfCrop = chunk.crop;
                emit pdfCropChanged();
            } else {
                if (m_library)
                    m_library->setChapterLengths(bookId, chunk.lengths);
                if (bookId != m_bookId || session != m_session)
                    return;
                m_lengths = chunk.lengths;
                m_progress = fractionAt(m_chapter, m_offset);
                emit lengthsChanged();
                emit locationChanged();
            }
        });
        connect(watcher, &QFutureWatcher<PdfInitChunk>::finished, watcher, &QObject::deleteLater);
        auto activeSession = m_activeSession;
        watcher->setFuture(QtConcurrent::run(
            [book, activeSession, session, needCrop, needLengths, pages](QPromise<PdfInitChunk> &promise) mutable {
                // Ensure the shared QPdfDocument is always destroyed on its affine UI thread.
                MainThreadPdfRelease releaseGuard{std::move(book)};

                auto cancelled = [&]() {
                    return promise.isCanceled() || activeSession->load(std::memory_order_relaxed) != session;
                };
                if (needCrop && !cancelled()) {
                    PdfInitChunk chunk;
                    chunk.kind = PdfInitChunk::Kind::Crop;
                    chunk.crop = PdfPages::bookCrop(*releaseGuard.handle);
                    if (!cancelled())
                        promise.addResult(std::move(chunk));
                }
                if (needLengths && !cancelled()) {
                    QList<int> lengths;
                    lengths.reserve(pages);
                    for (int page = 0; page < pages; ++page) {
                        if (cancelled())
                            return;
                        lengths.append(qMax(1, int(releaseGuard.handle->pageText(page).size())));
                    }
                    if (!cancelled()) {
                        PdfInitChunk chunk;
                        chunk.kind = PdfInitChunk::Kind::Lengths;
                        chunk.lengths = std::move(lengths);
                        promise.addResult(std::move(chunk));
                    }
                }
            }));
    }

    m_startPosition = QVariantMap{{"chapter", 0}, {"offset", 0}};
    const QJsonObject saved = QJsonDocument::fromJson(record.position.toUtf8()).object();
    if (saved.contains("chapter")) {
        const int page = saved.value("chapter").toInt();
        if (page >= 0 && page < pages)
            m_startPosition = QVariantMap{{"chapter", page}, {"offset", saved.value("offset").toDouble()}};
    }
    m_chapter = m_startPosition.value("chapter").toInt();
    m_offset = m_startPosition.value("offset").toLongLong();
    m_pageEnd = 0;
    m_snippet.clear();
    m_relocationSeq = 0;
    m_page = 0;
    m_pages = 1;
    m_visibleColumns = 1;
    m_visiblePageEnd = -1;
    m_progress = record.progress;
    m_pageLabel.clear();
    m_atEnd = false;
    m_rtl = false;
    m_canGoBack = false;
    m_canGoForward = false;
    m_backLabel.clear();
    loadBookAppearance(bookId);
    m_previousOpenedAt = record.openedAt;
    updateBookPages();
    m_library->markOpened(bookId);
    m_lastGlobal = -1;
    m_sinceTurn.start();
    m_timeFlush.start();
    emit bookChanged();
    emit appearanceChanged();
    emit locationChanged();
    emit pdfCropChanged();
    return true;
}

QVariantMap Reader::pdfPageSize(int page) const {
    const QSizeF size = m_pdf ? m_pdf->pageSize(page) : QSizeF();
    return {{"width", size.width()}, {"height", size.height()}};
}

QVariantMap Reader::pdfSelect(int page, qreal x1, qreal y1, qreal x2, qreal y2) const {
    if (!m_pdf)
        return {};
    const PdfBook::Selection selection = m_pdf->select(page, QPointF(x1, y1), QPointF(x2, y2));
    if (selection.start < 0)
        return {};
    // Line breaks are the page's, not the words': copied and noted, they're spaces.
    QString text = selection.text;
    text.replace(QString(QChar(0xFFFE)) + QLatin1String("\r\n"), QString())
        .replace(QLatin1String("\r\n"), QLatin1String(" "));
    return {{"chapter", page}, {"start", selection.start}, {"end", selection.end}, {"text", text.simplified()}};
}

QVariantMap Reader::pdfSelectWord(int page, qreal x, qreal y) const {
    if (!m_pdf)
        return {};
    const PdfBook::Selection selection = m_pdf->selectWord(page, QPointF(x, y));
    if (selection.start < 0 || selection.text.isEmpty())
        return {};
    return {{"chapter", page}, {"start", selection.start}, {"end", selection.end}, {"text", selection.text.simplified()}};
}

QVariantList Reader::pdfLinks(int page) const {
    QVariantList out;
    if (!m_pdf)
        return out;
    for (const PdfLinkEntry &entry : m_pdf->pageLinks(page)) {
        const QString url = entry.url.isValid() ? entry.url.toString() : QString();
        for (const QRectF &rect : entry.rects) {
            out.append(QVariantMap{
                {QStringLiteral("x"), rect.x()},
                {QStringLiteral("y"), rect.y()},
                {QStringLiteral("width"), rect.width()},
                {QStringLiteral("height"), rect.height()},
                {QStringLiteral("page"), entry.page},
                {QStringLiteral("destY"), entry.destY},
                {QStringLiteral("url"), url},
            });
        }
    }
    return out;
}

QString Reader::snipPdfRegionBase64(int pageIndex, double normX, double normY, double normW, double normH) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pdf)
        return {};
    const QByteArray png = m_pdf->renderRegionPng(pageIndex, normX, normY, normW, normH, 1400);
    if (png.isEmpty())
        return {};
    return QString::fromLatin1(png.toBase64());
}

int Reader::requestPdfSnip(int pageIndex, double normX, double normY, double normW, double normH,
                           const QString &action, const QString &customPrompt) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pdf || pageIndex < 0 || pageIndex >= m_pdf->pageCount())
        return 0;
    const int reqId = ++m_asyncPdfReqId;
    const qint64 session = m_session;
    auto book = m_pdf;
    auto activeSession = m_activeSession;

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, reqId, session, action, pageIndex, customPrompt] {
                Q_ASSERT(QThread::currentThread() == thread());
                watcher->deleteLater();
                if (session != m_session || !m_pdf)
                    return;
                emit pdfSnipReady(reqId, action, watcher->result(), pageIndex, customPrompt);
            });
    watcher->setFuture(QtConcurrent::run(
        [book, activeSession, session, pageIndex, normX, normY, normW, normH]() mutable -> QString {
            MainThreadPdfRelease releaseGuard{std::move(book)};
            if (activeSession->load(std::memory_order_relaxed) != session || !releaseGuard.handle)
                return {};
            const QByteArray png = releaseGuard.handle->renderRegionPng(pageIndex, normX, normY, normW, normH, 1400);
            if (png.isEmpty() || activeSession->load(std::memory_order_relaxed) != session)
                return {};
            return QString::fromLatin1(png.toBase64());
        }));
    return reqId;
}

QVariantMap Reader::peekPdfDestination(int destPage, double destY) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pdf)
        return {};
    return buildPeekDestinationMap(*m_pdf, m_pdfCrop, destPage, destY);
}

int Reader::requestPdfPeek(int destPage, double destY) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pdf || destPage < 0 || destPage >= m_pdf->pageCount())
        return 0;
    const int reqId = ++m_asyncPdfReqId;
    m_latestPeekReqId = reqId;
    const qint64 session = m_session;
    const QRectF crop = m_pdfCrop;
    auto book = m_pdf;
    auto activeSession = m_activeSession;

    auto *watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, reqId, session] {
        Q_ASSERT(QThread::currentThread() == thread());
        watcher->deleteLater();
        if (session != m_session || !m_pdf || reqId != m_latestPeekReqId)
            return;
        emit pdfPeekReady(reqId, watcher->result());
    });
    watcher->setFuture(
        QtConcurrent::run([book, activeSession, session, crop, destPage, destY]() mutable -> QVariantMap {
            MainThreadPdfRelease releaseGuard{std::move(book)};
            if (activeSession->load(std::memory_order_relaxed) != session || !releaseGuard.handle)
                return {};
            return buildPeekDestinationMap(*releaseGuard.handle, crop, destPage, destY);
        }));
    return reqId;
}

namespace {

struct CitationPerson {
    QString first;
    QString last;
};

QList<CitationPerson> parseCitationPeople(const QString &raw) {
    QList<CitationPerson> out;
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return out;

    static const QRegularExpression primarySplit(QStringLiteral(R"(\s*(?:;|/|\||\band\b|&)\s*)"),
                                                 QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
    static const QStringList particles = {QStringLiteral("de"), QStringLiteral("del"), QStringLiteral("da"),
                                          QStringLiteral("di"), QStringLiteral("du"),  QStringLiteral("la"),
                                          QStringLiteral("le"), QStringLiteral("van"), QStringLiteral("von")};

    auto parseSingle = [&](const QString &piece) {
        const QString clean = piece.trimmed();
        if (clean.isEmpty())
            return;
        const QStringList tokens = clean.split(whitespace, Qt::SkipEmptyParts);
        if (tokens.isEmpty())
            return;
        if (tokens.size() == 1) {
            out.append({QString(), tokens.first()});
            return;
        }
        qsizetype lastIdx = tokens.size() - 1;
        if (tokens.size() >= 3 && particles.contains(tokens[lastIdx - 1].toLower()))
            --lastIdx;
        const QString first = tokens.mid(0, lastIdx).join(QLatin1Char(' '));
        const QString last = tokens.mid(lastIdx).join(QLatin1Char(' '));
        out.append({first, last});
    };

    for (const QString &chunk : trimmed.split(primarySplit, Qt::SkipEmptyParts)) {
        const QString part = chunk.trimmed();
        if (part.isEmpty())
            continue;
        if (part.contains(QLatin1Char(','))) {
            const QStringList commaParts = part.split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (commaParts.size() == 2 && !commaParts[0].trimmed().contains(QLatin1Char(' '))) {
                out.append({commaParts[1].trimmed(), commaParts[0].trimmed()});
            } else {
                for (const QString &sub : commaParts)
                    parseSingle(sub);
            }
        } else {
            parseSingle(part);
        }
    }
    return out;
}

QString asciiSlug(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        if (ch.category() == QChar::Mark_NonSpacing)
            continue;
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
            (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
            (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')))
            out.append(ch.toLower());
    }
    return out;
}

QString bibtexEscape(QString text) {
    text.replace(QLatin1Char('{'), QStringLiteral("\\{"));
    text.replace(QLatin1Char('}'), QStringLiteral("\\}"));
    text.replace(QLatin1Char('&'), QStringLiteral("\\&"));
    text.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    text.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    text.replace(QLatin1Char('#'), QStringLiteral("\\#"));
    return text;
}

QString formatApaPerson(const CitationPerson &p) {
    if (p.first.isEmpty())
        return p.last;
    static const QRegularExpression splitRe(QStringLiteral(R"([\s.]+)"));
    QStringList initials;
    for (const QString &tok : p.first.split(splitRe, Qt::SkipEmptyParts)) {
        if (tok.contains(QLatin1Char('-'))) {
            QStringList hyphenParts;
            for (const QString &hp : tok.split(QLatin1Char('-'), Qt::SkipEmptyParts))
                if (!hp.isEmpty())
                    hyphenParts.append(hp.left(1).toUpper() + QLatin1Char('.'));
            if (!hyphenParts.isEmpty())
                initials.append(hyphenParts.join(QLatin1Char('-')));
        } else if (!tok.isEmpty()) {
            initials.append(tok.left(1).toUpper() + QLatin1Char('.'));
        }
    }
    return initials.isEmpty() ? p.last : QStringLiteral("%1, %2").arg(p.last, initials.join(QLatin1Char(' ')));
}

QString joinApaPeople(const QList<CitationPerson> &people) {
    QStringList formatted;
    formatted.reserve(people.size());
    for (const CitationPerson &p : people)
        formatted.append(formatApaPerson(p));
    if (formatted.isEmpty())
        return {};
    if (formatted.size() == 1)
        return formatted.first();
    if (formatted.size() == 2)
        return formatted[0] + QStringLiteral(", & ") + formatted[1];
    if (formatted.size() > 20) {
        return formatted.mid(0, 19).join(QStringLiteral(", ")) + QStringLiteral(", ... ") + formatted.last();
    }
    return formatted.mid(0, formatted.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(", & ") + formatted.last();
}

QString cleanDoiValue(QString doi) {
    doi = doi.trimmed();
    static const QRegularExpression prefixRe(QStringLiteral(R"(^(?:https?://(?:dx\.)?doi\.org/|doi:\s*))"),
                                             QRegularExpression::CaseInsensitiveOption);
    doi.remove(prefixRe);
    return doi.trimmed();
}

QString cleanArxivBare(QString arxiv) {
    arxiv = arxiv.trimmed();
    static const QRegularExpression prefixRe(QStringLiteral(R"(^arXiv:\s*)"), QRegularExpression::CaseInsensitiveOption);
    arxiv.remove(prefixRe);
    return arxiv.trimmed();
}

}  // namespace

QString Reader::formatCitationFor(const QVariantMap &bookInfo, const QString &style) {
    const QString fmt = style.trimmed().toLower();
    if (fmt != QLatin1String("bibtex") && fmt != QLatin1String("apa"))
        return {};

    const QString title = bookInfo.value(QStringLiteral("title")).toString().simplified();
    const QString authorRaw = bookInfo.value(QStringLiteral("author")).toString().simplified();
    const QString editorRaw = bookInfo.value(QStringLiteral("editor")).toString().simplified();
    const QString publisher = bookInfo.value(QStringLiteral("publisher")).toString().simplified();
    const QString isbn = bookInfo.value(QStringLiteral("isbn")).toString().simplified();
    const QString bookFormat = bookInfo.value(QStringLiteral("format")).toString().trimmed().toLower();
    QString doi = cleanDoiValue(bookInfo.value(QStringLiteral("doi")).toString());
    QString arxivBare = cleanArxivBare(bookInfo.value(QStringLiteral("arxiv")).toString());
    int year = bookInfo.value(QStringLiteral("year")).toInt();

    if (year <= 0 && arxivBare.size() >= 4 && arxivBare[0].isDigit() && arxivBare[1].isDigit()) {
        const int yy = arxivBare.left(2).toInt();
        if (yy >= 7 && yy <= 99)
            year = 2000 + yy;
    }

    const QList<CitationPerson> authors = parseCitationPeople(authorRaw);
    const QList<CitationPerson> editors = parseCitationPeople(editorRaw);
    const QString safeTitle = title.isEmpty() ? QStringLiteral("Untitled") : title;

    if (fmt == QLatin1String("bibtex")) {
        const bool isBook = (bookFormat == QLatin1String("epub") && !isbn.isEmpty());
        QString entryType = QStringLiteral("misc");
        if (!doi.isEmpty())
            entryType = isBook ? QStringLiteral("book") : QStringLiteral("article");
        else if (!arxivBare.isEmpty())
            entryType = QStringLiteral("misc");
        else if (!isbn.isEmpty() || !publisher.isEmpty() || bookFormat == QLatin1String("epub"))
            entryType = QStringLiteral("book");

        QString keyAuthor;
        if (!authors.isEmpty())
            keyAuthor = asciiSlug(authors.first().last);
        else if (!editors.isEmpty())
            keyAuthor = asciiSlug(editors.first().last);
        if (keyAuthor.isEmpty())
            keyAuthor = QStringLiteral("ref");

        static const QStringList stopWords = {QStringLiteral("the"), QStringLiteral("a"),   QStringLiteral("an"),
                                              QStringLiteral("of"),  QStringLiteral("on"),  QStringLiteral("in"),
                                              QStringLiteral("for"), QStringLiteral("to"),  QStringLiteral("and")};
        static const QRegularExpression wordRe(QStringLiteral(R"([^\p{L}\p{N}]+)"));
        QString keyWord;
        for (const QString &w : safeTitle.split(wordRe, Qt::SkipEmptyParts)) {
            const QString slug = asciiSlug(w);
            if (!slug.isEmpty() && !stopWords.contains(slug)) {
                keyWord = slug;
                break;
            }
        }
        const QString citeKey = keyAuthor + (year > 0 ? QString::number(year) : QString()) + keyWord;

        auto formatBibPeople = [](const QList<CitationPerson> &people) {
            QStringList parts;
            for (const CitationPerson &p : people) {
                const QString full =
                    p.first.isEmpty() ? p.last : QStringLiteral("%1, %2").arg(p.last, p.first);
                parts.append(bibtexEscape(full));
            }
            return parts.join(QStringLiteral(" and "));
        };

        QStringList fields;
        fields.append(QStringLiteral("  title = {%1}").arg(bibtexEscape(safeTitle)));
        if (!authors.isEmpty())
            fields.append(QStringLiteral("  author = {%1}").arg(formatBibPeople(authors)));
        if (!editors.isEmpty())
            fields.append(QStringLiteral("  editor = {%1}").arg(formatBibPeople(editors)));
        if (year > 0)
            fields.append(QStringLiteral("  year = {%1}").arg(year));
        if (!publisher.isEmpty())
            fields.append(QStringLiteral("  publisher = {%1}").arg(bibtexEscape(publisher)));
        if (!isbn.isEmpty())
            fields.append(QStringLiteral("  isbn = {%1}").arg(isbn));
        if (!doi.isEmpty())
            fields.append(QStringLiteral("  doi = {%1}").arg(doi));
        if (!arxivBare.isEmpty()) {
            fields.append(QStringLiteral("  eprint = {%1}").arg(arxivBare));
            fields.append(QStringLiteral("  archivePrefix = {arXiv}"));
            if (doi.isEmpty())
                fields.append(QStringLiteral("  url = {https://arxiv.org/abs/%1}").arg(arxivBare));
        }

        return QStringLiteral("@%1{%2,\n%3\n}").arg(entryType, citeKey, fields.join(QStringLiteral(",\n")));
    }

    // APA 7th edition.
    const QString yearPart = year > 0 ? QStringLiteral("(%1).").arg(year) : QStringLiteral("(n.d.).");
    const bool endsTerminal = safeTitle.endsWith(QLatin1Char('.')) || safeTitle.endsWith(QLatin1Char('?')) ||
                              safeTitle.endsWith(QLatin1Char('!'));
    const QString titlePart = endsTerminal ? safeTitle : (safeTitle + QLatin1Char('.'));

    QString out;
    if (!authors.isEmpty()) {
        out = QStringLiteral("%1 %2 %3").arg(joinApaPeople(authors), yearPart, titlePart);
    } else if (!editors.isEmpty()) {
        const QString edTag = editors.size() == 1 ? QStringLiteral("(Ed.).") : QStringLiteral("(Eds.).");
        out = QStringLiteral("%1 %2 %3 %4").arg(joinApaPeople(editors), edTag, yearPart, titlePart);
    } else {
        out = QStringLiteral("%1 %2").arg(titlePart, yearPart);
    }

    if (!publisher.isEmpty())
        out += QStringLiteral(" %1.").arg(publisher);
    if (!arxivBare.isEmpty() && doi.isEmpty())
        out += QStringLiteral(" arXiv preprint arXiv:%1. https://arxiv.org/abs/%1").arg(arxivBare);
    else if (!doi.isEmpty())
        out += QStringLiteral(" https://doi.org/%1").arg(doi);

    return out.simplified();
}

QString Reader::formatCitation(const QString &style) const {
    if (!isOpen())
        return {};
    const BookRecord record = m_library ? m_library->record(m_bookId) : BookRecord{};
    int year = record.year;
    if (year <= 0 && m_pdf && m_pdf->created().isValid())
        year = m_pdf->created().date().year();
    const QVariantMap info{
        {QStringLiteral("title"), m_title.isEmpty() ? record.title : m_title},
        {QStringLiteral("author"), m_author.isEmpty() ? record.author : m_author},
        {QStringLiteral("editor"), record.editor},
        {QStringLiteral("publisher"), record.publisher},
        {QStringLiteral("year"), year},
        {QStringLiteral("isbn"), record.isbn},
        {QStringLiteral("format"), m_format},
        {QStringLiteral("doi"), m_detectedDoi},
        {QStringLiteral("arxiv"), m_detectedArxiv},
    };
    return formatCitationFor(info, style);
}

QString Reader::formatBookCitation(const QVariantMap &bookInfo, const QString &style) const {
    QVariantMap enriched = bookInfo;
    const int id = enriched.value(QStringLiteral("id")).toInt();
    const QString fmt = enriched.value(QStringLiteral("format")).toString().trimmed().toLower();
    const QString path = enriched.value(QStringLiteral("path")).toString();
    bool matchesOpenPdf = false;
    if (m_pdf && !path.isEmpty()) {
        if (path == m_pdf->path()) {
            matchesOpenPdf = true;
        } else {
            const QString openCanon = QFileInfo(m_pdf->path()).canonicalFilePath();
            const QString infoCanon = QFileInfo(path).canonicalFilePath();
            matchesOpenPdf = !openCanon.isEmpty() && openCanon == infoCanon;
        }
    }
    if ((id > 0 && id == m_bookId) || matchesOpenPdf) {
        if (enriched.value(QStringLiteral("doi")).toString().isEmpty()) {
            const QString doi = !m_detectedDoi.isEmpty() ? m_detectedDoi : (m_pdf ? m_pdf->detectedDoi() : QString());
            if (!doi.isEmpty())
                enriched.insert(QStringLiteral("doi"), doi);
        }
        if (enriched.value(QStringLiteral("arxiv")).toString().isEmpty()) {
            const QString arxiv =
                !m_detectedArxiv.isEmpty() ? m_detectedArxiv : (m_pdf ? m_pdf->detectedArxiv() : QString());
            if (!arxiv.isEmpty())
                enriched.insert(QStringLiteral("arxiv"), arxiv);
        }
        if (enriched.value(QStringLiteral("year")).toInt() <= 0 && m_pdf && m_pdf->created().isValid())
            enriched.insert(QStringLiteral("year"), m_pdf->created().date().year());
    } else if (fmt == QLatin1String("pdf") && enriched.value(QStringLiteral("doi")).toString().isEmpty() &&
               enriched.value(QStringLiteral("arxiv")).toString().isEmpty()) {
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            PdfBook probe(path);
            if (probe.open()) {
                if (const QString doi = probe.detectedDoi(); !doi.isEmpty())
                    enriched.insert(QStringLiteral("doi"), doi);
                if (const QString arxiv = probe.detectedArxiv(); !arxiv.isEmpty())
                    enriched.insert(QStringLiteral("arxiv"), arxiv);
                if (enriched.value(QStringLiteral("year")).toInt() <= 0 && probe.created().isValid())
                    enriched.insert(QStringLiteral("year"), probe.created().date().year());
            }
        }
    }
    return formatCitationFor(enriched, style);
}

QVariantList Reader::pdfRects(int page, qint64 start, qint64 end) const {
    QVariantList out;
    if (!m_pdf)
        return out;
    for (const QRectF &rect : m_pdf->textRects(page, start, end))
        out.append(QVariantMap{{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}});
    return out;
}

QString Reader::pdfPageLabel(int page) const { return m_pdf ? m_pdf->pageLabel(page) : QString(); }

QString Reader::pdfPageText(int page) const { return m_pdf ? m_pdf->pageText(page) : QString(); }

void Reader::pdfPrefetch(const QVariantList &pages, int pixelWidth, const QString &colorQuery) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pdf || m_token.isEmpty() || pages.isEmpty() || pixelWidth <= 0)
        return;
    const int cappedWidth = qMin(pixelWidth, PdfPages::kMaxBaseWidthPx);
    QList<int> pageList;
    pageList.reserve(pages.size());
    const int total = m_pdf->pageCount();
    for (const QVariant &item : pages) {
        const int p = item.toInt();
        if (p >= 0 && p < total && !pageList.contains(p))
            pageList.append(p);
    }
    if (!pageList.isEmpty())
        PdfPages::prefetch(m_token, pageList, QSize(cappedWidth, 0), colorQuery);
}

void Reader::pdfSpeakable(int page) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pdf)
        return;
    const QJsonArray blocks = PdfBook::speakable(m_pdf->pageText(page), m_title);
    const QString json = QString::fromUtf8(QJsonDocument(blocks).toJson(QJsonDocument::Compact));
    const qint64 session = m_session;
    // Deliver asynchronously so consecutive blank/scanned PDF pages never
    // recurse synchronously through Narration::addBlocks() -> blocksWanted().
    QMetaObject::invokeMethod(
        this,
        [this, session, page, json] {
            if (session == m_session && m_pdf)
                emit speakable(page, json);
        },
        Qt::QueuedConnection);
}
