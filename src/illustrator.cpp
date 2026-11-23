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
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>

namespace {
constexpr qint64 maxImageBytes = 20 * 1024 * 1024;

QSettings illustratorSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

QString normalizeStyle(const QString &style) {
    const QString s = style.trimmed().toLower();
    if (s == QLatin1String("engraving") || s == QLatin1String("watercolor") || s == QLatin1String("ink") ||
        s == QLatin1String("editorial"))
        return s;
    return QStringLiteral("auto");
}

QString labelForStyle(const QString &styleId) {
    if (styleId == QLatin1String("engraving"))
        return QCoreApplication::translate("Illustrator", "Engraving");
    if (styleId == QLatin1String("watercolor"))
        return QCoreApplication::translate("Illustrator", "Watercolor");
    if (styleId == QLatin1String("ink"))
        return QCoreApplication::translate("Illustrator", "Ink sketch");
    if (styleId == QLatin1String("editorial"))
        return QCoreApplication::translate("Illustrator", "Editorial");
    return QCoreApplication::translate("Illustrator", "Book's time");
}

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
    m_style = normalizeStyle(illustratorSettings().value(QStringLiteral("illustrator/style"), QStringLiteral("auto")).toString());
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS illustrations (id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL, "
                              "chapter INTEGER NOT NULL, offset INTEGER NOT NULL, caption TEXT NOT NULL, prompt TEXT NOT NULL, "
                              "file TEXT NOT NULL, created INTEGER NOT NULL, style TEXT NOT NULL DEFAULT 'auto', "
                              "passage TEXT NOT NULL DEFAULT '')"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS illustrations_book ON illustrations (book_id)"));
    bool hasStyle = false, hasPassage = false;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(illustrations)"))) {
        while (info.next()) {
            const QString col = info.value(1).toString();
            if (col == QLatin1String("style"))
                hasStyle = true;
            else if (col == QLatin1String("passage"))
                hasPassage = true;
        }
    }
    if (!hasStyle)
        query.exec(QStringLiteral("ALTER TABLE illustrations ADD COLUMN style TEXT NOT NULL DEFAULT 'auto'"));
    if (!hasPassage)
        query.exec(QStringLiteral("ALTER TABLE illustrations ADD COLUMN passage TEXT NOT NULL DEFAULT ''"));
    connect(reader, &Reader::bookChanged, this, [this] {
        stop();
        m_problem.clear();
        emit changed();
        emit picturesChanged();
    });
    connect(ai, &AiServices::changed, this, &Illustrator::changed);
    connect(ai, &AiServices::routingChanged, this, &Illustrator::changed);
}

bool Illustrator::available() const { return m_ai->images() && m_ai->textFor(QStringLiteral("illustrate")); }

double Illustrator::cost() const { return m_ai->images() ? m_ai->images()->imageCost() + 0.002 : 0; }

QString Illustrator::where() const { return tr("Gemini · sent to Google"); }

void Illustrator::setStyle(const QString &style) {
    const QString normalized = normalizeStyle(style);
    if (normalized == m_style)
        return;
    m_style = normalized;
    illustratorSettings().setValue(QStringLiteral("illustrator/style"), m_style);
    emit styleChanged();
}

QVariantList Illustrator::styles() const {
    return {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("auto")}, {QStringLiteral("label"), labelForStyle(QStringLiteral("auto"))}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("engraving")}, {QStringLiteral("label"), labelForStyle(QStringLiteral("engraving"))}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("watercolor")}, {QStringLiteral("label"), labelForStyle(QStringLiteral("watercolor"))}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("ink")}, {QStringLiteral("label"), labelForStyle(QStringLiteral("ink"))}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("editorial")}, {QStringLiteral("label"), labelForStyle(QStringLiteral("editorial"))}},
    };
}

QVariantList Illustrator::pictures() const {
    QVariantList out;
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT id, file, caption, chapter, offset, style FROM illustrations WHERE book_id = ? ORDER BY created DESC, id DESC"));
    query.addBindValue(m_reader->bookId());
    query.exec();
    while (query.next()) {
        const QString file = query.value(1).toString();
        const QString styleId = normalizeStyle(query.value(5).toString());
        out.append(QVariantMap{{"id", query.value(0)},
                               {"url", QUrl::fromLocalFile(file)},
                               {"caption", query.value(2)},
                               {"chapter", query.value(3)},
                               {"offset", query.value(4)},
                               {"style", styleId},
                               {"styleLabel", labelForStyle(styleId)},
                               {"where", where()},
                               {"exists", !file.isEmpty() && QFileInfo::exists(file)}});
    }
    return out;
}

QString Illustrator::styleGuidance(const QString &styleId) {
    const QString s = normalizeStyle(styleId);
    if (s == QLatin1String("engraving"))
        return QStringLiteral(
            "Classical copperplate or woodcut book engraving with fine cross-hatching, rich tonal contrast, "
            "and detailed line work on warm archival paper.");
    if (s == QLatin1String("watercolor"))
        return QStringLiteral(
            "Luminous literary watercolor illustration with delicate ink-and-wash outlines, soft atmospheric light, "
            "and subtle cold-press paper texture.");
    if (s == QLatin1String("ink"))
        return QStringLiteral(
            "Expressive pen-and-ink line sketch with crisp dip-pen strokes, dramatic chiaroscuro shading, "
            "and hand-drawn literary character.");
    if (s == QLatin1String("editorial"))
        return QStringLiteral(
            "Refined contemporary editorial book illustration with painterly brushwork, evocative muted palette, "
            "and cinematic composition.");
    return {};
}

QString Illustrator::promptInstructions() { return promptInstructions(QStringLiteral("auto")); }

QString Illustrator::promptInstructions(const QString &styleId) {
    QString base = QStringLiteral(
        "You commission illustrations for books. From the passage the reader has reached, choose the moment it "
        "shows and describe one illustration of it for an image model: who and what is there, where, the light, "
        "the composition, and an art style fitting the book's own time and kind (for a Victorian novel, say, a "
        "detailed pen-and-ink engraving; for a modern thriller, a moody painted cover). Be faithful to what the "
        "text says people and places look like. Show only what the passage and what came before it establish; "
        "never anything later in the book. The picture must contain no words, letters, captions or signatures.\n"
        "Answer as JSON: \"prompt\" (the description, 60 to 140 words) and \"caption\" (a short line for under the "
        "picture, quoting the passage when a phrase fits).");
    const QString guidance = styleGuidance(styleId);
    if (!guidance.isEmpty())
        base += QStringLiteral("\nRequired art style: ") + guidance;
    return base;
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

void Illustrator::regenerate(int id) {
    QSqlQuery find(QSqlDatabase::database(m_connection));
    find.prepare(QStringLiteral("SELECT chapter, offset, passage FROM illustrations WHERE id = ? AND book_id = ?"));
    find.addBindValue(id);
    find.addBindValue(m_reader->bookId());
    if (!find.exec() || !find.next())
        return;
    const int chapter = find.value(0).toInt();
    const qint64 offset = find.value(1).toLongLong();
    const QString passage = find.value(2).toString();
    illustrate(passage, chapter, offset);
}

void Illustrator::illustrate(const QString &text, int chapter, qint64 end) {
    stop();
    m_problem.clear();
    if (!m_ai->images() || !m_ai->textFor(QStringLiteral("illustrate"))) {
        fail(m_ai->hasKey() ? tr("Pictures need Gemini: switch to Hybrid or Cloud in Intelligence settings.")
                            : tr("Pictures are made with Gemini: add your key in Intelligence settings."));
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
    const QString styleId = m_style;
    const int generation = m_generation;
    qint64 upTo = end >= 0 ? end : m_reader->pageEnd();
    if (chapter == m_reader->chapter() && m_reader->pageEnd() > 0)
        upTo = qMin(upTo, m_reader->pageEnd());
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Illustrator> self(this);
    QThreadPool::globalInstance()->start([self, path, format, chapter, upTo, text, title, author, styleId, cancel, generation] {
        const QList<Companion::Chapter> chapters = Companion::textUpTo(path, format, chapter, upTo, 16000, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, chapter, upTo, text, title, author, styleId, generation] {
                if (!self || generation != self->m_generation)
                    return;
                Ai::Provider *writerProvider = self->m_ai->textProviderFor(QStringLiteral("illustrate"));
                Ai::TextModel *writer = writerProvider ? writerProvider->text() : nullptr;
                if (!self->m_ai->images() || !writer) {
                    self->fail(self->m_ai->hasKey()
                                   ? tr("Pictures need Gemini: switch to Hybrid or Cloud in Intelligence settings.")
                                   : tr("Pictures are made with Gemini: add your key in Intelligence settings."));
                    return;
                }
                Ai::TextRequest request;
                request.feature = QStringLiteral("illustrate");
                request.fast = true;
                request.system = promptInstructions(styleId);
                request.schema = QJsonObject{{"type", "object"},
                                             {"properties", QJsonObject{{"prompt", QJsonObject{{"type", "string"}}},
                                                                        {"caption", QJsonObject{{"type", "string"}}}}},
                                             {"required", QJsonArray{"prompt", "caption"}},
                                             {"additionalProperties", false}};
                const QString selectedText = text.trimmed();
                QString passage = selectedText;
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
                connect(reply, &Ai::Reply::finished, self,
                        [self, reply, chapter, upTo, styleId, selectedText, generation, providerGuard] {
                            reply->deleteLater();
                            if (!self || generation != self->m_generation)
                                return;
                            if (reply->error()) {
                                if (reply->error().kind != Ai::Error::Cancelled)
                                    self->fail(self->m_ai->describeFor(providerGuard, int(reply->error().kind),
                                                                       reply->error().message));
                                return;
                            }
                            QJsonObject result = reply->result();
                            if (result.isEmpty())
                                result = QJsonDocument::fromJson(reply->text().toUtf8()).object();
                            QString prompt = result.value("prompt").toString();
                            if (prompt.isEmpty()) {
                                self->fail(tr("The picture couldn't be described. Try again, or choose a passage."));
                                return;
                            }
                            const QString guidance = styleGuidance(styleId);
                            if (!guidance.isEmpty() && !prompt.contains(labelForStyle(styleId), Qt::CaseInsensitive))
                                prompt += QStringLiteral(" ") + guidance;
                            self->draw(prompt, result.value("caption").toString(), chapter, upTo, styleId, selectedText);
                        });
            },
            Qt::QueuedConnection);
    });
}

void Illustrator::draw(const QString &prompt, const QString &caption, int chapter, qint64 offset,
                       const QString &styleId, const QString &passage) {
    Ai::ImageGenerator *images = m_ai->images();
    if (!images) {
        fail(tr("Pictures are made with Gemini: add your key in Intelligence settings."));
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
    connect(reply, &Ai::Reply::finished, this,
            [this, reply, prompt, caption, chapter, offset, styleId, passage, generation, bookId] {
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
                query.prepare(QStringLiteral(
                    "INSERT INTO illustrations (book_id, chapter, offset, caption, prompt, file, created, style, passage) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
                query.addBindValue(bookId);
                query.addBindValue(chapter);
                query.addBindValue(offset);
                query.addBindValue(caption);
                query.addBindValue(prompt);
                query.addBindValue(file);
                query.addBindValue(QDateTime::currentSecsSinceEpoch());
                query.addBindValue(normalizeStyle(styleId));
                query.addBindValue(passage);
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
