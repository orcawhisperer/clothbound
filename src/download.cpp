#include "download.h"

#include "aiservices.h"
#include "audiocache.h"
#include "cast.h"
#include "director.h"
#include "narration.h"
#include "reader.h"

#include <cmath>

namespace {
// About how fast narration goes, in characters a minute at normal speed.
constexpr double charsPerMinute = 900;
// Requests at a time: the slower tier answers in its own time, so a few
// run side by side.
constexpr int parallel = 3;
}

Download::Download(AiServices *ai, AudioCache *cache, Reader *reader, Narration *narration, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_narration(narration),
      m_director(new Director(ai, cache, reader, this)) {
    connect(reader, &Reader::speakable, this, &Download::addBlocks);
    // The chapters come from the open book's pages, so a download cancels
    // cleanly when its book closes; what's made is kept for next time.
    connect(reader, &Reader::bookChanged, this, [this] {
        if (m_active && m_reader->bookId() != m_bookId) {
            cancel();
            setProblem(tr("The download stopped when the book closed. What's made is kept; start it again to finish."));
        }
    });
    connect(cache, &AudioCache::stored, this, [this](const QString &key) {
        if (m_storing.remove(key))
            checkFinished();
    });
    connect(cache, &AudioCache::storeFailed, this, [this](const QString &key) {
        if (m_storing.remove(key) && m_active)
            stopWith(tr("Couldn't save a clip to the audio cache."));
    });
    connect(cache, &AudioCache::storesIdle, this, [this] {
        if (!m_storing.isEmpty()) {
            m_storing.clear();
            checkFinished();
        }
    });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        setProblem({});
        pump();
    });
}

Download::~Download() {
    ++m_session;
    m_active = false;
    m_director->cancel();
    if (m_cast)
        m_cast->cancel();
    const auto replies = m_replies;
    for (const auto &reply : replies)
        if (reply)
            reply->abort();
}

void Download::setCast(Cast *cast) { m_cast = cast; }

int Download::secondsLeft() const {
    const qint64 ms = m_activeMs + (running() && m_clock.isValid() ? m_clock.elapsed() : 0);
    if (m_audioSeconds < 30 || ms < 5000)
        return -1;
    // Audio made for each second spent: a book's minutes divided by it.
    const double pace = m_audioSeconds / (ms / 1000.0);
    return int(std::ceil(qMax(0.0, m_minutesTotal - m_minutesReady) * 60.0 / pace));
}

QList<int> Download::chaptersBetween(int first, int last) const {
    QList<int> chapters;
    const QVariantList spine = m_reader->manifest().value(QStringLiteral("spine")).toList();
    for (int i = qMax(0, first); i <= last && i < spine.size(); ++i)
        if (spine[i].toMap().value(QStringLiteral("linear")).toBool() && m_reader->charactersIn(i) > 0)
            chapters.append(i);
    return chapters;
}

double Download::estimatedMinutes(int chapter) const { return m_reader->charactersIn(chapter) / charsPerMinute; }

QVariantMap Download::estimate(int first, int last) const {
    double minutes = 0;
    const QList<int> chapters = chaptersBetween(first, last);
    for (int chapter : chapters)
        minutes += estimatedMinutes(chapter);
    const bool local = AiServices::isOnThisComputer(m_narration->voice());
    return {{"chapters", chapters.size()},
            {"minutes", minutes},
            {"local", local},
            {"cost", local ? 0.0 : m_ai->listeningCost(minutes, m_narration->economy(), true)}};
}

void Download::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit changed();
}

void Download::start(int first, int last) {
    m_plannedKeys.clear();
    cancel();
    const QString voice = m_narration->voice();
    if (!m_ai->canSpeakWith(voice)) {
        setProblem(AiServices::isBuiltinVoice(voice) ? tr("This built-in voice isn't downloaded yet.")
                   : AiServices::isStudioVoice(voice) ? tr("This Studio voice needs its model server running.")
                   : m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    m_chapters = chaptersBetween(first, last);
    if (m_chapters.isEmpty())
        return;
    m_bookId = m_reader->bookId();
    m_voice = voice;
    m_performance = m_narration->performance();
    m_economy = m_narration->economy();
    m_local = AiServices::isOnThisComputer(m_voice);
    m_flex = !m_local;
    m_active = true;
    m_paused = false;
    m_finished = false;
    m_nextChapter = 0;
    m_minutesReady = 0;
    m_minutesTotal = 0;
    for (int chapter : std::as_const(m_chapters))
        m_minutesTotal += estimatedMinutes(chapter);
    m_audioSeconds = 0;
    m_activeMs = 0;
    m_clock.start();
    setProblem({});
    emit changed();
    emit progressChanged();
    planAhead();
}

// A chapter or two is planned ahead of the requests, so they never wait
// on the reading surface.
void Download::planAhead() {
    while (running() && m_waiting.size() < 2 && m_queue.size() < 12) {
        int chapter = -1;
        if (!m_replan.isEmpty())
            chapter = m_replan.takeFirst();
        else if (m_nextChapter < m_chapters.size())
            chapter = m_chapters[m_nextChapter++];
        else
            break;
        m_waiting.insert(chapter);
        emit blocksWanted(chapter);
    }
    checkFinished();
}

void Download::addBlocks(int chapter, const QString &json) {
    if (!running() || !m_waiting.contains(chapter))
        return;
    const QList<SpeechBlock> blocks = Speech::parseBlocks(json);
    // Who speaks what first (with a full cast), then the director's moods.
    if (m_cast && m_cast->enabled() && m_cast->available() && !m_cast->knows(chapter)) {
        if (const auto lines = m_cast->kept(chapter, blocks)) {
            m_cast->learn(chapter, *lines);
        } else {
            const quint64 session = m_session;
            const int bookId = m_bookId;
            m_cast->ask(chapter, blocks, this, [this, session, bookId, chapter, blocks](const QMap<int, Cast::Line> &lines) {
                if (!running() || session != m_session || m_reader->bookId() != bookId || !m_waiting.contains(chapter))
                    return;
                if (m_cast)
                    m_cast->learn(chapter, lines);
                direct(chapter, blocks);
            });
            return;
        }
    }
    direct(chapter, blocks);
}

void Download::direct(int chapter, const QList<SpeechBlock> &blocks) {
    const Ai::SpeechSynthesizer *speaker = m_ai->speechFor(m_voice);
    if (m_performance == QStringLiteral("plain") || !speaker || !speaker->supportsStyle() || !m_director->canDirect(blocks)) {
        plan(chapter, blocks, {});
        return;
    }
    if (const auto kept = m_director->kept(chapter, blocks, m_performance)) {
        plan(chapter, blocks, *kept);
        return;
    }
    const quint64 session = m_session;
    const int bookId = m_bookId;
    m_director->ask(chapter, blocks, m_performance, this, [this, session, bookId, chapter, blocks](const QMap<int, QString> &moods) {
        if (running() && session == m_session && m_reader->bookId() == bookId && m_waiting.contains(chapter))
            plan(chapter, blocks, moods);
    });
}

void Download::plan(int chapter, const QList<SpeechBlock> &blocks, const QMap<int, QString> &moods) {
    m_waiting.remove(chapter);
    Ai::Provider *baseProvider = m_ai->providerFor(m_voice);
    Ai::SpeechSynthesizer *speaker = m_ai->speechFor(m_voice);
    if (!baseProvider || !speaker) {
        if (!m_replan.contains(chapter))
            m_replan.prepend(chapter);
        stopWith(AiServices::isBuiltinVoice(m_voice) ? tr("This built-in voice isn't downloaded yet.")
                 : AiServices::isStudioVoice(m_voice) ? tr("This Studio voice needs its model server running.")
                 : m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    const QString model = speaker->speechModelName(m_economy);
    const bool dramatic = m_performance == QStringLiteral("dramatic");
    const bool styled = m_performance != QStringLiteral("plain") && speaker->supportsStyle();
    double planned = 0;
    QSet<int> breaks = Director::breaks(moods);
    if (m_cast)
        breaks.unite(m_cast->breaks(chapter, blocks));
    m_plannedKeys[chapter].clear();
    for (const SpeechClip &clip : Speech::clips(chapter, blocks, breaks)) {
        if (clip.text.isEmpty())
            continue;
        Job job;
        job.text = clip.text;
        job.voice = m_cast ? m_cast->voiceAt(chapter, clip.block) : m_voice;
        if (job.voice.isEmpty() || !m_ai->canSpeakWith(job.voice))
            job.voice = m_voice;
        job.style = styled ? Speech::styleFor(Speech::moodAt(moods, clip.block), dramatic) : QString();
        for (const SpeechSentence &sentence : clip.sentences)
            job.sentences.append(sentence.text);
        Ai::Provider *jobProvider = m_ai->providerFor(job.voice);
        Ai::SpeechSynthesizer *reader = m_ai->speechFor(job.voice);
        if (!jobProvider || !reader) {
            job.voice = m_voice;
            jobProvider = baseProvider;
            reader = speaker;
        }
        job.key = AudioCache::keyFor(jobProvider->id(), reader ? reader->speechModelName(m_economy) : model,
                                     job.voice, job.style, job.text);
        job.minutes = job.text.size() / charsPerMinute;
        m_plannedKeys[chapter].append(job.key);
        planned += job.minutes;
        if (!m_cache->lookupPath(job.key).isEmpty())
            m_minutesReady += job.minutes;  // heard or downloaded before
        else
            m_queue.append(job);
    }
    // The chapter's length was a guess until now.
    m_minutesTotal += planned - estimatedMinutes(chapter);
    emit progressChanged();
    pump();
    planAhead();
}

void Download::pump() {
    if (!running() || m_retry.isActive())
        return;
    m_replies.removeAll(nullptr);
    while (m_replies.size() < (m_local ? 1 : parallel) && !m_queue.isEmpty()) {
        Job job = m_queue.takeFirst();
        const double cost = m_local ? 0.0 : m_ai->listeningCost(job.minutes, m_economy, m_flex);
        if (!m_local && !m_ai->fitsBudget(cost)) {
            m_queue.prepend(job);
            stopWith(m_ai->describe(int(Ai::Error::Budget)));
            return;
        }
        Ai::SpeechRequest request;
        request.feature = QStringLiteral("download");
        request.text = Speech::speakable(job.text);
        request.style = job.style;
        request.voice = job.voice;
        request.economy = m_economy;
        request.stream = false;
        request.background = m_flex;
        for (const QString &sentence : job.sentences)
            request.sentences.append(Speech::speakable(sentence));
        Ai::SpeechSynthesizer *speaker = m_ai->speechFor(job.voice);
        if (!speaker) {
            m_queue.prepend(job);
            stopWith(m_ai->describe(int(Ai::Error::NoKey)));
            return;
        }
        Ai::Reply *reply = speaker->synthesize(request);
        m_ai->track(reply, m_flex);
        m_replies.append(reply);
        connect(reply, &Ai::Reply::finished, this, [this, reply, job] {
            reply->deleteLater();
            m_replies.removeAll(reply);
            if (reply->error())
                failed(job, reply->error());
            else
                made(job, reply);
        });
    }
    checkFinished();
}

void Download::made(const Job &job, Ai::Reply *reply) {
    if (!m_active)
        return;
    if (reply->audio().isEmpty() || reply->sampleRate() <= 0) {
        failed(job, {Ai::Error::Server, tr("No audio came back for this passage.")});
        return;
    }
    m_storing.insert(job.key);
    m_cache->store(job.key, reply->audio(), reply->sampleRate(), reply->boundaries());
    m_audioSeconds += reply->audio().size() / 2.0 / reply->sampleRate();
    m_minutesReady += job.minutes;
    emit progressChanged();
    pump();
    planAhead();
}

void Download::failed(Job job, const Ai::Error &error) {
    if (!m_active)
        return;
    // Let go by a pause: made again on resuming.
    if (error.kind == Ai::Error::Cancelled) {
        m_queue.prepend(job);
        return;
    }
    job.tries += 1;
    switch (error.kind) {
    case Ai::Error::Limited:
    case Ai::Error::Server:
        // Busy or briefly broken: wait a little longer each time, then go on.
        m_queue.prepend(job);
        if (job.tries <= 4) {
            setProblem(error.kind == Ai::Error::Limited ? tr("Gemini's limit for your key was reached; going on in a moment.")
                                                         : tr("Gemini had a problem; trying again in a moment."));
            const int delayMs = error.retryAfterSeconds > 0 ? qBound(1000, error.retryAfterSeconds * 1000, 120000)
                                                            : 15000 * job.tries;
            m_retry.start(delayMs);
            return;
        }
        stopWith(m_ai->describe(int(error.kind), error.message));
        return;
    default:
        // Never silently switch off Flex (2x cost); keep the job queued and pause with the error.
        m_queue.prepend(job);
        stopWith(m_ai->describe(int(error.kind), error.message));
    }
}

void Download::stopWith(const QString &problem) {
    pause();
    setProblem(problem);
}

void Download::checkFinished() {
    if (!m_active || m_finished || !m_queue.isEmpty() || !m_waiting.isEmpty() || !m_replan.isEmpty() ||
        !m_storing.isEmpty() || m_nextChapter < m_chapters.size())
        return;
    m_replies.removeAll(nullptr);
    if (!m_replies.isEmpty())
        return;
    m_finished = true;
    m_paused = false;
    m_minutesReady = m_minutesTotal;
    if (m_clock.isValid())
        m_activeMs += m_clock.elapsed();
    m_clock.invalidate();
    emit progressChanged();
    emit changed();
    emit ready();
}

void Download::pause() {
    if (!m_active || m_paused)
        return;
    ++m_session;
    m_paused = true;
    m_retry.stop();
    if (m_clock.isValid())
        m_activeMs += m_clock.elapsed();
    m_clock.invalidate();
    m_director->cancel();
    if (m_cast)
        m_cast->cancel();
    // Chapters still waiting on blocks/direction are replanned cleanly on resume.
    for (int chapter : std::as_const(m_waiting))
        if (!m_replan.contains(chapter))
            m_replan.append(chapter);
    m_waiting.clear();
    // What's being made now is let go, and made again on resuming.
    const auto replies = m_replies;
    for (const auto &reply : replies)
        if (reply)
            reply->abort();
    emit changed();
}

void Download::resume() {
    if (!m_active || !m_paused)
        return;
    m_paused = false;
    m_clock.start();
    setProblem({});
    emit changed();
    pump();
    planAhead();
}

void Download::cancel() {
    ++m_session;
    const bool was = m_active;
    m_active = false;
    m_paused = false;
    m_retry.stop();
    m_director->cancel();
    if (m_cast)
        m_cast->cancel();
    const auto replies = m_replies;
    for (const auto &reply : replies)
        if (reply)
            reply->abort();
    m_replies.clear();
    m_queue.clear();
    m_waiting.clear();
    m_replan.clear();
    m_storing.clear();
    m_chapters.clear();
    m_finished = false;
    if (was) {
        emit changed();
        emit progressChanged();
    }
}
