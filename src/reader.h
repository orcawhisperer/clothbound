#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class BookScheme;
class EpubBook;
class Library;

// The book being read, between the window and the reading surface
// (reader.js, which reaches it as `bridge` over the web channel). It knows
// the book's structure, turns the surface's positions into progress through
// the whole book, saves where the reader is, and learns their pace.
class Reader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY bookChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(QString format READ format NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString author READ author NOTIFY bookChanged)
    Q_PROPERTY(QString language READ language NOTIFY bookChanged)
    Q_PROPERTY(QVariantMap manifest READ manifest NOTIFY bookChanged)
    Q_PROPERTY(QVariantList contents READ contents NOTIFY bookChanged)
    Q_PROPERTY(QVariantList chapterMarks READ chapterMarks NOTIFY lengthsChanged)
    Q_PROPERTY(QVariantMap startPosition READ startPosition NOTIFY bookChanged)
    Q_PROPERTY(bool hasPageList READ hasPageList NOTIFY bookChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY locationChanged)
    Q_PROPERTY(int chapterCount READ chapterCount NOTIFY bookChanged)
    Q_PROPERTY(QString chapterTitle READ chapterTitle NOTIFY locationChanged)
    Q_PROPERTY(int contentsIndex READ contentsIndex NOTIFY locationChanged)
    Q_PROPERTY(int page READ page NOTIFY locationChanged)
    Q_PROPERTY(int pages READ pages NOTIFY locationChanged)
    Q_PROPERTY(double progress READ progress NOTIFY locationChanged)
    Q_PROPERTY(QString pageLabel READ pageLabel NOTIFY locationChanged)
    Q_PROPERTY(int minutesLeftInChapter READ minutesLeftInChapter NOTIFY locationChanged)
    Q_PROPERTY(int minutesLeftInBook READ minutesLeftInBook NOTIFY locationChanged)
    Q_PROPERTY(bool atStart READ atStart NOTIFY locationChanged)
    Q_PROPERTY(bool atEnd READ atEnd NOTIFY locationChanged)

public:
    Reader(Library *library, BookScheme *scheme, QObject *parent = nullptr);
    ~Reader() override;

    bool isOpen() const { return m_bookId > 0; }
    int bookId() const { return m_bookId; }
    QString format() const { return m_format; }
    QString title() const { return m_title; }
    QString author() const { return m_author; }
    QString language() const { return m_language; }
    QVariantMap manifest() const { return m_manifest; }
    QVariantList contents() const { return m_contents; }
    QVariantList chapterMarks() const;
    QVariantMap startPosition() const { return m_startPosition; }
    bool hasPageList() const { return m_hasPageList; }
    int chapter() const { return m_chapter; }
    int chapterCount() const { return int(m_chapterTitles.size()); }
    QString chapterTitle() const { return m_chapterTitles.value(m_chapter); }
    int contentsIndex() const;
    int page() const { return m_page; }
    int pages() const { return m_pages; }
    double progress() const { return m_progress; }
    QString pageLabel() const { return m_pageLabel; }
    int minutesLeftInChapter() const;
    int minutesLeftInBook() const;
    bool atStart() const { return m_chapter <= m_firstLinear && m_page <= 0; }
    bool atEnd() const { return m_atEnd; }

    Q_INVOKABLE bool openBook(int bookId);
    Q_INVOKABLE void close();
    // A place in the book for a fraction of the whole, for the scrubber and
    // "go to 40%": {chapter, offset}.
    Q_INVOKABLE QVariantMap targetForFraction(double fraction) const;
    // The place a printed page number starts, for "go to page 87".
    Q_INVOKABLE QVariantMap targetForPageLabel(const QString &label) const;
    Q_INVOKABLE QString errorString() const { return m_error; }
    Q_INVOKABLE QString titleOfChapter(int chapter) const { return m_chapterTitles.value(chapter); }

    // From the reading surface.
    Q_INVOKABLE void relocated(const QVariantMap &location);
    Q_INVOKABLE void surfaceLoaded();
    Q_INVOKABLE void tapped(const QString &zone);
    Q_INVOKABLE void openExternal(const QString &url);
    Q_INVOKABLE void keyPressed(const QString &key, int modifiers);
    Q_INVOKABLE void log(const QString &message);

    // Progress through the whole book at a chapter and an offset in it.
    double fractionAt(int chapter, qint64 offset) const;

signals:
    void bookChanged();
    void lengthsChanged();
    void locationChanged();
    void surfaceReady();
    void tap(const QString &zone);
    void externalLink(const QString &url);
    void key(const QString &key, int modifiers);
    void failed(const QString &message);

private:
    void savePosition();
    void flushReadingTime();
    void measureChapters();
    qint64 globalOffset(int chapter, qint64 offset) const;

    Library *m_library;
    BookScheme *m_scheme;
    std::shared_ptr<EpubBook> m_book;
    QString m_token;
    int m_bookId = 0;
    QString m_format;
    QString m_title;
    QString m_author;
    QString m_language;
    QString m_error;
    QVariantMap m_manifest;
    QVariantList m_contents;
    QVariantMap m_startPosition;
    QStringList m_chapterTitles;
    QList<int> m_contentsChapter;
    QList<int> m_lengths;
    QList<bool> m_linear;
    QList<QPair<int, QString>> m_pageTargets;
    bool m_hasPageList = false;
    int m_firstLinear = 0;

    int m_chapter = 0;
    qint64 m_offset = 0;
    QString m_snippet;
    int m_page = 0;
    int m_pages = 1;
    double m_progress = 0;
    QString m_pageLabel;
    bool m_atEnd = false;

    QTimer m_saveTimer;
    QElapsedTimer m_sinceTurn;
    qint64 m_lastGlobal = -1;
    int m_pendingSeconds = 0;
    qint64 m_pendingChars = 0;
    QTimer m_timeFlush;
};
