#include "audio/director.h"

#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "formats/pdfbook.h"
#include "reader/reader.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace {
constexpr int maxDirectorPromptChars = 28000;
constexpr double estimatedDirectorCost = 0.005;
constexpr int maxPdfBatchPages = 8;
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

QPair<int, int> Director::pdfBatchRange(const Reader *reader, int page) {
    if (!reader || reader->format() != QLatin1String("pdf") || reader->chapterCount() <= 0)
        return {page, page};
    const int pageCount = reader->chapterCount();
    const int clamped = qBound(0, page, pageCount - 1);
    QList<int> boundaries;
    for (const QVariant &entry : reader->contents()) {
        const int p = entry.toMap().value(QStringLiteral("chapter"), -1).toInt();
        if (p >= 0 && p < pageCount)
            boundaries.append(p);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    int secStart = 0;
    int secEnd = pageCount - 1;
    for (int b : std::as_const(boundaries)) {
        if (b <= clamped)
            secStart = b;
        else {
            secEnd = b - 1;
            break;
        }
    }
    const int offset = clamped - secStart;
    const int first = secStart + (offset / maxPdfBatchPages) * maxPdfBatchPages;
    const int last = qMin(secEnd, first + maxPdfBatchPages - 1);
    return {first, last};
}

QHash<int, QMap<int, QString>> Director::splitMoodsByPage(const QMap<int, QString> &combined,
                                                          const QHash<int, QPair<int, int>> &ranges, int firstPage,
                                                          int lastPage) {
    QHash<int, QMap<int, QString>> byPage;
    QString carryMood;
    for (int p = firstPage; p <= lastPage; ++p) {
        QMap<int, QString> pageMoods;
        const auto [start, count] = ranges.value(p, {0, 0});
        if (count > 0) {
            if (!carryMood.isEmpty())
                pageMoods.insert(0, carryMood);
            for (auto it = combined.lowerBound(start); it != combined.end() && it.key() < start + count; ++it)
                pageMoods.insert(it.key() - start, it.value());
            carryMood = Speech::moodAt(combined, start + count - 1);
        }
        byPage.insert(p, pageMoods);
    }
    return byPage;
}

Director::PdfBatch Director::buildPdfBatch(int page) const {
    PdfBatch batch;
    const auto [first, last] = pdfBatchRange(m_reader, page);
    batch.firstPage = first;
    batch.lastPage = last;
    if (!m_reader || m_reader->format() != QLatin1String("pdf"))
        return batch;
    for (int p = first; p <= last; ++p) {
        const QJsonArray arr = PdfBook::speakable(m_reader->pdfPageText(p), m_reader->title());
        const QList<SpeechBlock> pageBlocks =
            Speech::parseBlocks(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        batch.ranges.insert(p, {int(batch.blocks.size()), int(pageBlocks.size())});
        batch.blocks.append(pageBlocks);
    }
    return batch;
}

std::optional<QHash<int, QMap<int, QString>>> Director::keptPdfBatch(int page, const QString &performance) const {
    const PdfBatch batch = buildPdfBatch(page);
    const auto combined = kept(batch.firstPage, batch.blocks, performance);
    if (!combined)
        return std::nullopt;
    return splitMoodsByPage(*combined, batch.ranges, batch.firstPage, batch.lastPage);
}

void Director::askPdfBatch(int page, const QString &performance, QObject *context,
                           std::function<void(const QHash<int, QMap<int, QString>> &byPage)> done) {
    const PdfBatch batch = buildPdfBatch(page);
    if (batch.blocks.isEmpty()) {
        QHash<int, QMap<int, QString>> empty;
        for (int p = batch.firstPage; p <= batch.lastPage; ++p)
            empty.insert(p, {});
        done(empty);
        return;
    }
    ask(batch.firstPage, batch.blocks, performance, context,
        [ranges = batch.ranges, first = batch.firstPage, last = batch.lastPage,
         done = std::move(done)](const QMap<int, QString> &combined) {
            done(splitMoodsByPage(combined, ranges, first, last));
        });
}

QPair<int, int> Director::chapterRange(int chapter) const {
    if (m_reader && m_reader->format() == QLatin1String("pdf"))
        return pdfBatchRange(m_reader, chapter);
    return {chapter, chapter};
}

std::optional<QHash<int, QMap<int, QString>>> Director::keptChapterOrBatch(int chapter,
                                                                           const QList<SpeechBlock> &blocks,
                                                                           const QString &performance) const {
    if (m_reader && m_reader->format() == QLatin1String("pdf"))
        return keptPdfBatch(chapter, performance);
    if (const auto single = kept(chapter, blocks, performance))
        return QHash<int, QMap<int, QString>>{{chapter, *single}};
    return std::nullopt;
}

void Director::askChapterOrBatch(int chapter, const QList<SpeechBlock> &blocks, const QString &performance,
                                 QObject *context,
                                 std::function<void(const QHash<int, QMap<int, QString>> &byChapter)> done) {
    if (m_reader && m_reader->format() == QLatin1String("pdf")) {
        askPdfBatch(chapter, performance, context, std::move(done));
        return;
    }
    ask(chapter, blocks, performance, context, [chapter, done = std::move(done)](const QMap<int, QString> &moods) {
        done(QHash<int, QMap<int, QString>>{{chapter, moods}});
    });
}
