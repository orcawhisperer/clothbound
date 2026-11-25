#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class Library;
class QUndoStack;

// One mark in a book: a bookmark at a page, or a highlight over some words,
// which may carry a note, plus W3C Web Annotation prefix/suffix context for
// healing drifted offsets.
struct Mark {
    int id = 0;
    QString kind;  // "bookmark" or "highlight"
    int chapter = 0;
    qint64 start = 0;
    qint64 end = 0;
    QString text;    // the words marked (a bookmark: the page's first words)
    QString note;
    QString color;   // a highlight's: "yellow", "green", "blue", or "pink"
    QString label;   // where it is, as the reader would say it ("p. 34")
    QString prefix;  // W3C TextQuoteSelector context immediately before `text`
    QString suffix;  // W3C TextQuoteSelector context immediately after `text`
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
    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole,
        ChapterRole,
        StartRole,
        EndRole,
        TextRole,
        NoteRole,
        ColorRole,
        LabelRole,
        CreatedRole,
        PrefixRole,
        SuffixRole,
    };

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

    Q_INVOKABLE int addBookmark(int chapter, qint64 offset, const QString &text, const QString &label,
                                const QString &prefix = {}, const QString &suffix = {});
    Q_INVOKABLE int addHighlight(int chapter, qint64 start, qint64 end, const QString &text, const QString &color,
                                 const QString &label, const QString &prefix = {}, const QString &suffix = {});
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
    // Marks matching a panel filter ("all", "highlights", "notes", "bookmarks").
    Q_INVOKABLE QVariantList filtered(const QString &filter) const;
    // Search highlights, notes, and bookmarks across the whole library.
    Q_INVOKABLE QVariantList searchAll(const QString &query, int limit = 50) const;
    // Everything marked, as Markdown ("standard" or "obsidian"), for keeping elsewhere.
    Q_INVOKABLE QString markdown(const QString &title, const QString &author, const QString &format = {}) const;
    // Saves Markdown notes atomically to disk; returns the written path, or "" on failure.
    Q_INVOKABLE QString exportMarkdownToFile(const QString &filePath, const QString &title, const QString &author,
                                             const QString &format = QStringLiteral("obsidian")) const;

    // Resilient W3C Web Annotation offset resolution & healing against chapter text.
    static QPair<qint64, qint64> locateQuote(const QString &chapterText, const QString &quote, const QString &prefix,
                                             const QString &suffix, qint64 hintStart = -1);
    Q_INVOKABLE QVariantMap resolveOffset(int id, const QString &chapterText) const;
    Q_INVOKABLE bool healMark(int id, const QString &chapterText);
    Q_INVOKABLE int healChapter(int chapter, const QString &chapterText);

    // Used by the undoable commands.
    int insert(Mark mark);
    void erase(int id);
    void update(const Mark &mark);
    Mark find(int id) const;
    std::shared_ptr<int> idTokenFor(int id);
    void registerIdToken(int id, const std::shared_ptr<int> &token);
    void remapIdToken(int oldId, int newId, const std::shared_ptr<int> &token);

signals:
    void changed();
    void undoChanged();

private:
    struct SearchCacheEntry {
        qint64 updatedAt = 0;
        QString rawNote;
        QString rawQuote;
        QString rawLabel;
        QString rawBookTitle;
        QString rawBookAuthor;
        QString hayNote;
        QString hayText;
        QString hayMeta;
    };

    void load();
    int rowOf(int id) const;

    Library *m_library;
    QUndoStack *m_undo;
    int m_bookId = 0;
    int m_minNextId = 1;
    QList<Mark> m_marks;
    QHash<int, std::shared_ptr<int>> m_idTokens;
    mutable QHash<int, SearchCacheEntry> m_searchCache;
};
