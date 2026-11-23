#include "illustrator.h"

#include "aiservices.h"
#include "companion.h"
#include "reader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThreadPool>
#include <QRegularExpression>
#include <QUrl>

namespace {
constexpr qint64 maxImageBytes = 20 * 1024 * 1024;

QString extensionFor(const QByteArray &format, const QString &mime) {
    const QByteArray f = format.toLower();
    if (f == "jpeg" || f == "jpg")
        return QStringLiteral("jpg");
    if (f == "webp")
        return QStringLiteral("webp");
    if (f == "png")
        return QStringLiteral("png");
    const QString m = mime.toLower();
    if (m.contains(QLatin1String("jpeg")) || m.contains(QLatin1String("jpg")))
        return QStringLiteral("jpg");
    if (m.contains(QLatin1String("webp")))
        return QStringLiteral("webp");
    return QStringLiteral("png");
}
}  // namespace

Illustrator::Illustrator(AiServices *ai, Reader *reader, const QString &databaseConnection, const QString &folder,
                         QObject *parent)
    : QObject(parent), m_ai(ai), m_reader(reader), m_connection(databaseConnection), m_folder(folder) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS illustrations (id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, "
                              "chapter INTEGER NOT NULL, offset INTEGER NOT NULL, caption TEXT NOT NULL, prompt TEXT NOT NULL, "
                              "file TEXT NOT NULL, created INTEGER NOT NULL)"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS illustrations_book ON illustrations (book_id)"));
    connect(reader, &Reader::bookChanged, this, [this] {
        stop();
        m_problem.clear();
        emit changed();
        emit picturesChanged();
    });
    connect(ai, &AiServices::changed, this, &Illustrator::changed);
}

bool Illustrator::available() const { return m_ai->images() && m_ai->textFor(QStringLiteral("illustrate")); }

double Illustrator::cost() const { return m_ai->images() ? m_ai->images()->imageCost() + 0.002 : 0; }

QVariantList Illustrator::pictures() const {
    QVariantList out;
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT id, file, caption, chapter, offset FROM illustrations WHERE book_id = ? ORDER BY created DESC, id DESC"));
    query.addBindValue(m_reader->bookId());
    query.exec();
    while (query.next())
        out.append(QVariantMap{{"id", query.value(0)},
                               {"url", QUrl::fromLocalFile(query.value(1).toString())},
                               {"caption", query.value(2)},
                               {"chapter", query.value(3)},
                               {"offset", query.value(4)}});
    return out;
}

QString Illustrator::promptInstructions() {
    return QStringLiteral(
        "You commission illustrations for books. From the passage the reader has reached, choose the moment it "
        "shows and describe one illustration of it for an image model: who and what is there, where, the light, "
        "the composition, and an art style fitting the book's own time and kind (for a Victorian novel, say, a "
        "detailed pen-and-ink engraving; for a modern thriller, a moody painted cover). Be faithful to what the "
        "text says people and places look like. Show only what the passage and what came before it establish; "
        "never anything later in the book. The picture must contain no words, letters, captions or signatures.\n"
        "Answer as JSON: \"prompt\" (the description, 60 to 140 words) and \"caption\" (a short line for under the "
        "picture, quoting the passage when a phrase fits).");
}

void Illustrator::fail(const QString &problem) {
    m_problem = problem;
    m_busy = false;
    emit changed();
}

void Illustrator::stop() {
    ++m_generation;
    if (m_readCancel) {
        *m_readCancel = true;
        m_readCancel.reset();
    }
    if (m_reply)
        m_reply->abort();
    if (m_busy) {
        m_busy = false;
        emit changed();
    }
}

void Illustrator::illustrate(const QString &text, int chapter, qint64 end) {
    stop();
    m_problem.clear();
    if (!m_ai->images() || !m_ai->textFor(QStringLiteral("illustrate"))) {
        fail(tr("Pictures are made with Gemini: add your key in Intelligence (✦)."));
        return;
    }
    if (!m_ai->fitsBudget(cost())) {
        fail(m_ai->describe(int(Ai::Error::Budget)));
        return;
    }
    m_busy = true;
    m_status = tr("Reading the passage…");
    emit changed();
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const QString title = m_reader->title(), author = m_reader->author();
    const int generation = m_generation;
    qint64 upTo = end >= 0 ? end : m_reader->pageEnd();
    if (chapter == m_reader->chapter() && m_reader->pageEnd() > 0)
        upTo = qMin(upTo, m_reader->pageEnd());
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Illustrator> self(this);
    QThreadPool::globalInstance()->start([self, path, format, chapter, upTo, text, title, author, cancel, generation] {
        const QList<Companion::Chapter> chapters = Companion::textUpTo(path, format, chapter, upTo, 16000, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, chapter, upTo, text, title, author, generation] {
                if (!self || generation != self->m_generation)
                    return;
                Ai::Provider *writerProvider = self->m_ai->textProviderFor(QStringLiteral("illustrate"));
                Ai::TextModel *writer = writerProvider ? writerProvider->text() : nullptr;
                if (!self->m_ai->images() || !writer) {
                    self->fail(tr("Pictures are made with Gemini: add your key in Intelligence (✦)."));
                    return;
                }
                Ai::TextRequest request;
                request.feature = QStringLiteral("illustrate");
                request.fast = true;
                request.system = promptInstructions();
                request.schema = QJsonObject{{"type", "object"},
                                             {"properties", QJsonObject{{"prompt", QJsonObject{{"type", "string"}}},
                                                                        {"caption", QJsonObject{{"type", "string"}}}}},
                                             {"required", QJsonArray{"prompt", "caption"}},
                                             {"additionalProperties", false}};
                QString passage = text.trimmed();
                if (passage.isEmpty() && !chapters.isEmpty())
                    passage = chapters.last().text.right(2500);
                request.messages = {{true, QStringLiteral("Book: %1, by %2.\n\nWhat came before (for who is who):\n%3\n\nThe passage to illustrate:\n%4")
                                               .arg(title, author, Companion::joined(chapters, 12000), passage)}};
                self->m_status = tr("Imagining the picture…");
                emit self->changed();
                Ai::Reply *reply = writer->generate(request);
                self->m_ai->track(reply);
                self->m_reply = reply;
                QPointer<Ai::Provider> providerGuard(writerProvider);
                connect(reply, &Ai::Reply::finished, self, [self, reply, chapter, upTo, generation, providerGuard] {
                    reply->deleteLater();
                    if (!self || generation != self->m_generation)
                        return;
                    if (reply->error()) {
                        if (reply->error().kind != Ai::Error::Cancelled)
                            self->fail(self->m_ai->describeFor(providerGuard, int(reply->error().kind), reply->error().message));
                        return;
                    }
                    QJsonObject result = reply->result();
                    if (result.isEmpty())
                        result = QJsonDocument::fromJson(reply->text().toUtf8()).object();
                    const QString prompt = result.value("prompt").toString();
                    if (prompt.isEmpty()) {
                        self->fail(tr("The picture couldn't be described. Try again, or choose a passage."));
                        return;
                    }
                    self->draw(prompt, result.value("caption").toString(), chapter, upTo);
                });
            },
            Qt::QueuedConnection);
    });
}

void Illustrator::draw(const QString &prompt, const QString &caption, int chapter, qint64 offset) {
    Ai::ImageGenerator *images = m_ai->images();
    if (!images) {
        fail(tr("Pictures are made with Gemini: add your key in Intelligence (✦)."));
        return;
    }
    m_status = tr("Drawing…");
    emit changed();
    Ai::ImageRequest request;
    request.prompt = prompt;
    Ai::Reply *reply = images->generateImage(request);
    m_ai->track(reply);
    m_reply = reply;
    const int generation = m_generation;
    const int bookId = m_reader->bookId();
    connect(reply, &Ai::Reply::finished, this, [this, reply, prompt, caption, chapter, offset, generation, bookId] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled)
                fail(reply->error().kind == Ai::Error::Refused
                         ? tr("Gemini wouldn't draw this one: %1").arg(reply->error().message)
                         : m_ai->describe(int(reply->error().kind), reply->error().message));
            return;
        }
        const QByteArray bytes = reply->image();
        if (bytes.isEmpty() || bytes.size() > maxImageBytes) {
            fail(tr("The picture couldn't be read. Try again."));
            return;
        }
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const QByteArray detectedFormat = reader.format();
        if (!reader.canRead() || reader.size().isEmpty()) {
            fail(tr("The picture couldn't be read. Try again."));
            return;
        }
        const QString ext = extensionFor(detectedFormat, reply->imageMimeType());
        QDir().mkpath(m_folder);
        const QString file = QDir(m_folder).filePath(QStringLiteral("%1-%2.%3")
                                                         .arg(bookId)
                                                         .arg(QDateTime::currentMSecsSinceEpoch())
                                                         .arg(ext));
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) {
            out.close();
            QFile::remove(file);
            fail(tr("The picture couldn't be kept: %1").arg(out.errorString()));
            return;
        }
        out.close();
        QSqlQuery query(QSqlDatabase::database(m_connection));
        query.prepare(QStringLiteral("INSERT INTO illustrations (book_id, chapter, offset, caption, prompt, file, created) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(bookId);
        query.addBindValue(chapter);
        query.addBindValue(offset);
        query.addBindValue(caption);
        query.addBindValue(prompt);
        query.addBindValue(file);
        query.addBindValue(QDateTime::currentSecsSinceEpoch());
        if (!query.exec()) {
            QFile::remove(file);
            fail(tr("The picture couldn't be kept in the library."));
            return;
        }
        m_busy = false;
        emit changed();
        emit picturesChanged();
        emit made(query.lastInsertId().toInt());
    });
}

void Illustrator::remove(int id) {
    QSqlQuery find(QSqlDatabase::database(m_connection));
    find.prepare(QStringLiteral("SELECT file FROM illustrations WHERE id = ?"));
    find.addBindValue(id);
    find.exec();
    if (find.next())
        QFile::remove(find.value(0).toString());
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("DELETE FROM illustrations WHERE id = ?"));
    query.addBindValue(id);
    query.exec();
    emit picturesChanged();
}

QString Illustrator::saveCopy(int id) {
    QSqlQuery find(QSqlDatabase::database(m_connection));
    find.prepare(QStringLiteral("SELECT file, caption FROM illustrations WHERE id = ?"));
    find.addBindValue(id);
    find.exec();
    if (!find.next())
        return {};
    const QString source = find.value(0).toString();
    const QString ext = QFileInfo(source).suffix().isEmpty() ? QStringLiteral("png") : QFileInfo(source).suffix().toLower();
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/Dogear");
    QDir().mkpath(folder);
    QString name = m_reader->title().left(60);
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral(" "));
    name = name.simplified();
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        name = QStringLiteral("Illustration");
    QString target = QDir(folder).filePath(QStringLiteral("%1.%2").arg(name, ext));
    for (int n = 2; QFile::exists(target); ++n)
        target = QDir(folder).filePath(QStringLiteral("%1 %2.%3").arg(name).arg(n).arg(ext));
    return QFile::copy(source, target) ? target : QString();
}
