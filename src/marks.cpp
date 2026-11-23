#include "marks.h"

#include "library.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QUndoCommand>
#include <QUndoStack>
#include <algorithm>

namespace {
// An empty text, never a null one: a null QString is SQL's NULL, which the
// columns refuse.
QVariant text(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

bool before(const Mark &a, const Mark &b) {
    return a.chapter != b.chapter ? a.chapter < b.chapter : a.start != b.start ? a.start < b.start : a.id < b.id;
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
    void put() { m_mark.id = m_marks->insert(m_mark); }
    void take() {
        m_mark = m_marks->find(m_mark.id);
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
}

Marks::Marks(Library *library, QObject *parent) : QAbstractListModel(parent), m_library(library), m_undo(new QUndoStack(this)) {
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
    default: return {};
    }
}

QHash<int, QByteArray> Marks::roleNames() const {
    return {{IdRole, "markId"}, {KindRole, "kind"},   {ChapterRole, "chapter"}, {StartRole, "start"},
            {EndRole, "end"},   {TextRole, "quote"},   {NoteRole, "note"},       {ColorRole, "color"},
            {LabelRole, "label"}, {CreatedRole, "created"}};
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
                                     " updated_at FROM annotations WHERE book_id = ?"));
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
                                       " label, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                      : QStringLiteral("INSERT INTO annotations (book_id, kind, chapter, start, finish, text, note, color,"
                                       " label, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    if (mark.id > 0)
        query.addBindValue(mark.id);
    for (const QVariant &value : {QVariant(m_bookId), text(mark.kind), QVariant(mark.chapter), QVariant(mark.start),
                                  QVariant(mark.end), text(mark.text), text(mark.note), text(mark.color),
                                  text(mark.label), QVariant(mark.created.toSecsSinceEpoch()),
                                  QVariant(mark.updated.toSecsSinceEpoch())})
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
    query.exec();
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
    query.prepare(QStringLiteral("UPDATE annotations SET note = ?, color = ?, updated_at = ? WHERE id = ?"));
    query.addBindValue(text(changed.note));
    query.addBindValue(text(changed.color));
    query.addBindValue(changed.updated.toSecsSinceEpoch());
    query.addBindValue(changed.id);
    query.exec();
    m_marks[row] = changed;
    emit dataChanged(index(row), index(row));
    emit this->changed();
}

int Marks::addBookmark(int chapter, qint64 offset, const QString &text, const QString &label) {
    Mark mark;
    mark.kind = QStringLiteral("bookmark");
    mark.chapter = chapter;
    mark.start = mark.end = offset;
    mark.text = text.simplified().left(200);
    mark.label = label;
    auto *command = new Place(this, mark, true, tr("Add the bookmark"));
    m_undo->push(command);
    return command->id();
}

int Marks::addHighlight(int chapter, qint64 start, qint64 end, const QString &text, const QString &color,
                        const QString &label) {
    if (end <= start)
        return 0;
    Mark mark;
    mark.kind = QStringLiteral("highlight");
    mark.chapter = chapter;
    mark.start = start;
    mark.end = end;
    mark.text = text.simplified();
    mark.color = color.isEmpty() ? QStringLiteral("yellow") : color;
    mark.label = label;
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
    return {{"id", mark.id},       {"kind", mark.kind},   {"chapter", mark.chapter}, {"start", mark.start},
            {"end", mark.end},     {"text", mark.text},   {"note", mark.note},       {"color", mark.color},
            {"label", mark.label}, {"created", mark.created}};
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

QString Marks::markdown(const QString &title, const QString &author) const {
    QString out = QStringLiteral("# %1\n").arg(title);
    if (!author.isEmpty())
        out += QStringLiteral("\n*%1*\n").arg(author);
    QString place;
    for (const Mark &mark : m_marks) {
        if (mark.label != place && !mark.label.isEmpty()) {
            place = mark.label;
            out += QStringLiteral("\n## %1\n").arg(place);
        }
        if (mark.kind == QStringLiteral("bookmark"))
            out += QStringLiteral("\n- %1 %2\n").arg(QChar(0x1F516), mark.text);
        else
            out += QStringLiteral("\n> %1\n").arg(mark.text);
        if (!mark.note.isEmpty())
            out += QStringLiteral("\n%1\n").arg(mark.note);
    }
    return out;
}
