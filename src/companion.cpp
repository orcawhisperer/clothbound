#include "companion.h"

#include "aiservices.h"
#include "aiusage.h"
#include "epubbook.h"
#include "pdfbook.h"
#include "reader.h"

#include <QCoreApplication>
#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>

namespace {

// Gemini reads up to this much of the book so far (about 300,000 tokens);
// a model on this computer, a chapter's worth.
constexpr qint64 cloudLimit = 1200000;
constexpr qint64 localLimit = 24000;
constexpr qint64 localWhosWhoLimit = 16000;

QStringList parseSdcvJson(const QByteArray &json, const QString &word) {
    QStringList out;
    for (const QJsonValue &entry : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = entry.toObject();
        if (object.value(QStringLiteral("word")).toString().compare(word, Qt::CaseInsensitive) == 0 || out.isEmpty())
            out.append(object.value(QStringLiteral("definition")).toString().trimmed() + QStringLiteral("\n— ") +
                       object.value(QStringLiteral("dict")).toString());
        if (out.size() >= 2)
            break;
    }
    return out;
}

const char *noSpoilers =
    "The reader is partway through the book. You see only what they have read so far. Never reveal, hint at, or "
    "guess what happens later, even if you know the book; if they ask about something not yet in the text, say "
    "it hasn't come up yet.";

QString instructions(const QString &task) {
    return task + QStringLiteral("\n\n") + QString::fromLatin1(noSpoilers) +
           QStringLiteral("\nWrite in %1. Plain prose, no headings; short paragraphs.").arg(Companion::readerLanguage());
}

QString passageText(const PassageIndex::Hit &hit, const QList<Companion::Chapter> &chapters) {
    QString title;
    for (const Companion::Chapter &chapter : chapters)
        if (chapter.index == hit.passage.chapter)
            title = chapter.title;
    return QStringLiteral("[%1]\n%2").arg(title.isEmpty() ? QStringLiteral("Chapter %1").arg(hit.passage.chapter + 1) : title,
                                          hit.passage.text);
}

}  // namespace

Companion::Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_ai(ai), m_reader(reader), m_index(ai, databaseConnection) {
    connect(reader, &Reader::bookChanged, this, [this] {
        stop();
        m_feature.clear();
        m_subject.clear();
        m_answer.clear();
        m_people.clear();
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_conversation.clear();
        m_pendingUserTurn = false;
        m_problem.clear();
        emit changed();
        emit answerChanged();
        emit conversationChanged();
    });
}

Companion::~Companion() { stop(); }

QVariantList Companion::conversation() const {
    QVariantList out;
    for (const Ai::Message &message : m_conversation)
        out.append(QVariantMap{{QStringLiteral("fromUser"), message.fromUser}, {QStringLiteral("text"), message.text}});
    return out;
}

QString Companion::cleanMarkdown(const QString &text) {
    if (text.isEmpty())
        return text;
    QString out = text;
    // Strip raw HTML <img> tags and neutralize all Markdown image embeds
    // (![alt](url), ![alt][ref], and any stray "![") so QQuickTextImageHandler
    // can never issue outbound HTTP or local file requests from untrusted prose.
    static const QRegularExpression htmlImg(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression inlineImg(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression refImg(QStringLiteral("!\\[([^\\]]*)\\]\\[[^\\]]*\\]"));
    out.remove(htmlImg);
    out.replace(inlineImg, QStringLiteral("\\1"));
    out.replace(refImg, QStringLiteral("\\1"));
    out.replace(QStringLiteral("!["), QStringLiteral("!\u200B["));
    return out;
}

QString Companion::readerLanguage() {
    return QLocale::languageToString(QLocale().language());
}

QList<Companion::Chapter> Companion::textUpTo(const QString &path, const QString &format, int chapter, qint64 offset,
                                              qint64 limit, std::shared_ptr<std::atomic_bool> cancel) {
    QList<Chapter> out;
    if (chapter < 0 || path.isEmpty())
        return out;
    const auto cancelled = [&cancel] { return cancel && cancel->load(); };
    if (format == QLatin1String("pdf")) {
        PdfBook book(path);
        if (!book.open())
            return out;
        const int last = qMin(chapter, book.pageCount() - 1);
        if (last < 0)
            return out;
        if (limit > 0) {
            qint64 collected = 0;
            for (int page = last; page >= 0 && !cancelled(); --page) {
                QString text = book.document()->getAllText(page).text();
                if (page == chapter && offset >= 0)
                    text = text.left(offset);
                out.append({page, QCoreApplication::translate("Companion", "Page %1").arg(page + 1), text});
                collected += text.trimmed().size();
                if (collected >= limit + 512)
                    break;
            }
            if (cancelled())
                return {};
            std::reverse(out.begin(), out.end());
            return out;
        }
        for (int page = 0; page <= last && !cancelled(); ++page) {
            QString text = book.document()->getAllText(page).text();
            if (page == chapter && offset >= 0)
                text = text.left(offset);
            out.append({page, QCoreApplication::translate("Companion", "Page %1").arg(page + 1), text});
        }
        return cancelled() ? QList<Chapter>{} : out;
    }
    EpubBook book(path);
    if (!book.open())
        return out;
    const int last = qMin(chapter, int(book.spine().size()) - 1);
    if (last < 0)
        return out;
    if (limit > 0) {
        qint64 collected = 0;
        for (int i = last; i >= 0 && !cancelled(); --i) {
            QString text = book.chapterText(i);
            if (i == chapter && offset >= 0)
                text = text.left(offset);
            if (!text.trimmed().isEmpty()) {
                out.append({i, QString(), text});
                collected += text.trimmed().size();
                if (collected >= limit + 512)
                    break;
            }
        }
        if (cancelled())
            return {};
        std::reverse(out.begin(), out.end());
        return out;
    }
    for (int i = 0; i <= last && !cancelled(); ++i) {
        QString text = book.chapterText(i);
        if (i == chapter && offset >= 0)
            text = text.left(offset);
        if (!text.trimmed().isEmpty())
            out.append({i, QString(), text});
    }
    return cancelled() ? QList<Chapter>{} : out;
}

QString Companion::joined(const QList<Chapter> &chapters, qint64 limit) {
    QString out;
    for (qsizetype i = chapters.size() - 1; i >= 0 && out.size() < limit; --i) {
        const Chapter &chapter = chapters[i];
        QString part = chapter.text.trimmed();
        if (out.size() + part.size() > limit)
            part = part.right(limit - out.size());
        const QString heading = chapter.title.isEmpty() ? QString() : QStringLiteral("## %1\n\n").arg(chapter.title);
        out.prepend(heading + part + QStringLiteral("\n\n"));
    }
    return out.trimmed();
}

QStringList Companion::dictionary(const QString &word) {
    if (word.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty())
        return {};
    QProcess sdcv;
    sdcv.start(QStringLiteral("sdcv"),
               {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), word});
    if (!sdcv.waitForFinished(4000)) {
        sdcv.kill();
        sdcv.waitForFinished(500);
        return {};
    }
    if (sdcv.exitStatus() != QProcess::NormalExit || sdcv.exitCode() != 0)
        return {};
    return parseSdcvJson(sdcv.readAllStandardOutput(), word);
}

QString Companion::bookLine() const {
    return QStringLiteral("Book: %1, by %2.").arg(m_reader->title(), m_reader->author());
}

bool Companion::cloudFor(const QString &feature) const {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    return provider && provider->id() != QLatin1String("local");
}

Ai::TextModel *Companion::modelFor(const QString &feature) {
    Ai::TextModel *model = m_ai->textFor(feature);
    if (!model)
        fail(m_ai->preset() == QLatin1String("local") || m_ai->overrideFor(feature) == QLatin1String("local")
                 ? tr("This needs a model on this computer: set one up in Intelligence (✦).")
                 : tr("This needs a Gemini key or a model on this computer, in Intelligence (✦)."));
    return model;
}

bool Companion::checkBudget(const QString &feature, qint64 charsEstimate, bool fast, qint64 outputTokens) {
    if (!cloudFor(feature))
        return true;
    Ai::TextModel *model = m_ai->textFor(feature);
    if (!model)
        return false;
    Ai::Usage usage;
    usage.inputTokens = qMax<qint64>(1, charsEstimate / 4);
    usage.outputTokens = outputTokens;
    const double cost = AiUsageLedger::cost(model->textModelName(fast), usage, QDate::currentDate(), false);
    if (!m_ai->fitsBudget(cost)) {
        fail(m_ai->describe(int(Ai::Error::Budget)));
        return false;
    }
    return true;
}

void Companion::begin(const QString &feature, const QString &subject) {
    stop();
    m_feature = feature;
    m_subject = subject;
    m_answer.clear();
    m_problem.clear();
    m_busy = true;
    m_where = m_ai->whereFor(feature);
    m_status = tr("Thinking…");
    emit changed();
    emit answerChanged();
}

void Companion::setStatus(const QString &status) {
    m_status = status;
    emit changed();
}

void Companion::rollbackPendingTurn() {
    if (!m_pendingUserTurn)
        return;
    m_pendingUserTurn = false;
    if (!m_answer.trimmed().isEmpty()) {
        m_conversation.append({false, m_answer});
        emit conversationChanged();
    } else if (!m_conversation.isEmpty() && m_conversation.last().fromUser) {
        m_conversation.removeLast();
        emit conversationChanged();
    }
}

void Companion::fail(const QString &problem) {
    rollbackPendingTurn();
    m_problem = problem;
    m_busy = false;
    emit changed();
}

void Companion::stop() {
    ++m_generation;
    if (m_readCancel) {
        *m_readCancel = true;
        m_readCancel.reset();
    }
    if (m_dictProcess) {
        if (m_dictProcess->state() != QProcess::NotRunning) {
            m_dictProcess->kill();
            m_dictProcess->waitForFinished(500);
        }
        m_dictProcess->deleteLater();
        m_dictProcess.clear();
    }
    m_index.cancel();
    if (m_reply)
        m_reply->abort();
    rollbackPendingTurn();
    if (m_busy) {
        m_busy = false;
        emit changed();
    }
}

void Companion::readSoFar(int chapter, qint64 offset, qint64 limit, std::function<void(const QList<Chapter> &)> then) {
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, path, format, chapter, offset, limit, cancel, generation, then] {
        const QList<Chapter> chapters = textUpTo(path, format, chapter, offset, limit, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, generation, then] {
                if (!self || self->m_generation != generation)
                    return;
                // The titles the reader shows, from the contents.
                QList<Chapter> titled = chapters;
                for (Chapter &chapter : titled)
                    if (chapter.title.isEmpty())
                        chapter.title = self->m_reader->titleOfChapter(chapter.index);
                then(titled);
            },
            Qt::QueuedConnection);
    });
}

void Companion::run(const QString &feature, const Ai::TextRequest &request, std::function<void(Ai::Reply *)> done) {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    Ai::TextModel *model = provider ? provider->text() : nullptr;
    if (!model) {
        modelFor(feature);
        return;
    }
    m_where = m_ai->whereFor(feature);
    Ai::Reply *reply = model->generate(request);
    m_ai->track(reply);
    m_reply = reply;
    const int generation = m_generation;
    QPointer<Ai::Provider> providerGuard(provider);
    setStatus(tr("Writing…"));
    connect(reply, &Ai::Reply::textReceived, this, [this, reply, generation](const QString &delta) {
        if (reply != m_reply || generation != m_generation)
            return;
        m_answer += delta;
        emit answerChanged();
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, done, generation, providerGuard] {
        reply->deleteLater();
        if (reply != m_reply || generation != m_generation)
            return;
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                fail(m_ai->describeFor(providerGuard, int(reply->error().kind), reply->error().message));
            return;
        }
        if (m_answer.isEmpty() && reply->result().isEmpty())
            m_answer = reply->text();
        if (done)
            done(reply);
        if (m_problem.isEmpty()) {
            m_busy = false;
            emit answerChanged();
            emit changed();
        }
    });
}

// --- Define, explain, translate ----------------------------------------------

void Companion::define(const QString &word, int chapter, qint64 offset) {
    const QString wanted = word.trimmed();
    begin(QStringLiteral("define"), wanted);
    const int generation = m_generation;
    const auto askModel = [this, wanted, chapter, offset, generation] {
        if (generation != m_generation)
            return;
        if (!modelFor(m_feature))
            return;
        if (!checkBudget(m_feature, 1600, true, 120))
            return;
        setStatus(tr("Reading the sentence…"));
        qint64 end = offset + 400;
        if (chapter == m_reader->chapter() && m_reader->pageEnd() > 0)
            end = qMin(end, qMax<qint64>(m_reader->pageEnd(), offset + wanted.size()));
        readSoFar(chapter, end, 2000, [this, wanted, generation](const QList<Chapter> &chapters) {
            if (generation != m_generation)
                return;
            const QString around = joined(chapters, 1200);
            Ai::TextRequest request;
            request.feature = QStringLiteral("define");
            request.fast = true;
            request.stream = true;
            request.system = instructions(QStringLiteral(
                "You are a dictionary. Give the word's meaning as it is used in this passage: part of speech, a "
                "one-line definition, and, if the usage is old, rare or figurative, one sentence on that. At most 60 words."));
            request.messages = {{true, bookLine() + QStringLiteral("\n\nPassage:\n") + around +
                                           QStringLiteral("\n\nWord: ") + wanted}};
            run(QStringLiteral("define"), request);
        });
    };

    // A dictionary on this computer answers first (asynchronously) and costs nothing.
    if (!wanted.isEmpty() && !wanted.contains(QLatin1Char(' ')) &&
        !QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty()) {
        setStatus(tr("Checking your dictionary…"));
        auto *sdcv = new QProcess(this);
        m_dictProcess = sdcv;
        auto *timer = new QTimer(sdcv);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, sdcv, [sdcv] {
            if (sdcv->state() != QProcess::NotRunning)
                sdcv->kill();
        });
        connect(sdcv, &QProcess::errorOccurred, this, [this, sdcv, generation, askModel](QProcess::ProcessError) {
            if (sdcv == m_dictProcess)
                m_dictProcess.clear();
            sdcv->deleteLater();
            if (generation == m_generation)
                askModel();
        });
        connect(sdcv, &QProcess::finished, this,
                [this, sdcv, wanted, generation, askModel](int code, QProcess::ExitStatus status) {
                    if (sdcv == m_dictProcess)
                        m_dictProcess.clear();
                    const QByteArray output = sdcv->readAllStandardOutput();
                    sdcv->deleteLater();
                    if (generation != m_generation)
                        return;
                    const QStringList entries = (status == QProcess::NormalExit && code == 0) ? parseSdcvJson(output, wanted) : QStringList();
                    if (!entries.isEmpty()) {
                        m_answer = entries.join(QStringLiteral("\n\n"));
                        m_where = tr("From your dictionary");
                        m_busy = false;
                        emit answerChanged();
                        emit changed();
                        return;
                    }
                    askModel();
                });
        timer->start(4000);
        sdcv->start(QStringLiteral("sdcv"),
                    {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), wanted});
        return;
    }
    askModel();
}

void Companion::explain(const QString &text, int chapter, qint64 end) {
    begin(QStringLiteral("explain"), text.trimmed());
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (!checkBudget(m_feature, cloud ? 60000 : 12000, false, 400))
        return;
    const int generation = m_generation;
    setStatus(tr("Reading up to here…"));
    readSoFar(chapter, end, cloud ? 60000 : 12000, [this, text, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const bool isCloud = cloudFor(QStringLiteral("explain"));
        const QString context = joined(chapters, isCloud ? 60000 : 12000);
        if (!checkBudget(QStringLiteral("explain"), context.size() + text.size(), false, 400))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("explain");
        request.stream = true;
        request.system = instructions(QStringLiteral(
            "Explain the selected words to a reader in the middle of this book: what they mean, what is happening, "
            "and any allusion, history, idiom or old usage a modern reader might miss. Use the text before it for "
            "context. Be brief: one to three short paragraphs."));
        request.messages = {{true, bookLine() + QStringLiteral("\n\nThe book up to the selection:\n") + context +
                                       QStringLiteral("\n\nSelected: \"") + text.trimmed() + QStringLiteral("\"")}};
        run(QStringLiteral("explain"), request);
    });
}

void Companion::translate(const QString &text, int chapter, qint64 end) {
    begin(QStringLiteral("translate"), text.trimmed());
    if (!modelFor(m_feature))
        return;
    if (!checkBudget(m_feature, 2400 + text.size(), true, 300))
        return;
    // Into the reader's language; if the selected words are already in it, into
    // English (or Spanish for English readers). Works even for PDFs or untagged
    // EPUBs with no book language, and for foreign quotes inside an English book.
    const QString readerLang = readerLanguage();
    const QString alternate = readerLang == QLatin1String("English") ? QStringLiteral("Spanish") : QStringLiteral("English");
    const QString bookLang = m_reader->language().trimmed();
    const bool knownSameLanguage = !bookLang.isEmpty() && QLocale(bookLang).language() == QLocale().language();
    const int generation = m_generation;
    readSoFar(chapter, end, 2500, [this, text, readerLang, alternate, knownSameLanguage, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const QString context = joined(chapters, 2000);
        if (!checkBudget(QStringLiteral("translate"), context.size() + text.size(), true, 300))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("translate");
        request.fast = true;
        request.stream = true;
        request.system =
            knownSameLanguage
                ? QStringLiteral(
                      "Translate the selected words into %1 (or if the selected words are already in %1, translate them "
                      "into %2), faithfully and in a natural literary style, using the passage before them for context. "
                      "Give only the translation.")
                      .arg(readerLang, alternate)
                : QStringLiteral(
                      "Translate the selected words into %1 (or if they are already in %1, into %2), faithfully and in a "
                      "natural literary style, using the passage before them for context. Give only the translation.")
                      .arg(readerLang, alternate);
        request.messages = {{true, QStringLiteral("Context:\n") + context + QStringLiteral("\n\nTranslate:\n") +
                                       text.trimmed()}};
        run(QStringLiteral("translate"), request);
    });
}

// --- Ask ---------------------------------------------------------------------

QVariantMap Companion::askEstimate() const {
    const bool cloud = cloudFor(QStringLiteral("ask"));
    // Characters so far, from the reader's progress through the book.
    qint64 soFar = m_reader->pageEnd();
    for (int i = 0; i < m_reader->chapter(); ++i)
        soFar += m_reader->charactersIn(i);
    // While chapter lengths are still being measured, fall back to a conservative
    // estimate so the cost readout and budget check never under-report as $0.
    if (soFar <= 0 && m_reader->chapter() > 0)
        soFar = qint64(m_reader->chapter() + 1) * 12000;
    qint64 historyChars = 0;
    for (const Ai::Message &message : m_conversation)
        historyChars += message.text.size();
    const double characters = qMin<double>(cloudLimit, soFar) + historyChars;
    double cost = 0;
    if (cloud) {
        if (Ai::TextModel *model = m_ai->textFor(QStringLiteral("ask"))) {
            Ai::Usage usage;
            usage.inputTokens = qMax<qint64>(1, qint64(characters / 4));
            usage.outputTokens = 400;
            cost = AiUsageLedger::cost(model->textModelName(false), usage, QDate::currentDate(), false);
        }
    }
    return {{QStringLiteral("where"), m_ai->whereFor(QStringLiteral("ask"))},
            {QStringLiteral("words"), qRound(qMin<double>(cloudLimit, soFar) / 6)},
            {QStringLiteral("cost"), cost},
            {QStringLiteral("local"), !cloud}};
}

void Companion::newConversation() {
    stop();
    m_conversation.clear();
    m_pendingUserTurn = false;
    m_answer.clear();
    m_feature = QStringLiteral("ask");
    m_problem.clear();
    emit changed();
    emit answerChanged();
    emit conversationChanged();
}

void Companion::ask(const QString &question) {
    const QString asked = question.trimmed();
    if (asked.isEmpty())
        return;
    begin(QStringLiteral("ask"), asked);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud) {
        const double cost = askEstimate().value(QStringLiteral("cost")).toDouble();
        if (!m_ai->fitsBudget(cost)) {
            fail(m_ai->describe(int(Ai::Error::Budget)));
            return;
        }
    }
    m_conversation.append({true, asked});
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();
    setStatus(cloud ? tr("Reading the book so far…") : tr("Finding the passages that matter…"));
    const int bookId = m_reader->bookId();
    const int generation = m_generation;
    const qint64 readLimit = cloud ? cloudLimit : -1;
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), readLimit, [this, asked, bookId, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const bool isCloud = cloudFor(QStringLiteral("ask"));
        const auto send = [this, isCloud, generation](const QString &context, const QString &how) {
            if (generation != m_generation)
                return;
            // Build alternating conversation turns (trimmed for local models so
            // multi-turn history doesn't overflow smaller context windows).
            QList<Ai::Message> turns;
            qint64 historyBudget = isCloud ? 100000 : 8000;
            for (qsizetype i = m_conversation.size() - 1; i >= 0; --i) {
                const Ai::Message &msg = m_conversation[i];
                if (!turns.isEmpty() && historyBudget - msg.text.size() < 0)
                    break;
                historyBudget -= msg.text.size();
                turns.prepend(msg);
            }
            if (!turns.isEmpty() && !turns.first().fromUser)
                turns.removeFirst();
            QList<Ai::Message> normalized;
            for (const Ai::Message &msg : turns) {
                if (!normalized.isEmpty() && normalized.last().fromUser == msg.fromUser)
                    normalized.last().text += QStringLiteral("\n\n") + msg.text;
                else
                    normalized.append(msg);
            }
            if (isCloud) {
                qint64 totalChars = context.size();
                for (const Ai::Message &msg : normalized)
                    totalChars += msg.text.size();
                if (!checkBudget(QStringLiteral("ask"), totalChars, false, 400))
                    return;
            }
            Ai::TextRequest request;
            request.feature = QStringLiteral("ask");
            request.stream = true;
            request.system = instructions(QStringLiteral(
                "Answer the reader's questions about the book they are reading, from its text below (%1). Quote "
                "briefly when it helps, and say which chapter. If the text doesn't say, say so.").arg(how));
            request.messages = {{true, bookLine() + QStringLiteral("\n\n") + context}};
            request.messages.append({false, QStringLiteral("I've read it. What would you like to know?")});
            request.messages += normalized;
            run(QStringLiteral("ask"), request, [this, generation](Ai::Reply *) {
                if (generation != m_generation)
                    return;
                m_pendingUserTurn = false;
                m_conversation.append({false, m_answer});
                emit conversationChanged();
            });
        };
        if (isCloud) {
            send(joined(chapters, cloudLimit), QStringLiteral("the whole book up to the reader's place"));
            return;
        }
        // On this computer: the passages closest to the question, and the
        // pages just read.
        QList<PassageIndex::Passage> passages;
        for (const Chapter &chapter : chapters)
            passages += PassageIndex::split(chapter.index, chapter.text);
        QPointer<Companion> self(this);
        const auto answer = [self, send, chapters, generation](const QList<PassageIndex::Hit> &hits, bool) {
            if (!self || self->m_generation != generation)
                return;
            QList<PassageIndex::Hit> ordered = hits;
            std::sort(ordered.begin(), ordered.end(), [](const PassageIndex::Hit &a, const PassageIndex::Hit &b) {
                return a.passage.chapter != b.passage.chapter ? a.passage.chapter < b.passage.chapter
                                                              : a.passage.start < b.passage.start;
            });
            QString context = QStringLiteral("Passages that may matter:\n\n");
            for (const PassageIndex::Hit &hit : ordered)
                context += passageText(hit, chapters) + QStringLiteral("\n\n");
            context += QStringLiteral("What the reader read last:\n\n") + joined(chapters, 4000);
            send(context, QStringLiteral("passages found for the question, and the latest pages"));
        };
        if (m_ai->embedder() && m_index.indexed(bookId) < passages.size()) {
            setStatus(tr("Getting to know the book…"));
            m_index.build(
                bookId, passages,
                [self, generation](double share) {
                    if (self && self->m_generation == generation)
                        self->setStatus(tr("Getting to know the book… %1%").arg(qRound(share * 100)));
                },
                [self, bookId, passages, asked, answer, generation](const QString &) {
                    if (!self || self->m_generation != generation)
                        return;
                    self->setStatus(tr("Finding the passages that matter…"));
                    self->m_index.search(bookId, passages, asked, 8, answer);
                });
            return;
        }
        m_index.search(bookId, passages, asked, 8, answer);
    });
}

// --- Previously…, Who's who --------------------------------------------------

void Companion::recap() {
    begin(QStringLiteral("recap"), m_reader->chapterTitle());
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud) {
        const double cost = askEstimate().value(QStringLiteral("cost")).toDouble();
        if (!m_ai->fitsBudget(cost)) {
            fail(m_ai->describe(int(Ai::Error::Budget)));
            return;
        }
    }
    const int generation = m_generation;
    setStatus(tr("Reading the book so far…"));
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), cloud ? cloudLimit : localLimit,
              [this, generation](const QList<Chapter> &chapters) {
                  if (generation != m_generation)
                      return;
                  const bool isCloud = cloudFor(QStringLiteral("recap"));
                  QString titles;
                  for (const Chapter &chapter : chapters)
                      if (!chapter.title.isEmpty())
                          titles += QStringLiteral("- ") + chapter.title + QLatin1Char('\n');
                  const QString text = joined(chapters, isCloud ? cloudLimit : localLimit);
                  if (!checkBudget(QStringLiteral("recap"), text.size() + titles.size(), false, 500))
                      return;
                  Ai::TextRequest request;
                  request.feature = QStringLiteral("recap");
                  request.stream = true;
                  request.system = instructions(QStringLiteral(
                      "Write \"Previously…\" for a reader coming back to this book: the story so far in a few short "
                      "paragraphs, most about the latest events, ending where they stopped. Name the people who matter now."));
                  request.messages = {{true, bookLine() + QStringLiteral("\n\nChapters so far:\n") + titles + QStringLiteral("\n") +
                                                 text}};
                  run(QStringLiteral("recap"), request);
              });
}

void Companion::refreshWhosWho() {
    m_peopleKey.clear();
    m_peopleWhere.clear();
    whosWho();
}

void Companion::whosWho() {
    const QString where = m_ai->whereFor(QStringLiteral("whoswho"));
    const QString key = QStringLiteral("%1:%2:%3:%4")
                            .arg(m_reader->bookId())
                            .arg(m_reader->chapter())
                            .arg(m_reader->pageEnd())
                            .arg(where);
    begin(QStringLiteral("whoswho"), m_reader->chapterTitle());
    if (key == m_peopleKey && !m_people.isEmpty()) {
        m_where = m_peopleWhere.isEmpty() ? where : m_peopleWhere;
        m_busy = false;
        emit changed();
        return;
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud) {
        const double cost = askEstimate().value(QStringLiteral("cost")).toDouble();
        if (!m_ai->fitsBudget(cost)) {
            fail(m_ai->describe(int(Ai::Error::Budget)));
            return;
        }
    }
    const int generation = m_generation;
    setStatus(tr("Reading the book so far…"));
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), cloud ? cloudLimit : localWhosWhoLimit,
              [this, key, generation](const QList<Chapter> &chapters) {
                  if (generation != m_generation)
                      return;
                  const bool isCloud = cloudFor(QStringLiteral("whoswho"));
                  const QString text = joined(chapters, isCloud ? cloudLimit : localWhosWhoLimit);
                  if (!checkBudget(QStringLiteral("whoswho"), text.size(), false, 800))
                      return;
                  const QJsonObject person{{"type", "object"},
                                           {"properties", QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                                                                      {"also", QJsonObject{{"type", "string"}}},
                                                                      {"who", QJsonObject{{"type", "string"}}},
                                                                      {"chapter", QJsonObject{{"type", "string"}}}}},
                                           {"required", QJsonArray{"name", "also", "who", "chapter"}},
                                           {"additionalProperties", false}};
                  Ai::TextRequest request;
                  request.feature = QStringLiteral("whoswho");
                  // Use full context window on local models (32K on Ollama) so 16K
                  // chars plus JSON schema never truncate on non-Latin scripts.
                  request.fast = false;
                  request.schema = QJsonObject{{"type", "object"},
                                               {"properties", QJsonObject{{"people", QJsonObject{{"type", "array"}, {"items", person}}}}},
                                               {"required", QJsonArray{"people"}},
                                               {"additionalProperties", false}};
                  request.system = instructions(QStringLiteral(
                      "List the people (and named animals or beings) in the book so far, most important first, at most 30: "
                      "\"name\" as most often called, \"also\" other names or titles (or \"\"), \"who\" one sentence on who "
                      "they are and their part so far, \"chapter\" where they first appear."));
                  request.messages = {{true, bookLine() + QStringLiteral("\n\n") + text}};
                  run(QStringLiteral("whoswho"), request, [this, key, generation](Ai::Reply *reply) {
                      if (generation != m_generation)
                          return;
                      QJsonObject result = reply->result();
                      if (result.isEmpty()) {
                          QJsonParseError err;
                          const QJsonDocument doc = QJsonDocument::fromJson(reply->text().toUtf8(), &err);
                          if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                              fail(tr("Who's who couldn't be read from the model's answer. Try again."));
                              return;
                          }
                          result = doc.object();
                      }
                      if (!result.contains(QStringLiteral("people")) || !result.value(QStringLiteral("people")).isArray()) {
                          fail(tr("Who's who couldn't be read from the model's answer. Try again."));
                          return;
                      }
                      m_people.clear();
                      // Names as the book prints them, without its note markers ("Filby*").
                      static const QRegularExpression marker(QStringLiteral("[*†‡§¹²³⁴⁵⁶⁷⁸⁹⁰]+(?=\\s|$)"));
                      for (const QJsonValue &value : result.value(QStringLiteral("people")).toArray()) {
                          QVariantMap person = value.toObject().toVariantMap();
                          for (const char *field : {"name", "also"})
                              person[QLatin1String(field)] = person.value(QLatin1String(field)).toString().remove(marker).trimmed();
                          if (!person.value(QStringLiteral("name")).toString().isEmpty())
                              m_people.append(person);
                      }
                      m_peopleKey = key;
                      m_peopleWhere = m_where;
                      m_answer.clear();
                  });
              });
}
