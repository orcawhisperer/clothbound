#include "narration.h"

#include "ai.h"
#include "aiservices.h"
#include "audiocache.h"
#include "director.h"
#include "cast.h"
#include "library.h"
#include "reader.h"
#include "speechplayer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <cmath>
#include <cstdio>

namespace {
QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
// DOGEAR_DEBUG traces the clips' lives on stderr.
void trace(const QString &message) {
    static const bool on = qEnvironmentVariableIsSet("DOGEAR_DEBUG");
    static const qint64 started = nowMs();
    if (on)
        fprintf(stderr, "narration %6.2fs %s\n", (nowMs() - started) / 1000.0, qPrintable(message));
}
// Clips after the first few wait this long for the director before going
// ahead in the ordinary reading.
constexpr qint64 directionsPatienceMs = 6000;
// Narration runs at about this many characters a minute at normal speed.
constexpr double charsPerMinute = 900;
constexpr int sampleRate = 24000;
}

Narration::Narration(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_library(library),
      m_director(new Director(ai, cache, reader, this)), m_player(new SpeechPlayer(sampleRate, this)) {
    connect(m_player, &SpeechPlayer::position, this, &Narration::heard);
    connect(m_player, &SpeechPlayer::starvedChanged, this, [this](bool starved) {
        if (m_wanted)
            setBuffering(starved);
    });
    connect(m_player, &SpeechPlayer::drained, this, [this] {
        // Everything queued has been heard: the end of the book, or the
        // next chapter's words haven't arrived yet.
        if (!m_active)
            return;
        if (m_queued + 1 < m_clips.size() && m_clips[m_queued + 1].state == Clip::Failed) {
            stallIfFailed();
            return;
        }
        const bool more = m_queued + 1 < m_clips.size() || m_requested.size() > m_loadedChapters.size() ||
                          nextChapterAfter(m_lastChapter) >= 0;
        if (!more)
            stop();
        else if (m_wanted)
            setBuffering(true);
    });
    m_directionsTimer.setSingleShot(true);
    connect(&m_directionsTimer, &QTimer::timeout, this, &Narration::pump);
    m_sleepTimer.setSingleShot(true);
    connect(&m_sleepTimer, &QTimer::timeout, this, [this] {
        pause();
        m_sleepAt = {};
        emit sleepChanged();
    });
    connect(m_reader, &Reader::speakable, this, &Narration::addBlocks);
    // The open book's narrator and style are known before listening starts,
    // so they can be shown and changed.
    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (m_active && m_reader->bookId() != m_bookId)
            stop();
        if (!m_active && m_reader->bookId() > 0) {
            m_bookId = m_reader->bookId();
            loadVoice();
        }
    });

    QSettings s = settings();
    m_rate = s.value(QStringLiteral("listen/rate"), 1.0).toDouble();
    m_economy = s.value(QStringLiteral("listen/economy"), false).toBool();
    m_player->setRate(m_rate);
}

Narration::~Narration() { stop(); }

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
    // The new reading takes over from the sentence being spoken.
    if (m_active && m_sentenceStart >= 0) {
        const int at = chapter();
        const qint64 offset = m_sentenceStart;
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
    // The new voice takes over from the sentence being spoken.
    if (m_active && m_sentenceStart >= 0) {
        const int at = chapter();
        const qint64 offset = m_sentenceStart;
        const bool wanted = m_wanted;
        start(at, offset);
        if (!wanted)
            pause();
    }
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
    m_bookId = m_reader->bookId();
    if (m_bookId <= 0)
        return;
    loadVoice();
    if (!m_ai->canSpeakWith(m_voice)) {
        if (AiServices::isBuiltinVoice(m_voice)) {
            setProblem(tr("This built-in voice isn't downloaded yet. Choose it again in the Narrator panel (V)."));
            emit voiceMissing();
        } else if (AiServices::isStudioVoice(m_voice)) {
            setProblem(tr("This Studio voice needs its model server running on this computer, chosen in Intelligence (✦)."));
            emit voiceMissing();
        } else {
            setProblem(m_ai->describe(int(Ai::Error::NoKey)));
            emit needsKey();
        }
        return;
    }
    m_active = true;
    m_wanted = true;
    m_following = true;
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
    // Direction kept from an earlier reading shapes the clips from the
    // start, so they match the audio made then.
    const bool kept = directed() && m_ai->textFor(QStringLiteral("director")) && keptDirections(chapter);
    // The cast too, kept from an earlier reading, or asked for below.
    bool castKept = true;
    if (m_cast && m_cast->enabled() && m_cast->available() && !m_cast->knows(chapter)) {
        if (const auto lines = m_cast->kept(chapter, blocks))
            m_cast->learn(chapter, *lines);
        else
            castKept = false;
    }
    const qint64 from = chapter == m_startChapter ? m_startOffset : 0;
    const bool firstOfSession = m_clips.isEmpty();
    const QList<SpeechClip> made = Speech::clipsFrom(chapter, blocks, from, breaksFor(chapter));
    const int before = int(m_clips.size());
    for (const SpeechClip &spoken : made)
        m_clips.append(clipFrom(spoken));
    // Otherwise the first few clips go ahead without direction, so the
    // voice starts at once and never waits on the director.
    if (firstOfSession && (!kept || !castKept))
        for (int i = before; i < qMin<qsizetype>(before + 3, m_clips.size()); ++i)
            m_clips[i].immediate = true;
    if (!castKept && !made.isEmpty())
        requestCast(chapter);
    if (directed() && m_ai->textFor(QStringLiteral("director")) && !made.isEmpty() && !kept)
        requestDirections(chapter);
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

bool Narration::waitingForDirections(const Clip &clip) const {
    if (clip.immediate || !directed() || m_moods.contains(clip.chapter) || !m_directionsAsked.contains(clip.chapter))
        return false;
    return nowMs() - m_directionsAsked.value(clip.chapter) < directionsPatienceMs;
}

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
    if (!m_active)
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
        if (waitingForDirections(clip)) {
            wait = directionsPatienceMs - (nowMs() - m_directionsAsked.value(clip.chapter));
            continue;
        }
        clip.style = styleFor(clip);
        // A character's voice that can't speak now (not downloaded, no key)
        // gives way to the narrator's.
        if (!clip.voice.isEmpty() && clip.voice != m_voice && !m_ai->canSpeakWith(clip.voice))
            clip.voice = m_voice;
        const QString voice = voiceOf(clip);
        const QString model = m_ai->speechFor(voice)->speechModelName(m_economy);
        clip.key = AudioCache::keyFor(m_ai->providerFor(voice)->id(), model, voice, clip.style, clip.text);
        const QString file = m_cache->lookup(clip.key);
        if (!file.isEmpty()) {
            decode(i, file);
            continue;
        }
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
        Clip &clip = m_clips[m_queued + 1];
        if (clip.state == Clip::Waiting || clip.state == Clip::Failed)
            break;
        ++m_queued;
        clip.queued = true;
        m_player->addTrack(m_queued);
        if (!clip.pcm.isEmpty())
            m_player->appendAudio(m_queued, clip.pcm);
        if (clip.state == Clip::Complete)
            m_player->finishTrack(m_queued);
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
    Ai::Reply *reply = m_ai->speechFor(request.voice)->synthesize(request);
    m_ai->track(reply);
    clip.reply = reply;
    clip.state = Clip::Working;
    trace(QStringLiteral("fetch #%1 (%2 chars%3)").arg(index).arg(clip.text.size())
              .arg(clip.style.isEmpty() ? QString() : QStringLiteral(", ") + clip.style));
    auto find = [this, reply]() {
        for (int i = 0; i < m_clips.size(); ++i)
            if (m_clips[i].reply == reply)
                return i;
        return -1;
    };
    connect(reply, &Ai::Reply::audioReceived, this, [this, find](const QByteArray &pcm) {
        const int i = find();
        if (i < 0)
            return;
        Clip &clip = m_clips[i];
        if (clip.pcm.isEmpty())
            trace(QStringLiteral("first audio #%1").arg(i));
        clip.pcm += pcm;
        if (const Ai::Reply *from = clip.reply)
            clip.boundaries = from->boundaries();
        if (clip.queued)
            m_player->appendAudio(i, pcm);
        else
            queue();
        checkHeadStart();
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, find] {
        reply->deleteLater();
        const int i = find();
        if (i < 0)
            return;
        Clip &clip = m_clips[i];
        clip.reply = nullptr;
        if (reply->error()) {
            clip.state = Clip::Failed;
            clip.pcm = QByteArray();
            trace(QStringLiteral("failed #%1: %2").arg(i).arg(reply->error().message));
            if (reply->error().kind != Ai::Error::Cancelled)
                setProblem(m_ai->describe(reply->error().kind, reply->error().message));
            if (clip.queued) {
                // Part of it was already playing: what arrived is heard, and
                // the voice stops at its end.
                m_player->finishTrack(i);
            }
            queue();
            return;
        }
        clip.state = Clip::Complete;
        clip.samples = clip.pcm.size() / 2;
        clip.boundaries = reply->boundaries();
        if (!clip.text.isEmpty() && clip.samples > 0)
            m_samplesPerChar = 0.8 * m_samplesPerChar + 0.2 * double(clip.samples) / clip.text.size();
        trace(QStringLiteral("complete #%1 (%2 s)").arg(i).arg(clip.samples / double(sampleRate), 0, 'f', 1));
        if (clip.queued)
            m_player->finishTrack(i);
        m_cache->store(clip.key, clip.pcm, reply->sampleRate(), clip.boundaries);
        pump();
    });
}

void Narration::decode(int index, const QString &file) {
    Clip &clip = m_clips[index];
    clip.state = Clip::Working;
    trace(QStringLiteral("cached #%1").arg(index));
    const QString key = clip.key;
    m_cache->decode(file, sampleRate, this, [this, key](const QByteArray &pcm, bool ok) {
        int i = -1;
        for (int c = 0; c < m_clips.size(); ++c)
            if (m_clips[c].key == key && m_clips[c].state == Clip::Working && !m_clips[c].reply) {
                i = c;
                break;
            }
        if (i < 0)
            return;
        Clip &clip = m_clips[i];
        if (!ok) {
            // A damaged cache entry is made again.
            clip.state = Clip::Waiting;
            clip.key.clear();
            pump();
            return;
        }
        clip.pcm = pcm;
        clip.samples = clip.pcm.size() / 2;
        clip.boundaries = m_cache->boundaries(key);
        clip.state = Clip::Complete;
        if (clip.queued) {
            m_player->appendAudio(i, clip.pcm);
            m_player->finishTrack(i);
        }
        checkHeadStart();
        pump();
    });
}

qint64 Narration::expectedSamples(const Clip &clip) const {
    if (clip.state == Clip::Complete && clip.samples > 0)
        return clip.samples;
    // Still arriving: judge by its length in letters, at the pace heard so far.
    return qMax(qint64(clip.pcm.size() / 2), qint64(clip.text.size() * m_samplesPerChar));
}

void Narration::heard(int track, qint64 sample) {
    if (!m_active || track < 0 || track >= m_clips.size())
        return;
    // The voice is moving.
    if (m_buffering)
        setBuffering(false);
    if (track != m_current) {
        const int previous = m_current;
        m_current = track;
        m_sentence = -1;
        trace(QStringLiteral("hearing #%1").arg(track));
        // A clip a couple behind is done with; its audio is in the cache.
        for (int i = 0; i < track - 1; ++i)
            if (m_clips[i].state == Clip::Complete)
                m_clips[i].pcm = QByteArray();
        const bool newChapter = previous >= 0 && m_clips[previous].chapter != m_clips[track].chapter;
        if (newChapter && m_sleepAtChapterEnd) {
            m_sleepAtChapterEnd = false;
            pause();
            emit sleepChanged();
        }
        emit positionChanged();
        pump();
    }
    const Clip &clip = m_clips[track];
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

void Narration::jumpTo(int index) {
    if (index < 0 || index >= m_clips.size())
        return;
    trace(QStringLiteral("jump to #%1").arg(index));
    m_player->clear();
    // Clips whose audio was let go are found again in the cache.
    for (int i = index; i < m_clips.size(); ++i) {
        Clip &clip = m_clips[i];
        clip.queued = false;
        if (clip.state == Clip::Complete && clip.pcm.isEmpty())
            clip.state = Clip::Waiting;
        if (clip.state == Clip::Failed)
            clip.state = Clip::Waiting;
    }
    m_current = index;
    m_queued = index - 1;
    m_sentence = -1;
    m_wanted = true;
    m_player->play();
    emit stateChanged();
    pump();
    if (!m_clips[index].sentences.isEmpty())
        showSentence(0);
}

void Narration::nextParagraph() {
    if (!m_active || m_current < 0)
        return;
    const Clip &now = m_clips.value(m_current);
    for (int i = m_current + 1; i < m_clips.size(); ++i)
        if (m_clips[i].block != now.block || m_clips[i].chapter != now.chapter) {
            jumpTo(i);
            return;
        }
}

void Narration::previousParagraph() {
    if (!m_active || m_current < 0)
        return;
    // A little way into a clip, back to its start; at its start, the one before.
    jumpTo(m_sentence > 0 ? m_current : qMax(0, m_current - 1));
}

void Narration::toggle() { playing() ? pause() : resume(); }

void Narration::pause() {
    if (!m_active)
        return;
    trace(QStringLiteral("pause"));
    m_wanted = false;
    m_player->pause();
    setBuffering(false);
    emit stateChanged();
}

void Narration::resume() {
    if (!m_active)
        return;
    trace(QStringLiteral("resume"));
    setProblem({});
    // A clip that couldn't be made is tried again.
    bool retry = false;
    for (int i = qMax(0, m_current); i < m_clips.size(); ++i)
        if (m_clips[i].state == Clip::Failed) {
            m_clips[i].state = Clip::Waiting;
            retry = true;
        }
    m_wanted = true;
    if (m_holdSamples <= 0)
        m_player->play();
    emit stateChanged();
    if (retry)
        pump();
}

void Narration::stop() {
    // The clips are let go first, so a request's ending finds nothing to update.
    const QList<Clip> clips = std::exchange(m_clips, {});
    for (const Clip &clip : clips)
        if (clip.reply)
            clip.reply->abort();
    m_directionsTimer.stop();
    m_player->pause();
    m_player->clear();
    m_current = -1;
    m_queued = -1;
    m_requested.clear();
    m_loadedChapters.clear();
    m_directionsAsked.clear();
    m_blocks.clear();
    m_moods.clear();
    m_lastChapter = -1;
    m_holdSamples = 0;
    m_sentence = -1;
    m_sentenceStart = m_sentenceEnd = -1;
    const bool was = m_active;
    m_active = false;
    m_wanted = false;
    m_buffering = false;
    if (was) {
        emit stateChanged();
        emit positionChanged();
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

void Narration::sleepIn(int minutes) {
    m_sleepAtChapterEnd = false;
    if (minutes <= 0) {
        m_sleepTimer.stop();
        m_sleepAt = {};
    } else {
        m_sleepAt = QDateTime::currentDateTime().addSecs(minutes * 60);
        m_sleepTimer.start(minutes * 60 * 1000);
    }
    emit sleepChanged();
}

void Narration::sleepAtEndOfChapter() {
    m_sleepTimer.stop();
    m_sleepAt = {};
    m_sleepAtChapterEnd = true;
    emit sleepChanged();
}

int Narration::sleepMinutes() const {
    if (!m_sleepAt.isValid())
        return 0;
    return qMax(1, int(std::ceil(QDateTime::currentDateTime().secsTo(m_sleepAt) / 60.0)));
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

Narration::Clip Narration::clipFrom(const SpeechClip &spoken) const {
    Clip clip;
    static_cast<SpeechClip &>(clip) = spoken;
    clip.spans = Speech::timeline(clip);
    clip.voice = m_cast ? m_cast->voiceAt(spoken.chapter, spoken.block) : m_voice;
    return clip;
}

QString Narration::voiceOf(const Clip &clip) const { return clip.voice.isEmpty() ? m_voice : clip.voice; }

void Narration::setCast(Cast *cast) {
    m_cast = cast;
    cast->setNarrator(m_voice);
    // A voice changed by hand, or the cast turned on or off: what's not yet
    // heard is read again by whoever reads it now.
    connect(cast, &Cast::recast, this, [this] {
        if (!m_active)
            return;
        QSet<int> chapters;
        for (const Clip &clip : std::as_const(m_clips))
            chapters.insert(clip.chapter);
        for (int chapter : chapters) {
            if (m_cast->enabled() && !m_cast->knows(chapter))
                requestCast(chapter);
            recut(chapter);
        }
    });
}

void Narration::requestCast(int chapter) {
    m_cast->ask(chapter, m_blocks.value(chapter), this, [this, chapter](const QMap<int, Cast::Line> &lines) {
        if (!m_active || !m_blocks.contains(chapter))
            return;
        m_cast->learn(chapter, lines);
        trace(QStringLiteral("cast: chapter %1: %2 lines").arg(chapter).arg(lines.size()));
        recut(chapter);
    });
}

void Narration::requestDirections(int chapter) {
    m_directionsAsked.insert(chapter, nowMs());
    m_director->ask(chapter, m_blocks.value(chapter), m_performance, this, [this, chapter](const QMap<int, QString> &moods) {
        if (!m_active || !m_blocks.contains(chapter))
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
           !m_clips[from - 1].immediate && !m_clips[from - 1].queued)
        --from;
    if (end > 0 && from < end) {
        const QList<SpeechBlock> blocks = m_blocks.value(chapter);
        const Clip &first = m_clips[from];
        const qint64 offset = first.pauseMs > 0 ? blocks.value(first.block).start : first.start();
        const QList<SpeechClip> remade = Speech::clipsFrom(chapter, blocks, offset, breaksFor(chapter));
        m_clips.remove(from, end - from);
        int at = from;
        for (const SpeechClip &spoken : remade)
            m_clips.insert(at++, clipFrom(spoken));
    }
    pump();
}
