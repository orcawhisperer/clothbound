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
#include <algorithm>

namespace {
constexpr int maxCastPromptChars = 24000;
constexpr double estimatedCastCost = 0.005;
}

Cast::Cast(AiServices *ai, AudioCache *cache, Reader *reader, Library *library, VoiceCatalog *catalog, VoicePacks *packs,
           QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader), m_library(library), m_catalog(catalog), m_packs(packs) {
    connect(reader, &Reader::bookChanged, this, &Cast::load);
    if (catalog)
        connect(catalog, &VoiceCatalog::changed, this, &Cast::changed);
    if (packs)
        connect(packs, &VoicePacks::changed, this, &Cast::changed);
}

Cast::~Cast() { cancel(); }

void Cast::cancel() {
    ++m_generation;
    const auto replies = std::exchange(m_replies, {});
    for (const QPointer<Ai::Reply> &reply : replies)
        if (reply)
            reply->abort();
}

QString Cast::family() const {
    if (m_narrator.startsWith(QLatin1String("builtin:kokoro:")))
        return QStringLiteral("kokoro");
    if (m_narrator.isEmpty() || AiServices::isOnThisComputer(m_narrator))
        return {};
    return QStringLiteral("gemini");
}

bool Cast::voiceMatchesFamily(const QString &voice, const QString &family) {
    if (family == QLatin1String("kokoro"))
        return voice.startsWith(QLatin1String("builtin:kokoro:"));
    if (family == QLatin1String("gemini"))
        return !voice.isEmpty() && !AiServices::isOnThisComputer(voice);
    return false;
}

void Cast::setNarrator(const QString &voice) {
    if (voice == m_narrator)
        return;
    m_narrator = voice;
    assignMissing();
    emit changed();
}

void Cast::load() {
    cancel();
    m_lines.clear();
    m_countedChapters.clear();
    m_familyVoices.clear();
    m_genders.clear();
    m_counts.clear();
    const int book = m_reader->bookId();
    m_enabled = book > 0 && m_library->bookSetting(book, QStringLiteral("fullCast")) == QLatin1String("on");
    if (book > 0) {
        const QJsonObject saved =
            QJsonDocument::fromJson(m_library->bookSetting(book, QStringLiteral("cast")).toUtf8()).object();
        if (saved.value(QStringLiteral("v")).toInt() == 2) {
            const QJsonObject families = saved.value(QStringLiteral("families")).toObject();
            for (auto fit = families.begin(); fit != families.end(); ++fit) {
                const QString fam = fit.key();
                const QJsonObject voices = fit.value().toObject();
                for (auto vit = voices.begin(); vit != voices.end(); ++vit) {
                    const QString v = vit.value().toString();
                    if (voiceMatchesFamily(v, fam))
                        m_familyVoices[fam].insert(vit.key(), v);
                }
            }
            const QJsonObject counts = saved.value(QStringLiteral("counts")).toObject();
            for (auto it = counts.begin(); it != counts.end(); ++it)
                if (it.value().toInt() > 0)
                    m_counts.insert(it.key(), it.value().toInt());
            const QJsonObject genders = saved.value(QStringLiteral("genders")).toObject();
            for (auto it = genders.begin(); it != genders.end(); ++it) {
                const QString g = it.value().toString();
                if (g == QLatin1String("female") || g == QLatin1String("male"))
                    m_genders.insert(it.key(), g);
            }
        } else {
            // Backward compatibility with earlier flat {speaker: voice} saves.
            for (auto it = saved.begin(); it != saved.end(); ++it) {
                const QString v = it.value().toString();
                if (v.startsWith(QLatin1String("builtin:kokoro:")))
                    m_familyVoices[QStringLiteral("kokoro")].insert(it.key(), v);
                else if (!v.isEmpty() && !AiServices::isOnThisComputer(v))
                    m_familyVoices[QStringLiteral("gemini")].insert(it.key(), v);
                if (!v.isEmpty())
                    m_counts.insert(it.key(), 1);
            }
        }
    }
    assignMissing();
    emit changed();
}

void Cast::save() const {
    const int book = m_reader->bookId();
    if (book <= 0)
        return;
    QJsonObject families;
    for (auto fit = m_familyVoices.cbegin(); fit != m_familyVoices.cend(); ++fit) {
        QJsonObject voices;
        for (auto vit = fit.value().cbegin(); vit != fit.value().cend(); ++vit)
            voices.insert(vit.key(), vit.value());
        families.insert(fit.key(), voices);
    }
    QJsonObject counts;
    for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it)
        counts.insert(it.key(), it.value());
    QJsonObject genders;
    for (auto it = m_genders.cbegin(); it != m_genders.cend(); ++it)
        genders.insert(it.key(), it.value());
    const QJsonObject saved{{"v", 2}, {"families", families}, {"counts", counts}, {"genders", genders}};
    m_library->setBookSetting(book, QStringLiteral("cast"),
                              QString::fromUtf8(QJsonDocument(saved).toJson(QJsonDocument::Compact)));
}

void Cast::setEnabled(bool enabled) {
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (!enabled)
        cancel();
    if (m_reader->bookId() > 0)
        m_library->setBookSetting(m_reader->bookId(), QStringLiteral("fullCast"), enabled ? QStringLiteral("on") : QString());
    emit changed();
    emit recast();
}

bool Cast::available() const { return !family().isEmpty() && pool().size() >= 3; }

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
    if (m_narrator.isEmpty() || AiServices::isOnThisComputer(m_narrator))
        return out;  // Piper, Pocket, and Studio voices: nothing to cast from.
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
    const QString fam = family();
    if (fam.isEmpty())
        return m_narrator;
    const QString voice = m_familyVoices.value(fam).value(speaker);
    if (voiceMatchesFamily(voice, fam))
        return voice;
    return m_narrator;
}

QString Cast::voiceAt(int chapter, int block) const {
    if (!m_enabled || family().isEmpty())
        return m_narrator;
    return voiceFor(m_lines.value(chapter).value(block).speaker);
}

QSet<int> Cast::breaks(int chapter, const QList<SpeechBlock> &blocks) const {
    QSet<int> out;
    if (!m_enabled || family().isEmpty() || !m_lines.contains(chapter))
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

bool Cast::assignVoice(const QString &speaker, const QString &gender, const QList<Choice> &choices,
                       QMap<QString, QString> &familyMap, QSet<QString> &used) {
    if (familyMap.contains(speaker) || choices.isEmpty())
        return false;
    QList<Choice> fitting;
    for (const Choice &choice : choices)
        if (gender.isEmpty() || choice.gender.isEmpty() || choice.gender == gender)
            fitting.append(choice);
    if (fitting.isEmpty())
        fitting = choices;
    QList<Choice> freeHome, freeAway;
    for (const Choice &choice : fitting)
        if (!used.contains(choice.id))
            (choice.home ? freeHome : freeAway).append(choice);
    const QList<Choice> &from = !freeHome.isEmpty() ? freeHome : !freeAway.isEmpty() ? freeAway : fitting;
    const quint32 hash = qFromBigEndian<quint32>(QCryptographicHash::hash(speaker.toUtf8(), QCryptographicHash::Md5).constData());
    const QString voice = from[int(hash % quint32(from.size()))].id;
    familyMap.insert(speaker, voice);
    used.insert(voice);
    return true;
}

void Cast::assignMissing() {
    const QString fam = family();
    if (fam.isEmpty() || m_counts.isEmpty())
        return;
    const QList<Choice> choices = pool();
    if (choices.isEmpty())
        return;
    QMap<QString, QString> &familyMap = m_familyVoices[fam];
    QSet<QString> used(familyMap.cbegin(), familyMap.cend());
    used.insert(m_narrator);
    bool assigned = false;
    for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it)
        if (assignVoice(it.key(), m_genders.value(it.key()), choices, familyMap, used))
            assigned = true;
    if (assigned)
        save();
}

void Cast::learn(int chapter, const QMap<int, Line> &lines) {
    m_lines.insert(chapter, lines);
    const bool firstTime = !m_countedChapters.contains(chapter);
    m_countedChapters.insert(chapter);
    // New speakers get a voice from the narrator's own family: their gender's,
    // not the narrator's, not another's while any are free, and deterministic.
    const QString fam = family();
    const QList<Choice> choices = fam.isEmpty() ? QList<Choice>{} : pool();
    QMap<QString, QString> *familyMap = fam.isEmpty() ? nullptr : &m_familyVoices[fam];
    QSet<QString> used;
    if (familyMap)
        used = QSet<QString>(familyMap->cbegin(), familyMap->cend());
    used.insert(m_narrator);
    bool dirty = false;
    for (const Line &line : lines) {
        if (line.speaker.isEmpty() || line.speaker.compare(QLatin1String("narrator"), Qt::CaseInsensitive) == 0)
            continue;
        if (firstTime) {
            ++m_counts[line.speaker];
            dirty = true;
        }
        if (!line.gender.isEmpty() && m_genders.value(line.speaker) != line.gender) {
            m_genders.insert(line.speaker, line.gender);
            dirty = true;
        }
        if (familyMap && assignVoice(line.speaker, m_genders.value(line.speaker, line.gender), choices, *familyMap, used))
            dirty = true;
    }
    if (dirty)
        save();
    emit changed();
}

void Cast::setVoice(const QString &speaker, const QString &voice) {
    const QString fam = family();
    if (speaker.isEmpty() || fam.isEmpty() || !voiceMatchesFamily(voice, fam))
        return;
    m_familyVoices[fam].insert(speaker, voice);
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

QString Cast::formatParagraphs(const QList<SpeechBlock> &blocks) {
    QString paragraphs;
    for (int i = 0; i < blocks.size(); ++i) {
        if (blocks[i].kind == QLatin1String("break"))
            continue;
        const QString line = QStringLiteral("[%1] %2\n").arg(i).arg(blocks[i].text.simplified().left(1200));
        if (!paragraphs.isEmpty() && paragraphs.size() + line.size() > maxCastPromptChars)
            break;
        paragraphs += line;
    }
    return paragraphs;
}

Ai::TextRequest Cast::request(int chapter, const QList<SpeechBlock> &blocks) const {
    const QString paragraphs = formatParagraphs(blocks);
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

QString Cast::key(const QList<SpeechBlock> &blocks) const {
    Ai::Provider *provider = m_ai->textProviderFor(QStringLiteral("cast"));
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("cast"));
    return AudioCache::keyFor(provider ? provider->id() : QString(), model ? model->textModelName(true) : QString(),
                              QStringLiteral("cast"), prompt(), formatParagraphs(blocks));
}

std::optional<QMap<int, Cast::Line>> Cast::kept(int chapter, const QList<SpeechBlock> &blocks) const {
    Q_UNUSED(chapter);
    if (!m_ai->textFor(QStringLiteral("cast")))
        return std::nullopt;
    const QString json = m_cache->directions(key(blocks));
    if (json.isEmpty())
        return std::nullopt;
    return parse(json, int(blocks.size()));
}

void Cast::ask(int chapter, const QList<SpeechBlock> &blocks, QObject *context,
               std::function<void(const QMap<int, Line> &)> done) {
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("cast"));
    Ai::Provider *provider = m_ai->textProviderFor(QStringLiteral("cast"));
    // Match double/curly/guillemet/CJK/German quotes, em-dash dialogue, or
    // British single-quoted dialogue without triggering on English contractions.
    static const QRegularExpression dialogueRe(
        QStringLiteral("[\"“”«»「」『』„‟]|(?:^|\\n)\\s*[—–―]\\s+\\S|(?<!\\w)['‘][^'‘’\\n]{2,}['’](?!\\w)"));
    bool dialogue = false;
    for (const SpeechBlock &block : blocks)
        if (dialogueRe.match(block.text).hasMatch()) {
            dialogue = true;
            break;
        }
    if (!model || !dialogue || (provider && !provider->isLocal() && !m_ai->fitsBudget(estimatedCastCost))) {
        done({});
        return;
    }
    const Ai::TextRequest question = request(chapter, blocks);
    const QString keptAs = key(blocks);
    const int count = int(blocks.size());
    const quint64 gen = m_generation;
    const int bookId = m_reader->bookId();
    Ai::Reply *reply = model->generate(question);
    m_ai->track(reply);
    m_replies.removeAll(nullptr);
    m_replies.append(reply);
    QPointer<QObject> guard(context ? context : this);
    connect(reply, &Ai::Reply::finished, this, [this, reply, keptAs, count, gen, bookId, guard, done] {
        reply->deleteLater();
        m_replies.removeAll(reply);
        if (gen != m_generation || m_reader->bookId() != bookId || !guard)
            return;
        if (reply->error()) {
            done({});
            return;
        }
        const QString text = reply->result().isEmpty()
                                 ? reply->text()
                                 : QString::fromUtf8(QJsonDocument(reply->result()).toJson(QJsonDocument::Compact));
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject() ||
            !doc.object().value(QStringLiteral("lines")).isArray()) {
            done({});
            return;
        }
        m_cache->storeDirections(keptAs, text);
        done(parse(text, count));
    });
}
