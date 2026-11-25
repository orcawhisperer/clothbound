#include "ai/illustrator.h"

#include "ai/aiservices.h"
#include "ai/companion.h"
#include "reader/reader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>

namespace {
constexpr qint64 maxImageBytes = 20 * 1024 * 1024;

QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

QJsonObject extractJsonObject(const QString &raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return {};
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        return doc.object();
    static const QRegularExpression fence(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)\\s*```"),
                                          QRegularExpression::CaseInsensitiveOption);
    const auto match = fence.match(trimmed);
    if (match.hasMatch()) {
        doc = QJsonDocument::fromJson(match.captured(1).trimmed().toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    const qsizetype open = trimmed.indexOf(QLatin1Char('{'));
    const qsizetype close = trimmed.lastIndexOf(QLatin1Char('}'));
    if (open >= 0 && close > open) {
        doc = QJsonDocument::fromJson(trimmed.mid(open, close - open + 1).toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    return {};
}

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
        m_problemIsConfig = false;
        m_hasLastRequest = false;
        emit changed();
        emit picturesChanged();
    });
    connect(ai, &AiServices::changed, this, &Illustrator::changed);
    connect(ai, &AiServices::routingChanged, this, &Illustrator::changed);
    connect(ai, &AiServices::usageChanged, this, &Illustrator::changed);
}

Illustrator::~Illustrator() {
    clearPendingSave();
    stop();
}

bool Illustrator::available() const { return m_ai->images() && m_ai->textFor(QStringLiteral("illustrate")); }

bool Illustrator::budgetOk() const { return !available() || m_ai->fitsBudget(cost()); }

QString Illustrator::unavailableReason() const {
    if (!m_ai->hasKey())
        return tr("Pictures are made with Gemini: add your key in Intelligence settings.");
    if (!available())
        return tr("Pictures need Gemini: switch to Hybrid or Cloud in Intelligence settings.");
    if (!m_ai->fitsBudget(cost())) {
        if (!m_ai->unlimitedBudget() && m_ai->budget() <= 0)
            return tr("Cloud spending is paused ($0 monthly limit). Raise it in Intelligence settings to draw pictures.");
        return tr("This would go past your monthly budget. Raise it in Intelligence settings to draw pictures.");
    }
    return {};
}

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

void Illustrator::fail(const QString &problem, bool isConfig) {
    m_problem = problem;
    m_problemIsConfig = isConfig;
    m_busy = false;
    emit changed();
}

void Illustrator::clearProblem() {
    if (m_problem.isEmpty() && !m_problemIsConfig)
        return;
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
}

void Illustrator::retryLast() {
    if (m_busy || !m_hasLastRequest)
        return;
    illustrate(m_lastText, m_lastChapter, m_lastEnd);
}

void Illustrator::stop() {
    ++m_generation;
    if (m_readCancel) {
        *m_readCancel = true;
        m_readCancel.reset();
    }
    if (m_reply) {
        QPointer<Ai::Reply> active = m_reply;
        m_reply.clear();
        if (active)
            active->abort();
    }
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
    m_problemIsConfig = false;
    m_hasLastRequest = true;
    m_lastText = text;
    m_lastChapter = chapter;
    m_lastEnd = end;
    if (!m_ai->images() || !m_ai->textFor(QStringLiteral("illustrate"))) {
        fail(m_ai->hasKey() ? tr("Pictures need Gemini: switch to Hybrid or Cloud in Intelligence settings.")
                            : tr("Pictures are made with Gemini: add your key in Intelligence settings."),
             true);
        return;
    }
    if (!m_ai->fitsBudget(cost())) {
        fail(m_ai->describe(int(Ai::Error::Budget)), true);
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
    qint64 upTo = end > 0 ? end : m_reader->pageEnd();
    if (upTo <= 0)
        upTo = (format == QLatin1String("pdf")) ? -1 : 2500;
    else if (chapter == m_reader->chapter() && m_reader->pageEnd() > 0)
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
                                   : tr("Pictures are made with Gemini: add your key in Intelligence settings."),
                               true);
                    return;
                }
                const QString selectedText = text.trimmed();
                QString passage = selectedText;
                if (passage.isEmpty() && !chapters.isEmpty())
                    passage = chapters.last().text.right(2500);
                if (passage.trimmed().isEmpty()) {
                    self->fail(tr("There isn't enough text on this page to picture yet. Try another page or select a passage."));
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
                request.messages = {{true, QStringLiteral("Book: %1, by %2.\n\nWhat came before (for who is who):\n%3\n\nThe passage to illustrate:\n%4")
                                               .arg(title, author, Companion::joined(chapters, 12000), passage)}};
                self->m_status = tr("Imagining the picture…");
                emit self->changed();
                Ai::Reply *reply = writer->generate(request);
                self->m_ai->track(reply, false, 0.002);
                self->m_reply = reply;
                QPointer<Ai::Provider> providerGuard(writerProvider);
                connect(reply, &Ai::Reply::finished, self,
                        [self, reply, chapter, upTo, styleId, selectedText, generation, providerGuard] {
                            reply->deleteLater();
                            if (!self || generation != self->m_generation)
                                return;
                            self->m_reply.clear();
                            if (reply->error()) {
                                if (reply->error().kind != Ai::Error::Cancelled) {
                                    const auto kind = reply->error().kind;
                                    const bool isConfig = (kind == Ai::Error::NoKey || kind == Ai::Error::BadKey ||
                                                           kind == Ai::Error::Budget);
                                    self->fail(self->m_ai->describeFor(providerGuard, int(kind),
                                                                       reply->error().message),
                                               isConfig);
                                }
                                return;
                            }
                            QJsonObject result = reply->result();
                            if (result.isEmpty())
                                result = extractJsonObject(reply->text());
                            QString prompt = result.value("prompt").toString().trimmed();
                            if (prompt.isEmpty()) {
                                self->fail(tr("The picture couldn't be described. Try again, or choose a passage."));
                                return;
                            }
                            const QString guidance = styleGuidance(styleId);
                            if (!guidance.isEmpty() && !prompt.contains(labelForStyle(styleId), Qt::CaseInsensitive))
                                prompt += QStringLiteral(" ") + guidance;
                            self->draw(prompt, result.value("caption").toString().trimmed(), chapter, upTo, styleId, selectedText);
                        });
            },
            Qt::QueuedConnection);
    });
}

void Illustrator::draw(const QString &prompt, const QString &caption, int chapter, qint64 offset,
                       const QString &styleId, const QString &passage) {
    Ai::ImageGenerator *images = m_ai->images();
    if (!images) {
        fail(tr("Pictures are made with Gemini: add your key in Intelligence settings."), true);
        return;
    }
    const double imgCost = images->imageCost();
    if (!m_ai->fitsBudget(imgCost)) {
        fail(m_ai->describe(int(Ai::Error::Budget)), true);
        return;
    }
    m_status = tr("Drawing…");
    emit changed();
    Ai::ImageRequest request;
    request.prompt = prompt;
    Ai::Reply *reply = images->generateImage(request);
    m_ai->track(reply, false, imgCost);
    m_reply = reply;
    const int generation = m_generation;
    const int bookId = m_reader->bookId();
    connect(reply, &Ai::Reply::finished, this,
            [this, reply, prompt, caption, chapter, offset, styleId, passage, generation, bookId] {
                reply->deleteLater();
                if (generation != m_generation)
                    return;
                m_reply.clear();
                if (reply->error()) {
                    if (reply->error().kind != Ai::Error::Cancelled) {
                        const auto kind = reply->error().kind;
                        const bool isConfig = (kind == Ai::Error::NoKey || kind == Ai::Error::BadKey ||
                                               kind == Ai::Error::Budget);
                        const QString msg = reply->error().message.trimmed();
                        fail(kind == Ai::Error::Refused
                                 ? (msg.isEmpty() ? tr("Gemini wouldn't draw this scene.")
                                                  : tr("Gemini wouldn't draw this one: %1").arg(msg))
                                 : m_ai->describe(int(kind), msg),
                             isConfig);
                    }
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

QString Illustrator::suggestedFileName(int id, QString *sourceOut) const {
    QSqlQuery find(QSqlDatabase::database(m_connection));
    find.prepare(QStringLiteral("SELECT file FROM illustrations WHERE id = ?"));
    find.addBindValue(id);
    if (!find.exec() || !find.next())
        return {};
    const QString source = find.value(0).toString();
    if (sourceOut)
        *sourceOut = source;
    const QString ext = QFileInfo(source).suffix().isEmpty() ? QStringLiteral("png") : QFileInfo(source).suffix().toLower();
    QString name = m_reader ? m_reader->title().left(60) : QString();
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral(" "));
    name = name.simplified();
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        name = QStringLiteral("Illustration");
    return QStringLiteral("%1.%2").arg(name, ext);
}

QString Illustrator::saveCopyTo(int id, const QString &destinationPath) {
    QString source;
    const QString fallbackName = suggestedFileName(id, &source);
    if (source.isEmpty() || !QFileInfo::exists(source))
        return {};
    QString target = destinationPath.trimmed();
    if (target.isEmpty())
        return {};
    const QFileInfo targetInfo(target);
    if (targetInfo.isDir())
        target = QDir(target).filePath(fallbackName);
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (QFileInfo::exists(target) && QFileInfo(source).canonicalFilePath() != QFileInfo(target).canonicalFilePath())
        QFile::remove(target);
    return QFile::copy(source, target) ? target : QString();
}

QString Illustrator::saveCopy(int id) {
    QString source;
    const QString fileName = suggestedFileName(id, &source);
    if (source.isEmpty() || !QFileInfo::exists(source))
        return {};
    const QFileInfo fi(fileName);
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QStringLiteral("png") : fi.suffix();
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/Clothbound");
    QDir().mkpath(folder);
    QString target = QDir(folder).filePath(QStringLiteral("%1.%2").arg(base, ext));
    for (int n = 2; QFile::exists(target); ++n)
        target = QDir(folder).filePath(QStringLiteral("%1 %2.%3").arg(base).arg(n).arg(ext));
    return QFile::copy(source, target) ? target : QString();
}

void Illustrator::clearPendingSave() {
    if (m_saveWatcher) {
        m_saveWatcher->deleteLater();
        m_saveWatcher = nullptr;
    }
    if (!m_pendingSavePath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingSavePath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleSaveResponse(uint,QVariantMap)));
        m_pendingSavePath.clear();
    }
    m_pendingSaveId = 0;
}

void Illustrator::chooseSaveCopy(int id) {
    QString source;
    const QString fileName = suggestedFileName(id, &source);
    if (source.isEmpty() || !QFileInfo::exists(source)) {
        emit saveFailed(tr("Couldn't save a copy of this picture"));
        return;
    }

    auto fallbackDirect = [this, id] {
        const QString saved = saveCopy(id);
        if (!saved.isEmpty())
            emit savedCopy(saved);
        else
            emit saveFailed(tr("Couldn't save a copy of this picture"));
    };

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        fallbackDirect();
        return;
    }

    clearPendingSave();
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/Clothbound");
    QDir().mkpath(folder);
    const QString token = QStringLiteral("clothbound_save_%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pendingSaveId = id;
    m_pendingSavePath = predictedPath;
    if (!bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingSavePath,
                     QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                     this, SLOT(handleSaveResponse(uint,QVariantMap)))) {
        clearPendingSave();
        fallbackDirect();
        return;
    }

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("accept_label"), tr("Save"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("current_name"), fileName);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(folder));

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"),
        QStringLiteral("SaveFile"));
    message << QString() << tr("Save Illustration") << options;

    const int generation = ++m_saveGeneration;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    m_saveWatcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation, id] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_saveWatcher == watcher)
            m_saveWatcher = nullptr;
        if (generation != m_saveGeneration || m_pendingSaveId == 0)
            return;
        if (reply.isError()) {
            clearPendingSave();
            const QString saved = saveCopy(id);
            if (!saved.isEmpty())
                emit savedCopy(saved);
            else
                emit saveFailed(tr("Couldn't save a copy of this picture"));
            return;
        }
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingSavePath) {
            QDBusConnection::sessionBus().disconnect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingSavePath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleSaveResponse(uint,QVariantMap)));
            m_pendingSavePath = actualPath;
            QDBusConnection::sessionBus().connect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingSavePath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleSaveResponse(uint,QVariantMap)));
        }
    });
}

void Illustrator::handleSaveResponse(uint response, const QVariantMap &results) {
    const int id = m_pendingSaveId;
    clearPendingSave();
    if (response == 1 || id <= 0)
        return;  // user cancelled
    if (response != 0) {
        emit saveFailed(tr("Couldn't save a copy of this picture"));
        return;
    }
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty())
        return;
    QUrl url = QUrl::fromEncoded(uris.first().toUtf8(), QUrl::StrictMode);
    if (!url.isValid())
        url = QUrl(uris.first());
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : QString();
    const QString saved = localPath.isEmpty() ? QString() : saveCopyTo(id, localPath);
    if (!saved.isEmpty())
        emit savedCopy(saved);
    else
        emit saveFailed(tr("Couldn't save a copy of this picture"));
}
