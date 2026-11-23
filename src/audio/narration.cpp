#include "audio/narration.h"

#include "ai/ai.h"
#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "audio/cast.h"
#include "audio/director.h"
#include "library/library.h"
#include "audio/localspeech.h"
#include "reader/reader.h"
#include "audio/speechplayer.h"
#include "audio/voicepacks.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
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
// Clips after the first few wait this long for the director or cast before
// going ahead in the ordinary reading.
constexpr qint64 directionsPatienceMs = 6000;
// Narration runs at about this many characters a minute at normal speed.
constexpr double charsPerMinute = 900;
constexpr int sampleRate = 24000;
constexpr qint64 sleepFadeMs = 10000;
constexpr qint64 timeEmitIntervalMs = 250;

QString classicGeminiTrait(const QString &voiceId) {
    static const QHash<QString, QString> traits{
        {QStringLiteral("sulafat"), QStringLiteral("Warm")},
        {QStringLiteral("charon"), QStringLiteral("Informative")},
        {QStringLiteral("kore"), QStringLiteral("Firm")},
        {QStringLiteral("puck"), QStringLiteral("Upbeat")},
        {QStringLiteral("aoede"), QStringLiteral("Breezy")},
        {QStringLiteral("fenrir"), QStringLiteral("Excitable")},
        {QStringLiteral("zephyr"), QStringLiteral("Bright")},
        {QStringLiteral("orus"), QStringLiteral("Firm")},
        {QStringLiteral("leda"), QStringLiteral("Youthful")},
        {QStringLiteral("autonoe"), QStringLiteral("Bright")},
        {QStringLiteral("enceladus"), QStringLiteral("Breathy")},
        {QStringLiteral("iapetus"), QStringLiteral("Clear")},
        {QStringLiteral("umbriel"), QStringLiteral("Easy-going")},
        {QStringLiteral("algieba"), QStringLiteral("Smooth")},
        {QStringLiteral("despina"), QStringLiteral("Smooth")},
        {QStringLiteral("erinome"), QStringLiteral("Clear")},
        {QStringLiteral("algenib"), QStringLiteral("Gravelly")},
        {QStringLiteral("rasalgethi"), QStringLiteral("Informative")},
        {QStringLiteral("laomedeia"), QStringLiteral("Upbeat")},
        {QStringLiteral("achernar"), QStringLiteral("Soft")},
        {QStringLiteral("alnilam"), QStringLiteral("Firm")},
        {QStringLiteral("schedar"), QStringLiteral("Even")},
        {QStringLiteral("gacrux"), QStringLiteral("Mature")},
        {QStringLiteral("pulcherrima"), QStringLiteral("Forward")},
        {QStringLiteral("achird"), QStringLiteral("Friendly")},
        {QStringLiteral("zubenelgenubi"), QStringLiteral("Casual")},
        {QStringLiteral("vindemiatrix"), QStringLiteral("Gentle")},
        {QStringLiteral("sadachbia"), QStringLiteral("Lively")},
        {QStringLiteral("sadaltager"), QStringLiteral("Knowledgeable")},
        {QStringLiteral("callirrhoe"), QStringLiteral("Easy-going")},
    };
    return traits.value(voiceId.toLower());
}
}

Narration::Narration(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_library(library),
      m_director(new Director(ai, cache, reader, this)), m_player(new SpeechPlayer(sampleRate, this)) {
    connect(m_player, &SpeechPlayer::position, this, &Narration::heard);
    connect(m_player, &SpeechPlayer::volumeChanged, this, &Narration::volumeChanged);
    connect(m_player, &SpeechPlayer::sinkError, this, [this](const QString &message) {
        if (!m_active)
            return;
        pause();
        setProblem(message);
    });
    connect(m_player, &SpeechPlayer::starvedChanged, this, [this](bool starved) {
        if (m_wanted)
            setBuffering(starved);
    });
    connect(m_player, &SpeechPlayer::drained, this, [this] {
        // Everything queued has been heard: chapter end (for sleep), the end
        // of the book, or the next chapter's words haven't arrived yet.
        if (!m_active)
            return;
        if (m_sleepAtChapterEnd && m_current >= 0 && m_current < m_clips.size() &&
            (m_queued + 1 >= m_clips.size() || m_clips[m_queued + 1].chapter != m_clips[m_current].chapter)) {
            m_sleepAtChapterEnd = false;
            pause();
            emit sleepChanged();
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
    m_sleepTimer.setSingleShot(true);
    connect(&m_sleepTimer, &QTimer::timeout, this, &Narration::finishSleep);
    m_sleepTick.setInterval(1000);
    connect(&m_sleepTick, &QTimer::timeout, this, &Narration::onSleepTick);
    connect(m_reader, &Reader::speakable, this, &Narration::addBlocks);
    // The open book's narrator and style are known before listening starts,
    // so they can be shown and changed.
    connect(m_reader, &Reader::bookChanged, this, [this] {
        m_pendingStart = false;
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
    // Explicit external volume change supersedes any pending sleep-fade restore.
    const bool wasFading = m_sleepFading;
    m_sleepPausedRestore = false;
    m_sleepFading = false;
    m_preFadeVolume = -1.0;
    m_player->setVolume(volume);
    if (wasFading)
        emit sleepChanged();
}

int Narration::indexOfClip(int trackId) const {
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].id == trackId)
            return i;
    return -1;
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

int Narration::chapter() const {
    return m_current >= 0 && m_current < m_clips.size() ? m_clips[m_current].chapter : m_startChapter;
}

QString Narration::voiceAccent() const {
    if (m_voice.isEmpty())
        return {};
    if (AiServices::isBuiltinVoice(m_voice)) {
        if (auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:"))))
            if (local->packs())
                return local->packs()->voice(m_voice).value(QStringLiteral("accent")).toString();
        return {};
    }
    if (AiServices::isStudioVoice(m_voice)) {
        const QString model = m_ai ? m_ai->localSpeechModel() : QString();
        return model.isEmpty() ? tr("Studio") : model;
    }
    return classicGeminiTrait(m_voice);
}

void Narration::setFollowing(bool following) {
    if (following == m_following)
        return;
    m_following = following;
    emit stateChanged();
    if (following && m_sentenceStart >= 0)
        emit speaking(chapter(), m_sentenceStart, m_sentenceEnd);
}

void Narration::setRate(double rate) {
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
    static const QStringList known{"plain", "natural", "dramatic"};
    if (!known.contains(performance) || performance == m_performance)
        return;
    m_performance = performance;
    settings().setValue(QStringLiteral("listen/performance"), performance);
    if (m_reader->bookId() > 0)
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("performance"), performance);
    emit settingsChanged();
    // The new reading takes over from the sentence being spoken (or where we started).
    if (m_active) {
        const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
        const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
        const bool wanted = m_wanted;
        start(at, offset);
        if (!wanted)
            pause();
    }
}

void Narration::setEconomy(bool economy) {
    if (economy == m_economy)
        return;
    m_economy = economy;
    settings().setValue(QStringLiteral("listen/economy"), economy);
    emit settingsChanged();
}

void Narration::setVoice(const QString &id, const QString &name) {
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
    // The new voice takes over from the sentence being spoken (or where we started).
    if (m_active) {
        const int at = m_sentenceStart >= 0 ? chapter() : m_startChapter;
        const qint64 offset = m_sentenceStart >= 0 ? m_sentenceStart : m_startOffset;
        const bool wanted = m_wanted;
        start(at, offset);
        if (!wanted)
            pause();
    }
}

void Narration::useFallbackVoice() {
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
    auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:")));
    if (!local)
        return {};
    const QVariantMap best = local->bestInstalledVoice(m_reader ? m_reader->language() : QString());
    if (best.isEmpty())
        return {};
    const QString id = best.value(QStringLiteral("id")).toString();
    const QString name = best.value(QStringLiteral("name")).toString();
    if (id.isEmpty() || !m_ai->canSpeakWith(id))
        return {};
    return {id, name.isEmpty() ? id : name};
}

bool Narration::localPacksSupported() const {
    auto *local = qobject_cast<LocalSpeech *>(m_ai->providerFor(QStringLiteral("builtin:")));
    return local && local->packsSupported();
}

void Narration::updateFallbackOffer() {
    const auto [id, name] = bestLocalVoice();
    QString nextId;
    QString nextName;
    bool nextSetup = false;
    if (!id.isEmpty() && id != m_voice) {
        nextId = id;
        nextName = name;
    } else if (id.isEmpty() && !AiServices::isBuiltinVoice(m_voice) && localPacksSupported()) {
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
    m_voice = fbId;
    m_voiceName = fbName;
    if (m_cast)
        m_cast->setNarrator(m_voice);
    emit voiceChanged();
    setProblem({});
    setFallbackNotice(reason.isEmpty() ? tr("Switched to %1 offline").arg(fbName)
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
        // installed local voice for this book's language, or open VoicePanel
        // cleanly on the Built-in tab for one-click offline setup.
        const auto [fbId, fbName] = bestLocalVoice();
        if (!fbId.isEmpty()) {
            m_voice = fbId;
            m_voiceName = fbName;
            if (m_cast)
                m_cast->setNarrator(m_voice);
            emit voiceChanged();
            setFallbackNotice(tr("Using %1 offline").arg(fbName));
        } else if (localPacksSupported()) {
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
        // Cloud voice already at monthly budget cap: seamlessly switch to local if installed.
        const auto [fbId, fbName] = bestLocalVoice();
        if (!fbId.isEmpty()) {
            m_voice = fbId;
            m_voiceName = fbName;
            if (m_cast)
                m_cast->setNarrator(m_voice);
            emit voiceChanged();
            setFallbackNotice(tr("Using %1 offline (budget reached)").arg(fbName));
        }
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
    m_player->play();
    emit stateChanged();
    emit blocksWanted(chapter);
}

void Narration::addBlocks(int chapter, const QString &json) {
    if (!m_active || !m_requested.contains(chapter) || m_loadedChapters.contains(chapter))
        return;
    m_loadedChapters.insert(chapter);
    m_lastChapter = qMax(m_lastChapter, chapter);
    const QList<SpeechBlock> blocks = Speech::parseBlocks(json);
    m_blocks.insert(chapter, blocks);
    const bool isPdf = m_reader && m_reader->format() == QLatin1String("pdf");
    // Direction kept from an earlier reading shapes the clips from the
    // start, so they match the audio made then.
    bool kept = false;
    if (directed() && m_ai->textFor(QStringLiteral("director"))) {
        if (m_moods.contains(chapter)) {
            kept = true;
        } else if (isPdf) {
            if (const auto byPage = m_director->keptPdfBatch(chapter, m_performance)) {
                for (auto it = byPage->cbegin(); it != byPage->cend(); ++it)
                    m_moods.insert(it.key(), it.value());
                kept = true;
            }
        } else {
            kept = keptDirections(chapter);
        }
    }
    // The cast too, kept from an earlier reading, or asked for below.
    bool castKept = true;
    if (m_cast && m_cast->enabled() && m_cast->available() && !m_cast->knows(chapter)) {
        if (isPdf) {
            if (const auto byPage = m_cast->keptPdfBatch(chapter)) {
                for (auto it = byPage->cbegin(); it != byPage->cend(); ++it)
                    m_cast->learn(it.key(), it.value());
            } else {
                castKept = false;
            }
        } else if (const auto lines = m_cast->kept(chapter, blocks)) {
            m_cast->learn(chapter, *lines);
        } else {
            castKept = false;
        }
    }
    const qint64 from = chapter == m_startChapter ? m_startOffset : 0;
    const bool firstOfSession = m_clips.isEmpty();
    const QList<SpeechClip> made =
        Speech::clipsFrom(chapter, blocks, from, breaksFor(chapter), blockVoicesFor(chapter, blocks), m_voice);
    const int before = int(m_clips.size());
    for (const SpeechClip &spoken : made)
        m_clips.append(clipFrom(spoken));
    // Otherwise the first few clips go ahead without direction, so the
    // voice starts at once and never waits on the director or cast.
    if (firstOfSession && (!kept || !castKept))
        for (int i = before; i < qMin<qsizetype>(before + 3, m_clips.size()); ++i)
            m_clips[i].immediate = true;
    if (m_current < 0 && !m_clips.isEmpty())
        m_current = 0;
    // A voice made more slowly than it's heard needs a head start:
    // enough that, made while it plays, it never runs out.
    if (firstOfSession && !made.isEmpty()) {
        const double speed = speaker() ? speaker()->speedOf(m_voice) : 0;
        if (speed > 0 && speed < 1.2 * m_rate) {
            qint64 characters = 0;
            for (const SpeechClip &clip : made)
                characters += clip.text.size();
            const double seconds = characters * m_samplesPerChar / sampleRate;
            const double head = seconds * qMax(0.0, 1.0 - speed / m_rate) * 1.1 + 2.0;
            m_holdSamples = qint64(head * sampleRate);
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

int Narration::preparedPercent() const {
    if (m_holdSamples <= 0)
        return 100;
    qint64 have = 0;
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        have += m_clips[i].pcm.size() / 2;
    return int(qMin<qint64>(100, have * 100 / m_holdSamples));
}

// Plays once the head start is made.
void Narration::checkHeadStart() {
    if (m_holdSamples <= 0)
        return;
    emit positionChanged();
    if (preparedPercent() < 100)
        return;
    trace(QStringLiteral("head start made"));
    m_holdSamples = 0;
    if (m_wanted)
        m_player->play();
    emit stateChanged();
}

qint64 Narration::directionWaitLeft(const Clip &clip) const {
    if (clip.immediate)
        return 0;
    qint64 left = 0;
    if (directed() && !m_moods.contains(clip.chapter) && m_directionsAsked.contains(clip.chapter))
        left = qMax(left, directionsPatienceMs - (nowMs() - m_directionsAsked.value(clip.chapter)));
    if (m_cast && m_cast->enabled() && m_cast->available() && !m_cast->knows(clip.chapter) &&
        m_castAsked.contains(clip.chapter))
        left = qMax(left, directionsPatienceMs - (nowMs() - m_castAsked.value(clip.chapter)));
    return left;
}

bool Narration::waitingForDirections(const Clip &clip) const { return directionWaitLeft(clip) > 0; }

// The voice speaks through its own provider: built-in voices here, the rest
// in the cloud.
Ai::SpeechSynthesizer *Narration::speaker() const { return m_ai->speechFor(m_voice); }

// Moods need a voice that can take direction; built-in voices read plainly.
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
    // Do not prefetch or spend while paused or inactive.
    if (!m_active || !m_wanted)
        return;
    // Ask for the next chapter's words before these run out, skipping past
    // chapters with nothing to say (a cover, an image page).
    const int remaining = int(m_clips.size()) - qMax(0, m_current);
    if (remaining < 5 && m_lastChapter >= 0) {
        const int next = nextChapterAfter(m_lastChapter);
        if (next >= 0 && !m_requested.contains(next)) {
            m_requested.insert(next);
            emit blocksWanted(next);
        }
    }
    // If every remaining chapter was empty (cover/image pages only), stop cleanly.
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
        // A character's voice that can't speak now (not downloaded, no key)
        // gives way to the narrator's.
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
            setProblem(AiServices::isBuiltinVoice(m_voice)
                           ? tr("This built-in voice isn't downloaded yet.")
                           : AiServices::isStudioVoice(m_voice)
                                 ? tr("This Studio voice needs its model server running.")
                                 : m_ai->describe(int(Ai::Error::NoKey)));
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
        if (m_retryTimer.isActive())
            continue;
        // Two requests at a time: enough to stay ahead of the voice, gentle
        // on the key's rate limits. Built-in voices take their turns.
        if (working >= (local ? ahead : 2))
            continue;
        fetch(i);
        ++working;
    }
    if (wait > 0)
        m_directionsTimer.start(int(wait) + 50);
    queue();
}

// Clips join the player strictly in order, as soon as their audio starts to
// arrive, so the voice plays on without a gap.
void Narration::queue() {
    while (m_queued + 1 < m_clips.size()) {
        const int nextIndex = m_queued + 1;
        Clip &clip = m_clips[nextIndex];
        if (clip.state == Clip::Waiting || clip.state == Clip::Failed ||
            (clip.state == Clip::Working && !clip.reply))
            break;
        // Stop queuing at the chapter boundary when sleeping at chapter end.
        if (m_sleepAtChapterEnd && m_current >= 0 && m_current < m_clips.size() &&
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

// The voice has caught up with a clip that couldn't be made: it stops
// there, with the reason shown, until asked to go on (which tries again).
void Narration::stallIfFailed() {
    if (!m_wanted || m_queued + 1 >= m_clips.size() || m_clips[m_queued + 1].state != Clip::Failed ||
        !m_player->drainedOut())
        return;
    m_wanted = false;
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
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
        setProblem(AiServices::isBuiltinVoice(m_voice)
                       ? tr("This built-in voice isn't downloaded yet.")
                       : AiServices::isStudioVoice(m_voice)
                             ? tr("This Studio voice needs its model server running.")
                             : m_ai->describe(int(Ai::Error::NoKey)));
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
        if (clip.queued)
            m_player->appendAudio(clip.id, pcm);
        else
            queue();
        checkHeadStart();
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
            const Ai::Error err = reply->error();
            trace(QStringLiteral("failed #%1: %2").arg(i).arg(err.message));
            const bool isCloud = !AiServices::isOnThisComputer(voiceOf(clip));
            if ((err.kind == Ai::Error::Limited || err.kind == Ai::Error::Server) && !clip.queued && clip.tries < 3 &&
                m_wanted) {
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
                if (fallbackToLocalMidSession(i, reason))
                    return;
            }
            clip.state = Clip::Failed;
            clip.pcm = QByteArray();
            if (err.kind != Ai::Error::Cancelled) {
                setProblem(m_ai->describe(int(err.kind), err.message));
                if (isCloud &&
                    (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget ||
                     err.kind == Ai::Error::NoKey || err.kind == Ai::Error::BadKey))
                    updateFallbackOffer();
            }
            if (clip.queued) {
                // Part of it was already playing: what arrived is heard, and
                // the voice stops at its end.
                m_player->finishTrack(clip.id);
            }
            queue();
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
        if (clip.queued)
            m_player->finishTrack(clip.id);
        if (!clip.pcm.isEmpty() && reply->sampleRate() > 0)
            m_cache->store(clip.key, clip.pcm, reply->sampleRate(), clip.boundaries);
        if (m_current >= 0 && m_current < m_clips.size() && clip.chapter == m_clips[m_current].chapter) {
            emit positionChanged();
            emit timeChanged();
        }
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
        if (clip.queued) {
            m_player->appendAudio(clip.id, clip.pcm);
            m_player->finishTrack(clip.id);
        }
        checkHeadStart();
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
    // Still arriving: judge by its length in letters, at the pace heard so far.
    return qMax(qint64(clip.pcm.size() / 2), qint64(clip.text.size() * m_samplesPerChar));
}

void Narration::heard(int trackId, qint64 sample) {
    if (!m_active)
        return;
    const int track = indexOfClip(trackId);
    if (track < 0)
        return;
    // The voice is moving.
    if (m_buffering)
        setBuffering(false);
    if (track != m_current) {
        const int previous = m_current;
        const bool newChapter = previous >= 0 && previous < m_clips.size() &&
                                m_clips[previous].chapter != m_clips[track].chapter;
        if (newChapter && m_sleepAtChapterEnd) {
            m_sleepAtChapterEnd = false;
            pause();
            emit sleepChanged();
            return;
        }
        m_current = track;
        m_currentSample = 0;
        m_sentence = -1;
        trace(QStringLiteral("hearing #%1 (id %2)").arg(track).arg(trackId));
        // Keep two clips behind in memory so -15s rewind is instantaneous;
        // older clips are in the cache.
        for (int i = 0; i < track - 2; ++i)
            if (m_clips[i].state == Clip::Complete)
                m_clips[i].pcm = QByteArray();
        emit positionChanged();
        emit timeChanged();
        pump();
    }
    m_currentSample = qMax<qint64>(0, sample);
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
    // Where the voice said each sentence ends, when it did; otherwise by
    // the sentences' share of the clip.
    if (!clip.boundaries.isEmpty() && clip.boundaries.size() <= clip.sentences.size()) {
        int at = 0;
        while (at < clip.boundaries.size() && sample >= clip.boundaries[at])
            ++at;
        showSentenceAt(qMin(at, int(clip.sentences.size()) - 1));
        return;
    }
    const qint64 total = expectedSamples(clip);
    showSentence(total > 0 ? double(sample) / double(total) : 0);
}

qint64 Narration::sampleOfSentence(const Clip &clip, int sentence) const {
    if (sentence <= 0 || clip.sentences.isEmpty())
        return 0;
    if (!clip.boundaries.isEmpty() && sentence - 1 < clip.boundaries.size())
        return qMax<qint64>(0, clip.boundaries[sentence - 1]);
    if (sentence < clip.spans.size()) {
        const qint64 total = expectedSamples(clip);
        if (total > 0)
            return qBound<qint64>(0, qint64(std::ceil(clip.spans[sentence].first * double(total))), total - 1);
    }
    return 0;
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

void Narration::showSentence(double fraction) {
    if (m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    if (clip.sentences.isEmpty())
        return;
    const auto &spans = clip.spans;
    int at = int(spans.size()) - 1;
    for (int i = 0; i < spans.size(); ++i)
        if (fraction < spans[i].second) {
            at = i;
            break;
        }
    showSentenceAt(at);
}

void Narration::jumpTo(int index, qint64 sampleOffset, bool preservePause, int forceSentence) {
    if (index < 0 || index >= m_clips.size())
        return;
    const qint64 clampedSample = qMax<qint64>(0, sampleOffset);
    trace(QStringLiteral("jump to #%1 @ %2").arg(index).arg(clampedSample));
    m_holdSamples = 0;
    m_retryTimer.stop();
    setProblem({});
    m_player->clear();
    const int ahead = AiServices::isOnThisComputer(m_voice) ? 8 : 4;
    for (int i = 0; i < m_clips.size(); ++i) {
        Clip &clip = m_clips[i];
        if (i < index || i >= index + ahead) {
            if (clip.reply) {
                QPointer<Ai::Reply> obsolete = clip.reply;
                clip.reply = nullptr;
                obsolete->abort();
                if (clip.state == Clip::Working) {
                    clip.state = Clip::Waiting;
                    clip.pcm = QByteArray();
                }
            }
        }
        if (i >= index) {
            clip.queued = false;
            clip.tries = 0;
            if (clip.state == Clip::Complete && clip.pcm.isEmpty())
                clip.state = Clip::Waiting;
            if (clip.state == Clip::Failed)
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
        if (m_sleepPausedRestore || m_sleepFading)
            restoreSleepVolume();
        setBuffering(m_clips[index].state != Clip::Complete);
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
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    // A little way into a clip, back to its start; at its start, the one before.
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
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    const int curS = qMax(0, m_sentence);
    if (clip.pauseMs == 0 && curS + 1 < clip.sentences.size()) {
        const qint64 sample = sampleOfSentence(clip, curS + 1);
        jumpTo(m_current, sample, true, curS + 1);
        return;
    }
    for (int i = m_current + 1; i < m_clips.size(); ++i) {
        if (m_clips[i].pauseMs == 0 && !m_clips[i].sentences.isEmpty()) {
            jumpTo(i, 0, true, 0);
            return;
        }
    }
    const int nextCh = nextChapterAfter(m_lastChapter);
    if (nextCh >= 0 && !m_requested.contains(nextCh)) {
        m_requested.insert(nextCh);
        emit blocksWanted(nextCh);
    }
}

void Narration::prevSentence() {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return;
    const Clip &clip = m_clips[m_current];
    const int curS = qMax(0, m_sentence);
    if (clip.pauseMs == 0 && curS > 0) {
        const qint64 sample = sampleOfSentence(clip, curS - 1);
        jumpTo(m_current, sample, true, curS - 1);
        return;
    }
    for (int i = m_current - 1; i >= 0; --i) {
        if (m_clips[i].pauseMs == 0 && !m_clips[i].sentences.isEmpty()) {
            const int lastS = int(m_clips[i].sentences.size()) - 1;
            const qint64 sample = sampleOfSentence(m_clips[i], lastS);
            jumpTo(i, sample, true, lastS);
            return;
        }
    }
    jumpTo(m_current, 0, true, 0);
}

void Narration::skipSeconds(int delta) {
    if (delta == 0)
        return;
    seekBySamples(qint64(delta) * sampleRate);
}

void Narration::seekByUs(qint64 deltaUs) {
    if (deltaUs == 0)
        return;
    seekBySamples(deltaUs * sampleRate / 1000000LL);
}

void Narration::seekBySamples(qint64 deltaSamples) {
    if (!m_active || m_clips.isEmpty() || m_current < 0 || m_current >= m_clips.size())
        return;
    int idx = m_current;
    qint64 pos = m_currentSample + deltaSamples;

    if (deltaSamples > 0) {
        while (idx < m_clips.size()) {
            const qint64 len = qMax<qint64>(1, expectedSamples(m_clips[idx]));
            if (pos < len)
                break;
            if (idx + 1 >= m_clips.size()) {
                const int nextCh = nextChapterAfter(m_lastChapter);
                if (nextCh >= 0 && !m_requested.contains(nextCh)) {
                    m_requested.insert(nextCh);
                    emit blocksWanted(nextCh);
                }
                pos = qMax<qint64>(0, len - sampleRate);
                break;
            }
            pos -= len;
            ++idx;
        }
        while (idx + 1 < m_clips.size() && m_clips[idx].pauseMs > 0) {
            ++idx;
            pos = 0;
        }
    } else {
        while (pos < 0 && idx > 0) {
            --idx;
            const qint64 len = qMax<qint64>(1, expectedSamples(m_clips[idx]));
            pos += len;
        }
        if (pos < 0)
            pos = 0;
        if (m_clips[idx].pauseMs > 0) {
            if (idx > 0) {
                --idx;
                pos = qMax<qint64>(0, expectedSamples(m_clips[idx]) - sampleRate);
            } else if (idx + 1 < m_clips.size()) {
                ++idx;
                pos = 0;
            }
        }
    }
    jumpTo(idx, pos, true);
}

void Narration::seekToUs(qint64 targetUs) {
    if (!m_active || m_clips.isEmpty() || m_current < 0 || m_current >= m_clips.size())
        return;
    const int at = m_clips[m_current].chapter;
    qint64 targetSamples = qMax<qint64>(0, targetUs * sampleRate / 1000000LL);
    int firstInChapter = -1;
    int lastInChapter = -1;
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].chapter == at) {
            if (firstInChapter < 0)
                firstInChapter = i;
            lastInChapter = i;
        }
    }
    if (firstInChapter < 0)
        return;
    int idx = firstInChapter;
    qint64 pos = targetSamples;
    while (idx < lastInChapter) {
        const qint64 len = qMax<qint64>(1, expectedSamples(m_clips[idx]));
        if (pos < len)
            break;
        pos -= len;
        ++idx;
    }
    const qint64 lastLen = qMax<qint64>(1, expectedSamples(m_clips[idx]));
    pos = qBound<qint64>(0, pos, qMax<qint64>(0, lastLen - 1));
    if (m_clips[idx].pauseMs > 0 && idx + 1 <= lastInChapter) {
        ++idx;
        pos = 0;
    }
    jumpTo(idx, pos, true);
}

void Narration::toggle() { playing() ? pause() : resume(); }

void Narration::pause() {
    if (!m_active)
        return;
    trace(QStringLiteral("pause"));
    const bool wasFading = m_sleepFading;
    if (m_sleepFading && !m_sleepPausedRestore)
        restoreSleepVolume();
    m_wanted = false;
    m_directionsTimer.stop();
    m_retryTimer.stop();
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
    if (wasFading)
        emit sleepChanged();
}

void Narration::resume() {
    if (!m_active)
        return;
    trace(QStringLiteral("resume"));
    setProblem({});
    const bool wasFading = m_sleepFading;
    if (m_sleepPausedRestore || m_sleepFading)
        restoreSleepVolume();
    // A clip that couldn't be made is tried again.
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        if (m_clips[i].state == Clip::Failed) {
            m_clips[i].state = Clip::Waiting;
            m_clips[i].tries = 0;
        }
    m_wanted = true;
    if (m_holdSamples <= 0)
        m_player->play();
    emit stateChanged();
    if (wasFading)
        emit sleepChanged();
    pump();
}

void Narration::stop() {
    ++m_session;
    // The clips are let go first, so a request's ending finds nothing to update.
    const QList<Clip> clips = std::exchange(m_clips, {});
    for (const Clip &clip : clips)
        if (clip.reply)
            clip.reply->abort();
    m_director->cancel();
    if (m_cast)
        m_cast->cancel();
    m_directionsTimer.stop();
    m_retryTimer.stop();
    const bool hadSleep = m_sleepAt.isValid() || m_sleepAtChapterEnd || m_sleepFading;
    m_sleepTimer.stop();
    m_sleepTick.stop();
    m_sleepAt = {};
    m_lastSleepSec = 0;
    m_sleepAtChapterEnd = false;
    if (m_sleepFading || m_sleepPausedRestore)
        restoreSleepVolume();
    m_player->pause();
    m_player->clear();
    m_current = -1;
    m_queued = -1;
    m_currentSample = 0;
    m_lastTimeEmitMs = 0;
    m_lastReportedSec = -1;
    m_requested.clear();
    m_loadedChapters.clear();
    m_directionsAsked.clear();
    m_castAsked.clear();
    m_directorBatchesAsked.clear();
    m_castBatchesAsked.clear();
    m_blocks.clear();
    m_moods.clear();
    m_lastChapter = -1;
    m_holdSamples = 0;
    m_sentence = -1;
    m_sentenceStart = m_sentenceEnd = -1;
    clearFallbackOffer();
    setFallbackNotice({});
    const bool was = m_active;
    m_active = false;
    m_wanted = false;
    m_buffering = false;
    if (hadSleep)
        emit sleepChanged();
    if (was) {
        emit stateChanged();
        emit positionChanged();
        emit timeChanged();
    }
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
    const int at = m_clips[m_current].chapter;
    qint64 samples = 0;
    for (int i = 0; i < m_current; ++i)
        if (m_clips[i].chapter == at)
            samples += expectedSamples(m_clips[i]);
    samples += qMin(m_currentSample, expectedSamples(m_clips[m_current]));
    return samples * 1000000LL / sampleRate;
}

qint64 Narration::durationUs() const {
    if (!m_active || m_current < 0 || m_current >= m_clips.size())
        return 0;
    const int at = m_clips[m_current].chapter;
    qint64 samples = 0;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].chapter == at)
            samples += expectedSamples(m_clips[i]);
    return samples * 1000000LL / sampleRate;
}

void Narration::restoreSleepVolume() {
    const double saved = m_preFadeVolume;
    m_preFadeVolume = -1.0;
    m_sleepFading = false;
    m_sleepPausedRestore = false;
    if (saved >= 0.0)
        m_player->setVolume(saved);
}

void Narration::onSleepTick() {
    if (!m_sleepAt.isValid()) {
        m_sleepTick.stop();
        return;
    }
    const qint64 leftMs = QDateTime::currentDateTime().msecsTo(m_sleepAt);
    if (leftMs <= 0) {
        finishSleep();
        return;
    }
    if (leftMs <= sleepFadeMs && m_active && m_wanted) {
        if (!m_sleepFading) {
            m_sleepFading = true;
            if (m_preFadeVolume < 0.0)
                m_preFadeVolume = m_player->volume();
            m_sleepTick.setInterval(100);
        }
        const double t = qBound(0.0, double(leftMs) / double(sleepFadeMs), 1.0);
        m_player->setVolume(m_preFadeVolume * t * t);
    } else if (m_sleepFading && !m_wanted) {
        restoreSleepVolume();
        m_sleepTick.setInterval(1000);
        emit sleepChanged();
    } else if (!m_wanted && m_sleepTick.interval() < 1000) {
        m_sleepTick.setInterval(1000);
    }
    const int secs = sleepSecondsLeft();
    if (secs != m_lastSleepSec || m_sleepFading) {
        m_lastSleepSec = secs;
        emit sleepChanged();
    }
}

void Narration::finishSleep() {
    if (m_preFadeVolume < 0.0)
        m_preFadeVolume = m_player->volume();
    m_player->setVolume(0.0);
    m_sleepFading = false;
    m_sleepPausedRestore = true;
    m_sleepTimer.stop();
    m_sleepTick.stop();
    m_sleepAt = {};
    m_lastSleepSec = 0;
    pause();
    emit sleepChanged();
}

void Narration::sleepIn(int minutes) {
    const bool wasChapterEnd = m_sleepAtChapterEnd;
    m_sleepAtChapterEnd = false;
    if (m_sleepFading || m_sleepPausedRestore)
        restoreSleepVolume();
    if (minutes <= 0) {
        m_sleepTimer.stop();
        m_sleepTick.stop();
        m_sleepAt = {};
        m_lastSleepSec = 0;
    } else {
        m_sleepAt = QDateTime::currentDateTime().addSecs(minutes * 60);
        m_lastSleepSec = minutes * 60;
        m_sleepTimer.start(minutes * 60 * 1000);
        m_sleepTick.setInterval(1000);
        m_sleepTick.start();
    }
    emit sleepChanged();
    if (wasChapterEnd && m_active && m_wanted)
        pump();
}

void Narration::sleepAtEndOfChapter() {
    if (m_sleepFading || m_sleepPausedRestore)
        restoreSleepVolume();
    m_sleepTimer.stop();
    m_sleepTick.stop();
    m_sleepAt = {};
    m_lastSleepSec = 0;
    m_sleepAtChapterEnd = true;
    emit sleepChanged();
}

int Narration::sleepMinutes() const {
    if (!m_sleepAt.isValid())
        return 0;
    return qMax(1, int(std::ceil(QDateTime::currentDateTime().msecsTo(m_sleepAt) / 60000.0)));
}

int Narration::sleepSecondsLeft() const {
    if (!m_sleepAt.isValid())
        return 0;
    return int(qMax<qint64>(0, (QDateTime::currentDateTime().msecsTo(m_sleepAt) + 999) / 1000));
}

bool Narration::keptDirections(int chapter) {
    const auto kept = m_director->kept(chapter, m_blocks.value(chapter), m_performance);
    if (!kept)
        return false;
    trace(QStringLiteral("director (kept): %1 changes for chapter %2").arg(kept->size()).arg(chapter));
    m_moods.insert(chapter, *kept);
    return true;
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

QString Narration::voiceOf(const Clip &clip) const { return clip.voice.isEmpty() ? m_voice : clip.voice; }

void Narration::setCast(Cast *cast) {
    m_cast = cast;
    if (!cast)
        return;
    cast->setNarrator(m_voice);
    // A voice changed by hand, or the cast turned on or off: what's not yet
    // heard is read again by whoever reads it now.
    connect(cast, &Cast::recast, this, [this] {
        if (!m_active || !m_cast)
            return;
        const bool isPdf = m_reader && m_reader->format() == QLatin1String("pdf");
        QSet<int> chapters;
        for (const Clip &clip : std::as_const(m_clips))
            chapters.insert(clip.chapter);
        for (int chapter : chapters) {
            if (m_cast->enabled() && m_cast->available() && !m_cast->knows(chapter)) {
                if (isPdf) {
                    if (const auto byPage = m_cast->keptPdfBatch(chapter)) {
                        for (auto it = byPage->cbegin(); it != byPage->cend(); ++it)
                            m_cast->learn(it.key(), it.value());
                    } else {
                        requestCast(chapter);
                    }
                } else if (const auto lines = m_cast->kept(chapter, m_blocks.value(chapter))) {
                    m_cast->learn(chapter, *lines);
                } else {
                    requestCast(chapter);
                }
            }
            recut(chapter);
        }
    });
}

void Narration::requestCast(int chapter) {
    if (!m_cast)
        return;
    const quint64 session = m_session;
    const int bookId = m_bookId;
    if (m_reader && m_reader->format() == QLatin1String("pdf")) {
        const auto [first, last] = Director::pdfBatchRange(m_reader, chapter);
        if (m_castBatchesAsked.contains(first))
            return;
        m_castBatchesAsked.insert(first);
        const qint64 askedAt = nowMs();
        for (int p = first; p <= last; ++p)
            m_castAsked.insert(p, askedAt);
        m_cast->askPdfBatch(chapter, this, [this, session, bookId, first, last](const QHash<int, QMap<int, Cast::Line>> &byPage) {
            if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_cast)
                return;
            m_castBatchesAsked.remove(first);
            for (int p = first; p <= last; ++p) {
                m_castAsked.remove(p);
                m_cast->learn(p, byPage.value(p));
                if (m_blocks.contains(p))
                    recut(p);
            }
            trace(QStringLiteral("cast pdf batch %1-%2").arg(first).arg(last));
        });
        return;
    }
    m_castAsked.insert(chapter, nowMs());
    m_cast->ask(chapter, m_blocks.value(chapter), this, [this, session, bookId, chapter](const QMap<int, Cast::Line> &lines) {
        if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_blocks.contains(chapter) || !m_cast)
            return;
        m_castAsked.remove(chapter);
        m_cast->learn(chapter, lines);
        trace(QStringLiteral("cast: chapter %1: %2 lines").arg(chapter).arg(lines.size()));
        recut(chapter);
    });
}

void Narration::requestDirections(int chapter) {
    const quint64 session = m_session;
    const int bookId = m_bookId;
    if (m_reader && m_reader->format() == QLatin1String("pdf")) {
        const auto [first, last] = Director::pdfBatchRange(m_reader, chapter);
        if (m_directorBatchesAsked.contains(first))
            return;
        m_directorBatchesAsked.insert(first);
        const qint64 askedAt = nowMs();
        for (int p = first; p <= last; ++p)
            m_directionsAsked.insert(p, askedAt);
        m_director->askPdfBatch(chapter, m_performance, this,
                                [this, session, bookId, first, last](const QHash<int, QMap<int, QString>> &byPage) {
                                    if (!m_active || session != m_session || m_reader->bookId() != bookId)
                                        return;
                                    m_directorBatchesAsked.remove(first);
                                    bool anyRecut = false;
                                    for (int p = first; p <= last; ++p) {
                                        m_directionsAsked.remove(p);
                                        const QMap<int, QString> moods = byPage.value(p);
                                        m_moods.insert(p, moods);
                                        if (m_blocks.contains(p) && !moods.isEmpty()) {
                                            recut(p);
                                            anyRecut = true;
                                        }
                                    }
                                    trace(QStringLiteral("director pdf batch %1-%2").arg(first).arg(last));
                                    if (!anyRecut)
                                        pump();
                                });
        return;
    }
    m_directionsAsked.insert(chapter, nowMs());
    m_director->ask(chapter, m_blocks.value(chapter), m_performance, this,
                    [this, session, bookId, chapter](const QMap<int, QString> &moods) {
                        if (!m_active || session != m_session || m_reader->bookId() != bookId || !m_blocks.contains(chapter))
                            return;
                        QStringList said;
                        for (auto it = moods.cbegin(); it != moods.cend(); ++it)
                            said.append(QStringLiteral("%1 %2").arg(it.key()).arg(it.value().isEmpty() ? "neutral" : it.value()));
                        trace(QStringLiteral("director: chapter %1: %2").arg(chapter).arg(said.join(", ")));
                        applyMoods(chapter, moods);
                    });
}

// The chapter's clips not yet begun are made again, so each change of mood
// begins a clip of its own and every clip is read in one mood.
void Narration::applyMoods(int chapter, const QMap<int, QString> &moods) {
    m_moods.insert(chapter, moods);
    if (!moods.isEmpty())
        recut(chapter);
    else
        pump();
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
