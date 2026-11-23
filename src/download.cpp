#include "download.h"

#include "aiservices.h"
#include "audiocache.h"
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
    // The chapters come from the open book's pages, so a download stops
    // when its book closes; what's made is kept for next time.
    connect(reader, &Reader::bookChanged, this, [this] {
        if (m_active && m_reader->bookId() != m_bookId)
            stopWith(tr("The download stopped when the book closed. What's made is kept; start it again to finish."));
    });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        setProblem({});
        pump();
    });
}

Download::~Download() {
    m_active = false;
    const auto replies = m_replies;
    for (const auto &reply : replies)
        if (reply)
            reply->abort();
}

int Download::secondsLeft() const {
    const qint64 ms = m_activeMs + (running() && m_clock.isValid() ? m_clock.elapsed() : 0);
    if (m_audioSeconds < 30 || ms < 5000)
        return -1;
    // Audio made for each second spent: a book's minutes divided by it.
    const double pace = m_audioSeconds / (ms / 1000.0);
    return int(std::ceil((m_minutesTotal - m_minutesReady) * 60.0 / pace));
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
    cancel();
    if (!m_ai->canSpeakWith(m_narration->voice())) {
        setProblem(AiServices::isBuiltinVoice(m_narration->voice()) ? tr("This built-in voice isn't downloaded yet.")
                                                                    : m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    m_chapters = chaptersBetween(first, last);
    if (m_chapters.isEmpty())
        return;
    m_bookId = m_reader->bookId();
    m_voice = m_narration->voice();
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
    while (m_active && m_nextChapter < m_chapters.size() && m_waiting.size() < 2 && m_queue.size() < 12) {
        const int chapter = m_chapters[m_nextChapter++];
        m_waiting.insert(chapter);
        emit blocksWanted(chapter);
    }
    checkFinished();
}

void Download::addBlocks(int chapter, const QString &json) {
    if (!m_active || !m_waiting.contains(chapter))
        return;
    const QList<SpeechBlock> blocks = Speech::parseBlocks(json);
    const Ai::SpeechSynthesizer *speaker = m_ai->speechFor(m_voice);
    if (m_performance == QStringLiteral("plain") || !speaker || !speaker->supportsStyle() || !m_director->canDirect(blocks)) {
        plan(chapter, blocks, {});
        return;
    }
    if (const auto kept = m_director->kept(chapter, blocks, m_performance)) {
        plan(chapter, blocks, *kept);
        return;
    }
    m_director->ask(chapter, blocks, m_performance, this, [this, chapter, blocks](const QMap<int, QString> &moods) {
        if (m_active && m_waiting.contains(chapter))
            plan(chapter, blocks, moods);
    });
}

void Download::plan(int chapter, const QList<SpeechBlock> &blocks, const QMap<int, QString> &moods) {
    m_waiting.remove(chapter);
    const QString provider = m_ai->providerFor(m_voice)->id();
    Ai::SpeechSynthesizer *speaker = m_ai->speechFor(m_voice);
    if (!speaker)
        return;
    const QString model = speaker->speechModelName(m_economy);
    const bool dramatic = m_performance == QStringLiteral("dramatic");
    const bool styled = m_performance != QStringLiteral("plain") && speaker->supportsStyle();
    double planned = 0;
    for (const SpeechClip &clip : Speech::clips(chapter, blocks, Director::breaks(moods))) {
        if (clip.text.isEmpty())
            continue;
        Job job;
        job.text = clip.text;
        job.style = styled ? Speech::styleFor(Speech::moodAt(moods, clip.block), dramatic) : QString();
        for (const SpeechSentence &sentence : clip.sentences)
            job.sentences.append(sentence.text);
        job.key = AudioCache::keyFor(provider, model, m_voice, job.style, job.text);
        job.minutes = job.text.size() / charsPerMinute;
        planned += job.minutes;
        if (!m_cache->lookup(job.key).isEmpty())
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
        request.text = job.text;
        request.style = job.style;
        request.voice = m_voice;
        request.economy = m_economy;
        request.stream = false;
        request.background = m_flex;
        request.sentences = job.sentences;
        Ai::SpeechSynthesizer *speaker = m_ai->speechFor(m_voice);
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
        if (job.tries <= 4) {
            m_queue.prepend(job);
            setProblem(error.kind == Ai::Error::Limited ? tr("Gemini's limit for your key was reached; going on in a moment.")
                                                         : tr("Gemini had a problem; trying again in a moment."));
            m_retry.start(15000 * job.tries);
            return;
        }
        stopWith(m_ai->describe(int(error.kind), error.message));
        return;
    case Ai::Error::Invalid:
        // The slower tier isn't open to every key: the standard one is.
        if (m_flex) {
            m_flex = false;
            m_queue.prepend(job);
            pump();
            return;
        }
        stopWith(m_ai->describe(int(error.kind), error.message));
        return;
    default:
        m_queue.prepend(job);
        stopWith(m_ai->describe(int(error.kind), error.message));
    }
}

void Download::stopWith(const QString &problem) {
    pause();
    setProblem(problem);
}

void Download::checkFinished() {
    if (!m_active || m_finished || !m_queue.isEmpty() || !m_waiting.isEmpty() || m_nextChapter < m_chapters.size())
        return;
    m_replies.removeAll(nullptr);
    if (!m_replies.isEmpty())
        return;
    m_finished = true;
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
    m_paused = true;
    m_retry.stop();
    if (m_clock.isValid())
        m_activeMs += m_clock.elapsed();
    m_clock.invalidate();
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
    const bool was = m_active;
    m_active = false;
    m_paused = false;
    m_retry.stop();
    const auto replies = m_replies;
    for (const auto &reply : replies)
        if (reply)
            reply->abort();
    m_replies.clear();
    m_queue.clear();
    m_waiting.clear();
    m_chapters.clear();
    m_finished = false;
    if (was) {
        emit changed();
        emit progressChanged();
    }
}
