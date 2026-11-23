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

namespace {

// Gemini reads up to this much of the book so far (about 300,000 tokens);
// a model on this computer, a chapter's worth.
constexpr qint64 cloudLimit = 1200000;
constexpr qint64 localLimit = 24000;

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
        m_answer.clear();
        m_people.clear();
        m_peopleKey.clear();
        m_conversation.clear();
        m_problem.clear();
        emit changed();
        emit answerChanged();
    });
}

QVariantList Companion::conversation() const {
    QVariantList out;
    for (const Ai::Message &message : m_conversation)
        out.append(QVariantMap{{QStringLiteral("fromUser"), message.fromUser}, {QStringLiteral("text"), message.text}});
    if (m_feature == QLatin1String("ask") && m_busy && !m_answer.isEmpty())
        out.append(QVariantMap{{QStringLiteral("fromUser"), false}, {QStringLiteral("text"), m_answer}});
    return out;
}

QString Companion::readerLanguage() {
    return QLocale::languageToString(QLocale().language());
}

QList<Companion::Chapter> Companion::textUpTo(const QString &path, const QString &format, int chapter, qint64 offset) {
    QList<Chapter> out;
    if (format == QLatin1String("pdf")) {
        PdfBook book(path);
        if (!book.open())
            return out;
        for (int page = 0; page <= chapter && page < book.pageCount(); ++page) {
            QString text = book.document()->getAllText(page).text();
            if (page == chapter && offset >= 0)
                text = text.left(offset);
            out.append({page, QCoreApplication::translate("Companion", "Page %1").arg(page + 1), text});
        }
        return out;
    }
    EpubBook book(path);
    if (!book.open())
        return out;
    const int count = int(book.spine().size());
    for (int i = 0; i <= chapter && i < count; ++i) {
        QString text = book.chapterText(i);
        if (i == chapter && offset >= 0)
            text = text.left(offset);
        if (!text.trimmed().isEmpty())
            out.append({i, QString(), text});
    }
    return out;
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
    if (QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty())
        return {};
    QProcess sdcv;
    sdcv.start(QStringLiteral("sdcv"), {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), word});
    if (!sdcv.waitForFinished(4000) || sdcv.exitCode() != 0)
        return {};
    QStringList out;
    for (const QJsonValue &entry : QJsonDocument::fromJson(sdcv.readAllStandardOutput()).array()) {
        const QJsonObject object = entry.toObject();
        if (object.value(QStringLiteral("word")).toString().compare(word, Qt::CaseInsensitive) == 0 || out.isEmpty())
            out.append(object.value(QStringLiteral("definition")).toString().trimmed() + QStringLiteral("\n— ") +
                       object.value(QStringLiteral("dict")).toString());
        if (out.size() >= 2)
            break;
    }
    return out;
}

QString Companion::bookLine() const {
    return QStringLiteral("Book: %1, by %2.").arg(m_reader->title(), m_reader->author());
}

bool Companion::cloudFor(const QString &feature) const {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    return provider && !provider->isLocal();
}

Ai::TextModel *Companion::modelFor(const QString &feature) {
    Ai::TextModel *model = m_ai->textFor(feature);
    if (!model)
        fail(m_ai->preset() == QLatin1String("local")
                 ? tr("This needs a model on this computer: set one up in Intelligence (✦).")
                 : tr("This needs a Gemini key or a model on this computer, in Intelligence (✦)."));
    return model;
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

void Companion::fail(const QString &problem) {
    m_problem = problem;
    m_busy = false;
    emit changed();
}

void Companion::stop() {
    ++m_generation;
    m_index.cancel();
    if (m_reply)
        m_reply->abort();
    if (m_busy) {
        m_busy = false;
        emit changed();
    }
}

void Companion::readSoFar(int chapter, qint64 offset, std::function<void(const QList<Chapter> &)> then) {
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int generation = m_generation;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, path, format, chapter, offset, generation, then] {
        const QList<Chapter> chapters = textUpTo(path, format, chapter, offset);
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

void Companion::run(Ai::TextModel *model, const Ai::TextRequest &request, std::function<void(Ai::Reply *)> done) {
    Ai::Reply *reply = model->generate(request);
    m_ai->track(reply);
    m_reply = reply;
    setStatus(tr("Writing…"));
    connect(reply, &Ai::Reply::textReceived, this, [this, reply](const QString &delta) {
        if (reply != m_reply)
            return;
        m_answer += delta;
        emit answerChanged();
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, done] {
        reply->deleteLater();
        if (reply != m_reply)
            return;
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                fail(m_ai->describe(int(reply->error().kind), reply->error().message));
            return;
        }
        if (m_answer.isEmpty() && reply->result().isEmpty())
            m_answer = reply->text();
        if (done)
            done(reply);
        m_busy = false;
        emit answerChanged();
        emit changed();
    });
}

// --- Define, explain, translate ----------------------------------------------

void Companion::define(const QString &word, int chapter, qint64 offset) {
    const QString wanted = word.trimmed();
    begin(QStringLiteral("define"), wanted);
    // A dictionary on this computer answers first, and costs nothing.
    const QStringList entries = wanted.contains(QLatin1Char(' ')) ? QStringList() : dictionary(wanted);
    if (!entries.isEmpty()) {
        m_answer = entries.join(QStringLiteral("\n\n"));
        m_where = tr("From your dictionary");
        m_busy = false;
        emit answerChanged();
        emit changed();
        return;
    }
    Ai::TextModel *model = modelFor(m_feature);
    if (!model)
        return;
    setStatus(tr("Reading the sentence…"));
    readSoFar(chapter, offset + 400, [this, model, wanted](const QList<Chapter> &chapters) {
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
        run(model, request);
    });
}

void Companion::explain(const QString &text, int chapter, qint64 end) {
    begin(QStringLiteral("explain"), text.trimmed());
    Ai::TextModel *model = modelFor(m_feature);
    if (!model)
        return;
    setStatus(tr("Reading up to here…"));
    readSoFar(chapter, end, [this, model, text](const QList<Chapter> &chapters) {
        const QString context = joined(chapters, cloudFor(m_feature) ? 60000 : 12000);
        Ai::TextRequest request;
        request.feature = QStringLiteral("explain");
        request.stream = true;
        request.system = instructions(QStringLiteral(
            "Explain the selected words to a reader in the middle of this book: what they mean, what is happening, "
            "and any allusion, history, idiom or old usage a modern reader might miss. Use the text before it for "
            "context. Be brief: one to three short paragraphs."));
        request.messages = {{true, bookLine() + QStringLiteral("\n\nThe book up to the selection:\n") + context +
                                       QStringLiteral("\n\nSelected: \"") + text.trimmed() + QStringLiteral("\"")}};
        run(model, request);
    });
}

void Companion::translate(const QString &text, int chapter, qint64 end) {
    begin(QStringLiteral("translate"), text.trimmed());
    Ai::TextModel *model = modelFor(m_feature);
    if (!model)
        return;
    // Into the reader's language, or English when the book is already in it.
    QString target = readerLanguage();
    if (QLocale(m_reader->language()).language() == QLocale().language())
        target = target == QLatin1String("English") ? QStringLiteral("Spanish") : QStringLiteral("English");
    readSoFar(chapter, end, [this, model, text, target](const QList<Chapter> &chapters) {
        Ai::TextRequest request;
        request.feature = QStringLiteral("translate");
        request.fast = true;
        request.stream = true;
        request.system = QStringLiteral(
                             "Translate the selected words into %1, faithfully and in a natural literary style, "
                             "using the passage before them for context. Give only the translation.")
                             .arg(target);
        request.messages = {{true, QStringLiteral("Context:\n") + joined(chapters, 2000) + QStringLiteral("\n\nTranslate:\n") +
                                       text.trimmed()}};
        run(model, request);
    });
}

// --- Ask ---------------------------------------------------------------------

QVariantMap Companion::askEstimate() const {
    const bool cloud = cloudFor(QStringLiteral("ask"));
    // Characters so far, from the reader's progress through the book.
    qint64 soFar = m_reader->pageEnd();
    for (int i = 0; i < m_reader->chapter(); ++i)
        soFar += m_reader->charactersIn(i);
    const double characters = qMin<double>(cloudLimit, soFar);
    double cost = 0;
    if (cloud) {
        if (Ai::TextModel *model = m_ai->textFor(QStringLiteral("ask"))) {
            Ai::Usage usage;
            usage.inputTokens = qint64(characters / 4);
            usage.outputTokens = 400;
            cost = AiUsageLedger::cost(model->textModelName(false), usage, QDate::currentDate(), false);
        }
    }
    return {{QStringLiteral("where"), m_ai->whereFor(QStringLiteral("ask"))},
            {QStringLiteral("words"), qRound(characters / 6)},
            {QStringLiteral("cost"), cost},
            {QStringLiteral("local"), !cloud}};
}

void Companion::newConversation() {
    stop();
    m_conversation.clear();
    m_answer.clear();
    m_feature = QStringLiteral("ask");
    m_problem.clear();
    emit changed();
    emit answerChanged();
}

void Companion::ask(const QString &question) {
    const QString asked = question.trimmed();
    if (asked.isEmpty())
        return;
    if (m_feature != QLatin1String("ask"))
        m_conversation.clear();
    begin(QStringLiteral("ask"), asked);
    Ai::TextModel *model = modelFor(m_feature);
    if (!model)
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
    emit answerChanged();
    setStatus(cloud ? tr("Reading the book so far…") : tr("Finding the passages that matter…"));
    const int bookId = m_reader->bookId();
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), [this, model, cloud, asked, bookId](const QList<Chapter> &chapters) {
        const auto send = [this, model, asked](const QString &context, const QString &how) {
            Ai::TextRequest request;
            request.feature = QStringLiteral("ask");
            request.stream = true;
            request.system = instructions(QStringLiteral(
                "Answer the reader's questions about the book they are reading, from its text below (%1). Quote "
                "briefly when it helps, and say which chapter. If the text doesn't say, say so.").arg(how));
            request.messages = {{true, bookLine() + QStringLiteral("\n\n") + context}};
            request.messages.append({false, QStringLiteral("I've read it. What would you like to know?")});
            // The conversation so far, then the question.
            request.messages += m_conversation;
            run(model, request, [this](Ai::Reply *) { m_conversation.append({false, m_answer}); });
        };
        if (cloud) {
            send(joined(chapters, cloudLimit), QStringLiteral("the whole book up to the reader's place"));
            return;
        }
        // On this computer: the passages closest to the question, and the
        // pages just read.
        QList<PassageIndex::Passage> passages;
        for (const Chapter &chapter : chapters)
            passages += PassageIndex::split(chapter.index, chapter.text);
        const auto answer = [this, send, chapters](const QList<PassageIndex::Hit> &hits, bool) {
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
            m_index.build(bookId, passages,
                          [this](double share) { setStatus(tr("Getting to know the book… %1%").arg(qRound(share * 100))); },
                          [this, bookId, passages, asked, answer](const QString &) {
                              setStatus(tr("Finding the passages that matter…"));
                              m_index.search(bookId, passages, asked, 8, answer);
                          });
            return;
        }
        m_index.search(bookId, passages, asked, 8, answer);
    });
}

// --- Previously…, Who's who --------------------------------------------------

void Companion::recap() {
    begin(QStringLiteral("recap"), m_reader->chapterTitle());
    Ai::TextModel *model = modelFor(m_feature);
    if (!model)
        return;
    setStatus(tr("Reading the book so far…"));
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), [this, model](const QList<Chapter> &chapters) {
        const bool cloud = cloudFor(m_feature);
        QString titles;
        for (const Chapter &chapter : chapters)
            if (!chapter.title.isEmpty())
                titles += QStringLiteral("- ") + chapter.title + QLatin1Char('\n');
        Ai::TextRequest request;
        request.feature = QStringLiteral("recap");
        request.stream = true;
        request.system = instructions(QStringLiteral(
            "Write \"Previously…\" for a reader coming back to this book: the story so far in a few short "
            "paragraphs, most about the latest events, ending where they stopped. Name the people who matter now."));
        request.messages = {{true, bookLine() + QStringLiteral("\n\nChapters so far:\n") + titles + QStringLiteral("\n") +
                                       joined(chapters, cloud ? cloudLimit : localLimit)}};
        run(model, request);
    });
}

void Companion::whosWho() {
    const QString key = QStringLiteral("%1:%2").arg(m_reader->bookId()).arg(m_reader->chapter());
    begin(QStringLiteral("whoswho"), m_reader->chapterTitle());
    if (key == m_peopleKey && !m_people.isEmpty()) {
        m_busy = false;
        emit changed();
        return;
    }
    Ai::TextModel *model = modelFor(m_feature);
    if (!model)
        return;
    setStatus(tr("Reading the book so far…"));
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), [this, model, key](const QList<Chapter> &chapters) {
        const bool cloud = cloudFor(m_feature);
        const QJsonObject person{{"type", "object"},
                                 {"properties", QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                                                            {"also", QJsonObject{{"type", "string"}}},
                                                            {"who", QJsonObject{{"type", "string"}}},
                                                            {"chapter", QJsonObject{{"type", "string"}}}}},
                                 {"required", QJsonArray{"name", "also", "who", "chapter"}},
                                 {"additionalProperties", false}};
        Ai::TextRequest request;
        request.feature = QStringLiteral("whoswho");
        request.fast = !cloud;
        request.schema = QJsonObject{{"type", "object"},
                                     {"properties", QJsonObject{{"people", QJsonObject{{"type", "array"}, {"items", person}}}}},
                                     {"required", QJsonArray{"people"}},
                                     {"additionalProperties", false}};
        request.system = instructions(QStringLiteral(
            "List the people (and named animals or beings) in the book so far, most important first, at most 30: "
            "\"name\" as most often called, \"also\" other names or titles (or \"\"), \"who\" one sentence on who "
            "they are and their part so far, \"chapter\" where they first appear."));
        request.messages = {{true, bookLine() + QStringLiteral("\n\n") + joined(chapters, cloud ? cloudLimit : localLimit)}};
        run(model, request, [this, key](Ai::Reply *reply) {
            QJsonObject result = reply->result();
            if (result.isEmpty())
                result = QJsonDocument::fromJson(reply->text().toUtf8()).object();
            m_people.clear();
            // Names as the book prints them, without its note markers ("Filby*").
            static const QRegularExpression marker(QStringLiteral("[*†‡§¹²³⁴⁵⁶⁷⁸⁹⁰]+(?=\\s|$)"));
            for (const QJsonValue &value : result.value(QStringLiteral("people")).toArray()) {
                QVariantMap person = value.toObject().toVariantMap();
                for (const char *field : {"name", "also"})
                    person[QLatin1String(field)] = person.value(QLatin1String(field)).toString().remove(marker).trimmed();
                m_people.append(person);
            }
            m_peopleKey = key;
            m_answer.clear();
        });
    });
}
