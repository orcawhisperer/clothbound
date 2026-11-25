#include "audio/responsespeech.h"

#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "audio/localspeech.h"
#include "audio/speech.h"
#include "audio/speechcleaner.h"
#include "audio/speechplayer.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <utility>

namespace {
constexpr double charsPerMinute = 900.0;
constexpr int sampleRate = 24000;

QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
}  // namespace

AudioFocus &AudioFocus::instance() {
    static AudioFocus arbiter;
    return arbiter;
}

void AudioFocus::claim(QObject *owner, Kind kind, YieldCallback onYield) {
    Q_ASSERT(!owner || owner->thread() == QThread::currentThread());
    Q_ASSERT(kind != Kind::None);
    if (m_owner && (m_owner != owner || m_kind != kind)) {
        YieldCallback yield = std::move(m_onYield);
        m_owner.clear();
        m_kind = Kind::None;
        m_onYield = {};
        if (yield && !m_yielding) {
            m_yielding = true;
            yield();
            m_yielding = false;
        }
    }
    m_owner = owner;
    m_kind = kind;
    m_onYield = std::move(onYield);
}

void AudioFocus::release(const QObject *owner, Kind kind) {
    Q_ASSERT(!owner || owner->thread() == QThread::currentThread());
    if (m_owner != owner)
        return;
    if (kind != Kind::None && m_kind != kind)
        return;
    m_owner.clear();
    m_kind = Kind::None;
    m_onYield = {};
}

ResponseSpeech::ResponseSpeech(AiServices *ai, AudioCache *cache, SpeechPlayer *player, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_player(player) {}

ResponseSpeech::~ResponseSpeech() { stop(); }

void ResponseSpeech::setCallbacks(Callbacks callbacks) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_callbacks = std::move(callbacks);
}

QPair<QString, QString> ResponseSpeech::bestLocalVoice(AiServices *ai, const QString &language,
                                                       bool allowAnyLanguageFallback) {
    if (!ai)
        return {};
    auto *local = qobject_cast<LocalSpeech *>(ai->providerFor(QStringLiteral("builtin:")));
    if (!local)
        return {};
    auto pick = [&](const QString &lang) -> QPair<QString, QString> {
        const QVariantMap best = local->bestInstalledVoice(lang);
        if (best.isEmpty())
            return {};
        const QString id = best.value(QStringLiteral("id")).toString();
        const QString name = best.value(QStringLiteral("name")).toString();
        if (id.isEmpty() || !ai->canSpeakWith(id))
            return {};
        return {id, name.isEmpty() ? id : name};
    };
    if (const auto match = pick(language); !match.first.isEmpty())
        return match;
    if (allowAnyLanguageFallback && !language.isEmpty())
        return pick({});
    return {};
}

bool ResponseSpeech::localPacksSupported(AiServices *ai) {
    if (!ai)
        return false;
    auto *local = qobject_cast<LocalSpeech *>(ai->providerFor(QStringLiteral("builtin:")));
    return local && local->packsSupported();
}

QString ResponseSpeech::voiceAccent(AiServices *ai, const QString &voice) {
    if (voice.isEmpty())
        return {};
    if (AiServices::isBuiltinVoice(voice)) {
        if (ai) {
            if (auto *local = qobject_cast<LocalSpeech *>(ai->providerFor(QStringLiteral("builtin:"))))
                if (local->packs())
                    return local->packs()->voice(voice).value(QStringLiteral("accent")).toString();
        }
        return {};
    }
    if (AiServices::isStudioVoice(voice)) {
        const QString model = ai ? ai->localSpeechModel() : QString();
        return model.isEmpty() ? tr("Studio") : model;
    }
    return Speech::classicGeminiTrait(voice);
}

QString ResponseSpeech::voiceUnavailableMessage(AiServices *ai, const QString &voice) {
    if (AiServices::isBuiltinVoice(voice))
        return tr("This built-in voice isn't downloaded yet.");
    if (AiServices::isStudioVoice(voice))
        return tr("This Studio voice needs its model server running.");
    return ai ? ai->describe(int(Ai::Error::NoKey)) : QString();
}

bool ResponseSpeech::economyMode() const { return m_callbacks.economy ? m_callbacks.economy() : false; }

QString ResponseSpeech::resolveVoice() const {
    if (!m_ai)
        return {};
    const QString lang = m_callbacks.bookLanguage ? m_callbacks.bookLanguage() : QString();
    auto tryLocal = [&]() -> QString { return bestLocalVoice(m_ai, lang, true).first; };

    QString candidate = m_callbacks.preferredVoice ? m_callbacks.preferredVoice() : QString();
    if (candidate.isEmpty())
        candidate = settings().value(QStringLiteral("listen/voice")).toString();

    if (!candidate.isEmpty() && m_ai->canSpeakWith(candidate)) {
        if (!AiServices::isOnThisComputer(candidate) && !m_ai->fitsBudget(0.001)) {
            if (const QString local = tryLocal(); !local.isEmpty())
                return local;
        }
        return candidate;
    }

    if (const QString local = tryLocal(); !local.isEmpty())
        return local;
    if (m_ai->canSpeakWith(QStringLiteral("sulafat")))
        return QStringLiteral("sulafat");
    return {};
}

int ResponseSpeech::indexOfChunk(int trackId) const {
    for (int i = 0; i < m_chunks.size(); ++i)
        if (m_chunks[i].trackId == trackId)
            return i;
    return -1;
}

void ResponseSpeech::setBuffering(bool buffering) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (buffering == m_buffering)
        return;
    m_buffering = buffering;
    emit changed();
}

void ResponseSpeech::fail(const QString &message) {
    Q_ASSERT(QThread::currentThread() == thread());
    stop();
    if (!message.isEmpty()) {
        if (m_callbacks.onProblem)
            m_callbacks.onProblem(message);
        emit problemOccurred(message);
    }
}

void ResponseSpeech::speak(const QString &markdownOrText, const QString &itemId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_player)
        return;

    const QString id = itemId.trimmed();
    // Clicking the active/buffering item again toggles playback off cleanly.
    if ((m_active || m_buffering) && m_id == id) {
        stop();
        return;
    }

    const QString cleaned = SpeechCleaner::cleanSpeechText(markdownOrText);
    if (cleaned.isEmpty()) {
        stop();
        return;
    }

    const QString voice = resolveVoice();
    if (voice.isEmpty()) {
        fail(tr("Choose or download a voice first (P) to listen to Companion responses."));
        return;
    }
    if (!AiServices::isOnThisComputer(voice) && m_ai && !m_ai->fitsBudget(0.001)) {
        fail(m_ai->describe(int(Ai::Error::Budget)));
        return;
    }
    const bool headless = m_player->isHeadless();
    if (!headless && QMediaDevices::defaultAudioOutput().isNull()) {
        fail(SpeechPlayer::tr("No audio output device is available."));
        return;
    }

    // Never play two voices simultaneously: hand off focus cleanly from book or preview.
    if (m_callbacks.onBeforeStart)
        m_callbacks.onBeforeStart();
    stop();
    AudioFocus::instance().claim(this, AudioFocus::Kind::Response, [this] { stop(); });

    // Split into clean sequential sentences/chunks so the first sentence begins immediately.
    QStringList sentences;
    const QStringList paragraphs = cleaned.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &paragraph : paragraphs) {
        SpeechBlock block;
        block.text = paragraph;
        block.end = paragraph.size();
        block.kind = QStringLiteral("text");
        const QList<SpeechSentence> split = Speech::sentences(block);
        for (const SpeechSentence &s : split)
            sentences.append(SpeechCleaner::splitOverlongSentence(s.text, 450));
    }
    if (sentences.isEmpty()) {
        AudioFocus::instance().release(this, AudioFocus::Kind::Response);
        return;
    }

    QList<Chunk> chunks;
    auto flushChunk = [&](QStringList &group) {
        if (group.isEmpty())
            return;
        Chunk chunk;
        chunk.trackId = m_callbacks.nextTrackId ? m_callbacks.nextTrackId() : m_localTrackId++;
        chunk.sentences = group;
        chunk.text = group.join(QLatin1Char(' '));
        chunk.voice = voice;
        chunks.append(std::move(chunk));
        group.clear();
    };

    QStringList current;
    qsizetype currentChars = 0;
    for (const QString &sentence : std::as_const(sentences)) {
        // Keep the very first chunk snappy (1 sentence, or merging a tiny <40-char opener up to 220 chars).
        const bool buildingFirst = chunks.isEmpty();
        const qsizetype limit = buildingFirst ? 220 : 450;
        const bool firstDone = buildingFirst && !current.isEmpty() && currentChars >= 40;
        if (!current.isEmpty() && (firstDone || currentChars + 1 + sentence.size() > limit)) {
            flushChunk(current);
            currentChars = 0;
        }
        current.append(sentence);
        currentChars += sentence.size() + (current.size() > 1 ? 1 : 0);
    }
    flushChunk(current);
    if (chunks.isEmpty()) {
        AudioFocus::instance().release(this, AudioFocus::Kind::Response);
        return;
    }

    ++m_session;
    m_chunks = std::move(chunks);
    m_current = 0;
    m_queued = -1;
    m_voice = voice;
    m_id = id;
    m_active = true;
    m_buffering = true;
    m_headless = headless;

    m_player->clear();
    if (!m_headless)
        m_player->play();
    emit changed();
    pump();
}

void ResponseSpeech::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    AudioFocus::instance().release(this, AudioFocus::Kind::Response);
    if (!m_active && !m_buffering && m_id.isEmpty() && m_chunks.isEmpty())
        return;
    ++m_session;
    const QList<Chunk> chunks = std::exchange(m_chunks, {});
    for (const Chunk &chunk : chunks)
        if (chunk.reply)
            chunk.reply->abort();
    m_current = 0;
    m_queued = -1;
    m_voice.clear();
    const bool ownedPlayer = m_active;
    m_active = false;
    m_buffering = false;
    m_headless = false;
    m_id.clear();
    if (ownedPlayer && m_player) {
        m_player->pause();
        m_player->clear();
    }
    emit changed();
}

bool ResponseSpeech::handlePosition(int trackId, qint64 /*sample*/) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active)
        return false;
    const int idx = indexOfChunk(trackId);
    if (idx >= 0) {
        if (m_buffering)
            setBuffering(false);
        if (idx != m_current) {
            m_current = idx;
            for (int i = 0; i < idx; ++i)
                if (m_chunks[i].state == Chunk::Complete)
                    m_chunks[i].pcm = QByteArray();
            pump();
        }
    }
    return true;
}

bool ResponseSpeech::handleDrained() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active)
        return false;
    if (m_queued + 1 >= m_chunks.size()) {
        stop();
        return true;
    }
    if (m_chunks[m_queued + 1].state == Chunk::Failed) {
        stop();
        return true;
    }
    pump();
    if (m_active && m_player && m_player->drainedOut())
        setBuffering(true);
    return true;
}

bool ResponseSpeech::handleStarved(bool starved) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active)
        return false;
    setBuffering(starved);
    return true;
}

bool ResponseSpeech::handleSinkError(const QString &message) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active)
        return false;
    fail(message);
    return true;
}

void ResponseSpeech::pump() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || !m_ai)
        return;
    int working = 0;
    for (const Chunk &chunk : std::as_const(m_chunks))
        working += (chunk.state == Chunk::Working && chunk.reply) ? 1 : 0;

    const int from = qMax(0, m_current);
    const bool local = AiServices::isOnThisComputer(m_voice);
    const int ahead = local ? 3 : 2;
    const int maxWorking = local ? 2 : 1;

    for (int i = from; i < m_chunks.size() && i < from + ahead; ++i) {
        Chunk &chunk = m_chunks[i];
        if (chunk.state != Chunk::Waiting)
            continue;
        if (chunk.voice.isEmpty())
            chunk.voice = m_voice;
        Ai::SpeechSynthesizer *synth = m_ai->speechFor(chunk.voice);
        Ai::Provider *provider = m_ai->providerFor(chunk.voice);
        if (!synth || !provider) {
            fail(tr("Choose or download a voice first (P) to listen to Companion responses."));
            return;
        }
        if (m_cache) {
            const QString model = synth->speechModelName(economyMode());
            chunk.key = AudioCache::keyFor(provider->id(), model, chunk.voice, {}, chunk.text);
            const QString file = m_cache->lookup(chunk.key);
            if (!file.isEmpty()) {
                decode(i, file);
                continue;
            }
        }
        if (working >= maxWorking)
            continue;
        fetch(i);
        ++working;
    }
    queue();
}

void ResponseSpeech::queue() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || !m_player)
        return;
    while (m_queued + 1 < m_chunks.size()) {
        const int nextIndex = m_queued + 1;
        Chunk &chunk = m_chunks[nextIndex];
        if (chunk.state == Chunk::Waiting || chunk.state == Chunk::Failed ||
            (chunk.state == Chunk::Working && !chunk.reply))
            break;
        ++m_queued;
        chunk.queued = true;
        m_player->addTrack(chunk.trackId, 0);
        if (!chunk.pcm.isEmpty())
            m_player->appendAudio(chunk.trackId, chunk.pcm);
        if (chunk.state == Chunk::Complete)
            m_player->finishTrack(chunk.trackId);
    }
}

void ResponseSpeech::fetch(int index) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (index < 0 || index >= m_chunks.size() || !m_ai)
        return;
    Chunk &chunk = m_chunks[index];
    Ai::SpeechRequest request;
    request.feature = QStringLiteral("companion speech");
    request.text = Speech::speakable(chunk.text);
    request.voice = chunk.voice;
    request.economy = economyMode();
    request.stream = true;
    for (const QString &sentence : std::as_const(chunk.sentences))
        request.sentences.append(Speech::speakable(sentence));

    Ai::SpeechSynthesizer *synth = m_ai->speechFor(request.voice);
    if (!synth) {
        chunk.state = Chunk::Failed;
        fail(tr("Choose or download a voice first (P) to listen to Companion responses."));
        return;
    }

    const bool local = AiServices::isOnThisComputer(request.voice);
    const double cost = local ? 0.0 : m_ai->listeningCost(qMax(0.05, chunk.text.size() / charsPerMinute), economyMode(), false);
    if (!local && !m_ai->fitsBudget(cost)) {
        chunk.state = Chunk::Failed;
        fail(m_ai->describe(int(Ai::Error::Budget)));
        return;
    }

    Ai::Reply *reply = synth->synthesize(request);
    m_ai->track(reply);
    chunk.reply = reply;
    chunk.state = Chunk::Working;
    const quint64 session = m_session;
    const int trackId = chunk.trackId;

    connect(reply, &Ai::Reply::audioReceived, this, [this, session, trackId, reply](const QByteArray &pcm) {
        if (!m_active || session != m_session || !m_player)
            return;
        const int i = indexOfChunk(trackId);
        if (i < 0 || m_chunks[i].reply != reply)
            return;
        Chunk &c = m_chunks[i];
        c.pcm += pcm;
        if (c.queued)
            m_player->appendAudio(c.trackId, pcm);
        else
            queue();
        if (!pcm.isEmpty() && m_buffering)
            setBuffering(false);
    });

    connect(reply, &Ai::Reply::finished, this, [this, session, trackId, reply] {
        reply->deleteLater();
        if (!m_active || session != m_session || !m_player)
            return;
        const int i = indexOfChunk(trackId);
        if (i < 0 || m_chunks[i].reply != reply)
            return;
        Chunk &c = m_chunks[i];
        c.reply = nullptr;
        if (reply->error()) {
            const Ai::Error err = reply->error();
            if (err.kind == Ai::Error::Cancelled)
                return;
            // Seamless cloud-to-local fallback if an installed local voice is ready.
            const bool isCloud = !AiServices::isOnThisComputer(c.voice);
            if (isCloud && !c.queued &&
                (err.kind == Ai::Error::Offline || err.kind == Ai::Error::Limited || err.kind == Ai::Error::Budget ||
                 err.kind == Ai::Error::NoKey || err.kind == Ai::Error::BadKey)) {
                const QString lang = m_callbacks.bookLanguage ? m_callbacks.bookLanguage() : QString();
                const QString fbId = bestLocalVoice(m_ai, lang, false).first;
                if (!fbId.isEmpty() && fbId != c.voice && m_ai->canSpeakWith(fbId)) {
                    m_voice = fbId;
                    for (int k = i; k < m_chunks.size(); ++k) {
                        if (m_chunks[k].queued)
                            continue;
                        if (m_chunks[k].reply) {
                            QPointer<Ai::Reply> obsolete = m_chunks[k].reply;
                            m_chunks[k].reply = nullptr;
                            obsolete->abort();
                        }
                        m_chunks[k].state = Chunk::Waiting;
                        m_chunks[k].pcm = QByteArray();
                        m_chunks[k].key.clear();
                        m_chunks[k].voice = fbId;
                    }
                    pump();
                    return;
                }
            }
            c.state = Chunk::Failed;
            if (c.queued)
                m_player->finishTrack(c.trackId);
            const QString msg = m_ai->describe(int(err.kind), err.message);
            if (i == 0 || m_headless || m_player->drainedOut()) {
                fail(msg);
                return;
            }
            if (m_callbacks.onProblem)
                m_callbacks.onProblem(msg);
            queue();
            return;
        }
        c.state = Chunk::Complete;
        if (c.queued)
            m_player->finishTrack(c.trackId);
        else
            queue();
        if (!c.pcm.isEmpty() && m_buffering)
            setBuffering(false);
        if (m_cache && !c.pcm.isEmpty() && !c.key.isEmpty() && reply->sampleRate() > 0)
            m_cache->store(c.key, c.pcm, reply->sampleRate());
        // Guard against an empty response yielding zero audio samples across all chunks.
        bool allDone = true;
        bool anyAudio = false;
        for (const Chunk &item : std::as_const(m_chunks)) {
            if (item.state == Chunk::Waiting || item.state == Chunk::Working)
                allDone = false;
            if (!item.pcm.isEmpty())
                anyAudio = true;
        }
        if (allDone && !anyAudio && m_player->drainedOut()) {
            stop();
            return;
        }
        pump();
    });
}

void ResponseSpeech::decode(int index, const QString &file) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (index < 0 || index >= m_chunks.size() || !m_cache)
        return;
    Chunk &chunk = m_chunks[index];
    chunk.state = Chunk::Working;
    const QString key = chunk.key;
    const int trackId = chunk.trackId;
    const quint64 session = m_session;
    m_cache->decode(file, sampleRate, this, [this, session, trackId, key](const QByteArray &pcm, bool ok) {
        if (!m_active || session != m_session || !m_player)
            return;
        const int i = indexOfChunk(trackId);
        if (i < 0 || m_chunks[i].state != Chunk::Working || m_chunks[i].reply)
            return;
        Chunk &c = m_chunks[i];
        if (!ok) {
            m_cache->remove(key);
            c.state = Chunk::Waiting;
            c.queued = false;
            c.key.clear();
            pump();
            return;
        }
        c.pcm = pcm;
        c.state = Chunk::Complete;
        if (c.queued) {
            m_player->appendAudio(c.trackId, c.pcm);
            m_player->finishTrack(c.trackId);
        } else {
            queue();
        }
        if (!c.pcm.isEmpty() && m_buffering)
            setBuffering(false);
        pump();
    });
}
