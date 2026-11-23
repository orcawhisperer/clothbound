#include "director.h"

#include "aiservices.h"
#include "audiocache.h"
#include "reader.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

Director::Director(AiServices *ai, AudioCache *cache, Reader *reader, QObject *parent)
    : QObject(parent), m_ai(ai), m_cache(cache), m_reader(reader) {}

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
    if (!m_ai->text())
        return false;
    for (const SpeechBlock &block : blocks)
        if (block.kind != QStringLiteral("break"))
            return true;
    return false;
}

Ai::TextRequest Director::request(int chapter, const QList<SpeechBlock> &blocks, const QString &performance) const {
    QString paragraphs;
    for (int i = 0; i < blocks.size(); ++i)
        if (blocks[i].kind != QStringLiteral("break"))
            paragraphs += QStringLiteral("[%1] %2\n").arg(i).arg(blocks[i].text.simplified().left(1600));
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
    return AudioCache::keyFor(m_ai->provider()->id(), m_ai->text()->textModelName(true), QStringLiteral("director"),
                              request.system, request.messages.value(0).text);
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
    const Ai::TextRequest question = request(chapter, blocks, performance);
    const QString keptAs = key(question);
    const int count = int(blocks.size());
    Ai::Reply *reply = m_ai->text()->generate(question);
    m_ai->track(reply);
    connect(reply, &Ai::Reply::finished, context, [this, reply, keptAs, count, done] {
        reply->deleteLater();
        if (reply->error()) {
            done({});
            return;
        }
        m_cache->storeDirections(keptAs, reply->text());
        done(parse(reply->text(), count));
    });
}
