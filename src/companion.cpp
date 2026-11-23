#include "companion.h"

#include "aiservices.h"
#include "aiusage.h"
#include "epubbook.h"
#include "pdfbook.h"
#include "reader.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>

namespace {

// Gemini reads up to this much of the book so far (about 300,000 tokens);
// a model on this computer, a chapter's worth (further bounded by actual num_ctx).
constexpr qint64 cloudLimit = 1200000;
constexpr qint64 localLimit = 24000;
constexpr qint64 localWhosWhoLimit = 16000;

QSettings companionSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

QStringList parseSdcvJson(const QByteArray &json, const QString &word) {
    QStringList out;
    for (const QJsonValue &entry : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = entry.toObject();
        const QString def = object.value(QStringLiteral("definition")).toString().trimmed();
        if (def.isEmpty())
            continue;
        if (object.value(QStringLiteral("word")).toString().compare(word, Qt::CaseInsensitive) == 0 || out.isEmpty()) {
            const QString dict = object.value(QStringLiteral("dict")).toString().trimmed();
            out.append(dict.isEmpty() ? def : def + QStringLiteral("\n— ") + dict);
        }
        if (out.size() >= 2)
            break;
    }
    return out;
}

QString cleanDictLookupWord(const QString &raw) {
    QString out = raw.trimmed();
    out.replace(QChar(0x2019), QLatin1Char('\''));
    out.replace(QChar(0x2018), QLatin1Char('\''));
    static const QRegularExpression lead(QStringLiteral("^[^\\p{L}\\p{N}]+"));
    static const QRegularExpression trail(QStringLiteral("[^\\p{L}\\p{N}]+$"));
    out.remove(lead);
    out.remove(trail);
    return out;
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

const char *noSpoilers =
    "The reader is partway through the book. You see only what they have read so far. Never reveal, hint at, or "
    "guess what happens later, even if you know the book; if they ask about something not yet in the text, say "
    "it hasn't come up yet.";

QString instructions(const QString &task) {
    return task + QStringLiteral("\n\n") + QString::fromLatin1(noSpoilers) +
           QStringLiteral("\nWrite in %1. Plain prose, no headings; short paragraphs.").arg(Companion::readerLanguage());
}

QString chapterDisplayLabel(const QList<Companion::Chapter> &chapters, int chapterIndex, const QString &format) {
    for (const Companion::Chapter &chapter : chapters)
        if (chapter.index == chapterIndex && !chapter.title.isEmpty())
            return chapter.title;
    return format == QLatin1String("pdf")
               ? QCoreApplication::translate("Companion", "Page %1").arg(chapterIndex + 1)
               : QCoreApplication::translate("Companion", "Chapter %1").arg(chapterIndex + 1);
}

QString passageText(const PassageIndex::Hit &hit, const QList<Companion::Chapter> &chapters, const QString &format) {
    return QStringLiteral("[%1]\n%2")
        .arg(chapterDisplayLabel(chapters, hit.passage.chapter, format), hit.passage.text);
}

QString makeExcerpt(const QString &text, qsizetype maxChars = 120) {
    QString clean = text.simplified();
    if (clean.size() <= maxChars)
        return clean;
    qsizetype cut = clean.lastIndexOf(QLatin1Char(' '), maxChars);
    if (cut < maxChars / 2)
        cut = maxChars;
    return clean.left(cut).trimmed() + QChar(0x2026);
}

QString makeChipQuote(const QString &text) {
    QString clean = text.simplified();
    static const QRegularExpression leadTrim(QStringLiteral("^[^\\p{L}\\p{N}]+"));
    clean.remove(leadTrim);
    if (clean.isEmpty())
        return {};
    const QStringList words = clean.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString out;
    int taken = 0;
    for (const QString &word : words) {
        if (taken > 0 && (taken >= 5 || out.size() + 1 + word.size() > 28))
            break;
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += word;
        ++taken;
    }
    static const QRegularExpression trailTrim(QStringLiteral("[,;:\\-—\"'“”‘’()\\[\\]]+$"));
    out.remove(trailTrim);
    return out.trimmed();
}

QList<Companion::Citation> citationsFromHits(const QList<PassageIndex::Hit> &hits,
                                             const QList<Companion::Chapter> &chapters, const QString &format,
                                             int maxCount = 4) {
    QList<Companion::Citation> out;
    for (const PassageIndex::Hit &hit : hits) {
        bool duplicate = false;
        for (const Companion::Citation &existing : out) {
            if (existing.chapter == hit.passage.chapter && qAbs(existing.offset - hit.passage.start) < 400) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        Companion::Citation cite;
        cite.chapter = hit.passage.chapter;
        cite.offset = hit.passage.start;
        const QString place = chapterDisplayLabel(chapters, hit.passage.chapter, format);
        const QString quote = makeChipQuote(hit.passage.text);
        cite.label = quote.isEmpty() ? place : QStringLiteral("%1 · “%2”").arg(place, quote);
        cite.excerpt = makeExcerpt(hit.passage.text);
        out.append(cite);
        if (out.size() >= maxCount)
            break;
    }
    return out;
}

QVariantList citationsToVariant(const QList<Companion::Citation> &citations) {
    QVariantList out;
    out.reserve(citations.size());
    for (const Companion::Citation &cite : citations) {
        out.append(QVariantMap{{QStringLiteral("chapter"), cite.chapter},
                               {QStringLiteral("offset"), cite.offset},
                               {QStringLiteral("label"), cite.label},
                               {QStringLiteral("excerpt"), cite.excerpt}});
    }
    return out;
}

QJsonArray citationsToJson(const QList<Companion::Citation> &citations) {
    QJsonArray out;
    for (const Companion::Citation &cite : citations) {
        out.append(QJsonObject{{QStringLiteral("chapter"), cite.chapter},
                               {QStringLiteral("offset"), cite.offset},
                               {QStringLiteral("label"), cite.label},
                               {QStringLiteral("excerpt"), cite.excerpt}});
    }
    return out;
}

QList<Companion::Citation> citationsFromJson(const QByteArray &json) {
    QList<Companion::Citation> out;
    for (const QJsonValue &val : QJsonDocument::fromJson(json).array()) {
        const QJsonObject obj = val.toObject();
        Companion::Citation cite;
        cite.chapter = obj.value(QStringLiteral("chapter")).toInt();
        cite.offset = obj.value(QStringLiteral("offset")).toInteger();
        cite.label = obj.value(QStringLiteral("label")).toString();
        cite.excerpt = obj.value(QStringLiteral("excerpt")).toString();
        if (!cite.label.isEmpty() || !cite.excerpt.isEmpty())
            out.append(cite);
    }
    return out;
}

int parseRomanNumeral(const QString &roman) {
    const QString s = roman.trimmed().toUpper();
    if (s.isEmpty() || s.size() > 10)
        return 0;
    auto val = [](QChar c) -> int {
        switch (c.unicode()) {
        case 'I': return 1;
        case 'V': return 5;
        case 'X': return 10;
        case 'L': return 50;
        case 'C': return 100;
        default: return 0;
        }
    };
    int total = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const int v = val(s[i]);
        if (v == 0)
            return 0;
        const int next = (i + 1 < s.size()) ? val(s[i + 1]) : 0;
        total += (v < next) ? -v : v;
    }
    return total;
}

struct FirstSeen {
    int chapterIndex = -1;
    qint64 offset = 0;
    QString label;
};

FirstSeen resolveFirstSeen(const QList<Companion::Chapter> &chapters, const QString &name, const QString &also,
                           const QString &llmChapter, const QString &format) {
    FirstSeen result;
    result.label = llmChapter.trimmed();
    if (chapters.isEmpty())
        return result;

    // 1. Search the actual chapters read so far for the character's EARLIEST mention
    // across full name, aliases, and surname (chapter order first, then earliest offset).
    QStringList probes;
    auto addProbe = [&probes](const QString &candidate) {
        const QString trimmed = candidate.trimmed();
        if (trimmed.size() >= 2 && !probes.contains(trimmed, Qt::CaseInsensitive))
            probes.append(trimmed);
    };
    addProbe(name);
    for (const QString &alias : also.split(QRegularExpression(QStringLiteral("[,;/()]")), Qt::SkipEmptyParts))
        addProbe(alias);
    static const QSet<QString> honorifics = {
        QStringLiteral("mr"),      QStringLiteral("mrs"),      QStringLiteral("miss"),    QStringLiteral("ms"),
        QStringLiteral("dr"),      QStringLiteral("sir"),      QStringLiteral("lord"),    QStringLiteral("lady"),
        QStringLiteral("captain"), QStringLiteral("doctor"),   QStringLiteral("professor"), QStringLiteral("master"),
        QStringLiteral("king"),    QStringLiteral("queen"),    QStringLiteral("prince"),  QStringLiteral("princess"),
        QStringLiteral("father"),  QStringLiteral("mother"),   QStringLiteral("uncle"),   QStringLiteral("aunt"),
        QStringLiteral("the"),     QStringLiteral("old"),      QStringLiteral("young"),
    };
    const QStringList nameParts = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (nameParts.size() >= 2) {
        const QString &last = nameParts.last();
        if (last.size() >= 3 && !honorifics.contains(last.toLower()))
            addProbe(last);
        const QString &first = nameParts.first();
        if (first.size() >= 3 && !honorifics.contains(first.toLower()))
            addProbe(first);
    }

    QList<QRegularExpression> regexes;
    regexes.reserve(probes.size());
    for (const QString &probe : probes) {
        regexes.append(QRegularExpression(
            QStringLiteral("(?<!\\p{L})") + QRegularExpression::escape(probe) + QStringLiteral("(?!\\p{L})"),
            QRegularExpression::CaseInsensitiveOption));
    }
    for (const Companion::Chapter &ch : chapters) {
        qint64 bestOffset = -1;
        for (const QRegularExpression &rx : regexes) {
            const auto match = rx.match(ch.text);
            if (match.hasMatch()) {
                const qint64 pos = match.capturedStart();
                if (bestOffset < 0 || pos < bestOffset)
                    bestOffset = pos;
            }
        }
        if (bestOffset >= 0) {
            result.chapterIndex = ch.index;
            result.offset = bestOffset;
            if (!ch.title.isEmpty())
                result.label = ch.title;
            else if (result.label.isEmpty())
                result.label = chapterDisplayLabel(chapters, ch.index, format);
            return result;
        }
    }

    // 2. Fallback: match the LLM's "chapter" label against chapter titles or numbers.
    const QString needle = llmChapter.trimmed();
    if (!needle.isEmpty()) {
        for (const Companion::Chapter &ch : chapters) {
            if (!ch.title.isEmpty() && (ch.title.compare(needle, Qt::CaseInsensitive) == 0 ||
                                        ch.title.contains(needle, Qt::CaseInsensitive) ||
                                        needle.contains(ch.title, Qt::CaseInsensitive))) {
                result.chapterIndex = ch.index;
                result.offset = 0;
                result.label = ch.title;
                return result;
            }
        }
        static const QRegularExpression pageRx(QStringLiteral("\\b(?:page|p\\.?)\\s*(\\d+)\\b"),
                                               QRegularExpression::CaseInsensitiveOption);
        const auto pageMatch = pageRx.match(needle);
        if (pageMatch.hasMatch()) {
            const int target = pageMatch.captured(1).toInt() - 1;
            for (const Companion::Chapter &ch : chapters) {
                if (ch.index == target) {
                    result.chapterIndex = ch.index;
                    result.offset = 0;
                    return result;
                }
            }
        }
        static const QRegularExpression numRx(QStringLiteral("\\b(?:chapter|ch\\.?)\\s*(\\d+|[IVXLC]+)\\b"),
                                              QRegularExpression::CaseInsensitiveOption);
        const auto numMatch = numRx.match(needle);
        if (numMatch.hasMatch()) {
            bool ok = false;
            int oneBased = numMatch.captured(1).toInt(&ok);
            if (!ok)
                oneBased = parseRomanNumeral(numMatch.captured(1));
            if (oneBased >= 1 && oneBased <= chapters.size()) {
                result.chapterIndex = chapters[oneBased - 1].index;
                result.offset = 0;
                return result;
            }
        }
    }
    return result;
}

}  // namespace

Companion::Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_ai(ai), m_reader(reader), m_connection(databaseConnection), m_index(ai, databaseConnection) {
    m_targetLanguage = companionSettings().value(QStringLiteral("companion/translateLanguage")).toString().trimmed();
    m_streamFlush.setSingleShot(true);
    m_streamFlush.setInterval(33);
    connect(&m_streamFlush, &QTimer::timeout, this, &Companion::flushStreamedAnswer);
    ensureTables();
    if (m_reader && m_reader->bookId() > 0)
        loadBookState(m_reader->bookId());
    connect(reader, &Reader::bookChanged, this, [this] {
        stop();
        m_feature.clear();
        m_subject.clear();
        m_answer.clear();
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_peopleWhereLocal = false;
        m_recapText.clear();
        m_recapLoaded = false;
        m_recapChapter = -1;
        m_recapOffset = -1;
        m_recapWhere.clear();
        m_recapWhereLocal = false;
        m_selectionFeature.clear();
        m_selectionSubject.clear();
        m_selectionAnswer.clear();
        m_selectionWhere.clear();
        m_selectionWhereLocal = false;
        m_conversation.clear();
        m_currentSources.clear();
        m_pendingUserTurn = false;
        m_problem.clear();
        m_problemIsConfig = false;
        m_lastAction = {};
        m_pendingSetup = {};
        if (m_reader && m_reader->bookId() > 0)
            loadBookState(m_reader->bookId());
        emit changed();
        emit answerChanged();
        emit conversationChanged();
        emit recapChanged();
        emit selectionChanged();
    });
    connect(reader, &Reader::locationChanged, this, [this] {
        if (!m_reader || m_reader->bookId() <= 0)
            return;
        const int bookId = m_reader->bookId();
        const int ch = m_reader->chapter();
        const qint64 end = m_reader->pageEnd();
        const qint64 start = m_reader->pageStart();
        // Cancel an in-flight chapter-scoped request only if the reader truly navigated
        // backward (not when pageEnd shrinks during a 2-column -> 1-column dock resize).
        if (m_busy && (m_feature == QLatin1String("whoswho") || m_feature == QLatin1String("recap")) &&
            m_requestChapter >= 0) {
            const bool movedBack =
                ch < m_requestChapter || (ch == m_requestChapter && m_requestStart > 0 && start < m_requestStart);
            if (movedBack)
                stop();
        }
        bool changedAny = false;
        bool recapUpdated = false;
        if (m_peopleLoaded) {
            const bool aheadOfReader =
                m_peopleChapter > ch || (m_peopleChapter == ch && end > 0 && m_peopleOffset > end);
            if (aheadOfReader) {
                if (!loadWhosWhoSnapshot(bookId, ch, end, start, false)) {
                    m_people.clear();
                    m_peopleLoaded = false;
                    m_peopleChapter = -1;
                    m_peopleOffset = -1;
                    m_peopleKey.clear();
                    m_peopleWhere.clear();
                    m_peopleWhereLocal = false;
                } else {
                    m_where = m_peopleWhere;
                    m_whereLocal = m_peopleWhereLocal;
                }
                changedAny = true;
            }
        }
        if ((!m_peopleLoaded || m_peopleChapter < ch) && !(m_busy && m_feature == QLatin1String("whoswho"))) {
            if (loadWhosWhoSnapshot(bookId, ch, end, start, true)) {
                if (m_feature == QLatin1String("whoswho")) {
                    m_where = m_peopleWhere;
                    m_whereLocal = m_peopleWhereLocal;
                }
                changedAny = true;
            }
        }
        if (m_recapLoaded) {
            const bool recapAhead =
                m_recapChapter > ch || (m_recapChapter == ch && end > 0 && m_recapOffset > end);
            if (recapAhead) {
                if (!loadRecapSnapshot(bookId, ch, end, start, false)) {
                    m_recapText.clear();
                    m_recapLoaded = false;
                    m_recapChapter = -1;
                    m_recapOffset = -1;
                    m_recapWhere.clear();
                    m_recapWhereLocal = false;
                }
                if (m_feature == QLatin1String("recap") && !m_busy) {
                    m_answer = m_recapText;
                    m_where = m_recapWhere;
                    m_whereLocal = m_recapWhereLocal;
                    emit answerChanged();
                }
                recapUpdated = true;
                changedAny = true;
            }
        }
        if ((!m_recapLoaded || m_recapChapter < ch) && !(m_busy && m_feature == QLatin1String("recap"))) {
            if (loadRecapSnapshot(bookId, ch, end, start, true)) {
                if (m_feature == QLatin1String("recap")) {
                    m_answer = m_recapText;
                    m_where = m_recapWhere;
                    m_whereLocal = m_recapWhereLocal;
                    emit answerChanged();
                }
                recapUpdated = true;
                changedAny = true;
            }
        }
        if (recapUpdated)
            emit recapChanged();
        if (changedAny)
            emit changed();
    });
}

Companion::~Companion() { stop(); }

void Companion::ensureTables() {
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_threads ("
        " id INTEGER PRIMARY KEY,"
        " book_id INTEGER NOT NULL,"
        " seq INTEGER NOT NULL,"
        " from_user INTEGER NOT NULL,"
        " text TEXT NOT NULL,"
        " sources TEXT NOT NULL DEFAULT '[]',"
        " provenance TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL)"));
    bool hasProvenance = false;
    bool hasIsLocal = false;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(companion_threads)"))) {
        while (info.next()) {
            const QString col = info.value(1).toString();
            if (col == QLatin1String("provenance"))
                hasProvenance = true;
            else if (col == QLatin1String("is_local"))
                hasIsLocal = true;
        }
    }
    if (!hasProvenance)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN provenance TEXT NOT NULL DEFAULT ''"));
    if (!hasIsLocal)
        query.exec(QStringLiteral("ALTER TABLE companion_threads ADD COLUMN is_local INTEGER NOT NULL DEFAULT 0"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS companion_threads_book ON companion_threads (book_id, seq)"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_whoswho ("
        " book_id INTEGER NOT NULL,"
        " chapter INTEGER NOT NULL,"
        " offset INTEGER NOT NULL,"
        " page_start INTEGER NOT NULL DEFAULT 0,"
        " where_label TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT -1,"
        " people TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " PRIMARY KEY (book_id, chapter, offset))"));
    bool whoswhoHasLocal = false;
    QSqlQuery wInfo(db);
    if (wInfo.exec(QStringLiteral("PRAGMA table_info(companion_whoswho)"))) {
        while (wInfo.next())
            if (wInfo.value(1).toString() == QLatin1String("is_local"))
                whoswhoHasLocal = true;
    }
    if (!whoswhoHasLocal)
        query.exec(QStringLiteral("ALTER TABLE companion_whoswho ADD COLUMN is_local INTEGER NOT NULL DEFAULT -1"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS companion_whoswho_book ON companion_whoswho (book_id, chapter DESC, offset DESC)"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS companion_recaps ("
        " book_id INTEGER NOT NULL,"
        " chapter INTEGER NOT NULL,"
        " offset INTEGER NOT NULL,"
        " page_start INTEGER NOT NULL DEFAULT 0,"
        " where_label TEXT NOT NULL DEFAULT '',"
        " is_local INTEGER NOT NULL DEFAULT 0,"
        " text TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " PRIMARY KEY (book_id, chapter, offset))"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS companion_recaps_book ON companion_recaps (book_id, chapter DESC, offset DESC)"));
    // Clean up companion data automatically when a book is deleted from `books`.
    QSqlQuery hasBooks(db);
    if (hasBooks.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='books'")) && hasBooks.next()) {
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS companion_cleanup_book AFTER DELETE ON books BEGIN "
            "DELETE FROM companion_threads WHERE book_id = OLD.id; "
            "DELETE FROM companion_whoswho WHERE book_id = OLD.id; "
            "END"));
        query.exec(QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS companion_cleanup_recaps AFTER DELETE ON books BEGIN "
            "DELETE FROM companion_recaps WHERE book_id = OLD.id; "
            "END"));
    }
}

void Companion::loadBookState(int bookId) {
    if (bookId <= 0)
        return;
    loadConversation(bookId);
    if (loadWhosWhoSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false) &&
        m_where.isEmpty()) {
        m_where = m_peopleWhere;
        m_whereLocal = m_peopleWhereLocal;
    }
    loadRecapSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
}

void Companion::saveConversation(int bookId) {
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    const bool inTx = db.transaction();
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
    del.addBindValue(bookId);
    bool ok = del.exec();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < m_conversation.size() && ok; ++i) {
        const Turn &turn = m_conversation[i];
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO companion_threads (book_id, seq, from_user, text, sources, provenance, is_local, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
        ins.addBindValue(bookId);
        ins.addBindValue(i);
        ins.addBindValue(turn.fromUser ? 1 : 0);
        ins.addBindValue(turn.text.isNull() ? QStringLiteral("") : turn.text);
        ins.addBindValue(QString::fromUtf8(QJsonDocument(citationsToJson(turn.sources)).toJson(QJsonDocument::Compact)));
        ins.addBindValue(turn.provenance.isNull() ? QStringLiteral("") : turn.provenance);
        ins.addBindValue(turn.local ? 1 : 0);
        ins.addBindValue(now);
        ok = ins.exec();
    }
    if (inTx)
        ok ? db.commit() : db.rollback();
}

void Companion::loadConversation(int bookId) {
    m_conversation.clear();
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT from_user, text, sources, provenance, is_local FROM companion_threads "
        "WHERE book_id = ? ORDER BY seq ASC, id ASC"));
    query.addBindValue(bookId);
    if (!query.exec())
        return;
    while (query.next()) {
        Turn turn;
        turn.fromUser = query.value(0).toInt() != 0;
        turn.text = query.value(1).toString();
        turn.sources = citationsFromJson(query.value(2).toString().toUtf8());
        turn.provenance = query.value(3).toString();
        turn.local = query.value(4).toInt() != 0;
        m_conversation.append(std::move(turn));
    }
}

bool Companion::loadWhosWhoSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter) {
    if (bookId <= 0)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT chapter, offset, page_start, where_label, is_local, people FROM companion_whoswho "
        "WHERE book_id = ? ORDER BY chapter DESC, offset DESC"));
    query.addBindValue(bookId);
    if (!query.exec())
        return false;
    while (query.next()) {
        const int savedChapter = query.value(0).toInt();
        const qint64 savedOffset = query.value(1).toLongLong();
        const qint64 savedStart = query.value(2).toLongLong();
        // Strictly spoiler-safe: never restore a checkpoint from ahead of the reader.
        if (savedChapter > chapter)
            continue;
        if (requireSameChapter && savedChapter != chapter)
            continue;
        if (savedChapter == chapter) {
            if (pageEnd > 0) {
                if (savedOffset > pageEnd)
                    continue;
            } else if (savedStart > pageStart) {
                continue;
            }
        }
        const QJsonArray arr = QJsonDocument::fromJson(query.value(5).toString().toUtf8()).array();
        QVariantList loaded;
        loaded.reserve(arr.size());
        for (const QJsonValue &val : arr)
            if (val.isObject())
                loaded.append(val.toObject().toVariantMap());
        m_people = loaded;
        m_peopleLoaded = true;
        m_peopleChapter = savedChapter;
        m_peopleOffset = savedOffset;
        m_peopleWhere = query.value(3).toString();
        const int savedLocal = query.value(4).toInt();
        m_peopleWhereLocal = savedLocal >= 0
                                 ? (savedLocal != 0)
                                 : m_peopleWhere.contains(QLatin1String("this computer"), Qt::CaseInsensitive);
        m_peopleKey = QStringLiteral("%1:%2:%3:%4").arg(bookId).arg(savedChapter).arg(savedOffset).arg(m_peopleWhere);
        return true;
    }
    return false;
}

void Companion::saveWhosWhoSnapshot(int bookId, int chapter, qint64 offset, qint64 pageStart, const QString &whereLabel,
                                    bool isLocal, const QVariantList &people) {
    if (bookId <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    QJsonArray arr;
    for (const QVariant &v : people)
        arr.append(QJsonObject::fromVariantMap(v.toMap()));
    const QString json = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO companion_whoswho (book_id, chapter, offset, page_start, where_label, is_local, people, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(bookId);
    ins.addBindValue(chapter);
    ins.addBindValue(offset);
    ins.addBindValue(pageStart);
    ins.addBindValue(whereLabel);
    ins.addBindValue(isLocal ? 1 : 0);
    ins.addBindValue(json);
    ins.addBindValue(now);
    if (ins.exec()) {
        // Keep the 12 most recent checkpoints per book.
        QSqlQuery prune(db);
        prune.prepare(QStringLiteral(
            "DELETE FROM companion_whoswho WHERE book_id = ? AND rowid NOT IN ("
            "SELECT rowid FROM companion_whoswho WHERE book_id = ? ORDER BY updated_at DESC LIMIT 12)"));
        prune.addBindValue(bookId);
        prune.addBindValue(bookId);
        prune.exec();
    }
}

bool Companion::loadRecapSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter) {
    if (bookId <= 0)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT chapter, offset, page_start, where_label, is_local, text FROM companion_recaps "
        "WHERE book_id = ? ORDER BY chapter DESC, offset DESC"));
    query.addBindValue(bookId);
    if (!query.exec())
        return false;
    while (query.next()) {
        const int savedChapter = query.value(0).toInt();
        const qint64 savedOffset = query.value(1).toLongLong();
        const qint64 savedStart = query.value(2).toLongLong();
        // Strictly spoiler-safe: never restore a recap from ahead of the reader.
        if (savedChapter > chapter)
            continue;
        if (requireSameChapter && savedChapter != chapter)
            continue;
        if (savedChapter == chapter) {
            if (pageEnd > 0) {
                if (savedOffset > pageEnd)
                    continue;
            } else if (savedStart > pageStart) {
                continue;
            }
        }
        const QString savedText = query.value(5).toString();
        if (savedText.trimmed().isEmpty())
            continue;
        m_recapText = savedText;
        m_recapLoaded = true;
        m_recapChapter = savedChapter;
        m_recapOffset = savedOffset;
        m_recapWhere = query.value(3).toString();
        m_recapWhereLocal = query.value(4).toInt() != 0;
        return true;
    }
    return false;
}

void Companion::saveRecapSnapshot(int bookId, int chapter, qint64 offset, qint64 pageStart, const QString &whereLabel,
                                  bool isLocal, const QString &text) {
    if (bookId <= 0 || text.trimmed().isEmpty())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.isOpen())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO companion_recaps (book_id, chapter, offset, page_start, where_label, is_local, text, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(bookId);
    ins.addBindValue(chapter);
    ins.addBindValue(offset);
    ins.addBindValue(pageStart);
    ins.addBindValue(whereLabel);
    ins.addBindValue(isLocal ? 1 : 0);
    ins.addBindValue(text);
    ins.addBindValue(now);
    if (ins.exec()) {
        QSqlQuery prune(db);
        prune.prepare(QStringLiteral(
            "DELETE FROM companion_recaps WHERE book_id = ? AND rowid NOT IN ("
            "SELECT rowid FROM companion_recaps WHERE book_id = ? ORDER BY updated_at DESC LIMIT 12)"));
        prune.addBindValue(bookId);
        prune.addBindValue(bookId);
        prune.exec();
    }
}

void Companion::clearBookHistory(int bookId) {
    const int target = bookId > 0 ? bookId : (m_reader ? m_reader->bookId() : 0);
    if (target <= 0)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (db.isOpen()) {
        QSqlQuery q1(db);
        q1.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
        q1.addBindValue(target);
        q1.exec();
        QSqlQuery q2(db);
        q2.prepare(QStringLiteral("DELETE FROM companion_whoswho WHERE book_id = ?"));
        q2.addBindValue(target);
        q2.exec();
        QSqlQuery q3(db);
        q3.prepare(QStringLiteral("DELETE FROM companion_recaps WHERE book_id = ?"));
        q3.addBindValue(target);
        q3.exec();
    }
    if (m_reader && target == m_reader->bookId()) {
        m_conversation.clear();
        m_currentSources.clear();
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_peopleWhereLocal = false;
        m_recapText.clear();
        m_recapLoaded = false;
        m_recapChapter = -1;
        m_recapOffset = -1;
        m_recapWhere.clear();
        m_recapWhereLocal = false;
        m_selectionFeature.clear();
        m_selectionSubject.clear();
        m_selectionAnswer.clear();
        m_selectionWhere.clear();
        m_selectionWhereLocal = false;
        emit changed();
        emit answerChanged();
        emit conversationChanged();
        emit recapChanged();
        emit selectionChanged();
    }
}

QVariantList Companion::conversation() const {
    QVariantList out;
    out.reserve(m_conversation.size());
    for (const Turn &turn : m_conversation) {
        out.append(QVariantMap{{QStringLiteral("fromUser"), turn.fromUser},
                               {QStringLiteral("text"), turn.text},
                               {QStringLiteral("sources"), citationsToVariant(turn.sources)},
                               {QStringLiteral("provenance"), turn.provenance},
                               {QStringLiteral("local"), turn.local}});
    }
    return out;
}

QStringList Companion::suggestions() const {
    QStringList askedLower;
    for (const Turn &turn : m_conversation)
        if (turn.fromUser)
            askedLower.append(turn.text.trimmed().toLower());

    QStringList out;
    auto addIfFresh = [&](const QString &prompt) {
        const QString clean = prompt.trimmed();
        if (clean.isEmpty() || out.size() >= 2)
            return;
        const QString low = clean.toLower();
        for (const QString &prev : askedLower)
            if (prev == low)
                return;
        if (!out.contains(clean, Qt::CaseInsensitive))
            out.append(clean);
    };

    // 1. Character prompt from already-loaded spoiler-safe Who's who entries.
    for (const QVariant &entry : m_people) {
        const QString name = entry.toMap().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            const QString candidate = tr("Who is %1?").arg(name);
            const QString lowName = name.toLower();
            bool mentioned = false;
            for (const QString &prev : askedLower) {
                if (prev.contains(lowName)) {
                    mentioned = true;
                    break;
                }
            }
            if (!mentioned) {
                addIfFresh(candidate);
                break;
            }
        }
    }

    // 2. Chapter-specific prompt when a readable chapter title is present.
    if (m_reader) {
        const QString chapterTitle = m_reader->chapterTitle().trimmed();
        if (!chapterTitle.isEmpty() && chapterTitle.size() <= 40)
            addIfFresh(tr("What has happened in %1 so far?").arg(chapterTitle));
    }

    // 3. Clean general fallbacks grounded in what has been read so far.
    addIfFresh(tr("Who are the key people so far?"));
    addIfFresh(tr("Summarize what has happened so far"));
    addIfFresh(tr("What should I remember from the last few pages?"));
    return out;
}

QVariantList Companion::currentSources() const { return citationsToVariant(m_currentSources); }

QString Companion::targetLanguage() const {
    if (!m_targetLanguage.isEmpty())
        return m_targetLanguage;
    const QString sys = readerLanguage();
    return sys.isEmpty() || sys == QLatin1String("C") ? QStringLiteral("English") : sys;
}

void Companion::setTargetLanguage(const QString &language) {
    const QString cleaned = language.trimmed();
    if (cleaned.isEmpty() || cleaned == targetLanguage())
        return;
    m_targetLanguage = cleaned;
    companionSettings().setValue(QStringLiteral("companion/translateLanguage"), m_targetLanguage);
    emit targetLanguageChanged();
}

QStringList Companion::targetLanguages() const {
    static const QStringList common = {
        QStringLiteral("English"),    QStringLiteral("Spanish"),  QStringLiteral("French"),
        QStringLiteral("German"),     QStringLiteral("Italian"),  QStringLiteral("Portuguese"),
        QStringLiteral("Dutch"),      QStringLiteral("Russian"),  QStringLiteral("Ukrainian"),
        QStringLiteral("Polish"),     QStringLiteral("Swedish"),  QStringLiteral("Turkish"),
        QStringLiteral("Greek"),      QStringLiteral("Arabic"),   QStringLiteral("Hindi"),
        QStringLiteral("Chinese"),    QStringLiteral("Japanese"), QStringLiteral("Korean"),
        QStringLiteral("Latin"),
    };
    QStringList out;
    const QString sys = readerLanguage();
    if (!sys.isEmpty() && sys != QLatin1String("C"))
        out.append(sys);
    if (!m_targetLanguage.isEmpty() && !out.contains(m_targetLanguage))
        out.append(m_targetLanguage);
    for (const QString &lang : common)
        if (!out.contains(lang))
            out.append(lang);
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

QString Companion::placeTitle(int chapter) const {
    const QString title = m_reader->titleOfChapter(chapter);
    if (!title.isEmpty())
        return title;
    if (m_reader->format() == QLatin1String("pdf")) {
        const QString lbl = m_reader->pdfPageLabel(chapter);
        return tr("Page %1").arg(lbl.isEmpty() ? QString::number(chapter + 1) : lbl);
    }
    return tr("Chapter %1").arg(chapter + 1);
}

bool Companion::cloudFor(const QString &feature) const {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    return provider && provider->id() != QLatin1String("local");
}

QString Companion::formatProvenance(const Ai::Provider *provider, const Ai::Reply *reply) const {
    if (!provider)
        return m_where;
    if (provider->isLocal()) {
        const QString rawModel = reply && !reply->model().isEmpty() ? reply->model() : m_ai->localTextModel();
        const QString title = AiServices::friendlyModelTitle(rawModel);
        return title.isEmpty() ? tr("On this computer · %1").arg(provider->name())
                               : tr("On this computer · %1").arg(title);
    }
    if (provider->id() == QLatin1String("local")) {
        const QString rawModel = reply && !reply->model().isEmpty() ? reply->model() : m_ai->localTextModel();
        const QString title = AiServices::friendlyModelTitle(rawModel);
        return title.isEmpty() ? tr("Sent to %1").arg(provider->name())
                               : tr("Sent to %1 · %2").arg(provider->name(), title);
    }
    const double dollars = reply ? AiUsageLedger::cost(reply->model(), reply->usage(), QDate::currentDate(), false) : 0.0;
    const QString costText = dollars < 0.01 ? tr("less than 1¢")
                           : dollars < 1.0  ? tr("about %1¢").arg(qMax(1, qRound(dollars * 100)))
                                            : QStringLiteral("$%1").arg(dollars, 0, 'f', 2);
    return tr("Gemini · sent to Google · %1").arg(costText);
}

qint64 Companion::localCharBudget(const QString &feature, bool fast, qint64 fallbackChars, double charsPerToken) const {
    Ai::TextModel *model = m_ai->textFor(feature);
    const int ctxTokens = model ? model->contextWindow(fast) : 0;
    if (ctxTokens <= 0)
        return fallbackChars;
    // Reserve headroom for instructions, JSON schema, and generated output.
    const int reserved = qMax(1024, ctxTokens / 4);
    const int usableTokens = qMax(512, ctxTokens - reserved);
    const qint64 budgeted = qint64(double(usableTokens) * charsPerToken);
    return qMin(fallbackChars, budgeted);
}

QString Companion::pendingSetupLabel() const {
    if (m_pendingSetup.feature.isEmpty())
        return {};
    if (m_pendingSetup.feature == QLatin1String("ask"))
        return tr("“%1”").arg(makeExcerpt(m_pendingSetup.text, 64));
    if (m_pendingSetup.feature == QLatin1String("define"))
        return tr("Define “%1”").arg(makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("explain"))
        return tr("Explain “%1”").arg(makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("translate"))
        return tr("Translate “%1”").arg(makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("recap"))
        return tr("Previously…");
    if (m_pendingSetup.feature == QLatin1String("whoswho"))
        return tr("Who's who");
    return {};
}

Ai::TextModel *Companion::modelFor(const QString &feature) {
    Ai::TextModel *model = m_ai->textFor(feature);
    if (!model) {
        if (!m_ai->hasKey() && !m_ai->hasLocal() && !m_lastAction.feature.isEmpty())
            m_pendingSetup = m_lastAction;
        fail(m_ai->preset() == QLatin1String("local") || m_ai->overrideFor(feature) == QLatin1String("local")
                 ? tr("This needs a model on this computer: set one up in Intelligence (✦).")
                 : tr("This needs a Gemini key or a model on this computer, in Intelligence (✦)."),
             true);
    }
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
        fail(m_ai->describe(int(Ai::Error::Budget)), true);
        return false;
    }
    return true;
}

qint64 Companion::soFarCharsEstimate() const {
    qint64 soFar = m_reader ? m_reader->pageEnd() : 0;
    if (m_reader) {
        for (int i = 0; i < m_reader->chapter(); ++i)
            soFar += m_reader->charactersIn(i);
        // While chapter lengths are still being measured, fall back to a conservative
        // estimate so the cost readout and budget check never under-report as $0.
        if (soFar <= 0 && m_reader->chapter() > 0)
            soFar = qint64(m_reader->chapter() + 1) * 12000;
    }
    return qMin<qint64>(cloudLimit, soFar);
}

void Companion::flushStreamedAnswer() {
    m_streamFlush.stop();
    if (!m_streamDirty)
        return;
    m_streamDirty = false;
    emit answerChanged();
}

void Companion::begin(const QString &feature, const QString &subject) {
    stop();
    m_feature = feature;
    m_subject = subject;
    m_answer.clear();
    m_currentSources.clear();
    m_problem.clear();
    m_problemIsConfig = false;
    m_busy = true;
    m_streamDirty = false;
    m_streamNotifiedFirst = false;
    m_requestChapter = m_reader ? m_reader->chapter() : -1;
    m_requestStart = m_reader ? m_reader->pageStart() : -1;
    m_requestOffset = m_reader ? m_reader->pageEnd() : -1;
    m_where = m_ai->whereFor(feature);
    m_whereLocal = (m_ai->routeFor(feature) == QLatin1String("local") && m_ai->localIsLoopback());
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
        m_conversation.append({false, m_answer, m_currentSources, m_where, m_whereLocal});
        m_answer.clear();
        m_currentSources.clear();
        saveConversation(m_reader->bookId());
        emit conversationChanged();
        emit answerChanged();
    } else if (!m_conversation.isEmpty() && m_conversation.last().fromUser) {
        m_conversation.removeLast();
        m_answer.clear();
        m_currentSources.clear();
        emit conversationChanged();
        emit answerChanged();
    }
}

void Companion::fail(const QString &problem, bool isConfig) {
    flushStreamedAnswer();
    rollbackPendingTurn();
    m_problem = problem;
    m_problemIsConfig = isConfig;
    m_busy = false;
    m_requestChapter = -1;
    m_requestStart = -1;
    m_requestOffset = -1;
    emit changed();
}

void Companion::clearProblem() {
    if (m_problem.isEmpty() && !m_problemIsConfig)
        return;
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
}

void Companion::executeAction(const Action &action) {
    if (action.feature == QLatin1String("ask"))
        ask(action.text);
    else if (action.feature == QLatin1String("define"))
        define(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("explain"))
        explain(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("translate"))
        translate(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("recap"))
        refreshRecap();
    else if (action.feature == QLatin1String("whoswho"))
        refreshWhosWho();
}

void Companion::retry() {
    if (m_busy || m_lastAction.feature.isEmpty())
        return;
    const Action action = m_lastAction;
    executeAction(action);
}

void Companion::resumePendingSetup() {
    if (m_pendingSetup.feature.isEmpty())
        return;
    const Action action = m_pendingSetup;
    m_pendingSetup = {};
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
    executeAction(action);
}

void Companion::stop() {
    ++m_generation;
    m_streamFlush.stop();
    m_streamDirty = false;
    m_streamNotifiedFirst = false;
    m_requestChapter = -1;
    m_requestStart = -1;
    m_requestOffset = -1;
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
    if (m_reply) {
        QPointer<Ai::Reply> active = m_reply;
        m_reply.clear();
        if (active)
            active->abort();
    }
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
    m_whereLocal = provider->isLocal();
    Ai::Reply *reply = model->generate(request);
    m_ai->track(reply);
    m_reply = reply;
    const int generation = m_generation;
    QPointer<Ai::Provider> providerGuard(provider);
    setStatus(tr("Writing…"));
    connect(reply, &Ai::Reply::textReceived, this, [this, reply, generation](const QString &delta) {
        if (reply != m_reply || generation != m_generation || delta.isEmpty())
            return;
        m_answer += delta;
        if (!m_streamNotifiedFirst) {
            m_streamNotifiedFirst = true;
            m_streamDirty = false;
            emit answerChanged();
        } else {
            m_streamDirty = true;
            if (!m_streamFlush.isActive())
                m_streamFlush.start();
        }
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, done, generation, providerGuard, feature] {
        reply->deleteLater();
        if (reply != m_reply || generation != m_generation)
            return;
        m_reply.clear();
        flushStreamedAnswer();
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled) {
                const auto kind = reply->error().kind;
                const bool isConfig = (kind == Ai::Error::NoKey || kind == Ai::Error::BadKey ||
                                       kind == Ai::Error::Budget || kind == Ai::Error::NotFound ||
                                       (providerGuard && providerGuard->id() == QLatin1String("local") &&
                                        kind == Ai::Error::Offline));
                fail(m_ai->describeFor(providerGuard, int(kind), reply->error().message), isConfig);
            }
            return;
        }
        if (m_answer.isEmpty() && reply->result().isEmpty())
            m_answer = reply->text();
        if (!done && m_answer.trimmed().isEmpty()) {
            fail(tr("The model returned an empty answer. Try again."));
            return;
        }
        m_pendingSetup = {};
        m_where = formatProvenance(providerGuard, reply);
        m_whereLocal = providerGuard && providerGuard->isLocal();
        if (feature == QLatin1String("define") || feature == QLatin1String("explain") ||
            feature == QLatin1String("translate")) {
            m_selectionFeature = feature;
            m_selectionSubject = m_subject;
            m_selectionAnswer = m_answer;
            m_selectionWhere = m_where;
            m_selectionWhereLocal = m_whereLocal;
            emit selectionChanged();
        }
        if (done)
            done(reply);
        if (m_problem.isEmpty()) {
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit answerChanged();
            emit changed();
        }
    });
}

// --- Define, explain, translate ----------------------------------------------

void Companion::define(const QString &word, int chapter, qint64 offset) {
    const QString wanted = word.trimmed();
    m_lastAction = {QStringLiteral("define"), wanted, chapter, offset};
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
                "You are a literary dictionary. Format the first line as *part of speech* — concise one-line "
                "definition for how the word is used in this passage. If the usage is archaic, rare, or figurative, "
                "add one brief sentence after. At most 60 words."));
            request.messages = {{true, bookLine() + QStringLiteral("\n\nPassage:\n") + around +
                                           QStringLiteral("\n\nWord: ") + wanted}};
            run(QStringLiteral("define"), request);
        });
    };

    // A dictionary on this computer answers first (asynchronously) and costs nothing.
    const QString lookupWord = cleanDictLookupWord(wanted);
    if (!lookupWord.isEmpty() && !lookupWord.contains(QLatin1Char(' ')) &&
        !QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty()) {
        setStatus(tr("Checking your dictionary…"));
        auto *sdcv = new QProcess(this);
        m_dictProcess = sdcv;
        auto handled = std::make_shared<bool>(false);
        auto *timer = new QTimer(sdcv);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, sdcv, [sdcv] {
            if (sdcv->state() != QProcess::NotRunning)
                sdcv->kill();
        });
        connect(sdcv, &QProcess::errorOccurred, this, [this, sdcv, generation, handled, askModel](QProcess::ProcessError) {
            if (*handled)
                return;
            *handled = true;
            if (sdcv == m_dictProcess)
                m_dictProcess.clear();
            sdcv->deleteLater();
            if (generation == m_generation)
                askModel();
        });
        connect(sdcv, &QProcess::finished, this,
                [this, sdcv, lookupWord, wanted, generation, handled, askModel](int code, QProcess::ExitStatus status) {
                    if (*handled)
                        return;
                    *handled = true;
                    if (sdcv == m_dictProcess)
                        m_dictProcess.clear();
                    const QByteArray output = sdcv->readAllStandardOutput();
                    sdcv->deleteLater();
                    if (generation != m_generation)
                        return;
                    const QStringList entries =
                        (status == QProcess::NormalExit && code == 0) ? parseSdcvJson(output, lookupWord) : QStringList();
                    if (!entries.isEmpty()) {
                        m_pendingSetup = {};
                        m_answer = entries.join(QStringLiteral("\n\n"));
                        m_where = tr("Dictionary on this computer");
                        m_whereLocal = true;
                        m_selectionFeature = QStringLiteral("define");
                        m_selectionSubject = wanted;
                        m_selectionAnswer = m_answer;
                        m_selectionWhere = m_where;
                        m_selectionWhereLocal = true;
                        m_busy = false;
                        m_requestChapter = -1;
                        m_requestOffset = -1;
                        emit selectionChanged();
                        emit answerChanged();
                        emit changed();
                        return;
                    }
                    askModel();
                });
        timer->start(4000);
        sdcv->start(QStringLiteral("sdcv"),
                    {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), lookupWord});
        return;
    }
    askModel();
}

void Companion::explain(const QString &text, int chapter, qint64 end) {
    const QString cleaned = text.trimmed();
    m_lastAction = {QStringLiteral("explain"), cleaned, chapter, end};
    begin(QStringLiteral("explain"), cleaned);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 charLimit = cloud ? 60000 : localCharBudget(m_feature, false, 12000);
    if (!checkBudget(m_feature, charLimit, false, 400))
        return;
    const int generation = m_generation;
    setStatus(tr("Reading up to here…"));
    readSoFar(chapter, end, charLimit, [this, cleaned, charLimit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const bool isCloud = cloudFor(QStringLiteral("explain"));
        const qint64 limit = isCloud ? 60000 : qMin(charLimit, localCharBudget(QStringLiteral("explain"), false, 12000));
        const QString context = joined(chapters, limit);
        if (!checkBudget(QStringLiteral("explain"), context.size() + cleaned.size(), false, 400))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("explain");
        request.stream = true;
        request.system = instructions(QStringLiteral(
            "Explain the selected words to a reader in the middle of this book: what they mean, what is happening, "
            "and any allusion, history, idiom or old usage a modern reader might miss. Use the text before it for "
            "context. Be brief: one to three short paragraphs."));
        request.messages = {{true, bookLine() + QStringLiteral("\n\nThe book up to the selection:\n") + context +
                                       QStringLiteral("\n\nSelected: \"") + cleaned + QStringLiteral("\"")}};
        run(QStringLiteral("explain"), request);
    });
}

void Companion::translate(const QString &text, int chapter, qint64 end) {
    const QString cleaned = text.trimmed();
    m_lastTranslateText = cleaned;
    m_lastTranslateChapter = chapter;
    m_lastTranslateEnd = end;
    m_lastAction = {QStringLiteral("translate"), cleaned, chapter, end};
    begin(QStringLiteral("translate"), cleaned);
    if (!modelFor(m_feature))
        return;
    if (!checkBudget(m_feature, 2400 + cleaned.size(), true, 300))
        return;
    // Translate into the explicitly chosen target language (defaulting to the reader's
    // own system language — never forcing another language on untagged PDFs or quotes).
    const QString target = targetLanguage();
    const int generation = m_generation;
    readSoFar(chapter, end, 2500, [this, cleaned, target, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const QString context = joined(chapters, 2000);
        if (!checkBudget(QStringLiteral("translate"), context.size() + cleaned.size(), true, 300))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("translate");
        request.fast = true;
        request.stream = true;
        request.system = QStringLiteral(
                             "Translate the selected words into %1, faithfully and in a natural literary style, "
                             "using the passage before them for context. If the selected words are already in %1, "
                             "render them clearly in modern %1 instead of switching languages. Give only the translation.")
                             .arg(target);
        request.messages = {{true, QStringLiteral("Context:\n") + context + QStringLiteral("\n\nTranslate:\n") + cleaned}};
        run(QStringLiteral("translate"), request);
    });
}

void Companion::retranslate() {
    if (m_lastTranslateText.isEmpty())
        return;
    translate(m_lastTranslateText, m_lastTranslateChapter, m_lastTranslateEnd);
}

// --- Ask ---------------------------------------------------------------------

QVariantMap Companion::askEstimate() const {
    const bool cloud = cloudFor(QStringLiteral("ask"));
    const qint64 soFar = soFarCharsEstimate();
    qint64 historyChars = 0;
    for (const Turn &turn : m_conversation)
        historyChars += turn.text.size();
    const double characters = double(soFar) + historyChars;
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
            {QStringLiteral("words"), qRound(double(soFar) / 6.0)},
            {QStringLiteral("cost"), cost},
            {QStringLiteral("local"), !cloud}};
}

void Companion::newConversation() {
    stop();
    m_conversation.clear();
    m_currentSources.clear();
    m_pendingUserTurn = false;
    m_answer.clear();
    m_feature = QStringLiteral("ask");
    m_problem.clear();
    m_problemIsConfig = false;
    m_pendingSetup = {};
    if (m_reader && m_reader->bookId() > 0) {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen()) {
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM companion_threads WHERE book_id = ?"));
            del.addBindValue(m_reader->bookId());
            del.exec();
        }
    }
    emit changed();
    emit answerChanged();
    emit conversationChanged();
}

void Companion::ask(const QString &question) {
    const QString asked = question.trimmed();
    if (asked.isEmpty())
        return;
    m_lastAction = {QStringLiteral("ask"), asked, m_reader ? m_reader->chapter() : 0,
                    m_reader ? m_reader->pageEnd() : 0};
    begin(QStringLiteral("ask"), asked);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud) {
        const double cost = askEstimate().value(QStringLiteral("cost")).toDouble();
        if (!m_ai->fitsBudget(cost)) {
            fail(m_ai->describe(int(Ai::Error::Budget)), true);
            return;
        }
    }
    m_conversation.append({true, asked, {}, {}, false});
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();
    setStatus(cloud ? tr("Reading the book so far…") : tr("Finding the passages that matter…"));
    const int bookId = m_reader->bookId();
    const QString format = m_reader->format();
    const int generation = m_generation;
    const qint64 readLimit = cloud ? cloudLimit : -1;
    readSoFar(m_reader->chapter(), m_reader->pageEnd(), readLimit,
              [this, asked, bookId, format, generation](const QList<Chapter> &chapters) {
                  if (generation != m_generation)
                      return;
                  if (chapters.isEmpty()) {
                      fail(tr("There isn't any readable text up to this page yet."));
                      return;
                  }
                  const bool isCloud = cloudFor(QStringLiteral("ask"));
                  const auto send = [this, isCloud, bookId, format, chapters, generation](
                                        const QString &context, const QString &how,
                                        const QList<Citation> &sources) {
                      if (generation != m_generation)
                          return;
                      m_currentSources = sources;
                      emit answerChanged();
                      // Build alternating conversation turns (trimmed to fit local context windows).
                      const qint64 localTotal = isCloud ? cloudLimit : localCharBudget(QStringLiteral("ask"), false, localLimit);
                      qint64 historyBudget = isCloud ? 100000 : qMin<qint64>(8000, qMax<qint64>(1500, localTotal / 3));
                      QList<Ai::Message> turns;
                      for (qsizetype i = m_conversation.size() - 1; i >= 0; --i) {
                          const Turn &msg = m_conversation[i];
                          if (!turns.isEmpty() && historyBudget - msg.text.size() < 0)
                              break;
                          historyBudget -= msg.text.size();
                          turns.prepend({msg.fromUser, msg.text});
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
                      request.system = instructions(
                          QStringLiteral("Answer the reader's questions about the book they are reading, from its text "
                                         "below (%1). Be concise, warm, and literary. Quote briefly when it helps, and "
                                         "mention which chapter it comes from. If the text so far doesn't say, say so plainly.")
                              .arg(how));
                      request.messages = {{true, bookLine() + QStringLiteral("\n\n") + context}};
                      request.messages.append({false, QStringLiteral("I've read it. What would you like to know?")});
                      request.messages += normalized;
                      run(QStringLiteral("ask"), request, [this, bookId, format, chapters, generation](Ai::Reply *) {
                          if (generation != m_generation)
                              return;
                          if (m_answer.trimmed().isEmpty()) {
                              fail(tr("The model returned an empty answer. Try again."));
                              return;
                          }
                          m_pendingUserTurn = false;
                          QList<Citation> finalSources = m_currentSources;
                          if (finalSources.isEmpty()) {
                              QList<PassageIndex::Passage> passages;
                              for (const Chapter &ch : chapters)
                                  passages += PassageIndex::split(ch.index, ch.text);
                              finalSources =
                                  citationsFromHits(PassageIndex::byWords(passages, m_answer, 3), chapters, format, 3);
                          }
                          const QString completedAnswer = m_answer;
                          m_answer.clear();
                          m_currentSources.clear();
                          m_conversation.append({false, completedAnswer, finalSources, m_where, m_whereLocal});
                          saveConversation(bookId);
                          emit conversationChanged();
                      });
                  };
                  if (isCloud) {
                      QString fallbackQuery;
                      if (m_conversation.size() >= 3) {
                          for (qsizetype i = m_conversation.size() - 2; i >= 0; --i) {
                              if (m_conversation[i].fromUser) {
                                  fallbackQuery = m_conversation[i].text + QLatin1Char(' ') + asked;
                                  break;
                              }
                          }
                      }
                      QPointer<Companion> self(this);
                      (void)QtConcurrent::run([self, send, chapters, asked, fallbackQuery, format, generation] {
                          QList<PassageIndex::Passage> passages;
                          for (const Chapter &ch : chapters)
                              passages += PassageIndex::split(ch.index, ch.text);
                          QList<PassageIndex::Hit> citeHits = PassageIndex::byWords(passages, asked, 4);
                          if (citeHits.isEmpty() && !fallbackQuery.isEmpty())
                              citeHits = PassageIndex::byWords(passages, fallbackQuery, 4);
                          const QList<Citation> cites = citationsFromHits(citeHits, chapters, format, 4);
                          const QString fullText = joined(chapters, cloudLimit);
                          if (QCoreApplication *app = QCoreApplication::instance()) {
                              QMetaObject::invokeMethod(
                                  app,
                                  [self, send, fullText, cites, generation] {
                                      if (!self || self->m_generation != generation)
                                          return;
                                      send(fullText, QStringLiteral("the whole book up to the reader's place"), cites);
                                  },
                                  Qt::QueuedConnection);
                          }
                      });
                      return;
                  }
                  // On this computer: the passages closest to the question, and the
                  // pages just read, budgeted to the local model's actual context window.
                  QList<PassageIndex::Passage> passages;
                  for (const Chapter &chapter : chapters)
                      passages += PassageIndex::split(chapter.index, chapter.text);
                  QPointer<Companion> self(this);
                  const auto answer = [self, send, chapters, format, generation](const QList<PassageIndex::Hit> &hits, bool) {
                      if (!self || self->m_generation != generation)
                          return;
                      const QList<Citation> sources = citationsFromHits(hits, chapters, format, 4);
                      const qint64 localBudget =
                          self->localCharBudget(QStringLiteral("ask"), false, localLimit);
                      const qint64 recentBudget = qMin<qint64>(4000, qMax<qint64>(1200, localBudget / 4));
                      const qint64 passageBudget = qMax<qint64>(2000, localBudget - recentBudget - 2500);
                      QList<PassageIndex::Hit> ordered = hits;
                      std::sort(ordered.begin(), ordered.end(),
                                [](const PassageIndex::Hit &a, const PassageIndex::Hit &b) {
                                    return a.passage.chapter != b.passage.chapter ? a.passage.chapter < b.passage.chapter
                                                                                  : a.passage.start < b.passage.start;
                                });
                      QString context = QStringLiteral("Passages that may matter:\n\n");
                      qint64 used = 0;
                      for (const PassageIndex::Hit &hit : ordered) {
                          const QString block = passageText(hit, chapters, format) + QStringLiteral("\n\n");
                          if (used > 0 && used + block.size() > passageBudget)
                              break;
                          context += block;
                          used += block.size();
                      }
                      context += QStringLiteral("What the reader read last:\n\n") + joined(chapters, recentBudget);
                      send(context, QStringLiteral("passages found for the question, and the latest pages"), sources);
                  };
                  if (m_ai->embedder()) {
                      // build() skips already-cached passages (0 network calls when warm) and embeds
                      // any newly-reached or grown page-end passage so hybrid search never silently degrades.
                      m_index.build(
                          bookId, passages,
                          [self, generation](double share) {
                              if (self && self->m_generation == generation)
                                  self->setStatus(share > 0.0
                                                      ? tr("Getting to know the book… %1%").arg(qRound(share * 100))
                                                      : tr("Getting to know the book…"));
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

void Companion::recap() { runRecap(false); }

void Companion::refreshRecap() { runRecap(true); }

void Companion::runRecap(bool forceRefresh) {
    const int bookId = m_reader->bookId();
    const int chapter = m_reader->chapter();
    const qint64 pageEnd = m_reader->pageEnd();
    const qint64 pageStart = m_reader->pageStart();
    m_lastAction = {QStringLiteral("recap"), QString(), chapter, pageEnd};
    begin(QStringLiteral("recap"), placeTitle(chapter));
    if (!forceRefresh) {
        const bool memoryValid = m_recapLoaded && !m_recapText.trimmed().isEmpty() && m_recapChapter == chapter &&
                                 (pageEnd <= 0 || m_recapOffset <= pageEnd);
        if (memoryValid) {
            m_answer = m_recapText;
            m_where = m_recapWhere;
            m_whereLocal = m_recapWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit answerChanged();
            emit changed();
            return;
        }
        if (loadRecapSnapshot(bookId, chapter, pageEnd, pageStart, true)) {
            m_answer = m_recapText;
            m_where = m_recapWhere;
            m_whereLocal = m_recapWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit recapChanged();
            emit answerChanged();
            emit changed();
            return;
        }
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud && !checkBudget(m_feature, soFarCharsEstimate(), false, 500))
        return;
    const qint64 limit = cloud ? cloudLimit : localCharBudget(m_feature, false, localLimit);
    const int generation = m_generation;
    setStatus(tr("Reading the book so far…"));
    readSoFar(chapter, pageEnd, limit,
              [this, bookId, chapter, pageEnd, pageStart, limit, generation](const QList<Chapter> &chapters) {
                  if (generation != m_generation)
                      return;
                  // Spoiler-safety guard in case the reader jumped backward while reading disk.
                  if (m_reader && (chapter > m_reader->chapter() ||
                                   (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd()))) {
                      stop();
                      return;
                  }
                  const bool isCloud = cloudFor(QStringLiteral("recap"));
                  const qint64 effectiveLimit =
                      isCloud ? cloudLimit : qMin(limit, localCharBudget(QStringLiteral("recap"), false, localLimit));
                  QString titles;
                  for (const Chapter &ch : chapters)
                      if (!ch.title.isEmpty())
                          titles += QStringLiteral("- ") + ch.title + QLatin1Char('\n');
                  const QString text = joined(chapters, effectiveLimit);
                  if (text.trimmed().isEmpty()) {
                      fail(tr("There isn't any text to summarize yet. Turn a page or two first."));
                      return;
                  }
                  if (!checkBudget(QStringLiteral("recap"), text.size() + titles.size(), false, 500))
                      return;
                  Ai::TextRequest request;
                  request.feature = QStringLiteral("recap");
                  request.stream = true;
                  request.system = instructions(QStringLiteral(
                      "Write \"Previously…\" for a reader coming back to this book: recap the story so far in 2 to 4 "
                      "crisp, engaging paragraphs, focusing most on the latest events and ending right where they "
                      "stopped. Name the key people and tensions that matter right now."));
                  request.messages = {{true, bookLine() + QStringLiteral("\n\nChapters so far:\n") + titles +
                                                 QStringLiteral("\n") + text}};
                  run(QStringLiteral("recap"), request,
                      [this, bookId, chapter, pageEnd, pageStart, generation](Ai::Reply *) {
                          if (generation != m_generation)
                              return;
                          if (m_answer.trimmed().isEmpty()) {
                              fail(tr("The model returned an empty answer. Try again."));
                              return;
                          }
                          if (m_reader &&
                              (chapter > m_reader->chapter() ||
                               (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd())))
                              return;
                          m_recapText = m_answer;
                          m_recapLoaded = true;
                          m_recapChapter = chapter;
                          m_recapOffset = pageEnd;
                          m_recapWhere = m_where;
                          m_recapWhereLocal = m_whereLocal;
                          saveRecapSnapshot(bookId, chapter, pageEnd, pageStart, m_where, m_whereLocal, m_recapText);
                          emit recapChanged();
                      });
              });
}

void Companion::refreshWhosWho() {
    m_peopleKey.clear();
    m_peopleWhere.clear();
    m_peopleWhereLocal = false;
    runWhosWho(true);
}

void Companion::whosWho() { runWhosWho(false); }

void Companion::runWhosWho(bool forceRefresh) {
    const int bookId = m_reader->bookId();
    const int chapter = m_reader->chapter();
    const qint64 pageEnd = m_reader->pageEnd();
    const qint64 pageStart = m_reader->pageStart();
    const QString format = m_reader->format();
    const QString where = m_ai->whereFor(QStringLiteral("whoswho"));
    const QString key = QStringLiteral("%1:%2:%3:%4").arg(bookId).arg(chapter).arg(pageEnd).arg(where);
    m_lastAction = {QStringLiteral("whoswho"), QString(), chapter, pageEnd};
    begin(QStringLiteral("whoswho"), placeTitle(chapter));
    if (!forceRefresh) {
        const bool memoryValid = m_peopleLoaded && m_peopleChapter == chapter &&
                                 (pageEnd <= 0 || m_peopleOffset <= pageEnd) &&
                                 (key == m_peopleKey || !m_people.isEmpty());
        if (memoryValid) {
            m_where = m_peopleWhere.isEmpty() ? where : m_peopleWhere;
            m_whereLocal = m_peopleWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit changed();
            return;
        }
        if (loadWhosWhoSnapshot(bookId, chapter, pageEnd, pageStart, true)) {
            m_where = m_peopleWhere.isEmpty() ? where : m_peopleWhere;
            m_whereLocal = m_peopleWhereLocal;
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit changed();
            return;
        }
    }
    // Never leave a later-chapter snapshot visible if the reader jumped backward.
    if (m_peopleChapter > chapter || (m_peopleChapter == chapter && pageEnd > 0 && m_peopleOffset > pageEnd)) {
        m_people.clear();
        m_peopleLoaded = false;
        m_peopleChapter = -1;
        m_peopleOffset = -1;
        m_peopleKey.clear();
        m_peopleWhere.clear();
        m_peopleWhereLocal = false;
        emit changed();
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud && !checkBudget(m_feature, soFarCharsEstimate(), false, 800))
        return;
    // Budget strictly against the local model's actual context window (2.5 chars/token for JSON/non-Latin safety).
    const qint64 readLimit = cloud ? cloudLimit : localCharBudget(m_feature, false, localWhosWhoLimit, 2.5);
    const int generation = m_generation;
    setStatus(tr("Reading the book so far…"));
    readSoFar(chapter, pageEnd, readLimit,
              [this, bookId, chapter, pageEnd, pageStart, format, key, readLimit, generation](const QList<Chapter> &chapters) {
                  if (generation != m_generation)
                      return;
                  if (m_reader && (chapter > m_reader->chapter() ||
                                   (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd()))) {
                      stop();
                      return;
                  }
                  const bool isCloud = cloudFor(QStringLiteral("whoswho"));
                  const qint64 effectiveLimit =
                      isCloud ? cloudLimit
                              : qMin(readLimit, localCharBudget(QStringLiteral("whoswho"), false, localWhosWhoLimit, 2.5));
                  const QString text = joined(chapters, effectiveLimit);
                  if (text.trimmed().isEmpty()) {
                      m_people.clear();
                      m_peopleLoaded = true;
                      m_peopleChapter = chapter;
                      m_peopleOffset = pageEnd;
                      m_peopleKey = key;
                      m_peopleWhere.clear();
                      m_peopleWhereLocal = false;
                      m_busy = false;
                      m_requestChapter = -1;
                      m_requestOffset = -1;
                      emit changed();
                      return;
                  }
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
                  // Use the full context window on local models (never forcing 24K chars into num_ctx: 8192).
                  request.fast = false;
                  request.schema = QJsonObject{{"type", "object"},
                                               {"properties", QJsonObject{{"people", QJsonObject{{"type", "array"}, {"items", person}}}}},
                                               {"required", QJsonArray{"people"}},
                                               {"additionalProperties", false}};
                  request.system = instructions(QStringLiteral(
                      "List the notable people (and named animals or beings) introduced in the book so far, most "
                      "important first, at most 30: \"name\" as most often called, \"also\" other names or titles "
                      "(or \"\"), \"who\" one crisp, spoiler-free sentence on who they are and their role so far, "
                      "\"chapter\" the exact chapter title or number where they first appear."));
                  request.messages = {{true, bookLine() + QStringLiteral("\n\n") + text}};
                  run(QStringLiteral("whoswho"), request,
                      [this, bookId, chapter, pageEnd, pageStart, format, chapters, key, generation](Ai::Reply *reply) {
                          if (generation != m_generation)
                              return;
                          if (m_reader &&
                              (chapter > m_reader->chapter() ||
                               (chapter == m_reader->chapter() && m_reader->pageEnd() > 0 && pageEnd > m_reader->pageEnd())))
                              return;
                          QJsonObject result = reply->result();
                          if (result.isEmpty())
                              result = extractJsonObject(reply->text());
                          if (result.isEmpty() || !result.contains(QStringLiteral("people")) ||
                              !result.value(QStringLiteral("people")).isArray()) {
                              fail(tr("Who's who couldn't be read from the model's answer. Try again."));
                              return;
                          }
                          m_people.clear();
                          // Names as the book prints them, without its note markers ("Filby*").
                          static const QRegularExpression marker(QStringLiteral("[*†‡§¹²³⁴⁵⁶⁷⁸⁹⁰]+(?=\\s|$)"));
                          for (const QJsonValue &value : result.value(QStringLiteral("people")).toArray()) {
                              QVariantMap personMap = value.toObject().toVariantMap();
                              for (const char *field : {"name", "also"})
                                  personMap[QLatin1String(field)] =
                                      personMap.value(QLatin1String(field)).toString().remove(marker).trimmed();
                              const QString name = personMap.value(QStringLiteral("name")).toString();
                              if (name.isEmpty())
                                  continue;
                              const QString also = personMap.value(QStringLiteral("also")).toString();
                              const QString llmChapter = personMap.value(QStringLiteral("chapter")).toString();
                              const FirstSeen seen = resolveFirstSeen(chapters, name, also, llmChapter, format);
                              personMap.insert(QStringLiteral("chapter"), seen.label);
                              personMap.insert(QStringLiteral("chapterIndex"), seen.chapterIndex);
                              personMap.insert(QStringLiteral("offset"), seen.offset);
                              m_people.append(personMap);
                          }
                          m_peopleLoaded = true;
                          m_peopleChapter = chapter;
                          m_peopleOffset = pageEnd;
                          m_peopleKey = key;
                          m_peopleWhere = m_where;
                          m_peopleWhereLocal = m_whereLocal;
                          m_answer.clear();
                          saveWhosWhoSnapshot(bookId, chapter, pageEnd, pageStart, m_where, m_whereLocal, m_people);
                      });
              });
}
