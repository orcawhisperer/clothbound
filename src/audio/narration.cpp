#include "audio/narration.h"

#include "ai/ai.h"
#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "audio/cast.h"
#include "audio/director.h"
#include "audio/speechplayer.h"
#include "library/library.h"
#include "reader/reader.h"

#include <QDateTime>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
// CLOTHBOUND_DEBUG (or legacy DOGEAR_DEBUG) traces the clips' lives on stderr.
void trace(const QString &message) {
    static const bool on = qEnvironmentVariableIsSet("CLOTHBOUND_DEBUG") || qEnvironmentVariableIsSet("DOGEAR_DEBUG");
    static const qint64 started = nowMs();
    if (on)
        fprintf(stderr, "narration %6.2fs %s\n", (nowMs() - started) / 1000.0, qPrintable(message));
}
constexpr qint64 directionsPatienceMs = 6000;
constexpr double charsPerMinute = 900;
constexpr int sampleRate = 24000;
constexpr qint64 timeEmitIntervalMs = 250;
}  // namespace

Narration::Narration(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_library(library),
      m_director(new Director(ai, cache, reader, this)), m_player(new SpeechPlayer(sampleRate, this)),
      m_sleep(new SleepController(m_player, this)), m_response(new ResponseSpeech(ai, cache, m_player, this)),
      m_sleepAt(m_sleep->deadlineRef()), m_sleepFading(m_sleep->fadingRef()),
      m_preFadeVolume(m_sleep->preFadeVolumeRef()), m_responseChunks(m_response->chunks()),
      m_responseVoice(m_response->voice()) {
    m_sleep->setCallbacks({[this] { return m_active; }, [this] { return m_wanted; },
                           [this] { stopResponse(); }, [this] { pause(); }});
    connect(m_sleep, &SleepController::changed, this, &Narration::sleepChanged);

    m_response->setCallbacks({
        [this] { return m_nextClipId++; },
        [this] {
            if (!m_voice.isEmpty())
                return m_voice;
            if (m_library && m_reader && m_reader->bookId() > 0)
                return m_library->bookSetting(m_reader->bookId(), QStringLiteral("voice"));
            return QString();
        },
        [this] { return m_reader ? m_reader->language() : QString(); },
        [this] { return m_economy; },
        [this] {
            if (playing())
                pause();
            setProblem({});
            if (m_active)
                m_bookPlayerDirty = true;
            if (m_reader && m_reader->bookId() > 0)
                m_bookId = m_reader->bookId();
        },
        [this](const QString &msg) { setProblem(msg); },
    });
    connect(m_response, &ResponseSpeech::changed, this, &Narration::responseSpeechChanged);
    connect(m_response, &ResponseSpeech::problemOccurred, this, &Narration::responseSpeechProblem);

    connect(m_player, &SpeechPlayer::position, this, &Narration::heard);
    connect(m_player, &SpeechPlayer::volumeChanged, this, &Narration::volumeChanged);
    connect(m_player, &SpeechPlayer::sinkError, this, [this](const QString &message) {
        if (m_response->handleSinkError(message) || !m_active)
            return;
        pause();
        setProblem(message);
    });
    connect(m_player, &SpeechPlayer::starvedChanged, this, [this](bool starved) {
        if (!m_response->handleStarved(starved) && m_wanted)
            setBuffering(starved);
    });
    connect(m_player, &SpeechPlayer::drained, this, [this] {
        if (m_response->handleDrained() || !m_active)
            return;
        if (m_sleep->atChapterEnd() && m_current >= 0 && m_current < m_clips.size() &&
            (m_queued + 1 >= m_clips.size() || m_clips[m_queued + 1].chapter != m_clips[m_current].chapter)) {
            stopAtChapterBoundary(m_queued + 1 < m_clips.size() ? m_queued + 1 : -1);
            return;
        }
        if (m_queued + 1 < m_clips.size() && m_clips[m_queued + 1].state == Clip::Failed) {
            stallIfFailed();
            return;
        }
        const bool more = m_queued + 1 < m_clips.size() || m_requested.size() > m_loadedChapters.size() ||
                          nextChapterAfter(m_lastChapter) >= 0;
        if (!more) {
            stop();
        } else if (m_wanted) {
            pump();
            if (m_active && m_wanted && m_player->drainedOut())
                setBuffering(true);
        }
    });
    m_directionsTimer.setSingleShot(true);
    connect(&m_directionsTimer, &QTimer::timeout, this, &Narration::pump);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_active && m_wanted)
            pump();
    });
    connect(m_reader, &Reader::speakable, this, &Narration::addBlocks);
    connect(m_reader, &Reader::bookChanged, this, [this] {
        m_pendingStart = false;
        if (speakingResponse() && (m_reader->bookId() <= 0 || m_reader->bookId() != m_bookId))
            stopResponse();
        if (m_active && m_reader->bookId() != m_bookId)
            stop();
        if (!m_active && m_reader->bookId() > 0) {
            m_bookId = m_reader->bookId();
            loadVoice();
        }
    });

    QSettings s = settings();
    m_rate = std::clamp(s.value(QStringLiteral("listen/rate"), 1.0).toDouble(), 0.75, 2.0);
    m_economy = s.value(QStringLiteral("listen/economy"), false).toBool();
    m_player->setRate(m_rate);
}

Narration::~Narration() { stop(); }

double Narration::volume() const { return m_player->volume(); }

void Narration::setVolume(double volume) {
    Q_ASSERT(QThread::currentThread() == thread());
    // Explicit external volume change supersedes any pending sleep-fade restore.
    const bool wasFading = m_sleep->onExternalVolumeChange();
    m_player->setVolume(volume);
    if (wasFading)
        emit sleepChanged();
}

void Narration::claimBookFocus() {
    AudioFocus::instance().claim(this, AudioFocus::Kind::Book, [this] {
        if (playing())
            pause();
    });
}

void Narration::restartAtCurrentPosition() {
    if (!m_active)
        return;
    const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
    const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
    const bool wanted = m_wanted;
    start(at, offset);
    if (!wanted)
        pause();
}

void Narration::resetQueuedClipsFrom(int startIndex) {
    Q_ASSERT(startIndex >= 0 && startIndex < m_clips.size());
    m_queued = startIndex - 1;
    for (int i = startIndex; i < m_clips.size(); ++i) {
        m_clips[i].queued = false;
        if (m_clips[i].state == Clip::Complete && m_clips[i].pcm.isEmpty())
            m_clips[i].state = Clip::Waiting;
    }
}

void Narration::stopAtChapterBoundary(int nextIndex) {
    const bool hasNextClip = nextIndex >= 0 && nextIndex < m_clips.size();
    if (hasNextClip) {
        m_current = nextIndex;
        m_currentSample = 0;
        m_sentence = -1;
        m_bookPlayerDirty = true;
        m_player->clear();
    }
    pause();
    if (hasNextClip) {
        if (!m_clips[m_current].sentences.isEmpty())
            showSentenceForSample(m_current, 0, 0);
        else
            emit positionChanged();
        emit timeChanged();
    }
    m_sleep->consumeChapterEnd();
}

void Narration::adoptLocalVoice(const QString &id, const QString &name, const QString &notice) {
    m_voice = id;
    m_voiceName = name;
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
    setFallbackNotice(notice);
}

void Narration::loadVoice() {
    QSettings s = settings();
    const QString performance = m_library->bookSetting(m_bookId, QStringLiteral("performance"));
    const QString chosen = performance.isEmpty()
                               ? s.value(QStringLiteral("listen/performance"), QStringLiteral("natural")).toString()
                               : performance;
    if (chosen != m_performance) {
        m_performance = chosen;
        emit settingsChanged();
    }
    const QString bookVoice = m_library->bookSetting(m_bookId, QStringLiteral("voice"));
    if (!bookVoice.isEmpty()) {
        m_voice = bookVoice;
        m_voiceName = m_library->bookSetting(m_bookId, QStringLiteral("voiceName"));
    } else {
        m_voice = s.value(QStringLiteral("listen/voice"), QStringLiteral("sulafat")).toString();
        m_voiceName = s.value(QStringLiteral("listen/voiceName"), QStringLiteral("Sulafat")).toString();
    }
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
}

void Narration::setFollowing(bool following) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (following == m_following)
        return;
    m_following = following;
    emit stateChanged();
    if (following && m_sentenceStart >= 0)
        emit speaking(chapter(), m_sentenceStart, m_sentenceEnd);
}

void Narration::setRate(double rate) {
    Q_ASSERT(QThread::currentThread() == thread());
    rate = std::clamp(std::round(rate * 20) / 20, 0.75, 2.0);
    if (qFuzzyCompare(rate, m_rate))
        return;
    m_rate = rate;
    trace(QStringLiteral("rate %1").arg(rate));
    m_player->setRate(rate);
    settings().setValue(QStringLiteral("listen/rate"), m_rate);
    emit rateChanged();
    emit positionChanged();
    emit timeChanged();
}

void Narration::setPerformance(const QString &performance) {
    Q_ASSERT(QThread::currentThread() == thread());
    static const QStringList known{"plain", "natural", "dramatic"};
    if (!known.contains(performance) || performance == m_performance)
        return;
    m_performance = performance;
    settings().setValue(QStringLiteral("listen/performance"), performance);
    if (m_reader->bookId() > 0)
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("performance"), performance);
    emit settingsChanged();
    restartAtCurrentPosition();
}

void Narration::setEconomy(bool economy) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (economy == m_economy)
        return;
    m_economy = economy;
    settings().setValue(QStringLiteral("listen/economy"), economy);
    emit settingsChanged();
}

void Narration::setVoice(const QString &id, const QString &name) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (id.isEmpty())
        return;
    m_voice = id;
    m_voiceName = name.isEmpty() ? id : name;
    clearFallbackOffer();
    setFallbackNotice({});
    QSettings s = settings();
    s.setValue(QStringLiteral("listen/voice"), m_voice);
    s.setValue(QStringLiteral("listen/voiceName"), m_voiceName);
    if (m_reader->bookId() > 0) {
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("voice"), m_voice);
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("voiceName"), m_voiceName);
    }
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
    restartAtCurrentPosition();
}

void Narration::useFallbackVoice() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_fallbackVoice.isEmpty() || !m_ai->canSpeakWith(m_fallbackVoice))
        return;
    const QString id = m_fallbackVoice;
    const QString name = m_fallbackVoiceName;
    clearFallbackOffer();
    setProblem({});
    if (m_active) {
        const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
        const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
        setVoice(id, name);
        if (!m_active)
            start(at, offset);
    } else {
        setVoice(id, name);
        if (m_pendingStart)
            startPending();
    }
}

void Narration::startPending() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_pendingStart)
        return;
    const int ch = m_pendingChapter;
    const qint64 off = m_pendingOffset;
    m_pendingStart = false;
    emit stateChanged();
    start(ch, off);
}

void Narration::setBuffering(bool buffering) {
    if (buffering == m_buffering)
        return;
    m_buffering = buffering;
    if (buffering)
        trace(QStringLiteral("buffering at #%1").arg(m_current));
    emit stateChanged();
}

void Narration::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

QPair<QString, QString> Narration::bestLocalVoice() const {
    return ResponseSpeech::bestLocalVoice(m_ai, m_reader ? m_reader->language() : QString(), false);
}

void Narration::updateFallbackOffer() {
    const auto [id, name] = bestLocalVoice();
    QString nextId, nextName;
    bool nextSetup = false;
    if (!id.isEmpty() && id != m_voice) {
        nextId = id;
        nextName = name;
    } else if (id.isEmpty() && !AiServices::isBuiltinVoice(m_voice) && ResponseSpeech::localPacksSupported(m_ai)) {
        nextSetup = true;
    }
    if (nextId != m_fallbackVoice || nextName != m_fallbackVoiceName || nextSetup != m_offerOfflineSetup) {
        m_fallbackVoice = nextId;
        m_fallbackVoiceName = nextName;
        m_offerOfflineSetup = nextSetup;
        emit fallbackChanged();
    }
}

void Narration::clearFallbackOffer() {
    if (m_fallbackVoice.isEmpty() && m_fallbackVoiceName.isEmpty() && !m_offerOfflineSetup)
        return;
    m_fallbackVoice.clear();
    m_fallbackVoiceName.clear();
    m_offerOfflineSetup = false;
    emit fallbackChanged();
}

void Narration::setFallbackNotice(const QString &notice) {
    if (notice == m_fallbackNotice)
        return;
    m_fallbackNotice = notice;
    emit fallbackChanged();
}

bool Narration::fallbackToLocalMidSession(int fromIndex, const QString &reason) {
    const auto [fbId, fbName] = bestLocalVoice();
    if (fbId.isEmpty() || fbId == m_voice || !m_ai->canSpeakWith(fbId))
        return false;
    trace(QStringLiteral("fallback mid-session -> %1 (%2)").arg(fbId, reason));
    m_retryTimer.stop();
    clearFallbackOffer();
    setProblem({});
    adoptLocalVoice(fbId, fbName,
                    reason.isEmpty() ? tr("Switched to %1 offline").arg(fbName)
                                     : tr("Switched to %1 offline (%2)").arg(fbName, reason));
    // Abort any other in-flight cloud requests for unqueued clips from fromIndex
    // onward and re-synthesize them locally.
    for (int i = qMax(0, fromIndex); i < m_clips.size(); ++i) {
        Clip &c = m_clips[i];
        if (c.queued)
            continue;
        if (c.reply) {
            QPointer<Ai::Reply> obsolete = c.reply;
            c.reply = nullptr;
            obsolete->abort();
        }
        if (c.state != Clip::Complete || c.pcm.isEmpty()) {
            c.state = Clip::Waiting;
            c.pcm = QByteArray();
            c.tries = 0;
            c.key.clear();
            c.voice = m_voice;
        }
    }
    pump();
    return true;
}

int Narration::nextChapterAfter(int chapter) const {
    if (chapter < 0)
        return -1;
    const QVariantList spine = m_reader->manifest().value(QStringLiteral("spine")).toList();
    const bool fromLinear = spine.value(chapter).toMap().value(QStringLiteral("linear")).toBool();
    for (int i = chapter + 1; i < spine.size(); ++i)
        if (!fromLinear || spine[i].toMap().value(QStringLiteral("linear")).toBool())
            return i;
    return -1;
}

void Narration::start(int chapter, qint64 offset) {
    Q_ASSERT(QThread::currentThread() == thread());
    stop();
    setProblem({});
    clearFallbackOffer();
    setFallbackNotice({});
    m_pendingStart = false;
    m_bookId = m_reader->bookId();
    if (m_bookId <= 0)
        return;
    loadVoice();
    if (!m_ai->canSpeakWith(m_voice)) {
        if (AiServices::isBuiltinVoice(m_voice)) {
            m_pendingStart = true;
            m_pendingChapter = chapter;
            m_pendingOffset = offset;
            setProblem(tr("This built-in voice isn't downloaded yet. Choose it again in the Narrator panel (V)."));
            emit stateChanged();
            emit voiceMissing();
            return;
        }
        if (AiServices::isStudioVoice(m_voice)) {
            setProblem(tr("This Studio voice needs its model server running on this computer, chosen in Intelligence (✦)."));
            emit voiceMissing();
            return;
        }
        // Cloud voice with no Gemini key: seamlessly fall back to the best
        // installed local voice for this book's language, or offer offline setup.
        const auto [fbId, fbName] = bestLocalVoice();
        if (!fbId.isEmpty()) {
            adoptLocalVoice(fbId, fbName, tr("Using %1 offline").arg(fbName));
        } else if (ResponseSpeech::localPacksSupported(m_ai)) {
            m_pendingStart = true;
            m_pendingChapter = chapter;
            m_pendingOffset = offset;
            setProblem(tr("Download an offline voice to listen right away, or add a Gemini key."));
            emit stateChanged();
            emit voiceMissing();
            return;
        } else {
            setProblem(m_ai->describe(int(Ai::Error::NoKey)));
            emit needsKey();
            return;
        }
    } else if (!AiServices::isOnThisComputer(m_voice) && !m_ai->fitsBudget(0.001)) {
        const auto [fbId, fbName] = bestLocalVoice();
        if (!fbId.isEmpty())
            adoptLocalVoice(fbId, fbName, tr("Using %1 offline (budget reached)").arg(fbName));
    }
    m_active = true;
    m_wanted = true;
    m_following = true;
    if (m_library)
        m_library->markListened(m_bookId);
    m_startChapter = chapter;
    m_startOffset = offset;
    m_requested.insert(chapter);
    setBuffering(true);
    claimBookFocus();
    m_player->play();
    emit stateChanged();
    emit blocksWanted(chapter);
}

bool Narration::loadCastForChapter(int chapter, const QList<SpeechBlock> &blocks, bool requestIfMissing) {
    if (!m_cast || !m_cast->enabled() || !m_cast->available() || m_cast->knows(chapter))
        return true;
    if (const auto byChapter = m_cast->keptChapterOrBatch(chapter, blocks)) {
        for (auto it = byChapter->cbegin(); it != byChapter->cend(); ++it)
            m_cast->learn(it.key(), it.value());
        return true;
    }
    if (requestIfMissing)
        requestCast(chapter);
    return false;
}

void Narration::addBlocks(int chapter, const QString &json) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || !m_requested.contains(chapter) || m_loadedChapters.contains(chapter))
        return;
    m_loadedChapters.insert(chapter);
    m_lastChapter = qMax(m_lastChapter, chapter);
    const QList<SpeechBlock> blocks = Speech::parseBlocks(json);
    m_blocks.insert(chapter, blocks);
    bool kept = false;
    if (directed() && m_ai->textFor(QStringLiteral("director"))) {
        if (m_moods.contains(chapter)) {
            kept = true;
        } else if (const auto byChapter = m_director->keptChapterOrBatch(chapter, blocks, m_performance)) {
            for (auto it = byChapter->cbegin(); it != byChapter->cend(); ++it)
                m_moods.insert(it.key(), it.value());
            kept = true;
        }
    }
    const bool castKept = loadCastForChapter(chapter, blocks, false);
    const qint64 from = chapter == m_startChapter ? m_startOffset : 0;
    const bool firstOfSession = m_clips.isEmpty();
    const QList<SpeechClip> made =
        Speech::clipsFrom(chapter, blocks, from, breaksFor(chapter), blockVoicesFor(chapter, blocks), m_voice);
    const int before = int(m_clips.size());
    for (const SpeechClip &spoken : made)
        m_clips.append(clipFrom(spoken));
    // First few clips go ahead without waiting on the director or cast.
    if (firstOfSession && (!kept || !castKept))
        for (int i = before; i < qMin<qsizetype>(before + 3, m_clips.size()); ++i)
            m_clips[i].immediate = true;
    if (m_current < 0 && !m_clips.isEmpty())
        m_current = 0;
    // A voice made more slowly than it's heard gets a head start first.
    if (firstOfSession && !made.isEmpty()) {
        const double speed = speaker() ? speaker()->speedOf(m_voice) : 0;
        if (speed > 0 && speed < 1.2 * m_rate) {
            qint64 characters = 0;
            for (const SpeechClip &clip : made)
                characters += clip.text.size();
            const double seconds = characters * m_samplesPerChar / sampleRate;
            const double head = seconds * qMax(0.0, 1.0 - speed / m_rate) * 1.1 + 2.0;
            m_holdSamples = qMax<qint64>(0, qint64(head * sampleRate));
            Q_ASSERT(m_holdSamples >= 0);
            trace(QStringLiteral("head start: %1 s of audio at %2x").arg(head, 0, 'f', 1).arg(speed, 0, 'f', 2));
            m_player->pause();
            emit stateChanged();
        }
    }
    if (!castKept && !made.isEmpty())
        requestCast(chapter);
    if (directed() && m_ai->textFor(QStringLiteral("director")) && !made.isEmpty() && !kept)
        requestDirections(chapter);
    if (!made.isEmpty()) {
        emit positionChanged();
        emit timeChanged();
    }
    pump();
}

bool Narration::allRemainingComplete() const {
    if (m_clips.isEmpty())
        return false;
    const int start = qMax(0, m_current);
    const int currentChapter = (m_current >= 0 && m_current < m_clips.size()) ? m_clips[m_current].chapter : -1;
    const bool chapterStop = m_sleep->atChapterEnd();
    for (int i = start; i < m_clips.size(); ++i) {
        if (chapterStop && currentChapter >= 0 && m_clips[i].chapter != currentChapter)
            return true;
        if (m_clips[i].state == Clip::Waiting || m_clips[i].state == Clip::Working)
            return false;
    }
    if (chapterStop && currentChapter >= 0 && m_loadedChapters.contains(currentChapter))
        return true;
    return m_requested.size() <= m_loadedChapters.size() && nextChapterAfter(m_lastChapter) < 0;
}

int Narration::preparedPercent() const {
    Q_ASSERT(m_holdSamples >= 0);
    if (m_holdSamples <= 0 || allRemainingComplete())
        return 100;
    qint64 have = 0;
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        have += m_clips[i].pcm.size() / 2;
    return int(qMin<qint64>(100, have * 100 / m_holdSamples));
}

void Narration::checkHeadStart() {
    Q_ASSERT(m_holdSamples >= 0);
    if (m_holdSamples <= 0)
        return;
    emit positionChanged();
    if (preparedPercent() < 100 && !allRemainingComplete())
        return;
    trace(QStringLiteral("head start made"));
    m_holdSamples = 0;
    if (m_active && canFeedBookPlayer())
        m_player->play();
    emit stateChanged();
}

qint64 Narration::directionWaitLeft(const Clip &clip) const {
    if (clip.immediate)
        return 0;
    qint64 left = 0;
    if (m_directionsAsked.contains(clip.chapter) && directed() && !m_moods.contains(clip.chapter))
        left = qMax(left, directionsPatienceMs - (nowMs() - m_directionsAsked.value(clip.chapter)));
    if (m_castAsked.contains(clip.chapter) && m_cast && m_cast->enabled() && !m_cast->knows(clip.chapter) &&
        m_cast->available())
        left = qMax(left, directionsPatienceMs - (nowMs() - m_castAsked.value(clip.chapter)));
    return left;
}

Ai::SpeechSynthesizer *Narration::speaker() const { return m_ai->speechFor(m_voice); }

bool Narration::directed() const {
    const Ai::SpeechSynthesizer *voice = speaker();
    return m_performance != QStringLiteral("plain") && voice && voice->supportsStyle();
}

QString Narration::styleFor(const Clip &clip) const {
    if (!directed())
        return {};
    return Speech::styleFor(Speech::moodAt(m_moods.value(clip.chapter), clip.block),
                            m_performance == QStringLiteral("dramatic"));
}

void Narration::pump() {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(m_holdSamples >= 0);
    if (!m_active || !canFeedBookPlayer())
        return;
    const int remaining = int(m_clips.size()) - qMax(0, m_current);
    if (remaining < 5 && m_lastChapter >= 0) {
        const int next = nextChapterAfter(m_lastChapter);
        if (next >= 0 && !m_requested.contains(next)) {
            m_requested.insert(next);
            emit blocksWanted(next);
        }
    }
    if (m_clips.isEmpty() && m_requested.size() == m_loadedChapters.size() && nextChapterAfter(m_lastChapter) < 0) {
        stop();
        setProblem(tr("Nothing left to read aloud in this book."));
        return;
    }

    int working = 0;
    for (const Clip &clip : std::as_const(m_clips))
        working += clip.state == Clip::Working && clip.reply ? 1 : 0;
    const int from = qMax(0, m_current);
    qint64 wait = -1;
    const bool local = AiServices::isOnThisComputer(m_voice);
    const int ahead = local ? (m_holdSamples > 0 ? 64 : 8) : 4;
    for (int i = from; i < m_clips.size() && i < from + ahead; ++i) {
        Clip &clip = m_clips[i];
        if (clip.state != Clip::Waiting)
            continue;
        if (clip.pauseMs > 0) {
            clip.pcm = QByteArray(int(qint64(clip.pauseMs) * sampleRate / 1000) * 2, '\0');
            clip.samples = clip.pcm.size() / 2;
            clip.state = Clip::Complete;
            continue;
        }
        if (const qint64 left = directionWaitLeft(clip); left > 0) {
            wait = wait < 0 ? left : qMin(wait, left);
            continue;
        }
        clip.style = styleFor(clip);
        if (!clip.voice.isEmpty() && clip.voice != m_voice && !m_ai->canSpeakWith(clip.voice))
            clip.voice = m_voice;
        QString voice = voiceOf(clip);
        Ai::SpeechSynthesizer *synth = m_ai->speechFor(voice);
        Ai::Provider *provider = m_ai->providerFor(voice);
        if ((!synth || !provider) && voice != m_voice) {
            clip.voice = m_voice;
            voice = m_voice;
            synth = m_ai->speechFor(voice);
            provider = m_ai->providerFor(voice);
        }
        if (!synth || !provider) {
            pause();
            setProblem(ResponseSpeech::voiceUnavailableMessage(m_ai, m_voice));
            updateFallbackOffer();
            return;
        }
        const QString model = synth->speechModelName(m_economy);
        clip.key = AudioCache::keyFor(provider->id(), model, voice, clip.style, clip.text);
        const QString file = m_cache->lookup(clip.key);
        if (!file.isEmpty()) {
            decode(i, file);
            continue;
        }
        if (m_retryTimer.isActive() || working >= (local ? ahead : 2))
            continue;
        fetch(i);
        ++working;
    }
    if (wait > 0)
        m_directionsTimer.start(int(wait) + 50);
    queue();
    if (m_holdSamples > 0)
        checkHeadStart();
}

void Narration::queue() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || !canFeedBookPlayer())
        return;
    Q_ASSERT(!speakingResponse());
    while (m_queued + 1 < m_clips.size()) {
        const int nextIndex = m_queued + 1;
        Clip &clip = m_clips[nextIndex];
        if (clip.state == Clip::Waiting || clip.state == Clip::Failed ||
            (clip.state == Clip::Working && !clip.reply))
            break;
        if (m_sleep->atChapterEnd() && m_current >= 0 && m_current < m_clips.size() &&
            clip.chapter != m_clips[m_current].chapter)
            break;
        ++m_queued;
        clip.queued = true;
        const qint64 startSample = (nextIndex == m_current && m_currentSample > 0) ? m_currentSample : 0;
        m_player->addTrack(clip.id, startSample);
        if (!clip.pcm.isEmpty())
            m_player->appendAudio(clip.id, clip.pcm);
        if (clip.state == Clip::Complete)
            m_player->finishTrack(clip.id);
    }
    stallIfFailed();
}

void Narration::stallIfFailed() {
    if (!m_wanted || m_queued + 1 >= m_clips.size() || m_clips[m_queued + 1].state != Clip::Failed ||
        !m_player->drainedOut())
        return;
    m_wanted = false;
    AudioFocus::instance().release(this, AudioFocus::Kind::Book);
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
}

void Narration::handleFetchError(int index, const Ai::Error &err) {
    Clip &clip = m_clips[index];
    trace(QStringLiteral("failed #%1: %2").arg(index).arg(err.message));
    const bool isCloud = !AiServices::isOnThisComputer(voiceOf(clip));
    if ((err.kind == Ai::Error::Limited || err.kind == Ai::Error::Server) && !clip.queued && clip.tries < 3 && m_wanted) {
        ++clip.tries;
        clip.state = Clip::Waiting;
        clip.pcm = QByteArray();
        setProblem(m_ai->describe(int(err.kind), err.message));
        if (isCloud && err.kind == Ai::Error::Limited)
            updateFallbackOffer();
        const int delayMs = err.retryAfterSeconds > 0 ? qBound(1000, err.retryAfterSeconds * 1000, 60000)
                                                      : 3000 * clip.tries;
        if (!m_retryTimer.isActive())
            m_retryTimer.start(delayMs);
        return;
    }
    if (isCloud && !clip.queued &&
        (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget)) {
        const QString reason = err.kind == Ai::Error::Offline ? tr("offline")
                             : err.kind == Ai::Error::Limited ? tr("Gemini limit reached")
                                                              : tr("budget reached");
        clip.state = Clip::Waiting;
        clip.pcm = QByteArray();
        if (fallbackToLocalMidSession(index, reason))
            return;
    }
    clip.state = Clip::Failed;
    clip.pcm = QByteArray();
    if (err.kind != Ai::Error::Cancelled) {
        setProblem(m_ai->describe(int(err.kind), err.message));
        if (isCloud && (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget ||
                        err.kind == Ai::Error::NoKey || err.kind == Ai::Error::BadKey))
            updateFallbackOffer();
    }
    if (canFeedBookPlayer()) {
        Q_ASSERT(!speakingResponse());
        if (clip.queued)
            m_player->finishTrack(clip.id);
        queue();
        checkHeadStart();
    }
}

void Narration::fetch(int index) {
    Clip &clip = m_clips[index];
    Ai::SpeechRequest request;
    request.text = Speech::speakable(clip.text);
    request.style = clip.style;
    request.voice = voiceOf(clip);
    request.economy = m_economy;
    request.stream = true;
    for (const SpeechSentence &sentence : std::as_const(clip.sentences))
        request.sentences.append(Speech::speakable(sentence.text));

    Ai::SpeechSynthesizer *synth = m_ai->speechFor(request.voice);
    if (!synth && request.voice != m_voice) {
        clip.voice = m_voice;
        request.voice = m_voice;
        synth = m_ai->speechFor(request.voice);
    }
    if (!synth) {
        clip.state = Clip::Failed;
        setProblem(ResponseSpeech::voiceUnavailableMessage(m_ai, m_voice));
        updateFallbackOffer();
        queue();
        return;
    }

    const bool local = AiServices::isOnThisComputer(request.voice);
    const double cost = local ? 0.0 : m_ai->listeningCost(clip.text.size() / charsPerMinute, m_economy, false);
    if (!local && !m_ai->fitsBudget(cost)) {
        if (fallbackToLocalMidSession(index, tr("monthly budget reached")))
            return;
        clip.state = Clip::Failed;
        setProblem(m_ai->describe(int(Ai::Error::Budget)));
        updateFallbackOffer();
        queue();
        return;
    }

    Ai::Reply *reply = synth->synthesize(request);
    m_ai->track(reply);
    clip.reply = reply;
    clip.state = Clip::Working;
    const quint64 session = m_session;
    const int clipId = clip.id;
    trace(QStringLiteral("fetch #%1 (id %2, %3 chars%4)")
              .arg(index)
              .arg(clipId)
              .arg(clip.text.size())
              .arg(clip.style.isEmpty() ? QString() : QStringLiteral(", ") + clip.style));

    connect(reply, &Ai::Reply::audioReceived, this, [this, session, clipId, reply](const QByteArray &pcm) {
        if (!m_active || session != m_session)
            return;
        const int i = indexOfClip(clipId);
        if (i < 0 || m_clips[i].reply != reply)
            return;
        Clip &clip = m_clips[i];
        if (clip.pcm.isEmpty()) {
            trace(QStringLiteral("first audio #%1").arg(i));
            setProblem({});
            clearFallbackOffer();
        }
        clip.pcm += pcm;
        if (const Ai::Reply *from = clip.reply)
            clip.boundaries = from->boundaries();
        if (canFeedBookPlayer()) {
            Q_ASSERT(!speakingResponse());
            if (clip.queued)
                m_player->appendAudio(clip.id, pcm);
            else
                queue();
            checkHeadStart();
        }
    });
    connect(reply, &Ai::Reply::finished, this, [this, session, clipId, reply] {
        reply->deleteLater();
        if (!m_active || session != m_session)
            return;
        const int i = indexOfClip(clipId);
        if (i < 0 || m_clips[i].reply != reply)
            return;
        Clip &clip = m_clips[i];
        clip.reply = nullptr;
        if (reply->error()) {
            handleFetchError(i, reply->error());
            return;
        }
        clip.state = Clip::Complete;
        clip.tries = 0;
        clip.samples = clip.pcm.size() / 2;
        clip.boundaries = reply->boundaries();
        setProblem({});
        clearFallbackOffer();
        if (!clip.text.isEmpty() && clip.samples > 0)
            m_samplesPerChar = 0.8 * m_samplesPerChar + 0.2 * double(clip.samples) / clip.text.size();
        trace(QStringLiteral("complete #%1 (%2 s)").arg(i).arg(clip.samples / double(sampleRate), 0, 'f', 1));
        if (canFeedBookPlayer()) {
            Q_ASSERT(!speakingResponse());
            if (clip.queued)
                m_player->finishTrack(clip.id);
        }
        if (!clip.pcm.isEmpty() && reply->sampleRate() > 0)
            m_cache->store(clip.key, clip.pcm, reply->sampleRate(), clip.boundaries);
        if (m_current >= 0 && m_current < m_clips.size() && clip.chapter == m_clips[m_current].chapter) {
            emit positionChanged();
            emit timeChanged();
        }
        if (canFeedBookPlayer())
            checkHeadStart();
        pump();
    });
}

void Narration::decode(int index, const QString &file) {
    Clip &clip = m_clips[index];
    clip.state = Clip::Working;
    trace(QStringLiteral("cached #%1").arg(index));
    const QString key = clip.key;
    const int clipId = clip.id;
    const quint64 session = m_session;
    m_cache->decode(file, sampleRate, this, [this, session, clipId, key](const QByteArray &pcm, bool ok) {
        if (!m_active || session != m_session)
            return;
        const int i = indexOfClip(clipId);
        if (i < 0 || m_clips[i].state != Clip::Working || m_clips[i].reply)
            return;
        Clip &clip = m_clips[i];
        if (!ok) {
            // Evict the damaged cache entry before re-queuing so we never loop on it.
            m_cache->remove(key);
            clip.state = Clip::Waiting;
            clip.queued = false;
            clip.key.clear();
            pump();
            return;
        }
        clip.pcm = pcm;
        clip.samples = clip.pcm.size() / 2;
        clip.boundaries = m_cache->boundaries(key);
        clip.state = Clip::Complete;
        if (canFeedBookPlayer()) {
            Q_ASSERT(!speakingResponse());
            if (clip.queued) {
                m_player->appendAudio(clip.id, clip.pcm);
                m_player->finishTrack(clip.id);
            }
            checkHeadStart();
        }
        if (m_current >= 0 && m_current < m_clips.size() && clip.chapter == m_clips[m_current].chapter) {
            emit positionChanged();
            emit timeChanged();
        }
        pump();
    });
}

qint64 Narration::expectedSamples(const Clip &clip) const {
    if (clip.state == Clip::Complete && clip.samples > 0)
        return clip.samples;
    if (clip.pauseMs > 0)
        return qint64(clip.pauseMs) * sampleRate / 1000;
    return qMax(qint64(clip.pcm.size() / 2), qint64(clip.text.size() * m_samplesPerChar));
}

QList<Speech::TimelineEntry> Narration::timelineEntries() const {
    QList<Speech::TimelineEntry> out;
    out.reserve(m_clips.size());
    for (const Clip &clip : m_clips)
        out.append({clip.chapter, clip.pauseMs, expectedSamples(clip)});
    return out;
}

void Narration::heard(int trackId, qint64 sample) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_response->handlePosition(trackId, sample) || !m_active)
        return;
    const int track = indexOfClip(trackId);
    if (track < 0)
        return;
    if (m_buffering)
        setBuffering(false);
    if (track != m_current) {
        const int previous = m_current;
        const bool newChapter = previous >= 0 && previous < m_clips.size() &&
                                m_clips[previous].chapter != m_clips[track].chapter;
        if (newChapter && m_sleep->atChapterEnd()) {
            stopAtChapterBoundary(track);
            return;
        }
        m_current = track;
        m_currentSample = 0;
        m_sentence = -1;
        trace(QStringLiteral("hearing #%1 (id %2)").arg(track).arg(trackId));
        for (int i = 0; i < track - 2; ++i)
            if (m_clips[i].state == Clip::Complete)
                m_clips[i].pcm = QByteArray();
        emit positionChanged();
        emit timeChanged();
        pump();
    }
    m_currentSample = qMax<qint64>(0, sample);
    Q_ASSERT(m_currentSample >= 0);
    showSentenceForSample(track, m_currentSample);
    const qint64 now = nowMs();
    const qint64 sec = positionUs() / 1000000LL;
    if (sec != m_lastReportedSec || now - m_lastTimeEmitMs >= timeEmitIntervalMs) {
        m_lastReportedSec = sec;
        m_lastTimeEmitMs = now;
        emit timeChanged();
    }
}

void Narration::showSentenceForSample(int index, qint64 sample, int forceSentence) {
    if (index < 0 || index >= m_clips.size())
        return;
    const Clip &clip = m_clips[index];
    if (clip.sentences.isEmpty())
        return;
    if (forceSentence >= 0 && forceSentence < clip.sentences.size()) {
        showSentenceAt(forceSentence);
        return;
    }
    showSentenceAt(Speech::sentenceForSample(clip, clip.spans, clip.boundaries, expectedSamples(clip), sample));
}

void Narration::showSentenceAt(int at) {
    if (m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    if (at < 0 || at >= clip.sentences.size() || at == m_sentence)
        return;
    m_sentence = at;
    m_sentenceStart = clip.sentences[at].start;
    m_sentenceEnd = clip.sentences[at].end;
    emit positionChanged();
    emit speaking(clip.chapter, m_sentenceStart, m_sentenceEnd);
}

void Narration::jumpTo(int index, qint64 sampleOffset, bool preservePause, int forceSentence) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (index < 0 || index >= m_clips.size())
        return;
    if (speakingResponse())
        stopResponse();
    m_bookPlayerDirty = false;
    const qint64 clampedSample = qMax<qint64>(0, sampleOffset);
    trace(QStringLiteral("jump to #%1 @ %2").arg(index).arg(clampedSample));
    m_holdSamples = 0;
    m_retryTimer.stop();
    setProblem({});
    m_player->clear();
    const int ahead = AiServices::isOnThisComputer(m_voice) ? 8 : 4;
    for (int i = 0; i < m_clips.size(); ++i) {
        Clip &clip = m_clips[i];
        if ((i < index || i >= index + ahead) && clip.reply) {
            QPointer<Ai::Reply> obsolete = clip.reply;
            clip.reply = nullptr;
            obsolete->abort();
            if (clip.state == Clip::Working) {
                clip.state = Clip::Waiting;
                clip.pcm = QByteArray();
            }
        }
        if (i >= index) {
            clip.queued = false;
            clip.tries = 0;
            if ((clip.state == Clip::Complete && clip.pcm.isEmpty()) || clip.state == Clip::Failed)
                clip.state = Clip::Waiting;
        }
    }
    const bool playAfter = preservePause ? m_wanted : true;
    m_current = index;
    m_currentSample = clampedSample;
    m_queued = index - 1;
    m_sentence = -1;
    m_wanted = playAfter;
    if (playAfter) {
        m_sleep->onJumpToPlay();
        setBuffering(m_clips[index].state != Clip::Complete);
        claimBookFocus();
        m_player->play();
    } else {
        setBuffering(false);
    }
    emit stateChanged();
    if (playAfter)
        pump();
    if (!m_clips[index].sentences.isEmpty())
        showSentenceForSample(index, clampedSample, forceSentence);
    else
        emit positionChanged();
    const qint64 posUs = positionUs();
    m_lastTimeEmitMs = nowMs();
    m_lastReportedSec = posUs / 1000000LL;
    emit timeChanged();
    emit seeked(posUs);
}

void Narration::nextParagraph() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &now = m_clips[m_current];
    for (int i = m_current + 1; i < m_clips.size(); ++i)
        if (m_clips[i].pauseMs == 0 && (m_clips[i].block != now.block || m_clips[i].chapter != now.chapter)) {
            jumpTo(i);
            return;
        }
}

void Narration::previousParagraph() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    if (m_sentence > 0 && m_clips[m_current].pauseMs == 0) {
        jumpTo(m_current);
        return;
    }
    int target = m_current - 1;
    while (target > 0 && m_clips[target].pauseMs > 0)
        --target;
    jumpTo(qMax(0, target));
}

void Narration::nextSentence() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    const int curS = qMax(0, m_sentence);
    if (clip.pauseMs == 0 && curS + 1 < clip.sentences.size()) {
        const qint64 sample = Speech::sampleOfSentence(clip, clip.spans, clip.boundaries, expectedSamples(clip), curS + 1);
        jumpTo(m_current, sample, true, curS + 1);
        return;
    }
    for (int i = m_current + 1; i < m_clips.size(); ++i)
        if (m_clips[i].pauseMs == 0 && !m_clips[i].sentences.isEmpty()) {
            jumpTo(i, 0, true, 0);
            return;
        }
    const int nextCh = nextChapterAfter(m_lastChapter);
    if (nextCh >= 0 && !m_requested.contains(nextCh)) {
        m_requested.insert(nextCh);
        emit blocksWanted(nextCh);
    }
}

void Narration::prevSentence() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    const int curS = qMax(0, m_sentence);
    if (clip.pauseMs == 0 && curS > 0) {
        const qint64 sample = Speech::sampleOfSentence(clip, clip.spans, clip.boundaries, expectedSamples(clip), curS - 1);
        jumpTo(m_current, sample, true, curS - 1);
        return;
    }
    for (int i = m_current - 1; i >= 0; --i) {
        if (m_clips[i].pauseMs == 0 && !m_clips[i].sentences.isEmpty()) {
            const int lastS = int(m_clips[i].sentences.size()) - 1;
            const qint64 sample =
                Speech::sampleOfSentence(m_clips[i], m_clips[i].spans, m_clips[i].boundaries, expectedSamples(m_clips[i]), lastS);
            jumpTo(i, sample, true, lastS);
            return;
        }
    }
    jumpTo(m_current, 0, true, 0);
}

void Narration::skipSeconds(int delta) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (delta != 0)
        seekBySamples(qint64(delta) * sampleRate);
}

void Narration::seekByUs(qint64 deltaUs) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (deltaUs != 0)
        seekBySamples(Speech::usToSamples(deltaUs, sampleRate));
}

void Narration::seekBySamples(qint64 deltaSamples) {
    if (!m_active || m_clips.isEmpty() || m_current < 0 || m_current >= m_clips.size())
        return;
    const Speech::SeekTarget target =
        Speech::seekBySamples(timelineEntries(), m_current, m_currentSample, deltaSamples, sampleRate);
    if (target.wantNextChapter) {
        const int nextCh = nextChapterAfter(m_lastChapter);
        if (nextCh >= 0 && !m_requested.contains(nextCh)) {
            m_requested.insert(nextCh);
            emit blocksWanted(nextCh);
        }
    }
    if (target.index >= 0)
        jumpTo(target.index, target.sample, true);
}

void Narration::seekToUs(qint64 targetUs) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || m_clips.isEmpty() || m_current < 0 || m_current >= m_clips.size())
        return;
    const Speech::SeekTarget target = Speech::seekToChapterUs(timelineEntries(), m_current, targetUs, sampleRate);
    if (target.index >= 0)
        jumpTo(target.index, target.sample, true);
}

void Narration::toggle() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (speakingResponse())
        stopResponse();
    playing() ? pause() : resume();
}

void Narration::pause() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (speakingResponse())
        stopResponse();
    if (!m_active)
        return;
    trace(QStringLiteral("pause"));
    const bool wasFading = m_sleep->onPause();
    m_wanted = false;
    AudioFocus::instance().release(this, AudioFocus::Kind::Book);
    m_directionsTimer.stop();
    m_retryTimer.stop();
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
    if (wasFading)
        emit sleepChanged();
}

void Narration::resume() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (speakingResponse())
        stopResponse();
    if (!m_active)
        return;
    trace(QStringLiteral("resume"));
    setProblem({});
    const bool wasFading = m_sleep->onResume();
    if (m_bookPlayerDirty) {
        m_bookPlayerDirty = false;
        m_player->clear();
        if (m_current >= 0 && m_current < m_clips.size())
            resetQueuedClipsFrom(m_current);
    }
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        if (m_clips[i].state == Clip::Failed) {
            m_clips[i].state = Clip::Waiting;
            m_clips[i].tries = 0;
        }
    m_wanted = true;
    claimBookFocus();
    if (m_holdSamples > 0)
        checkHeadStart();
    if (m_holdSamples <= 0)
        m_player->play();
    emit stateChanged();
    if (wasFading)
        emit sleepChanged();
    pump();
}

void Narration::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    stopResponse();
    AudioFocus::instance().release(this, AudioFocus::Kind::Book);
    m_bookPlayerDirty = false;
    ++m_session;
    const QList<Clip> clips = std::exchange(m_clips, {});
    for (const Clip &clip : clips)
        if (clip.reply)
            clip.reply->abort();
    m_director->cancel();
    if (m_cast)
        m_cast->cancel(this);
    m_directionsTimer.stop();
    m_retryTimer.stop();
    const bool hadSleep = m_sleep->reset();
    m_player->pause();
    m_player->clear();
    m_current = m_queued = m_lastChapter = m_sentence = -1;
    m_currentSample = m_lastTimeEmitMs = m_holdSamples = 0;
    m_lastReportedSec = m_sentenceStart = m_sentenceEnd = -1;
    m_requested.clear();
    m_loadedChapters.clear();
    m_directionsAsked.clear();
    m_castAsked.clear();
    m_directorBatchesAsked.clear();
    m_castBatchesAsked.clear();
    m_blocks.clear();
    m_moods.clear();
    clearFallbackOffer();
    setFallbackNotice({});
    const bool was = m_active;
    m_active = m_wanted = m_buffering = false;
    if (hadSleep)
        emit sleepChanged();
    if (was) {
        emit stateChanged();
        emit positionChanged();
        emit timeChanged();
    }
}

void Narration::speakResponse(const QString &markdownOrText, const QString &itemId) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_response->speak(markdownOrText, itemId);
}

void Narration::stopResponse() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_response->stop();
}

int Narration::minutesLeftInChapter() const {
    if (m_current < 0 || m_current >= m_clips.size())
        return 0;
    const int at = m_clips[m_current].chapter;
    qint64 chars = 0;
    for (int i = m_current; i < m_clips.size() && m_clips[i].chapter == at; ++i)
        chars += m_clips[i].text.size();
    return chars > 0 ? qMax(1, int(std::ceil(chars / (charsPerMinute * m_rate)))) : 0;
}

qint64 Narration::positionUs() const {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return 0;
    return Speech::chapterTimingUs(timelineEntries(), m_current, m_currentSample, sampleRate).first;
}

qint64 Narration::durationUs() const {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return 0;
    return Speech::chapterTimingUs(timelineEntries(), m_current, m_currentSample, sampleRate).second;
}

void Narration::sleepIn(int minutes) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_sleep->sleepIn(minutes) && m_active && m_wanted)
        pump();
}

void Narration::sleepAtEndOfChapter() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_sleep->sleepAtEndOfChapter();
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const int currentChapter = m_clips[m_current].chapter;
    bool nextChapterQueued = false;
    for (int i = m_current; i <= m_queued && i < m_clips.size(); ++i)
        if (m_clips[i].chapter != currentChapter) {
            nextChapterQueued = true;
            break;
        }
    if (nextChapterQueued) {
        if (!speakingResponse() && !m_bookPlayerDirty) {
            const bool playAfter = m_wanted && m_holdSamples <= 0;
            m_player->clear();
            resetQueuedClipsFrom(m_current);
            if (playAfter)
                m_player->play();
            queue();
        } else {
            for (int i = m_current; i < m_clips.size(); ++i)
                if (m_clips[i].chapter != currentChapter)
                    m_clips[i].queued = false;
        }
    }
    if (m_holdSamples > 0)
        checkHeadStart();
}

QSet<int> Narration::breaksFor(int chapter) const {
    QSet<int> breaks = Director::breaks(m_moods.value(chapter));
    if (m_cast)
        breaks.unite(m_cast->breaks(chapter, m_blocks.value(chapter)));
    return breaks;
}

QMap<int, QString> Narration::blockVoicesFor(int chapter, const QList<SpeechBlock> &blocks) const {
    return m_cast ? m_cast->blockVoices(chapter, blocks) : QMap<int, QString>{};
}

Narration::Clip Narration::clipFrom(const SpeechClip &spoken) {
    Clip clip;
    static_cast<SpeechClip &>(clip) = spoken;
    clip.id = m_nextClipId++;
    clip.spans = Speech::timeline(clip);
    clip.voice = !spoken.voice.isEmpty() ? spoken.voice : (m_cast ? m_cast->voiceAt(spoken.chapter, spoken.block) : m_voice);
    return clip;
}

void Narration::setCast(Cast *cast) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_cast = cast;
    if (!cast)
        return;
    cast->setNarrator(m_voice);
    connect(cast, &Cast::recast, this, [this] {
        if (!m_active || !m_cast)
            return;
        QSet<int> chapters;
        for (const Clip &clip : std::as_const(m_clips))
            chapters.insert(clip.chapter);
        for (int chapter : chapters) {
            loadCastForChapter(chapter, m_blocks.value(chapter), true);
            recut(chapter);
        }
    });
}

void Narration::requestCast(int chapter) {
    if (!m_cast)
        return;
    const auto [first, last] = m_cast->chapterRange(chapter);
    if (m_castBatchesAsked.contains(first))
        return;
    m_castBatchesAsked.insert(first);
    const qint64 askedAt = nowMs();
    for (int p = first; p <= last; ++p)
        m_castAsked.insert(p, askedAt);
    const quint64 session = m_session;
    const int bookId = m_bookId;
    m_cast->askChapterOrBatch(chapter, m_blocks.value(chapter), this,
                              [this, session, bookId, first, last](const QHash<int, QMap<int, Cast::Line>> &byChapter) {
                                  if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_cast)
                                      return;
                                  m_castBatchesAsked.remove(first);
                                  for (int p = first; p <= last; ++p) {
                                      m_castAsked.remove(p);
                                      m_cast->learn(p, byChapter.value(p));
                                      if (m_blocks.contains(p))
                                          recut(p);
                                  }
                                  trace(QStringLiteral("cast %1-%2").arg(first).arg(last));
                              });
}

void Narration::requestDirections(int chapter) {
    const auto [first, last] = m_director->chapterRange(chapter);
    if (m_directorBatchesAsked.contains(first))
        return;
    m_directorBatchesAsked.insert(first);
    const qint64 askedAt = nowMs();
    for (int p = first; p <= last; ++p)
        m_directionsAsked.insert(p, askedAt);
    const quint64 session = m_session;
    const int bookId = m_bookId;
    m_director->askChapterOrBatch(
        chapter, m_blocks.value(chapter), m_performance, this,
        [this, session, bookId, first, last](const QHash<int, QMap<int, QString>> &byChapter) {
            if (!m_active || session != m_session || m_reader->bookId() != bookId)
                return;
            m_directorBatchesAsked.remove(first);
            bool anyRecut = false;
            for (int p = first; p <= last; ++p) {
                m_directionsAsked.remove(p);
                const QMap<int, QString> moods = byChapter.value(p);
                m_moods.insert(p, moods);
                if (m_blocks.contains(p) && !moods.isEmpty()) {
                    recut(p);
                    anyRecut = true;
                }
            }
            trace(QStringLiteral("director %1-%2").arg(first).arg(last));
            if (!anyRecut)
                pump();
        });
}

void Narration::recut(int chapter) {
    int end = -1;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].chapter == chapter)
            end = i + 1;
    int from = end;
    while (from > 0 && m_clips[from - 1].chapter == chapter && m_clips[from - 1].state == Clip::Waiting &&
           !m_clips[from - 1].immediate && !m_clips[from - 1].queued && (from - 1) > m_queued && (from - 1) > m_current)
        --from;
    if (end > 0 && from < end) {
        const int currentId = (m_current >= 0 && m_current < m_clips.size()) ? m_clips[m_current].id : -1;
        const int queuedId = (m_queued >= 0 && m_queued < m_clips.size()) ? m_clips[m_queued].id : -1;
        const QList<SpeechBlock> blocks = m_blocks.value(chapter);
        const Clip &first = m_clips[from];
        const qint64 offset = first.pauseMs > 0 ? blocks.value(first.block).start : first.start();
        const QList<SpeechClip> remade = Speech::clipsFrom(chapter, blocks, offset, breaksFor(chapter),
                                                           blockVoicesFor(chapter, blocks), m_voice);
        m_clips.remove(from, end - from);
        int at = from;
        for (const SpeechClip &spoken : remade)
            m_clips.insert(at++, clipFrom(spoken));
        if (currentId >= 0)
            m_current = indexOfClip(currentId);
        if (queuedId >= 0)
            m_queued = indexOfClip(queuedId);
    }
    pump();
}
