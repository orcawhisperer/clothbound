#include "library/marks.h"

#include "library/library.h"
#include "library/librarymodel.h"
#include "library/libraryschema.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

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
    return title.isEmpty() ? QStringLiteral("clothbound-notes") : title;
}

// Scores how closely `haystack` around [pos, pos + len) matches expected prefix/suffix.
// Tolerates boundary and internal whitespace differences since stored marks simplify context.
int contextScore(const QString &haystack, qsizetype pos, qsizetype len, const QString &prefix, const QString &suffix) {
    int score = 0;
    if (!prefix.isEmpty()) {
        qsizetype h = pos;
        if (!prefix.back().isSpace())
            while (h > 0 && haystack[h - 1].isSpace())
                --h;
        qsizetype p = prefix.size();
        qsizetype matched = 0;
        while (p > 0 && h > 0) {
            if (prefix[p - 1].isSpace()) {
                if (!haystack[h - 1].isSpace())
                    break;
                while (h > 0 && haystack[h - 1].isSpace())
                    --h;
                while (p > 0 && prefix[p - 1].isSpace()) {
                    --p;
                    ++matched;
                }
            } else {
                if (haystack[h - 1] != prefix[p - 1])
                    break;
                --h;
                --p;
                ++matched;
            }
        }
        score += int(matched) * 4;
        if (matched == prefix.size())
            score += 40;
    }
    if (!suffix.isEmpty()) {
        qsizetype h = pos + len;
        if (!suffix.front().isSpace())
            while (h < haystack.size() && haystack[h].isSpace())
                ++h;
        qsizetype p = 0;
        qsizetype matched = 0;
        while (p < suffix.size() && h < haystack.size()) {
            if (suffix[p].isSpace()) {
                if (!haystack[h].isSpace())
                    break;
                while (h < haystack.size() && haystack[h].isSpace())
                    ++h;
                while (p < suffix.size() && suffix[p].isSpace()) {
                    ++p;
                    ++matched;
                }
            } else {
                if (haystack[h] != suffix[p])
                    break;
                ++h;
                ++p;
                ++matched;
            }
        }
        score += int(matched) * 4;
        if (matched == suffix.size())
            score += 40;
    }
    return score;
}

// A mark made, or taken away: undoing one does the other. Commands share a
// mutable id handle (`m_idToken`) so that if SQLite's rowid is reused
// externally while a mark is deleted, undoing the delete allocates a fresh
// non-colliding id and rebinds every undo command for that logical mark.
class Place : public QUndoCommand {
public:
    Place(Marks *marks, const Mark &mark, std::shared_ptr<int> idToken, bool adding, const QString &text)
        : QUndoCommand(text), m_marks(marks), m_mark(mark), m_idToken(std::move(idToken)), m_adding(adding) {}
    void redo() override { m_adding ? put() : take(); }
    void undo() override { m_adding ? take() : put(); }
    int id() const { return *m_idToken; }

private:
    void put() {
        const int prevId = *m_idToken;
        m_mark.id = prevId;
        const int inserted = m_marks->insert(m_mark);
        if (inserted > 0) {
            m_mark.id = inserted;
            *m_idToken = inserted;
            if (prevId <= 0)
                m_marks->registerIdToken(inserted, m_idToken);
            else if (prevId != inserted)
                m_marks->remapIdToken(prevId, inserted, m_idToken);
        }
    }
    void take() {
        const int curId = *m_idToken;
        if (curId <= 0)
            return;
        const Mark found = m_marks->find(curId);
        if (found.id != 0)
            m_mark = found;
        m_marks->erase(curId);
    }
    Marks *m_marks;
    Mark m_mark;
    std::shared_ptr<int> m_idToken;
    bool m_adding;
};

// Edits a single user-editable field (note or color) against the live mark
// state, so undoing/redoing after healChapter() never clobbers freshly healed
// offsets with stale pre-heal start/end values.
class Change : public QUndoCommand {
public:
    enum class Field { Note, Color };

    Change(Marks *marks, std::shared_ptr<int> idToken, Field field, QString beforeValue, QString afterValue,
           const QString &text)
        : QUndoCommand(text),
          m_marks(marks),
          m_idToken(std::move(idToken)),
          m_field(field),
          m_before(std::move(beforeValue)),
          m_after(std::move(afterValue)) {}
    void redo() override { apply(m_after); }
    void undo() override { apply(m_before); }

private:
    void apply(const QString &value) {
        const int curId = *m_idToken;
        if (curId <= 0)
            return;
        Mark current = m_marks->find(curId);
        if (current.id == 0)
            return;
        if (m_field == Field::Note)
            current.note = value;
        else
            current.color = value;
        m_marks->update(current);
    }

    Marks *m_marks;
    std::shared_ptr<int> m_idToken;
    Field m_field;
    QString m_before;
    QString m_after;
};
}  // namespace

Marks::Marks(Library *library, QObject *parent)
    : QAbstractListModel(parent), m_library(library), m_undo(new QUndoStack(this)) {
    Q_ASSERT(m_library != nullptr);
    connect(m_undo, &QUndoStack::canUndoChanged, this, &Marks::undoChanged);
    connect(m_undo, &QUndoStack::canRedoChanged, this, &Marks::undoChanged);
    connect(m_undo, &QUndoStack::undoTextChanged, this, &Marks::undoChanged);
}

Marks::~Marks() = default;

bool Marks::canUndo() const { return m_undo->canUndo(); }
bool Marks::canRedo() const { return m_undo->canRedo(); }
QString Marks::undoText() const { return m_undo->undoText(); }

int Marks::rowCount(const QModelIndex &parent) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return parent.isValid() ? 0 : int(m_marks.size());
}

QVariant Marks::data(const QModelIndex &index, int role) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!index.isValid() || index.row() < 0 || index.row() >= m_marks.size())
        return {};
    Q_ASSERT(index.row() >= 0 && index.row() < m_marks.size());
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
    Q_ASSERT(QThread::currentThread() == thread());
    if (bookId == m_bookId)
        return;
    m_bookId = bookId;
    // Undo belongs to the book it was done in.
    m_undo->clear();
    m_idTokens.clear();
    load();
}

void Marks::load() {
    Q_ASSERT(QThread::currentThread() == thread());
    beginResetModel();
    m_marks.clear();
    if (m_bookId > 0) {
        QSqlDatabase db = m_library->database();
        {
            QSqlQuery maxQ(db);
            if (maxQ.exec(QStringLiteral("SELECT COALESCE(MAX(id), 0) + 1 FROM annotations")) && maxQ.next())
                m_minNextId = qMax(m_minNextId, maxQ.value(0).toInt());
        }
        QSqlQuery query(db);
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

std::shared_ptr<int> Marks::idTokenFor(int id) {
    auto it = m_idTokens.constFind(id);
    if (it != m_idTokens.cend())
        return *it;
    auto token = std::make_shared<int>(id);
    m_idTokens.insert(id, token);
    return token;
}

void Marks::registerIdToken(int id, const std::shared_ptr<int> &token) {
    if (id > 0 && token)
        m_idTokens.insert(id, token);
}

void Marks::remapIdToken(int oldId, int newId, const std::shared_ptr<int> &token) {
    if (oldId > 0) {
        const auto it = m_idTokens.find(oldId);
        if (it != m_idTokens.end() && it.value() == token)
            m_idTokens.erase(it);
    }
    if (newId > 0 && token)
        m_idTokens.insert(newId, token);
}

int Marks::insert(Mark mark) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_bookId <= 0)
        return 0;
    if (!mark.created.isValid())
        mark.created = QDateTime::currentDateTime();
    mark.updated = QDateTime::currentDateTime();
    QSqlDatabase db = m_library->database();

    int dbNextId = 1;
    {
        QSqlQuery maxQ(db);
        if (maxQ.exec(QStringLiteral("SELECT COALESCE(MAX(id), 0) + 1 FROM annotations")) && maxQ.next())
            dbNextId = qMax(1, maxQ.value(0).toInt());
    }
    m_minNextId = qMax(m_minNextId, dbNextId);

    // Guard undo-of-delete against SQLite rowid reuse: if an external writer
    // claimed `mark.id` while this mark was deleted, allocate a fresh id rather
    // than colliding with or losing the mark. Likewise, for brand-new marks
    // (`mark.id <= 0`), avoid reusing a recently deleted id from this session.
    bool useExplicitId = false;
    if (mark.id > 0) {
        QSqlQuery exists(db);
        exists.prepare(QStringLiteral("SELECT 1 FROM annotations WHERE id = ?"));
        exists.addBindValue(mark.id);
        if (exists.exec() && exists.next()) {
            mark.id = m_minNextId;
            useExplicitId = true;
        } else {
            useExplicitId = true;
        }
    } else if (m_minNextId > dbNextId) {
        mark.id = m_minNextId;
        useExplicitId = true;
    }

    QSqlQuery query(db);
    query.prepare(useExplicitId
                      ? QStringLiteral("INSERT INTO annotations (id, book_id, kind, chapter, start, finish, text, note, color,"
                                       " label, prefix, suffix, created_at, updated_at)"
                                       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                      : QStringLiteral("INSERT INTO annotations (book_id, kind, chapter, start, finish, text, note, color,"
                                       " label, prefix, suffix, created_at, updated_at)"
                                       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    if (useExplicitId)
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
    m_minNextId = qMax(m_minNextId, mark.id + 1);
    const auto at = std::lower_bound(m_marks.begin(), m_marks.end(), mark, before);
    const int row = int(at - m_marks.begin());
    Q_ASSERT(row >= 0 && row <= m_marks.size());
    beginInsertRows({}, row, row);
    m_marks.insert(row, mark);
    endInsertRows();
    Q_ASSERT(m_marks[row].id == mark.id);
    if (mark.kind == QLatin1String("highlight"))
        m_library->adjustHighlightCount(m_bookId, 1);
    emit changed();
    return mark.id;
}

void Marks::erase(int id) {
    Q_ASSERT(QThread::currentThread() == thread());
    const int row = rowOf(id);
    if (row < 0)
        return;
    Q_ASSERT(row >= 0 && row < m_marks.size());
    const bool wasHighlight = (m_marks[row].kind == QLatin1String("highlight"));
    QSqlQuery query(m_library->database());
    query.prepare(QStringLiteral("DELETE FROM annotations WHERE id = ? AND book_id = ?"));
    query.addBindValue(id);
    query.addBindValue(m_bookId);
    if (!query.exec()) {
        qWarning("marks: %s", qPrintable(query.lastError().text()));
        return;
    }
    m_minNextId = qMax(m_minNextId, id + 1);
    m_searchCache.remove(id);
    beginRemoveRows({}, row, row);
    m_marks.removeAt(row);
    endRemoveRows();
    if (wasHighlight)
        m_library->adjustHighlightCount(m_bookId, -1);
    emit changed();
}

void Marks::update(const Mark &mark) {
    Q_ASSERT(QThread::currentThread() == thread());
    const int row = rowOf(mark.id);
    if (row < 0)
        return;
    Q_ASSERT(row >= 0 && row < m_marks.size());
    Mark changed = mark;
    changed.updated = QDateTime::currentDateTime();
    QSqlQuery query(m_library->database());
    query.prepare(QStringLiteral("UPDATE annotations SET start = ?, finish = ?, note = ?, color = ?, prefix = ?, "
                                 "suffix = ?, updated_at = ? WHERE id = ? AND book_id = ?"));
    query.addBindValue(changed.start);
    query.addBindValue(changed.end);
    query.addBindValue(text(changed.note));
    query.addBindValue(text(changed.color));
    query.addBindValue(text(changed.prefix));
    query.addBindValue(text(changed.suffix));
    query.addBindValue(changed.updated.toSecsSinceEpoch());
    query.addBindValue(changed.id);
    query.addBindValue(m_bookId);
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
    Q_ASSERT(QThread::currentThread() == thread());
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
    auto token = std::make_shared<int>(0);
    auto *command = new Place(this, mark, token, true, tr("Add the bookmark"));
    m_undo->push(command);
    return command->id();
}

int Marks::addHighlight(int chapter, qint64 start, qint64 end, const QString &text, const QString &color,
                        const QString &label, const QString &prefix, const QString &suffix) {
    Q_ASSERT(QThread::currentThread() == thread());
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
    auto token = std::make_shared<int>(0);
    auto *command = new Place(this, mark, token, true, tr("Highlight"));
    m_undo->push(command);
    return command->id();
}

void Marks::setNote(int id, const QString &note) {
    Q_ASSERT(QThread::currentThread() == thread());
    const Mark before = find(id);
    const QString trimmed = note.trimmed();
    if (before.id == 0 || before.note == trimmed)
        return;
    m_undo->push(new Change(this, idTokenFor(id), Change::Field::Note, before.note, trimmed, tr("Edit the note")));
}

void Marks::setColor(int id, const QString &color) {
    Q_ASSERT(QThread::currentThread() == thread());
    const Mark before = find(id);
    if (before.id == 0 || before.color == color)
        return;
    m_undo->push(new Change(this, idTokenFor(id), Change::Field::Color, before.color, color, tr("Change the color")));
}

void Marks::remove(int id) {
    Q_ASSERT(QThread::currentThread() == thread());
    const Mark mark = find(id);
    if (mark.id == 0)
        return;
    m_undo->push(new Place(this, mark, idTokenFor(id), false,
                           mark.kind == QStringLiteral("bookmark") ? tr("Remove the bookmark") : tr("Remove the highlight")));
}

void Marks::undo() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_undo->undo();
}
void Marks::redo() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_undo->redo();
}

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
                                     {"noted", !mark.note.isEmpty()},
                                     {"note", mark.note}});
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
    Q_ASSERT(QThread::currentThread() == thread());
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
    QSet<int> seenIds;
    const QString trimmed = query.trimmed();
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
            seenIds.insert(id);
            auto it = m_searchCache.find(id);
            if (it == m_searchCache.end() || it->updatedAt != updatedAt || it->rawNote != note ||
                it->rawQuote != quote || it->rawLabel != label || it->rawBookTitle != bookTitle ||
                it->rawBookAuthor != bookAuthor) {
                SearchCacheEntry entry;
                entry.updatedAt = updatedAt;
                entry.rawNote = note;
                entry.rawQuote = quote;
                entry.rawLabel = label;
                entry.rawBookTitle = bookTitle;
                entry.rawBookAuthor = bookAuthor;
                entry.hayNote = note.isEmpty() ? QString() : WorksModel::searchable(note);
                entry.hayText = quote.isEmpty() ? QString() : WorksModel::searchable(quote);
                entry.hayMeta = WorksModel::searchable(label + QLatin1Char(' ') + bookTitle + QLatin1Char(' ') + bookAuthor);
                it = m_searchCache.insert(id, std::move(entry));
            }
            const QString &hayNote = it->hayNote;
            const QString &hayText = it->hayText;
            const QString &hayMeta = it->hayMeta;
            bool ok = true;
            for (const QString &tok : tokens) {
                const bool inNote = !hayNote.isEmpty() && hayNote.contains(tok);
                const bool inText = !hayText.isEmpty() && hayText.contains(tok);
                const bool inMeta = !hayMeta.isEmpty() && hayMeta.contains(tok);
                if (!inNote && !inText && !inMeta) {
                    ok = false;
                    break;
                }
                if (inNote)
                    score += 30;
                if (inText)
                    score += 20;
                if (inMeta)
                    score += 10;
            }
            if (!ok)
                continue;
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

    if (!tokens.isEmpty() && m_searchCache.size() > seenIds.size()) {
        for (auto it = m_searchCache.begin(); it != m_searchCache.end();)
            it = seenIds.contains(it.key()) ? std::next(it) : m_searchCache.erase(it);
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
        out += QStringLiteral("tags:\n  - clothbound\n  - reading-notes\n");
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
        target = QDir(docs).filePath(QStringLiteral("Clothbound Notes/%1.md").arg(safeFileName(title)));
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

    // Fast path: hint offset matches verbatim AND matches full W3C prefix/suffix
    // context (falling back to full scored search if context mismatches so
    // repeated phrases never silently anchor to the wrong occurrence).
    if (hintStart >= 0 && quote.size() <= chapterText.size() &&
        hintStart <= chapterText.size() - quote.size() &&
        QStringView(chapterText).mid(qsizetype(hintStart), quote.size()) == quote) {
        int expectedScore = 0;
        if (!prefix.isEmpty())
            expectedScore += int(prefix.size()) * 4 + 40;
        if (!suffix.isEmpty())
            expectedScore += int(suffix.size()) * 4 + 40;
        if (expectedScore == 0 ||
            contextScore(chapterText, qsizetype(hintStart), quote.size(), prefix, suffix) == expectedScore) {
            return {hintStart, hintStart + quote.size()};
        }
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
    Q_ASSERT(QThread::currentThread() == thread());
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
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_library || !m_library->database().isOpen() || chapterText.isEmpty())
        return 0;

    struct PendingHeal {
        int row = -1;
        int id = 0;
        qint64 start = 0;
        qint64 end = 0;
    };
    QList<PendingHeal> pending;
    for (int row = 0; row < m_marks.size(); ++row) {
        const Mark &m = m_marks[row];
        if (m.chapter != chapter)
            continue;
        const auto span = locateQuote(chapterText, m.text, m.prefix, m.suffix, m.start);
        if (span.first < 0)
            continue;
        const qint64 resolvedEnd = m.kind == QLatin1String("bookmark") ? span.first : span.second;
        if (span.first == m.start && resolvedEnd == m.end)
            continue;
        pending.append({row, m.id, span.first, resolvedEnd});
    }
    if (pending.isEmpty())
        return 0;

    QSqlDatabase db = m_library->database();
    LibrarySavepoint sp(db, "heal_chapter");
    if (!sp.isActive())
        return 0;

    const QDateTime now = QDateTime::currentDateTime();
    const qint64 nowEpoch = now.toSecsSinceEpoch();
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE annotations SET start = ?, finish = ?, updated_at = ? WHERE id = ? AND book_id = ?"));
    for (const PendingHeal &h : std::as_const(pending)) {
        query.bindValue(0, h.start);
        query.bindValue(1, h.end);
        query.bindValue(2, nowEpoch);
        query.bindValue(3, h.id);
        query.bindValue(4, m_bookId);
        if (!query.exec()) {
            qWarning("marks: healChapter failed: %s", qPrintable(query.lastError().text()));
            return 0;
        }
    }
    if (!sp.commit())
        return 0;

    QList<Mark> updated = m_marks;
    int firstRow = int(m_marks.size());
    int lastRow = -1;
    for (const PendingHeal &h : std::as_const(pending)) {
        Q_ASSERT(h.row >= 0 && h.row < updated.size());
        updated[h.row].start = h.start;
        updated[h.row].end = h.end;
        updated[h.row].updated = now;
        firstRow = qMin(firstRow, h.row);
        lastRow = qMax(lastRow, h.row);
    }
    const bool orderChanged = !std::is_sorted(updated.cbegin(), updated.cend(), before);
    if (orderChanged) {
        beginResetModel();
        m_marks = std::move(updated);
        std::sort(m_marks.begin(), m_marks.end(), before);
        endResetModel();
    } else {
        m_marks = std::move(updated);
        Q_ASSERT(firstRow >= 0 && lastRow >= firstRow && lastRow < m_marks.size());
        emit dataChanged(index(firstRow), index(lastRow), {StartRole, EndRole});
    }
    emit changed();
    return int(pending.size());
}
