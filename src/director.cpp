#include "director.h"

#include "aiservices.h"
#include "audiocache.h"
#include "reader.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
constexpr int maxDirectorPromptChars = 28000;
constexpr double estimatedDirectorCost = 0.005;
}

Director::Director(AiServices *ai, AudioCache *cache, Reader *reader, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader) {}

Director::~Director() { cancel(); }

void Director::cancel() {
    const auto replies = std::exchange(m_replies, {});
    for (const QPointer<Ai::Reply> &reply : replies)
        if (reply)
            reply->abort();
}

QString Director::prompt() {
    QFile file(QStringLiteral(":/prompts/director.md"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QMap<int, QString> Director::parse(const QString &json, int blocks) {
    QMap<int, QString> changes;
    for (const auto &value : QJsonDocument::fromJson(json.toUtf8()).object().value("changes").toArray()) {
        const QJsonObject change = value.toObject();
        const int paragraph = change.value("paragraph").toInt(-1);
        const QString mood = change.value("mood").toString();
        if (paragraph >= 0 && paragraph < blocks && Speech::moods().contains(mood))
            changes.insert(paragraph, mood == QStringLiteral("neutral") ? QString() : mood);
    }
    return changes;
}

QSet<int> Director::breaks(const QMap<int, QString> &moods) {
    QSet<int> result;
    for (auto it = moods.cbegin(); it != moods.cend(); ++it)
        result.insert(it.key());
    return result;
}

bool Director::canDirect(const QList<SpeechBlock> &blocks) const {
    if (!m_ai->textFor(QStringLiteral("director")))
        return false;
    for (const SpeechBlock &block : blocks)
        if (block.kind != QStringLiteral("break"))
            return true;
    return false;
}

Ai::TextRequest Director::request(int chapter, const QList<SpeechBlock> &blocks, const QString &performance) const {
    QString paragraphs;
    for (int i = 0; i < blocks.size(); ++i) {
        if (blocks[i].kind == QStringLiteral("break"))
            continue;
        const QString line = QStringLiteral("[%1] %2\n").arg(i).arg(blocks[i].text.simplified().left(1600));
        if (!paragraphs.isEmpty() && paragraphs.size() + line.size() > maxDirectorPromptChars)
            break;
        paragraphs += line;
    }
    Ai::TextRequest request;
    request.feature = QStringLiteral("director");
    request.fast = true;
    request.effort = QStringLiteral("low");
    request.system = prompt();
    request.messages = {{true, QStringLiteral("Book: %1, by %2.\nChapter: %3.\nPerformance: %4.\n\n%5")
                                   .arg(m_reader->title(), m_reader->author(), m_reader->titleOfChapter(chapter),
                                        performance, paragraphs)}};
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
    return request;
}

// The same chapter, performance, prompt and model always get the same
// direction.
QString Director::key(const Ai::TextRequest &request) const {
    Ai::Provider *provider = m_ai->textProviderFor(QStringLiteral("director"));
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("director"));
    return AudioCache::keyFor(provider ? provider->id() : QString(), model ? model->textModelName(true) : QString(),
                              QStringLiteral("director"), request.system, request.messages.value(0).text);
}

std::optional<QMap<int, QString>> Director::kept(int chapter, const QList<SpeechBlock> &blocks,
                                                 const QString &performance) const {
    if (!canDirect(blocks))
        return std::nullopt;
    const QString json = m_cache->directions(key(request(chapter, blocks, performance)));
    if (json.isEmpty())
        return std::nullopt;
    return parse(json, int(blocks.size()));
}

void Director::ask(int chapter, const QList<SpeechBlock> &blocks, const QString &performance, QObject *context,
                   std::function<void(const QMap<int, QString> &)> done) {
    if (!canDirect(blocks)) {
        done({});
        return;
    }
    Ai::Provider *provider = m_ai->textProviderFor(QStringLiteral("director"));
    Ai::TextModel *model = m_ai->textFor(QStringLiteral("director"));
    if (!model || (provider && !provider->isLocal() && !m_ai->fitsBudget(estimatedDirectorCost))) {
        done({});
        return;
    }
    const Ai::TextRequest question = request(chapter, blocks, performance);
    const QString keptAs = key(question);
    const int count = int(blocks.size());
    Ai::Reply *reply = model->generate(question);
    m_ai->track(reply);
    m_replies.removeAll(nullptr);
    m_replies.append(reply);
    QPointer<QObject> guard(context ? context : this);
    connect(reply, &Ai::Reply::finished, this, [this, reply, keptAs, count, guard, done] {
        reply->deleteLater();
        m_replies.removeAll(reply);
        if (!guard)
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
            !doc.object().value(QStringLiteral("changes")).isArray()) {
            done({});
            return;
        }
        m_cache->storeDirections(keptAs, text);
        done(parse(text, count));
    });
}
