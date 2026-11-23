#include "cast.h"

#include "aiservices.h"
#include "audiocache.h"
#include "library.h"
#include "reader.h"
#include "voicecatalog.h"
#include "voicepacks.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

Cast::Cast(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, VoiceCatalog *catalog, VoicePacks *packs,
           QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_library(library), m_catalog(catalog), m_packs(packs) {
    connect(reader, &Reader::bookChanged, this, &Cast::load);
}

void Cast::load() {
    m_lines.clear();
    m_voices.clear();
    m_genders.clear();
    m_counts.clear();
    const int book = m_reader->bookId();
    m_enabled = book > 0 && m_library->bookSetting(book, QStringLiteral("fullCast")) == QLatin1String("on");
    const QJsonObject saved = QJsonDocument::fromJson(m_library->bookSetting(book, QStringLiteral("cast")).toUtf8()).object();
    for (auto it = saved.begin(); it != saved.end(); ++it)
        m_voices.insert(it.key(), it.value().toString());
    emit changed();
}

void Cast::save() const {
    QJsonObject saved;
    for (auto it = m_voices.cbegin(); it != m_voices.cend(); ++it)
        saved.insert(it.key(), it.value());
    m_library->setBookSetting(m_reader->bookId(), QStringLiteral("cast"),
                              QString::fromUtf8(QJsonDocument(saved).toJson(QJsonDocument::Compact)));
}

void Cast::setEnabled(bool enabled) {
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    m_library->setBookSetting(m_reader->bookId(), QStringLiteral("fullCast"), enabled ? QStringLiteral("on") : QString());
    emit changed();
    emit recast();
}

bool Cast::available() const { return pool().size() >= 3; }

QList<Cast::Choice> Cast::pool() const {
    QList<Choice> out;
    if (m_narrator.startsWith(QLatin1String("builtin:kokoro:"))) {
        // The installed Kokoro voices of the book's language.
        // The narrator's own accent first (a British narrator, British
        // voices), then others before any voice is heard twice.
        const QString accent = m_packs->voice(m_narrator).value(QStringLiteral("language")).toString();
        for (const QVariant &value : m_packs->voices(m_reader->language())) {
            const QVariantMap voice = value.toMap();
            const QString id = voice.value(QStringLiteral("id")).toString();
            if (!id.startsWith(QLatin1String("builtin:kokoro:")) || !voice.value(QStringLiteral("installed")).toBool())
                continue;
            out.append({id, voice.value(QStringLiteral("name")).toString(), voice.value(QStringLiteral("gender")).toString(),
                        voice.value(QStringLiteral("language")).toString() == accent});
        }
        // Home voices first.
        std::stable_sort(out.begin(), out.end(), [](const Choice &a, const Choice &b) { return a.home && !b.home; });
        return out;
    }
    if (AiServices::isOnThisComputer(m_narrator))
        return out;  // Piper and Studio voices: one voice each, nothing to cast from.
    for (const QVariant &value : m_catalog->classicVoices()) {
        const QVariantMap voice = value.toMap();
        out.append({voice.value(QStringLiteral("id")).toString(), voice.value(QStringLiteral("name")).toString(),
                    voice.value(QStringLiteral("gender")).toString().toLower()});
    }
    return out;
}

QVariantList Cast::choices() const {
    QVariantList out;
    for (const Choice &choice : pool())
        out.append(QVariantMap{{"id", choice.id}, {"name", choice.name}, {"gender", choice.gender}});
    return out;
}

QString Cast::voiceFor(const QString &speaker) const {
    if (speaker.isEmpty() || speaker.compare(QLatin1String("narrator"), Qt::CaseInsensitive) == 0)
        return m_narrator;
    if (m_voices.contains(speaker))
        return m_voices.value(speaker);
    return m_narrator;
}

QString Cast::voiceAt(int chapter, int block) const {
    if (!m_enabled)
        return m_narrator;
    return voiceFor(m_lines.value(chapter).value(block).speaker);
}

QSet<int> Cast::breaks(int chapter, const QList<SpeechBlock> &blocks) const {
    QSet<int> out;
    if (!m_enabled || !m_lines.contains(chapter))
        return out;
    QString before = m_narrator;
    for (int i = 0; i < blocks.size(); ++i) {
        const QString voice = voiceAt(chapter, i);
        if (voice != before)
            out.insert(i);
        before = voice;
    }
    return out;
}

void Cast::learn(int chapter, const QMap<int, Line> &lines) {
    m_lines.insert(chapter, lines);
    // New speakers get a voice: their gender's, not the narrator's, not
    // another's while any are free, and the same one every time.
    const QList<Choice> choices = pool();
    QSet<QString> used(m_voices.cbegin(), m_voices.cend());
    used.insert(m_narrator);
    bool assigned = false;
    for (const Line &line : lines) {
        if (line.speaker.isEmpty() || line.speaker.compare(QLatin1String("narrator"), Qt::CaseInsensitive) == 0)
            continue;
        ++m_counts[line.speaker];
        if (!line.gender.isEmpty())
            m_genders.insert(line.speaker, line.gender);
        if (m_voices.contains(line.speaker) || choices.isEmpty())
            continue;
        QList<Choice> fitting;
        for (const Choice &choice : choices)
            if (line.gender.isEmpty() || choice.gender.isEmpty() || choice.gender == line.gender)
                fitting.append(choice);
        if (fitting.isEmpty())
            fitting = choices;
        QList<Choice> freeHome, freeAway;
        for (const Choice &choice : fitting)
            if (!used.contains(choice.id))
                (choice.home ? freeHome : freeAway).append(choice);
        const QList<Choice> &from = !freeHome.isEmpty() ? freeHome : !freeAway.isEmpty() ? freeAway : fitting;
        const quint32 hash = qFromBigEndian<quint32>(QCryptographicHash::hash(line.speaker.toUtf8(), QCryptographicHash::Md5).constData());
        const QString voice = from[int(hash % quint32(from.size()))].id;
        m_voices.insert(line.speaker, voice);
        used.insert(voice);
        assigned = true;
    }
    if (assigned)
        save();
    emit changed();
}

void Cast::setVoice(const QString &speaker, const QString &voice) {
    m_voices.insert(speaker, voice);
    save();
    emit changed();
    emit recast();
}

QVariantList Cast::members() const {
    QList<QString> speakers = m_counts.keys();
    std::sort(speakers.begin(), speakers.end(), [this](const QString &a, const QString &b) { return m_counts[a] > m_counts[b]; });
    QHash<QString, QString> names;
    for (const Choice &choice : pool())
        names.insert(choice.id, choice.name);
    QVariantList out;
    for (const QString &speaker : speakers) {
        const QString voice = voiceFor(speaker);
        out.append(QVariantMap{{"speaker", speaker},
                               {"gender", m_genders.value(speaker)},
                               {"voice", voice},
                               {"voiceName", names.value(voice, voice.section(QLatin1Char(':'), -1))},
                               {"lines", m_counts.value(speaker)}});
    }
    return out;
}

QString Cast::prompt() {
    return QStringLiteral(
        "You prepare a book for a full-cast audiobook. For each numbered paragraph that contains spoken dialogue, "
        "say who speaks it: the character's name as the book most often calls them (the same spelling every time), "
        "and their gender (\"female\", \"male\", or \"\" if unknown). A paragraph of narration, or one where the "
        "speaker can't be told, is \"narrator\". If a first-person narrator speaks aloud, they are \"narrator\". "
        "Use only what the text says; never guess ahead. Answer as JSON: {\"lines\": [{\"block\": n, \"speaker\": "
        "\"…\", \"gender\": \"…\"}]}, listing only paragraphs with dialogue.");
}

QMap<int, Cast::Line> Cast::parse(const QString &json, int blocks) {
    QMap<int, Line> out;
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    for (const QJsonValue &value : object.value(QStringLiteral("lines")).toArray()) {
        const QJsonObject line = value.toObject();
        const int block = line.value(QStringLiteral("block")).toInt(-1);
        const QString speaker = line.value(QStringLiteral("speaker")).toString().trimmed();
        if (block < 0 || block >= blocks || speaker.isEmpty() || speaker.compare(QLatin1String("narrator"), Qt::CaseInsensitive) == 0)
            continue;
        QString gender = line.value(QStringLiteral("gender")).toString().toLower();
        if (gender != QLatin1String("female") && gender != QLatin1String("male"))
            gender.clear();
        out.insert(block, {speaker, gender});
    }
    return out;
}

Ai::TextRequest Cast::request(int chapter, const QList<SpeechBlock> &blocks) const {
    QString paragraphs;
    for (int i = 0; i < blocks.size(); ++i)
        if (blocks[i].kind != QLatin1String("break"))
            paragraphs += QStringLiteral("[%1] %2\n").arg(i).arg(blocks[i].text.simplified().left(1200));
    Ai::TextRequest request;
    request.feature = QStringLiteral("cast");
    request.fast = true;
    request.system = prompt();
    request.schema = QJsonObject{
        {"type", "object"},
        {"properties",
         QJsonObject{{"lines", QJsonObject{{"type", "array"},
                                           {"items", QJsonObject{{"type", "object"},
                                                                 {"properties", QJsonObject{{"block", QJsonObject{{"type", "integer"}}},
                                                                                            {"speaker", QJsonObject{{"type", "string"}}},
                                                                                            {"gender", QJsonObject{{"type", "string"}}}}},
                                                                 {"required", QJsonArray{"block", "speaker", "gender"}},
                                                                 {"additionalProperties", false}}}}}}},
        {"required", QJsonArray{"lines"}},
        {"additionalProperties", false}};
    QStringList known = m_counts.keys();
    request.messages = {{true, QStringLiteral("Book: %1, by %2.\nChapter: %3.\nCharacters named so far: %4.\n\n%5")
                                   .arg(m_reader->title(), m_reader->author(), m_reader->titleOfChapter(chapter),
                                        known.isEmpty() ? QStringLiteral("none") : known.join(QStringLiteral(", ")), paragraphs)}};
    return request;
}

QString Cast::key(const Ai::TextRequest &request) const {
    Ai::Provider *provider = m_ai->textProviderFor(QStringLiteral("cast"));
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("cast"));
    return AudioCache::keyFor(provider ? provider->id() : QString(), model ? model->textModelName(true) : QString(),
                              QStringLiteral("cast"), QString(), request.messages.value(0).text.section(QLatin1Char('\n'), 3));
}

std::optional<QMap<int, Cast::Line>> Cast::kept(int chapter, const QList<SpeechBlock> &blocks) const {
    if (!m_ai->textFor(QStringLiteral("cast")))
        return std::nullopt;
    const QString json = m_cache->directions(key(request(chapter, blocks)));
    if (json.isEmpty())
        return std::nullopt;
    return parse(json, int(blocks.size()));
}

void Cast::ask(int chapter, const QList<SpeechBlock> &blocks, QObject *context,
               std::function<void(const QMap<int, Line> &)> done) {
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("cast"));
    bool dialogue = false;
    for (const SpeechBlock &block : blocks)
        dialogue = dialogue || block.text.contains(QRegularExpression(QStringLiteral("[\"“”‘’'«»]")));
    if (!model || !dialogue) {
        done({});
        return;
    }
    const Ai::TextRequest question = request(chapter, blocks);
    const QString keptAs = key(question);
    const int count = int(blocks.size());
    Ai::Reply *reply = model->generate(question);
    m_ai->track(reply);
    connect(reply, &Ai::Reply::finished, context, [this, reply, keptAs, count, done] {
        reply->deleteLater();
        if (reply->error()) {
            done({});
            return;
        }
        const QString text = reply->result().isEmpty() ? reply->text()
                                                       : QString::fromUtf8(QJsonDocument(reply->result()).toJson(QJsonDocument::Compact));
        m_cache->storeDirections(keptAs, text);
        done(parse(text, count));
    });
}
