#include "audio/voicecatalog.h"

#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "audio/responsespeech.h"
#include "reader/reader.h"
#include "audio/speech.h"
#include "audio/speechplayer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlQuery>
#include <QThread>
#include <algorithm>
#include <memory>

namespace {
constexpr int sampleRate = 24000;
constexpr double estimatedCastCost = 0.005;
constexpr double estimatedDesignCost = 0.03;

// The kept list is Clothbound's own record of the voices, whatever provider
// they came from.
QJsonObject toJson(const Ai::Voice &voice) {
    QJsonObject json{{"id", voice.id},           {"name", voice.name},         {"description", voice.description},
                     {"language", voice.language}, {"accent", voice.accent},   {"gender", voice.gender},
                     {"pitch", voice.pitch},     {"persona", voice.persona}, {"designed", voice.designed},
                     {"prompt", voice.prompt}};
    if (voice.expires.isValid())
        json.insert("expires", voice.expires.toString(Qt::ISODate));
    return json;
}

Ai::Voice fromJson(const QJsonObject &json) {
    Ai::Voice voice;
    voice.id = json.value("id").toString();
    voice.name = json.value("name").toString();
    voice.description = json.value("description").toString();
    voice.language = json.value("language").toString();
    voice.accent = json.value("accent").toString();
    voice.gender = json.value("gender").toString();
    voice.pitch = json.value("pitch").toString();
    voice.persona = json.value("persona").toString();
    voice.designed = json.value("designed").toBool();
    voice.prompt = json.value("prompt").toString();
    voice.expires = QDateTime::fromString(json.value("expires").toString(), Qt::ISODate);
    return voice;
}

bool isNarrator(const Ai::Voice &voice) { return voice.persona.startsWith(QStringLiteral("Storyteller")); }
bool isClassic(const Ai::Voice &voice) { return !voice.designed && !voice.id.contains('-'); }
QString baseOf(const QString &language) { return language.section('-', 0, 0).section('_', 0, 0).toLower(); }
QString regionOf(const QString &language) {
    const QString norm = QString(language).replace('_', '-');
    return norm.section('-', 1, 1).toUpper();
}
}

VoiceCatalog::VoiceCatalog(AiServices *ai, AudioCache *cache, Reader *reader, const QString &databaseConnection,
                           const QString &cacheFile, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_connection(databaseConnection),
      m_cacheFile(cacheFile), m_player(new SpeechPlayer(sampleRate, this)) {
    if (m_reader)
        connect(m_reader, &Reader::bookChanged, this, &VoiceCatalog::resetCasting);
    connect(m_player, &SpeechPlayer::position, this, [this] {
        if (m_previewBuffering) {
            m_previewBuffering = false;
            emit previewChanged();
        }
    });
    connect(m_player, &SpeechPlayer::drained, this, [this] {
        AudioFocus::instance().release(this, AudioFocus::Kind::Preview);
        if (m_previewing.isEmpty())
            return;
        m_previewing.clear();
        m_previewBuffering = false;
        emit previewChanged();
    });
    QSqlQuery(QSqlDatabase::database(m_connection))
        .exec(QStringLiteral("CREATE TABLE IF NOT EXISTS designed_voices (id TEXT PRIMARY KEY, name TEXT NOT NULL,"
                             " description TEXT NOT NULL, gender TEXT NOT NULL, language TEXT NOT NULL,"
                             " book_id INTEGER, made_at INTEGER NOT NULL)"));
    load();
}

VoiceCatalog::~VoiceCatalog() {
    AudioFocus::instance().release(this, AudioFocus::Kind::Preview);
    if (m_refreshReply)
        m_refreshReply->abort();
    if (m_studioReply)
        m_studioReply->abort();
    if (m_previewReply)
        m_previewReply->abort();
    if (m_castingReply)
        m_castingReply->abort();
}

QString VoiceCatalog::castingPrompt() {
    QFile file(QStringLiteral(":/prompts/casting.md"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void VoiceCatalog::load() {
    QFile file(m_cacheFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    QList<Ai::Voice> voices;
    for (const auto &value : json.value("voices").toArray())
        voices.append(fromJson(value.toObject()));
    m_fetched = QDateTime::fromString(json.value("fetched").toString(), Qt::ISODate);
    setVoices(voices);
}

void VoiceCatalog::save() const {
    if (m_cacheFile.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_cacheFile).absolutePath());
    QJsonArray voices;
    for (const Ai::Voice &voice : m_voices)
        voices.append(toJson(voice));
    QSaveFile file(m_cacheFile);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{"fetched", m_fetched.toString(Qt::ISODate)}, {"voices", voices}})
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void VoiceCatalog::setVoices(const QList<Ai::Voice> &voices) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_voices = voices;
    emit changed();
}

void VoiceCatalog::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit changed();
}

void VoiceCatalog::refresh(bool force) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_loading)
        return;
    if (!force && !m_voices.isEmpty() && m_fetched.isValid() && m_fetched.secsTo(QDateTime::currentDateTime()) < 24 * 3600)
        return;
    if (!m_ai->speech()) {
        setProblem(m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    if (m_refreshReply)
        m_refreshReply->abort();
    m_loading = true;
    setProblem({});
    emit changed();
    Ai::Reply *reply = m_ai->speech()->listVoices({});
    m_refreshReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_refreshReply)
            return;
        m_refreshReply = nullptr;
        m_loading = false;
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                setProblem(m_ai->describe(int(reply->error().kind), reply->error().message));
            emit changed();
            return;
        }
        m_fetched = QDateTime::currentDateTime();
        setVoices(reply->voices());
        save();
    });
}

QVariantMap VoiceCatalog::toMap(const Ai::Voice &voice) {
    static const QRegularExpression role(QStringLiteral("\\(([^)]+)\\)"));
    static const QRegularExpression age(QStringLiteral("^(\\d+)-year-old"));
    static const QRegularExpression sentenceSplit(QStringLiteral("(?<=\\.)\\s+"));
    QVariantMap map;
    map["id"] = voice.id;
    map["name"] = voice.name.isEmpty() ? voice.id : voice.name;
    // The library numbers its voices within each accent ("Storyteller 1"),
    // so the accent is what tells them apart.
    map["title"] = !voice.designed && !isClassic(voice) && !voice.accent.isEmpty() ? voice.accent : map["name"];
    map["description"] = voice.description;
    const bool studio = voice.id.startsWith(QLatin1String("studio:"));
    const QString trait = isClassic(voice) && !studio ? Speech::classicGeminiTrait(voice.id) : QString();
    QString accent = voice.accent;
    if (accent.isEmpty())
        accent = trait;
    else if (!trait.isEmpty() && !accent.contains(trait, Qt::CaseInsensitive))
        accent += QStringLiteral(", ") + trait.toLower();
    map["language"] = voice.language;
    map["accent"] = accent;
    map["gender"] = voice.gender;
    map["pitch"] = voice.pitch;
    map["persona"] = voice.persona;
    map["role"] = role.match(voice.persona).captured(1);
    map["kind"] = studio ? QStringLiteral("studio")
                : voice.designed ? QStringLiteral("designed")
                : isClassic(voice) ? QStringLiteral("classic")
                : QStringLiteral("library");
    map["narrator"] = isNarrator(voice);
    map["age"] = age.match(voice.description).captured(1).toInt();
    map["prompt"] = voice.prompt;
    map["expires"] = voice.expires;
    // What the voice is like, without the age, place and accent shown beside it.
    QString summary = voice.designed ? voice.prompt : voice.description;
    if (!voice.designed && !studio) {
        // The catalog sets a scene for each voice ("Currently in a
        // bookstore.") and suggests uses for it ("Recommended for onboarding
        // screens"), which say nothing about how it sounds.
        QStringList sentences = summary.split(sentenceSplit, Qt::SkipEmptyParts);
        sentences.removeIf([&](const QString &sentence) {
            return age.match(sentence).hasMatch() || sentence.startsWith(QStringLiteral("Speaks with")) ||
                   sentence.startsWith(QStringLiteral("Currently")) || sentence.startsWith(QStringLiteral("Recommended"));
        });
        summary = sentences.join(' ');
    }
    const QString simplified = summary.simplified();
    map["summary"] = simplified;
    map["note"] = studio ? simplified
                : voice.designed ? simplified
                : (!simplified.isEmpty() ? simplified : tr("Moods change with the scene"));
    map["suggested"] = false;
    return map;
}

QVariantList VoiceCatalog::suggestions(const QString &language) const {
    QString base = baseOf(language);
    if (base.isEmpty())
        base = baseOf(QLocale::system().bcp47Name());
    // Narrators who speak the language, by region; within a region, the
    // ones made to tell stories first.
    QMap<QString, QList<const Ai::Voice *>> regions;
    for (const Ai::Voice &voice : m_voices)
        if (!voice.designed && !isClassic(voice) && isNarrator(voice) && !base.isEmpty() && baseOf(voice.language) == base)
            regions[regionOf(voice.language)].append(&voice);
    for (auto &voices : regions)
        std::stable_sort(voices.begin(), voices.end(), [](const Ai::Voice *a, const Ai::Voice *b) {
            return a->name.startsWith(QStringLiteral("Storyteller")) && !b->name.startsWith(QStringLiteral("Storyteller"));
        });
    // A sampling of accents: the book's region, then the reader's own, then
    // the regions with the most narrators, one voice from each in turn.
    QStringList order = regions.keys();
    std::stable_sort(order.begin(), order.end(),
                     [&regions](const QString &a, const QString &b) { return regions[a].size() > regions[b].size(); });
    for (const QString &preferred : {QLocale::territoryToCode(QLocale::system().territory()), regionOf(language)})
        if (order.removeOne(preferred))
            order.prepend(preferred);
    QVariantList result;
    for (int round = 0; result.size() < 12; ++round) {
        bool any = false;
        for (const QString &region : std::as_const(order))
            if (round < regions[region].size() && result.size() < 12) {
                QVariantMap entry = toMap(*regions[region][round]);
                if (result.size() < 2)
                    entry[QStringLiteral("suggested")] = true;
                result.append(entry);
                any = true;
            }
        if (!any)
            break;
    }
    int suggestedCount = int(qMin<qsizetype>(2, result.size()));
    for (const Ai::Voice &voice : m_voices)
        if (isClassic(voice) && isNarrator(voice)) {
            QVariantMap entry = toMap(voice);
            if (suggestedCount < 2) {
                entry[QStringLiteral("suggested")] = true;
                ++suggestedCount;
            }
            result.append(entry);
        }
    return result;
}

QVariantList VoiceCatalog::classicVoices() const {
    QVariantList out;
    for (const Ai::Voice &voice : m_voices)
        if (isClassic(voice))
            out.append(toMap(voice));
    return out;
}

QVariantList VoiceCatalog::designedVoices() const {
    QList<const Ai::Voice *> designed;
    for (const Ai::Voice &voice : m_voices)
        if (voice.designed)
            designed.append(&voice);
    std::stable_sort(designed.begin(), designed.end(),
                     [](const Ai::Voice *a, const Ai::Voice *b) { return a->expires > b->expires; });
    QVariantList result;
    for (const Ai::Voice *voice : designed)
        result.append(toMap(*voice));
    return result;
}

QVariantList VoiceCatalog::search(const QString &text, const QString &language, int limit) const {
    const QStringList words = text.toLower().split(' ', Qt::SkipEmptyParts);
    const QString base = baseOf(language);
    QList<const Ai::Voice *> found;
    for (const Ai::Voice &voice : m_voices) {
        // The classic voices speak every language.
        if (!base.isEmpty() && !voice.designed && !isClassic(voice) && baseOf(voice.language) != base)
            continue;
        const QString haystack = QStringList{voice.id, voice.name, voice.accent, voice.persona, voice.description,
                                             voice.language, voice.gender, voice.pitch, voice.prompt}
                                     .join(' ')
                                     .toLower();
        if (std::all_of(words.begin(), words.end(), [&haystack](const QString &word) { return haystack.contains(word); }))
            found.append(&voice);
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Ai::Voice *a, const Ai::Voice *b) { return isNarrator(*a) && !isNarrator(*b); });
    QVariantList result;
    for (const Ai::Voice *voice : found.mid(0, limit))
        result.append(toMap(*voice));
    return result;
}

QVariantMap VoiceCatalog::voice(const QString &id) const {
    for (const Ai::Voice &voice : m_voices)
        if (voice.id == id)
            return toMap(voice);
    for (const Ai::Voice &voice : m_studioVoices)
        if (voice.id == id)
            return toMap(voice);
    return {};
}

QVariantList VoiceCatalog::studioVoices() const {
    QVariantList out;
    for (int i = 0; i < m_studioVoices.size(); ++i) {
        QVariantMap entry = toMap(m_studioVoices[i]);
        if (i == 0)
            entry[QStringLiteral("suggested")] = true;
        out.append(entry);
    }
    return out;
}

void VoiceCatalog::loadStudioVoices() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_studioReply) {
        m_studioReply->abort();
        m_studioReply = nullptr;
    }
    Ai::SpeechSynthesizer *studio = m_ai->speechFor(QStringLiteral("studio:"));
    if (!studio) {
        m_studioVoices.clear();
        emit studioChanged();
        return;
    }
    const bool supportsStyle = studio->supportsStyle();
    const QString modelName = m_ai->localSpeechModel();
    Ai::Reply *reply = studio->listVoices(QString());
    m_studioReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply, supportsStyle, modelName] {
        reply->deleteLater();
        if (reply != m_studioReply)
            return;
        m_studioReply = nullptr;
        m_studioVoices.clear();
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                setProblem(m_ai->describe(int(reply->error().kind), reply->error().message));
            emit studioChanged();
            return;
        }
        setProblem({});
        for (Ai::Voice voice : reply->voices()) {
            voice.id = QStringLiteral("studio:") + voice.id;
            if (voice.accent.isEmpty())
                voice.accent = modelName;
            if (voice.description.isEmpty())
                voice.description = supportsStyle ? tr("On your server · follows styles") : tr("On your server");
            m_studioVoices.append(voice);
        }
        emit studioChanged();
    });
}

int VoiceCatalog::startPreview(const QString &voiceId) {
    Q_ASSERT(QThread::currentThread() == thread());
    stopPreview();
    AudioFocus::instance().claim(this, AudioFocus::Kind::Preview, [this] { stopPreview(); });
    m_previewing = voiceId;
    m_previewBuffering = true;
    emit previewChanged();
    const int track = ++m_previewTrack;
    m_player->clear();
    m_player->addTrack(track);
    m_player->play();
    return track;
}

void VoiceCatalog::synthesizePreview(int track, const QString &voiceId, const QString &words, const QString &key) {
    Ai::SpeechSynthesizer *speaker = m_ai->speechFor(voiceId);
    if (!speaker) {
        stopPreview();
        setProblem(AiServices::isBuiltinVoice(voiceId) ? tr("This built-in voice isn't downloaded yet.")
                   : AiServices::isStudioVoice(voiceId) ? tr("This Studio voice needs its model server running.")
                   : m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    const bool local = AiServices::isOnThisComputer(voiceId);
    if (!local && !m_ai->fitsBudget(m_ai->listeningCost(qMax(0.1, words.size() / 900.0), false, false))) {
        stopPreview();
        setProblem(m_ai->describe(int(Ai::Error::Budget)));
        return;
    }
    Ai::SpeechRequest request;
    request.feature = QStringLiteral("voice preview");
    request.text = words;
    request.voice = voiceId;
    request.stream = true;
    Ai::Reply *reply = speaker->synthesize(request);
    m_ai->track(reply);
    m_previewReply = reply;
    auto pcm = std::make_shared<QByteArray>();
    connect(reply, &Ai::Reply::audioReceived, this, [this, track, pcm](const QByteArray &chunk) {
        if (track != m_previewTrack)
            return;
        *pcm += chunk;
        m_player->appendAudio(track, chunk);
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, track, pcm, key] {
        reply->deleteLater();
        if (track != m_previewTrack)
            return;
        m_player->finishTrack(track);
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                setProblem(m_ai->describe(int(reply->error().kind), reply->error().message));
            return;
        }
        if (!pcm->isEmpty() && reply->sampleRate() > 0)
            m_cache->store(key, *pcm, reply->sampleRate());
    });
}

void VoiceCatalog::preview(const QString &voiceId, const QString &text) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (voiceId.isEmpty())
        return;
    if (!m_ai->canSpeakWith(voiceId)) {
        setProblem(AiServices::isBuiltinVoice(voiceId) ? tr("This built-in voice isn't downloaded yet.")
                   : AiServices::isStudioVoice(voiceId) ? tr("This Studio voice needs its model server running.")
                   : m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    QString words = Speech::speakable(text).simplified();
    if (words.isEmpty())
        words = tr("This is how the book will sound when read aloud.");
    Ai::SpeechSynthesizer *speaker = m_ai->speechFor(voiceId);
    Ai::Provider *provider = m_ai->providerFor(voiceId);
    if (!speaker || !provider) {
        setProblem(m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    setProblem({});
    const int track = startPreview(voiceId);
    const QString model = speaker->speechModelName(false);
    const QString key = AudioCache::keyFor(provider->id(), model, voiceId, {}, words);
    const QString file = m_cache->lookup(key);
    if (!file.isEmpty()) {
        m_cache->decode(file, sampleRate, this, [this, track, voiceId, words, key](const QByteArray &pcm, bool ok) {
            if (track != m_previewTrack)
                return;
            if (!ok) {
                m_cache->remove(key);
                synthesizePreview(track, voiceId, words, key);
                return;
            }
            m_player->appendAudio(track, pcm);
            m_player->finishTrack(track);
        });
        return;
    }
    synthesizePreview(track, voiceId, words, key);
}

void VoiceCatalog::stopPreview() {
    Q_ASSERT(QThread::currentThread() == thread());
    AudioFocus::instance().release(this, AudioFocus::Kind::Preview);
    ++m_previewTrack;
    if (m_previewReply)
        m_previewReply->abort();
    m_player->pause();
    m_player->clear();
    if (!m_previewing.isEmpty() || m_previewBuffering) {
        m_previewing.clear();
        m_previewBuffering = false;
        emit previewChanged();
    }
}

void VoiceCatalog::setCasting(const QString &state, const QString &problem) {
    m_casting = state;
    m_castingProblem = problem;
    emit castingChanged();
}

void VoiceCatalog::resetCasting() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_castingReply)
        m_castingReply->abort();
    stopPreview();
    m_proposal.clear();
    m_designed.clear();
    m_proposed.clear();
    m_sample.clear();
    setCasting({});
}

void VoiceCatalog::cast(const QString &passage) {
    Q_ASSERT(QThread::currentThread() == thread());
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("casting"));
    Ai::Provider *provider = m_ai->textProviderFor(QStringLiteral("casting"));
    if (!model || !m_ai->voiceDesigner()) {
        setCasting(QStringLiteral("failed"), m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    if (provider && !provider->isLocal() && !m_ai->fitsBudget(estimatedCastCost)) {
        setCasting(QStringLiteral("failed"), m_ai->describe(int(Ai::Error::Budget)));
        return;
    }
    if (m_castingReply)
        m_castingReply->abort();
    Ai::TextRequest request;
    request.feature = QStringLiteral("casting");
    request.effort = QStringLiteral("low");
    request.system = castingPrompt();
    QString message = QStringLiteral("Title: %1\nAuthor: %2\nLanguage: %3\n")
                          .arg(m_reader->title(), m_reader->author(), m_reader->language());
    if (!m_proposed.isEmpty())
        message += QStringLiteral("\nEarlier proposals, to differ from:\n- ") + m_proposed.join(QStringLiteral("\n- ")) + '\n';
    message += QStringLiteral("\nA passage from the book:\n\n") + passage.left(4000);
    request.messages = {{true, message}};
    request.schema = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                                   {"description", QJsonObject{{"type", "string"}}},
                                   {"gender", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"male", "female"}}}},
                                   {"language", QJsonObject{{"type", "string"}}}}},
        {"required", QJsonArray{"name", "description", "gender", "language"}},
    };
    setCasting(QStringLiteral("thinking"));
    Ai::Reply *reply = model->generate(request);
    m_ai->track(reply);
    m_castingReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_castingReply)
            return;
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                setCasting(QStringLiteral("failed"), m_ai->describe(int(reply->error().kind), reply->error().message));
            return;
        }
        const QJsonObject json = reply->result().isEmpty()
                                     ? QJsonDocument::fromJson(reply->text().toUtf8()).object()
                                     : reply->result();
        QVariantMap proposal{{"name", json.value("name").toString().simplified().left(60)},
                             {"description", json.value("description").toString().simplified()},
                             {"gender", json.value("gender").toString()},
                             {"language", json.value("language").toString().simplified()}};
        if (proposal["description"].toString().isEmpty()) {
            setCasting(QStringLiteral("failed"), tr("No narrator came back. Try again."));
            return;
        }
        m_proposed.append(proposal["description"].toString());
        m_proposal = proposal;
        setCasting(QStringLiteral("proposed"));
    });
}

void VoiceCatalog::design(const QVariantMap &proposal) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_ai->voiceDesigner()) {
        setCasting(QStringLiteral("failed"), m_ai->describe(int(Ai::Error::NoKey)));
        return;
    }
    if (!m_ai->fitsBudget(estimatedDesignCost)) {
        setCasting(QStringLiteral("failed"), m_ai->describe(int(Ai::Error::Budget)));
        return;
    }
    Ai::VoiceDesign design;
    design.name = proposal.value("name").toString().simplified();
    design.description = proposal.value("description").toString().simplified();
    design.gender = proposal.value("gender").toString().trimmed().toLower();
    if (design.gender != QLatin1String("male") && design.gender != QLatin1String("female"))
        design.gender.clear();
    design.language = proposal.value("language").toString().trimmed();
    if (design.description.isEmpty())
        return;
    if (design.name.isEmpty())
        design.name = tr("Narrator");
    if (m_castingReply)
        m_castingReply->abort();
    stopPreview();
    m_proposal = QVariantMap{{"name", design.name},
                             {"description", design.description},
                             {"gender", design.gender},
                             {"language", design.language}};
    setCasting(QStringLiteral("designing"));
    Ai::Reply *reply = m_ai->voiceDesigner()->design(design);
    m_ai->track(reply);
    m_castingReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply, design] {
        reply->deleteLater();
        if (reply != m_castingReply)
            return;
        const Ai::Voice voice = reply->voices().value(0);
        if (reply->error() || voice.id.isEmpty()) {
            if (reply->error().kind != Ai::Error::Cancelled) {
                const QString message = reply->error()
                                            ? m_ai->describe(int(reply->error().kind), reply->error().message)
                                            : tr("No voice came back from the server. Try again.");
                setCasting(QStringLiteral("failed"), message);
            }
            return;
        }
        Ai::Voice kept = voice;
        kept.designed = true;
        if (kept.prompt.isEmpty())
            kept.prompt = design.description;
        if (kept.name.isEmpty())
            kept.name = design.name;
        m_voices.prepend(kept);
        save();
        emit changed();
        remember(kept, design);
        m_sample = reply->audio();
        m_sampleRate = reply->sampleRate();
        m_designed = toMap(kept);
        setCasting(QStringLiteral("designed"));
        playSample();
    });
}

void VoiceCatalog::forget(const QString &voiceId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_ai->voiceDesigner() || voiceId.isEmpty())
        return;
    if (m_previewing == voiceId)
        stopPreview();
    Ai::Reply *reply = m_ai->voiceDesigner()->forget(voiceId);
    connect(reply, &Ai::Reply::finished, this, [this, reply, voiceId] {
        reply->deleteLater();
        // Gone already is as good as deleted.
        if (reply->error() && reply->error().kind != Ai::Error::NotFound) {
            setProblem(m_ai->describe(int(reply->error().kind), reply->error().message));
            return;
        }
        m_voices.removeIf([&voiceId](const Ai::Voice &voice) { return voice.id == voiceId; });
        save();
        QSqlQuery query(QSqlDatabase::database(m_connection));
        query.prepare(QStringLiteral("DELETE FROM designed_voices WHERE id = ?"));
        query.addBindValue(voiceId);
        query.exec();
        if (m_designed.value("id").toString() == voiceId)
            resetCasting();
        emit changed();
    });
}

void VoiceCatalog::remember(const Ai::Voice &voice, const Ai::VoiceDesign &design) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO designed_voices (id, name, description, gender, language,"
                                 " book_id, made_at) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(voice.id);
    query.addBindValue(design.name);
    query.addBindValue(design.description);
    query.addBindValue(design.gender);
    query.addBindValue(design.language);
    query.addBindValue(m_reader->bookId());
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

void VoiceCatalog::revise() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_proposal.isEmpty())
        return;
    stopPreview();
    setCasting(QStringLiteral("proposed"));
}

void VoiceCatalog::playSample() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_sample.isEmpty() || m_designed.isEmpty())
        return;
    const int track = startPreview(m_designed.value("id").toString());
    QByteArray pcm = m_sample;
    if (m_sampleRate > 0 && m_sampleRate != sampleRate) {
        const qsizetype count = m_sample.size() / 2;
        const auto *src = reinterpret_cast<const qint16 *>(m_sample.constData());
        QVector<float> floats(count);
        for (qsizetype i = 0; i < count; ++i)
            floats[i] = src[i] / 32768.0f;
        pcm = Speech::pcm16(Speech::resample(floats, m_sampleRate, sampleRate));
    }
    m_player->appendAudio(track, pcm);
    m_player->finishTrack(track);
}
