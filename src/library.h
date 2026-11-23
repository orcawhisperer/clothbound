#pragma once

#include "bookrecord.h"
#include "librarymodel.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class Scanner;
struct ScannedBook;

// The reading library: the folders it watches, every book found in them,
// and each book's reading state, kept in SQLite under ~/.local/share/dogear.
// Book files themselves are only ever read.
class Library : public QObject {
    Q_OBJECT
    Q_PROPERTY(WorksModel *works READ works CONSTANT)
    Q_PROPERTY(QVariantList continueReading READ continueReading NOTIFY booksChanged)
    Q_PROPERTY(int bookCount READ bookCount NOTIFY booksChanged)
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QVariantList collections READ collections NOTIFY collectionsChanged)
    // {all, reading, want, finished, listening}: how many books each sidebar shelf holds.
    Q_PROPERTY(QVariantMap shelfCounts READ shelfCounts NOTIFY booksChanged)
    Q_PROPERTY(QVariantMap readingStats READ readingStats NOTIFY readingStatsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int scanDone READ scanDone NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(double charsPerMinute READ charsPerMinute NOTIFY readingSpeedChanged)

public:
    explicit Library(const QString &dataDirectory = {}, const QString &cacheDirectory = {}, QObject *parent = nullptr);
    ~Library() override;

    bool open();
    int schemaVersion() const;
    QString errorString() const { return m_error; }
    QString coverDirectory() const { return m_coverDirectory; }

    WorksModel *works() const { return m_works; }
    QVariantList continueReading() const;
    int bookCount() const;
    QStringList folders() const { return m_folders; }
    bool scanning() const { return m_scanning; }
    int scanDone() const { return m_scanDone; }
    int scanTotal() const { return m_scanTotal; }
    double charsPerMinute() const { return m_charsPerMinute; }

    Q_INVOKABLE void addFolder(const QString &path);
    // Files and folders dropped on the window or chosen in a picker.
    Q_INVOKABLE void addUrls(const QList<QUrl> &urls);
    Q_INVOKABLE void removeFolder(const QString &path);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void cancelScan();
    // Reads one file now and stores it, for books opened from outside the
    // library; returns its id, or 0 with errorString() set.
    int addBookFile(const QString &path);

    Q_INVOKABLE QVariantMap book(int id) const;
    Q_INVOKABLE QVariantList allBooks() const;
    Q_INVOKABLE QVariantList editions(const QString &workKey) const;
    Q_INVOKABLE void setFinished(int id, bool finished);
    Q_INVOKABLE void setWantToRead(int id, bool want);
    // Narration started for this book: it joins the Listening shelf.
    void markListened(int id);
    QVariantMap shelfCounts() const;
    static QString chapterTitleOf(const BookRecord &record);
    static QString snippetOf(const BookRecord &record);
    Q_INVOKABLE void setHidden(int id, bool hidden);
    Q_INVOKABLE void showInFolder(int id) const;
    Q_INVOKABLE QString coverUrl(int id) const;
    Q_INVOKABLE int lastOpenedBook() const;
    // Minutes to read what's left of a book at the reader's own pace.
    Q_INVOKABLE int minutesLeft(int id) const;

    // User collections / shelves.
    Q_INVOKABLE QVariantList collections() const;
    Q_INVOKABLE int createCollection(const QString &name);
    Q_INVOKABLE bool renameCollection(int id, const QString &name);
    Q_INVOKABLE bool deleteCollection(int id);
    Q_INVOKABLE bool addToCollection(int bookId, int collectionId);
    Q_INVOKABLE bool removeFromCollection(int bookId, int collectionId);
    Q_INVOKABLE QVariantList collectionsForBook(int bookId) const;
    Q_INVOKABLE bool isInCollection(int bookId, int collectionId) const;

    // Reading stats and insights from reading_days and books.
    Q_INVOKABLE QVariantMap readingStats() const;
    Q_INVOKABLE QVariantMap bookReadingStats(int id) const;

    BookRecord record(int id) const { return m_books.value(id); }
    // Every book shown in the library (not missing, not removed).
    QList<BookRecord> visibleRecords() const {
        QList<BookRecord> list;
        for (const auto &r : m_books)
            if (r.visible())
                list.append(r);
        return list;
    }
    int idForPath(const QString &path) const;
    QList<BookRecord> records() const { return m_books.values(); }
    QVariantMap toMap(const BookRecord &record) const;

    // Reading state, written by the reader as it goes.
    Q_INVOKABLE void markOpened(int id);
    void savePosition(int id, const QString &position, double progress);
    void addReadingTime(int id, int seconds, qint64 charsRead);
    QList<int> chapterLengths(int id) const;
    // Small per-book choices, such as the narrator's voice.
    QString bookSetting(int id, const QString &key) const;
    void setBookSetting(int id, const QString &key, const QString &value);
    void setChapterLengths(int id, const QList<int> &lengths);

    QSqlDatabase database() const { return QSqlDatabase::database(m_connection); }

signals:
    void booksChanged();
    void bookChanged(int id);
    void foldersChanged();
    void collectionsChanged();
    void readingStatsChanged();
    void scanningChanged();
    void scanProgressChanged();
    void readingSpeedChanged();
    void scanFinished(int added, int updated);
    void problem(const QString &message);

private:
    struct StagedBook {
        BookRecord record;
        QString oldPath;
        QString fingerprint;
        bool updated = false;
    };
    struct CollectionRecord {
        int id = 0;
        QString name;
        qint64 createdAt = 0;
    };

    bool migrate();
    void loadBooks();
    void loadFolders();
    void loadCollections();
    void loadAudioBadges();
    void syncWorksCollections();
    void enrichBookMap(QVariantMap &map, int id) const;
    bool storeInDb(const ScannedBook &book, QList<StagedBook> &staged);
    void applyStaged(const QList<StagedBook> &staged);
    bool writeChapterLengths(QSqlDatabase &db, int id, const QList<int> &lengths, qint64 *totalOut = nullptr);
    void flushPending();
    void markListed(const QStringList &paths, const QStringList &folders);
    void changed();
    BookRecord readRecord(const class QSqlQuery &query) const;
    bool exec(class QSqlQuery &query);

    QString m_dataDirectory;
    QString m_coverDirectory;
    QString m_connection;
    QString m_error;
    QHash<int, BookRecord> m_books;
    QHash<QString, int> m_pathToId;
    QHash<int, QString> m_fingerprints;
    QHash<int, CollectionRecord> m_collections;
    QHash<int, QSet<int>> m_collectionBooks;
    QHash<int, QSet<int>> m_bookCollections;
    QSet<int> m_downloadedBooks;
    QSet<int> m_audiobookBooks;
    QStringList m_folders;
    QStringList m_queuedFiles;
    Scanner *m_scanner = nullptr;
    WorksModel *m_works = nullptr;
    QList<ScannedBook> m_pending;
    QTimer m_flush;
    QTimer m_notify;
    bool m_worksDirty = false;
    bool m_scanning = false;
    int m_scanDone = 0;
    int m_scanTotal = 0;
    int m_added = 0;
    int m_updated = 0;
    double m_charsPerMinute = 1300;
};
