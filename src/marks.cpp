#include "marks.h"

#include "library.h"
#include "librarymodel.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace {
// An empty text, never a null one: a null QString is SQL's NULL, which the
// columns refuse.
QVariant text(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

bool before(const Mark &a, const Mark &b) {
    return a.chapter != b.chapter ? a.chapter < b.chapter : a.start != b.start ? a.start < b.start : a.id < b.id;
}

QString colorTitle(const QString &color) {
    if (color.isEmpty())
        return {};
    return color.left(1).toUpper() + color.mid(1).toLower();
}

QString formatBlockquote(const QString &raw) {
    QString quote = raw;
    quote.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    quote.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = quote.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    for (const QString &line : lines)
        out.append(line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line);
    return out.join(QLatin1Char('\n'));
}

QString yamlEscape(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString safeFileName(QString title) {
    title = title.trimmed();
    static const QRegularExpression bad(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f]+)"));
    title.replace(bad, QStringLiteral("-"));
    title.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    title = title.left(120).trimmed();
    return title.isEmpty() ? QStringLiteral("dogear-notes") : title;
}

// Scores how closely `haystack` around [pos, pos + len) matches expected prefix/suffix.
int contextScore(const QString &haystack, qsizetype pos, qsizetype len, const QString &prefix, const QString &suffix) {
    int score = 0;
    if (!prefix.isEmpty()) {
        const qsizetype avail = qMin(pos, prefix.size());
        qsizetype matched = 0;
        while (matched < avail && haystack[pos - 1 - matched] == prefix[prefix.size() - 1 - matched])
            ++matched;
        score += int(matched) * 4;
        if (matched == prefix.size())
            score += 40;
    }
    if (!suffix.isEmpty()) {
        const qsizetype afterPos = pos + len;
        const qsizetype avail = qMin(haystack.size() - afterPos, suffix.size());
        qsizetype matched = 0;
        while (matched < avail && haystack[afterPos + matched] == suffix[matched])
            ++matched;
        score += int(matched) * 4;
        if (matched == suffix.size())
            score += 40;
    }
    return score;
}

// A mark made, or taken away: undoing one does the other. The mark keeps
// its id across both, so later changes to it still find it.
class Place : public QUndoCommand {
public:
    Place(Marks *marks, const Mark &mark, bool adding, const QString &text)
        : QUndoCommand(text), m_marks(marks), m_mark(mark), m_adding(adding) {}
    void redo() override { m_adding ? put() : take(); }
    void undo() override { m_adding ? take() : put(); }
    int id() const { return m_mark.id; }

private:
    void put() {
        const int inserted = m_marks->insert(m_mark);
        if (inserted > 0)
            m_mark.id = inserted;
    }
    void take() {
        if (m_mark.id <= 0)
            return;
        const Mark found = m_marks->find(m_mark.id);
        if (found.id != 0)
            m_mark = found;
        m_marks->erase(m_mark.id);
    }
    Marks *m_marks;
    Mark m_mark;
    bool m_adding;
};

class Change : public QUndoCommand {
public:
    Change(Marks *marks, const Mark &before, const Mark &after, const QString &text)
        : QUndoCommand(text), m_marks(marks), m_before(before), m_after(after) {}
    void redo() override { m_marks->update(m_after); }
    void undo() override { m_marks->update(m_before); }

private:
    Marks *m_marks;
    Mark m_before;
    Mark m_after;
};
}  // namespace

Marks::Marks(Library *library, QObject *parent)
    : QAbstractListModel(parent), m_library(library), m_undo(new QUndoStack(this)) {
    connect(m_undo, &QUndoStack::canUndoChanged, this, &Marks::undoChanged);
    connect(m_undo, &QUndoStack::canRedoChanged, this, &Marks::undoChanged);
    connect(m_undo, &QUndoStack::undoTextChanged, this, &Marks::undoChanged);
}

Marks::~Marks() = default;

bool Marks::canUndo() const { return m_undo->canUndo(); }
bool Marks::canRedo() const { return m_undo->canRedo(); }
QString Marks::undoText() const { return m_undo->undoText(); }

int Marks::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_marks.size()); }

QVariant Marks::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_marks.size())
        return {};
    const Mark &mark = m_marks[index.row()];
    switch (role) {
    case IdRole: return mark.id;
    case KindRole: return mark.kind;
    case ChapterRole: return mark.chapter;
    case StartRole: return mark.start;
    case EndRole: return mark.end;
    case TextRole: return mark.text;
    case NoteRole: return mark.note;
    case ColorRole: return mark.color;
    case LabelRole: return mark.label;
    case CreatedRole: return mark.created;
    case PrefixRole: return mark.prefix;
    case SuffixRole: return mark.suffix;
    default: return {};
    }
}

QHash<int, QByteArray> Marks::roleNames() const {
    return {{IdRole, "markId"},  {KindRole, "kind"},       {ChapterRole, "chapter"}, {StartRole, "start"},
            {EndRole, "end"},    {TextRole, "quote"},      {NoteRole, "note"},       {ColorRole, "color"},
            {LabelRole, "label"}, {CreatedRole, "created"}, {PrefixRole, "prefix"},   {SuffixRole, "suffix"}};
}

void Marks::setBook(int bookId) {
    if (bookId == m_bookId)
        return;
    m_bookId = bookId;
    // Undo belongs to the book it was done in.
    m_undo->clear();
    load();
}

void Marks::load() {
    beginResetModel();
    m_marks.clear();
    if (m_bookId > 0) {
        QSqlQuery query(m_library->database());
        query.prepare(QStringLiteral("SELECT id, kind, chapter, start, finish, text, note, color, label, created_at,"
                                     " updated_at, prefix, suffix FROM annotations WHERE book_id = ?"));
        query.addBindValue(m_bookId);
        if (query.exec())
            while (query.next()) {
                Mark mark;
                mark.id = query.value(0).toInt();
                mark.kind = query.value(1).toString();
                mark.chapter = query.value(2).toInt();
                mark.start = query.value(3).toLongLong();
                mark.end = query.value(4).toLongLong();
                mark.text = query.value(5).toString();
                mark.note = query.value(6).toString();
                mark.color = query.value(7).toString();
                mark.label = query.value(8).toString();
                mark.created = QDateTime::fromSecsSinceEpoch(query.value(9).toLongLong());
                mark.updated = QDateTime::fromSecsSinceEpoch(query.value(10).toLongLong());
                mark.prefix = query.value(11).toString();
                mark.suffix = query.value(12).toString();
                m_marks.append(mark);
            }
        std::sort(m_marks.begin(), m_marks.end(), before);
    }
    endResetModel();
    emit changed();
}

int Marks::rowOf(int id) const {
    for (int i = 0; i < m_marks.size(); ++i)
        if (m_marks[i].id == id)
            return i;
    return -1;
}

Mark Marks::find(int id) const {
    const int row = rowOf(id);
    return row >= 0 ? m_marks[row] : Mark{};
}

int Marks::insert(Mark mark) {
    if (m_bookId <= 0)
        return 0;
    if (!mark.created.isValid())
        mark.created = QDateTime::currentDateTime();
    mark.updated = QDateTime::currentDateTime();
    QSqlQuery query(m_library->database());
    // A mark put back by undo keeps its id.
    query.prepare(mark.id > 0
                      ? QStringLiteral("INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color,"
                                       " label, prefix, suffix, created_at, updated_at)"
                                       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                      : QStringLiteral("INSERT INTO annotations (book_id, kind, chapter, start, finish, text, note, color,"
                                       " label, prefix, suffix, created_at, updated_at)"
                                       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    if (mark.id > 0)
        query.addBindValue(mark.id);
    for (const QVariant &value : {QVariant(m_bookId), text(mark.kind), QVariant(mark.chapter), QVariant(mark.start),
                                  QVariant(mark.end), text(mark.text), text(mark.note), text(mark.color),
                                  text(mark.label), text(mark.prefix), text(mark.suffix),
                                  QVariant(mark.created.toSecsSinceEpoch()), QVariant(mark.updated.toSecsSinceEpoch())})
        query.addBindValue(value);
    if (!query.exec()) {
        qWarning("marks: %s", qPrintable(query.lastError().text()));
        return 0;
    }
    mark.id = query.lastInsertId().toInt();
    const auto at = std::lower_bound(m_marks.begin(), m_marks.end(), mark, before);
    const int row = int(at - m_marks.begin());
    beginInsertRows({}, row, row);
    m_marks.insert(row, mark);
    endInsertRows();
    emit changed();
    return mark.id;
}

void Marks::erase(int id) {
    const int row = rowOf(id);
    if (row < 0)
        return;
    QSqlQuery query(m_library->database());
    query.prepare(QStringLiteral("DELETE FROM annotations WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        qWarning("marks: %s", qPrintable(query.lastError().text()));
        return;
    }
    beginRemoveRows({}, row, row);
    m_marks.removeAt(row);
    endRemoveRows();
    emit changed();
}

void Marks::update(const Mark &mark) {
    const int row = rowOf(mark.id);
    if (row < 0)
        return;
    Mark changed = mark;
    changed.updated = QDateTime::currentDateTime();
    QSqlQuery query(m_library->database());
    query.prepare(QStringLiteral("UPDATE annotations SET start = ?, finish = ?, note = ?, color = ?, prefix = ?, "
                                 "suffix = ?, updated_at = ? WHERE id = ?"));
    query.addBindValue(changed.start);
    query.addBindValue(changed.end);
    query.addBindValue(text(changed.note));
    query.addBindValue(text(changed.color));
    query.addBindValue(text(changed.prefix));
    query.addBindValue(text(changed.suffix));
    query.addBindValue(changed.updated.toSecsSinceEpoch());
    query.addBindValue(changed.id);
    if (!query.exec()) {
        qWarning("marks: %s", qPrintable(query.lastError().text()));
        return;
    }
    const bool reordered = m_marks[row].start != changed.start || m_marks[row].chapter != changed.chapter;
    m_marks[row] = changed;
    if (reordered) {
        beginResetModel();
        std::sort(m_marks.begin(), m_marks.end(), before);
        endResetModel();
    } else {
        emit dataChanged(index(row), index(row));
    }
    emit this->changed();
}

int Marks::addBookmark(int chapter, qint64 offset, const QString &text, const QString &label, const QString &prefix,
                       const QString &suffix) {
    if (m_bookId <= 0 || !m_library->database().isOpen())
        return 0;
    Mark mark;
    mark.kind = QStringLiteral("bookmark");
    mark.chapter = chapter;
    mark.start = mark.end = offset;
    mark.text = text.simplified().left(200);
    mark.label = label;
    mark.prefix = prefix.simplified().right(80);
    mark.suffix = suffix.simplified().left(80);
    auto *command = new Place(this, mark, true, tr("Add the bookmark"));
    m_undo->push(command);
    return command->id();
}

int Marks::addHighlight(int chapter, qint64 start, qint64 end, const QString &text, const QString &color,
                        const QString &label, const QString &prefix, const QString &suffix) {
    if (end <= start || m_bookId <= 0 || !m_library->database().isOpen())
        return 0;
    Mark mark;
    mark.kind = QStringLiteral("highlight");
    mark.chapter = chapter;
    mark.start = start;
    mark.end = end;
    mark.text = text.simplified();
    mark.color = color.isEmpty() ? QStringLiteral("yellow") : color;
    mark.label = label;
    mark.prefix = prefix.simplified().right(80);
    mark.suffix = suffix.simplified().left(80);
    auto *command = new Place(this, mark, true, tr("Highlight"));
    m_undo->push(command);
    return command->id();
}

void Marks::setNote(int id, const QString &note) {
    const Mark before = find(id);
    if (before.id == 0 || before.note == note.trimmed())
        return;
    Mark after = before;
    after.note = note.trimmed();
    m_undo->push(new Change(this, before, after, tr("Edit the note")));
}

void Marks::setColor(int id, const QString &color) {
    const Mark before = find(id);
    if (before.id == 0 || before.color == color)
        return;
    Mark after = before;
    after.color = color;
    m_undo->push(new Change(this, before, after, tr("Change the color")));
}

void Marks::remove(int id) {
    const Mark mark = find(id);
    if (mark.id == 0)
        return;
    m_undo->push(new Place(this, mark, false,
                           mark.kind == QStringLiteral("bookmark") ? tr("Remove the bookmark") : tr("Remove the highlight")));
}

void Marks::undo() { m_undo->undo(); }
void Marks::redo() { m_undo->redo(); }

QVariantMap Marks::mark(int id) const {
    const Mark mark = find(id);
    if (mark.id == 0)
        return {};
    return {{"id", mark.id},         {"kind", mark.kind},   {"chapter", mark.chapter}, {"start", mark.start},
            {"end", mark.end},       {"text", mark.text},   {"note", mark.note},       {"color", mark.color},
            {"label", mark.label},   {"prefix", mark.prefix}, {"suffix", mark.suffix}, {"created", mark.created}};
}

int Marks::bookmarkBetween(int chapter, qint64 from, qint64 to) const {
    for (const Mark &mark : m_marks)
        if (mark.kind == QStringLiteral("bookmark") && mark.chapter == chapter && mark.start >= from && mark.start < to)
            return mark.id;
    return 0;
}

int Marks::highlightAt(int chapter, qint64 offset) const {
    for (const Mark &mark : m_marks)
        if (mark.kind == QStringLiteral("highlight") && mark.chapter == chapter && offset >= mark.start && offset < mark.end)
            return mark.id;
    return 0;
}

QVariantList Marks::highlightsIn(int chapter) const {
    QVariantList spans;
    for (const Mark &mark : m_marks)
        if (mark.kind == QStringLiteral("highlight") && mark.chapter == chapter)
            spans.append(QVariantMap{{"id", mark.id},
                                     {"start", mark.start},
                                     {"end", mark.end},
                                     {"color", mark.color},
                                     {"noted", !mark.note.isEmpty()}});
    return spans;
}

QVariantList Marks::filtered(const QString &filter) const {
    QVariantList out;
    for (const Mark &mark : m_marks) {
        const bool included = filter.isEmpty() || filter == QLatin1String("all") ||
                              (filter == QLatin1String("highlights") && mark.kind == QLatin1String("highlight")) ||
                              (filter == QLatin1String("notes") && !mark.note.isEmpty()) ||
                              (filter == QLatin1String("bookmarks") && mark.kind == QLatin1String("bookmark"));
        if (!included)
            continue;
        out.append(QVariantMap{{"markId", mark.id},
                               {"kind", mark.kind},
                               {"chapter", mark.chapter},
                               {"start", mark.start},
                               {"end", mark.end},
                               {"quote", mark.text},
                               {"note", mark.note},
                               {"color", mark.color},
                               {"label", mark.label},
                               {"prefix", mark.prefix},
                               {"suffix", mark.suffix},
                               {"created", mark.created}});
    }
    return out;
}

QVariantList Marks::searchAll(const QString &query, int limit) const {
    QVariantList out;
    if (!m_library || !m_library->database().isOpen() || limit <= 0)
        return out;
    const QStringList tokens = WorksModel::searchable(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QSqlQuery sql(m_library->database());
    sql.prepare(QStringLiteral(
        "SELECT a.id, a.book_id, a.kind, a.chapter, a.start, a.finish, a.text, a.note, a.color, a.label, "
        "a.created_at, a.updated_at, b.title, b.author "
        "FROM annotations a "
        "JOIN books b ON b.id = a.book_id "
        "WHERE b.hidden = 0 AND b.missing = 0 "
        "ORDER BY a.updated_at DESC, a.id DESC"));
    if (!sql.exec())
        return out;

    struct ScoredHit {
        int score = 0;
        qint64 updatedAt = 0;
        QVariantMap map;
    };
    QList<ScoredHit> hits;
    while (sql.next()) {
        const int id = sql.value(0).toInt();
        const int bookId = sql.value(1).toInt();
        const QString kind = sql.value(2).toString();
        const int chapter = sql.value(3).toInt();
        const qint64 start = sql.value(4).toLongLong();
        const qint64 end = sql.value(5).toLongLong();
        const QString quote = sql.value(6).toString();
        const QString note = sql.value(7).toString();
        const QString color = sql.value(8).toString();
        const QString label = sql.value(9).toString();
        const qint64 createdAt = sql.value(10).toLongLong();
        const qint64 updatedAt = sql.value(11).toLongLong();
        const QString bookTitle = sql.value(12).toString();
        const QString bookAuthor = sql.value(13).toString();

        int score = (bookId == m_bookId && m_bookId > 0) ? 20 : 0;
        if (!tokens.isEmpty()) {
            const QString hayNote = WorksModel::searchable(note);
            const QString hayText = WorksModel::searchable(quote);
            const QString hayMeta = WorksModel::searchable(label + QLatin1Char(' ') + bookTitle + QLatin1Char(' ') + bookAuthor);
            const QString combined = hayNote + QLatin1Char(' ') + hayText + QLatin1Char(' ') + hayMeta;
            bool ok = true;
            for (const QString &tok : tokens) {
                if (!combined.contains(tok)) {
                    ok = false;
                    break;
                }
                if (hayNote.contains(tok))
                    score += 30;
                if (hayText.contains(tok))
                    score += 20;
                if (hayMeta.contains(tok))
                    score += 10;
            }
            if (!ok)
                continue;
            const QString trimmed = query.trimmed();
            if (!trimmed.isEmpty()) {
                if (note.contains(trimmed, Qt::CaseInsensitive))
                    score += 40;
                if (quote.contains(trimmed, Qt::CaseInsensitive))
                    score += 30;
            }
        }
        hits.append({score, updatedAt,
                     QVariantMap{{"id", id},
                                 {"bookId", bookId},
                                 {"bookTitle", bookTitle},
                                 {"bookAuthor", bookAuthor},
                                 {"kind", kind},
                                 {"chapter", chapter},
                                 {"start", start},
                                 {"end", end},
                                 {"text", quote},
                                 {"note", note},
                                 {"color", color},
                                 {"label", label},
                                 {"created", QDateTime::fromSecsSinceEpoch(createdAt)},
                                 {"updated", QDateTime::fromSecsSinceEpoch(updatedAt)}}});
        if (tokens.isEmpty() && hits.size() >= limit)
            break;
    }

    if (!tokens.isEmpty()) {
        std::sort(hits.begin(), hits.end(), [](const ScoredHit &a, const ScoredHit &b) {
            return a.score != b.score ? a.score > b.score : a.updatedAt > b.updatedAt;
        });
    }
    const int n = qMin(limit, int(hits.size()));
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        out.append(hits[i].map);
    return out;
}

QString Marks::markdown(const QString &title, const QString &author, const QString &format) const {
    const bool obsidian = format.compare(QStringLiteral("obsidian"), Qt::CaseInsensitive) == 0 ||
                          format.compare(QStringLiteral("readwise"), Qt::CaseInsensitive) == 0;
    QString out;
    if (obsidian) {
        out += QStringLiteral("---\n");
        out += QStringLiteral("title: %1\n").arg(yamlEscape(title));
        if (!author.isEmpty())
            out += QStringLiteral("author: %1\n").arg(yamlEscape(author));
        out += QStringLiteral("exported: %1\n").arg(QDate::currentDate().toString(Qt::ISODate));
        out += QStringLiteral("highlights: %1\n").arg(m_marks.size());
        out += QStringLiteral("tags:\n  - dogear\n  - reading-notes\n");
        out += QStringLiteral("---\n\n");
    }
    out += QStringLiteral("# %1\n").arg(title);
    if (!author.isEmpty())
        out += QStringLiteral("\n*%1*\n").arg(author);
    QString place;
    for (const Mark &mark : m_marks) {
        if (mark.label != place && !mark.label.isEmpty()) {
            place = mark.label;
            out += QStringLiteral("\n## %1\n").arg(place);
        }
        const QString dateStr = mark.created.isValid() ? mark.created.date().toString(Qt::ISODate) : QString();
        if (mark.kind == QStringLiteral("bookmark")) {
            out += QStringLiteral("\n- \U0001F516 %1").arg(mark.text);
            if (obsidian && !dateStr.isEmpty())
                out += QStringLiteral(" *(%1)*").arg(dateStr);
            out += QLatin1Char('\n');
        } else {
            out += QLatin1Char('\n') + formatBlockquote(mark.text) + QLatin1Char('\n');
        }
        if (!mark.note.isEmpty())
            out += QStringLiteral("\n%1\n").arg(mark.note);
        if (obsidian) {
            QStringList meta;
            if (!mark.color.isEmpty())
                meta << colorTitle(mark.color);
            if (!mark.label.isEmpty())
                meta << mark.label;
            if (!dateStr.isEmpty())
                meta << dateStr;
            if (!meta.isEmpty())
                out += QStringLiteral("\n— *%1*\n").arg(meta.join(QStringLiteral(" · ")));
        }
    }
    return out;
}

QString Marks::exportMarkdownToFile(const QString &filePath, const QString &title, const QString &author,
                                    const QString &format) const {
    QString target = filePath.trimmed();
    if (target.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        const QUrl url(target);
        if (url.isLocalFile())
            target = url.toLocalFile();
    }
    if (target.isEmpty()) {
        QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (docs.isEmpty())
            docs = QDir::homePath();
        target = QDir(docs).filePath(QStringLiteral("Dogear Notes/%1.md").arg(safeFileName(title)));
    }
    const QFileInfo info(target);
    if (!QDir().mkpath(info.absolutePath())) {
        qWarning("marks: failed to create directory %s", qPrintable(info.absolutePath()));
        return {};
    }
    const QByteArray payload = markdown(title, author, format).toUtf8();
    QSaveFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit()) {
        qWarning("marks: failed to write %s: %s", qPrintable(info.absoluteFilePath()), qPrintable(file.errorString()));
        return {};
    }
    return info.absoluteFilePath();
}

QPair<qint64, qint64> Marks::locateQuote(const QString &chapterText, const QString &quote, const QString &prefix,
                                         const QString &suffix, qint64 hintStart) {
    if (chapterText.isEmpty() || quote.isEmpty())
        return {-1, -1};

    // Fast path: hint offset still matches verbatim.
    if (hintStart >= 0 && hintStart + quote.size() <= chapterText.size() &&
        chapterText.mid(hintStart, quote.size()) == quote) {
        return {hintStart, hintStart + quote.size()};
    }

    // Pass 1: exact substring search, disambiguated by W3C prefix/suffix and proximity.
    qint64 bestPos = -1;
    int bestScore = -1;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    qsizetype pos = 0;
    while ((pos = chapterText.indexOf(quote, pos)) != -1) {
        const int score = contextScore(chapterText, pos, quote.size(), prefix, suffix);
        const qint64 dist = hintStart >= 0 ? std::abs(qint64(pos) - hintStart) : qint64(pos);
        if (score > bestScore || (score == bestScore && dist < bestDist)) {
            bestScore = score;
            bestDist = dist;
            bestPos = qint64(pos);
        }
        pos += 1;
    }
    if (bestPos >= 0)
        return {bestPos, bestPos + quote.size()};

    // Pass 2: whitespace-collapsed search (marks store simplified quotes).
    const QString normQuote = quote.simplified();
    if (normQuote.isEmpty())
        return {-1, -1};
    QString collapsed;
    collapsed.reserve(chapterText.size());
    QList<int> origIdx;
    origIdx.reserve(chapterText.size());
    bool inSpace = true;
    for (int i = 0; i < chapterText.size(); ++i) {
        const QChar c = chapterText[i];
        if (c.isSpace()) {
            if (!inSpace) {
                collapsed.append(QLatin1Char(' '));
                origIdx.append(i);
                inSpace = true;
            }
        } else {
            collapsed.append(c);
            origIdx.append(i);
            inSpace = false;
        }
    }
    if (!collapsed.isEmpty() && collapsed.endsWith(QLatin1Char(' '))) {
        collapsed.chop(1);
        origIdx.removeLast();
    }

    const QString normPrefix = prefix.simplified();
    const QString normSuffix = suffix.simplified();
    qint64 bestNormStart = -1;
    qint64 bestNormEnd = -1;
    bestScore = -1;
    bestDist = std::numeric_limits<qint64>::max();
    pos = 0;
    while ((pos = collapsed.indexOf(normQuote, pos)) != -1) {
        const int score = contextScore(collapsed, pos, normQuote.size(), normPrefix, normSuffix);
        const qint64 mappedStart = origIdx[int(pos)];
        const qint64 mappedEnd = origIdx[int(pos + normQuote.size() - 1)] + 1;
        const qint64 dist = hintStart >= 0 ? std::abs(mappedStart - hintStart) : mappedStart;
        if (score > bestScore || (score == bestScore && dist < bestDist)) {
            bestScore = score;
            bestDist = dist;
            bestNormStart = mappedStart;
            bestNormEnd = mappedEnd;
        }
        pos += 1;
    }
    if (bestNormStart >= 0)
        return {bestNormStart, bestNormEnd};

    return {-1, -1};
}

QVariantMap Marks::resolveOffset(int id, const QString &chapterText) const {
    const Mark m = find(id);
    if (m.id == 0)
        return {{"found", false}, {"start", -1}, {"end", -1}, {"drifted", false}};
    const auto span = locateQuote(chapterText, m.text, m.prefix, m.suffix, m.start);
    if (span.first < 0)
        return {{"found", false}, {"start", m.start}, {"end", m.end}, {"drifted", false}};
    const qint64 resolvedEnd = m.kind == QLatin1String("bookmark") ? span.first : span.second;
    const bool drifted = span.first != m.start || resolvedEnd != m.end;
    return {{"found", true}, {"start", span.first}, {"end", resolvedEnd}, {"drifted", drifted}};
}

bool Marks::healMark(int id, const QString &chapterText) {
    Mark m = find(id);
    if (m.id == 0)
        return false;
    const auto span = locateQuote(chapterText, m.text, m.prefix, m.suffix, m.start);
    if (span.first < 0)
        return false;
    const qint64 resolvedEnd = m.kind == QLatin1String("bookmark") ? span.first : span.second;
    if (span.first == m.start && resolvedEnd == m.end)
        return false;
    m.start = span.first;
    m.end = resolvedEnd;
    update(m);
    return true;
}

int Marks::healChapter(int chapter, const QString &chapterText) {
    QList<int> ids;
    for (const Mark &m : std::as_const(m_marks))
        if (m.chapter == chapter)
            ids.append(m.id);
    int healed = 0;
    for (const int id : ids)
        if (healMark(id, chapterText))
            ++healed;
    return healed;
}
