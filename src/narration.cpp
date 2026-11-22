#include "narration.h"

#include "ai.h"
#include "aiservices.h"
#include "audiocache.h"
#include "library.h"
#include "reader.h"
#include "speechplayer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
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
      m_player(new SpeechPlayer(sampleRate, this)) {
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
    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (m_active && m_reader->bookId() != m_bookId)
            stop();
    });

    QSettings s = settings();
    m_rate = s.value(QStringLiteral("listen/rate"), 1.0).toDouble();
    m_economy = s.value(QStringLiteral("listen/economy"), false).toBool();
    m_player->setRate(m_rate);
}

Narration::~Narration() { stop(); }

QString Narration::directorPrompt() {
    QFile file(QStringLiteral(":/prompts/director.md"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
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
    if (!m_ai->canSpeak()) {
        setProblem(m_ai->describe(int(Ai::Error::NoKey)));
        emit needsKey();
        return;
    }
    m_bookId = m_reader->bookId();
    if (m_bookId <= 0)
        return;
    loadVoice();
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
    const qint64 from = chapter == m_startChapter ? m_startOffset : 0;
    const bool firstOfSession = m_clips.isEmpty();
    const QList<SpeechClip> made = Speech::clips(chapter, blocks, from, firstOfSession);
    const int before = int(m_clips.size());
    for (const SpeechClip &spoken : made) {
        Clip clip;
        static_cast<SpeechClip &>(clip) = spoken;
        clip.spans = Speech::timeline(clip);
        m_clips.append(clip);
    }
    // The first few clips go ahead without the director's notes, so the
    // voice starts at once and never waits on them.
    if (firstOfSession)
        for (int i = before; i < qMin<qsizetype>(before + 3, m_clips.size()); ++i)
            m_clips[i].immediate = true;
    if (directed() && m_ai->text() && !made.isEmpty())
        requestDirections(chapter);
    if (m_current < 0 && !m_clips.isEmpty())
        m_current = 0;
    pump();
}

bool Narration::waitingForDirections(const Clip &clip) const {
    if (clip.immediate || !directed() || m_moods.contains(clip.chapter) || !m_directionsAsked.contains(clip.chapter))
        return false;
    return nowMs() - m_directionsAsked.value(clip.chapter) < directionsPatienceMs;
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
    for (int i = from; i < m_clips.size() && i < from + 4; ++i) {
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
        const QString model = m_ai->speech()->speechModelName(m_economy);
        clip.key = AudioCache::keyFor(m_ai->provider()->id(), model, m_voice, clip.style, clip.text);
        const QString file = m_cache->lookup(clip.key);
        if (!file.isEmpty()) {
            decode(i, file);
            continue;
        }
        // Two requests at a time: enough to stay ahead of the voice, gentle
        // on the key's rate limits.
        if (working >= 2)
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
    request.text = clip.text;
    request.style = clip.style;
    request.voice = m_voice;
    request.economy = m_economy;
    request.stream = true;
    Ai::Reply *reply = m_ai->speech()->synthesize(request);
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
        if (clip.queued)
            m_player->appendAudio(i, pcm);
        else
            queue();
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
        if (!clip.text.isEmpty() && clip.samples > 0)
            m_samplesPerChar = 0.8 * m_samplesPerChar + 0.2 * double(clip.samples) / clip.text.size();
        trace(QStringLiteral("complete #%1 (%2 s)").arg(i).arg(clip.samples / double(sampleRate), 0, 'f', 1));
        if (clip.queued)
            m_player->finishTrack(i);
        m_cache->store(clip.key, clip.pcm, reply->sampleRate());
        pump();
    });
}

void Narration::decode(int index, const QString &file) {
    Clip &clip = m_clips[index];
    clip.state = Clip::Working;
    trace(QStringLiteral("cached #%1").arg(index));
    auto *ffmpeg = new QProcess(this);
    const QString key = clip.key;
    connect(ffmpeg, &QProcess::finished, this, [this, ffmpeg, key](int code, QProcess::ExitStatus status) {
        ffmpeg->deleteLater();
        int i = -1;
        for (int c = 0; c < m_clips.size(); ++c)
            if (m_clips[c].key == key && m_clips[c].state == Clip::Working && !m_clips[c].reply) {
                i = c;
                break;
            }
        if (i < 0)
            return;
        Clip &clip = m_clips[i];
        if (code != 0 || status != QProcess::NormalExit) {
            // A damaged cache entry is made again.
            clip.state = Clip::Waiting;
            clip.key.clear();
            pump();
            return;
        }
        clip.pcm = ffmpeg->readAllStandardOutput();
        clip.samples = clip.pcm.size() / 2;
        clip.state = Clip::Complete;
        if (clip.queued) {
            m_player->appendAudio(i, clip.pcm);
            m_player->finishTrack(i);
        }
        pump();
    });
    ffmpeg->start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                  {"-hide_banner", "-loglevel", "error", "-i", file, "-f", "s16le", "-ac", "1", "-ar",
                   QString::number(sampleRate), "pipe:1"});
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
    const qint64 total = expectedSamples(clip);
    showSentence(total > 0 ? double(sample) / double(total) : 0);
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
    if (at < 0 || at == m_sentence)
        return;
    m_sentence = at;
    m_sentenceStart = clip.sentences[at].start;
    m_sentenceEnd = clip.sentences[at].end;
    emit positionChanged();
    emit speaking(clip.chapter, m_sentenceStart, m_sentenceEnd);
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

void Narration::requestDirections(int chapter) {
    const QList<SpeechBlock> blocks = m_blocks.value(chapter);
    QString paragraphs;
    for (int i = 0; i < blocks.size(); ++i)
        if (blocks[i].kind != QStringLiteral("break"))
            paragraphs += QStringLiteral("[%1] %2\n").arg(i).arg(blocks[i].text.simplified().left(1600));
    if (paragraphs.isEmpty())
        return;
    Ai::TextRequest request;
    request.feature = QStringLiteral("director");
    request.fast = true;
    request.effort = QStringLiteral("low");
    request.system = directorPrompt();
    request.messages = {{true, QStringLiteral("Book: %1, by %2.\nChapter: %3.\nPerformance: %4.\n\n%5")
                                   .arg(m_reader->title(), m_reader->author(), m_reader->titleOfChapter(chapter),
                                        m_performance, paragraphs)}};
    QJsonArray moods;
    for (const QString &mood : Speech::moods())
        moods.append(mood);
    request.schema = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"changes", QJsonObject{
            {"type", "array"},
            {"items", QJsonObject{{"type", "object"},
                                  {"properties", QJsonObject{{"paragraph", QJsonObject{{"type", "integer"}}},
                                                             {"mood", QJsonObject{{"type", "string"}, {"enum", moods}}}}},
                                  {"required", QJsonArray{"paragraph", "mood"}}}}}}}},
        {"required", QJsonArray{"changes"}},
    };

    // The same chapter, performance, prompt and model always get the same
    // direction: asked once, then kept.
    const QString key = AudioCache::keyFor(m_ai->provider()->id(), m_ai->text()->textModelName(true),
                                           QStringLiteral("director"), request.system, request.messages.first().text);
    auto parse = [blocks](const QString &json) {
        QMap<int, QString> changes;
        for (const auto &value : QJsonDocument::fromJson(json.toUtf8()).object().value("changes").toArray()) {
            const QJsonObject change = value.toObject();
            const int paragraph = change.value("paragraph").toInt(-1);
            const QString mood = change.value("mood").toString();
            if (paragraph >= 0 && paragraph < blocks.size() && Speech::moods().contains(mood))
                changes.insert(paragraph, mood == QStringLiteral("neutral") ? QString() : mood);
        }
        return changes;
    };
    const QString kept = m_cache->directions(key);
    if (!kept.isEmpty()) {
        trace(QStringLiteral("director (kept): %1 changes for chapter %2").arg(parse(kept).size()).arg(chapter));
        applyMoods(chapter, parse(kept));
        return;
    }
    m_directionsAsked.insert(chapter, nowMs());
    Ai::Reply *reply = m_ai->text()->generate(request);
    m_ai->track(reply);
    connect(reply, &Ai::Reply::finished, this, [this, reply, chapter, key, parse] {
        reply->deleteLater();
        if (!m_active || !m_blocks.contains(chapter))
            return;
        QMap<int, QString> changes;
        if (!reply->error()) {
            changes = parse(reply->text());
            m_cache->storeDirections(key, reply->text());
        }
        QStringList said;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            said.append(QStringLiteral("%1 %2").arg(it.key()).arg(it.value().isEmpty() ? "neutral" : it.value()));
        trace(QStringLiteral("director: chapter %1: %2").arg(chapter).arg(said.join(", ")));
        // No direction (or a failed request) means the ordinary reading, not a wait.
        applyMoods(chapter, changes);
    });
}

// The chapter's clips not yet begun are made again, so each change of mood
// begins a clip of its own and every clip is read in one mood.
void Narration::applyMoods(int chapter, const QMap<int, QString> &moods) {
    m_moods.insert(chapter, moods);
    int end = -1;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].chapter == chapter)
            end = i + 1;
    int from = end;
    while (from > 0 && m_clips[from - 1].chapter == chapter && m_clips[from - 1].state == Clip::Waiting &&
           !m_clips[from - 1].immediate && !m_clips[from - 1].queued)
        --from;
    if (end > 0 && from < end && !moods.isEmpty()) {
        const QList<SpeechBlock> blocks = m_blocks.value(chapter);
        const Clip &first = m_clips[from];
        const qint64 offset = first.pauseMs > 0 ? blocks.value(first.block).start : first.start();
        QSet<int> breaks;
        for (auto it = moods.cbegin(); it != moods.cend(); ++it)
            breaks.insert(it.key());
        const QList<SpeechClip> remade = Speech::clips(chapter, blocks, offset, false, 1500, 600, breaks);
        m_clips.remove(from, end - from);
        int at = from;
        for (const SpeechClip &spoken : remade) {
            Clip clip;
            static_cast<SpeechClip &>(clip) = spoken;
            clip.spans = Speech::timeline(clip);
            m_clips.insert(at++, clip);
        }
    }
    pump();
}
