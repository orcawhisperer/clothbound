#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariantList>

class Library;
class QUndoStack;

// One mark in a book: a bookmark at a page, or a highlight over some words,
// which may carry a note.
struct Mark {
    int id = 0;
    QString kind;  // "bookmark" or "highlight"
    int chapter = 0;
    qint64 start = 0;
    qint64 end = 0;
    QString text;   // the words marked (a bookmark: the page's first words)
    QString note;
    QString color;  // a highlight's: "yellow", "green", "blue", or "pink"
    QString label;  // where it is, as the reader would say it ("p. 34")
    QDateTime created;
    QDateTime updated;
};

// The open book's bookmarks, highlights, and notes, kept in the library and
// listed in the book's order. Every change can be undone.
class Marks : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)

public:
    enum Role { IdRole = Qt::UserRole + 1, KindRole, ChapterRole, StartRole, EndRole, TextRole, NoteRole, ColorRole,
                LabelRole, CreatedRole };

    explicit Marks(Library *library, QObject *parent = nullptr);
    ~Marks() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_marks.size()); }
    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;

    // The book whose marks these are (0 for none).
    void setBook(int bookId);
    int bookId() const { return m_bookId; }

    Q_INVOKABLE int addBookmark(int chapter, qint64 offset, const QString &text, const QString &label);
    Q_INVOKABLE int addHighlight(int chapter, qint64 start, qint64 end, const QString &text, const QString &color,
                                 const QString &label);
    Q_INVOKABLE void setNote(int id, const QString &note);
    Q_INVOKABLE void setColor(int id, const QString &color);
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    Q_INVOKABLE QVariantMap mark(int id) const;
    // A bookmark on the page that runs from `from` to `to` in a chapter.
    Q_INVOKABLE int bookmarkBetween(int chapter, qint64 from, qint64 to) const;
    // The highlight over a place, if any.
    Q_INVOKABLE int highlightAt(int chapter, qint64 offset) const;
    // A chapter's highlights for the page: {id, start, end, color, noted}.
    Q_INVOKABLE QVariantList highlightsIn(int chapter) const;
    // Everything marked, as Markdown, for keeping elsewhere.
    Q_INVOKABLE QString markdown(const QString &title, const QString &author) const;

    // Used by the undoable commands.
    int insert(Mark mark);
    void erase(int id);
    void update(const Mark &mark);
    Mark find(int id) const;

signals:
    void changed();
    void undoChanged();

private:
    void load();
    int rowOf(int id) const;

    Library *m_library;
    QUndoStack *m_undo;
    int m_bookId = 0;
    QList<Mark> m_marks;
};
