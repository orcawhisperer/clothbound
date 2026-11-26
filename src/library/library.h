#pragma once

#include "formats/bookrecord.h"
#include "library/librarymodel.h"
#include "library/libraryschema.h"

#include <QHash>
#include <QMultiHash>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class Scanner;
struct ScannedBook;

// The reading library: the folders it watches, every book found in them,
// and each book's reading state, kept in SQLite under ~/.local/share/clothbound.
// Book files themselves are only ever read.
class Library : public QObject {
    Q_OBJECT
    Q_PROPERTY(WorksModel *works READ works CONSTANT)
    Q_PROPERTY(QVariantList continueReading READ continueReading NOTIFY continueReadingChanged)
    Q_PROPERTY(int bookCount READ bookCount NOTIFY booksChanged)
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QVariantList collections READ collections NOTIFY collectionsChanged)
    // {all, reading, recent, want, finished, listening, epub, pdf, articles, downloaded, audiobooks}:
    // how many books each sidebar shelf holds.
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
    // Opens library.sqlite strictly with QSQLITE_OPEN_READONLY, skipping any
    // directory creation, DDL, migrations, or WAL mode writes.
    bool openReadOnly(const QString &dbPath = {});
    bool isReadOnly() const { return m_readOnly; }
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
    // When a custom collection is active in works(), individual files are also
    // added directly to that collection (even if already present in the library).
    Q_INVOKABLE void addUrls(const QList<QUrl> &urls);
    // Adds dropped/selected URLs directly into a specific collection (0 = library only).
    Q_INVOKABLE void addUrlsToCollection(const QList<QUrl> &urls, int collectionId);
    Q_INVOKABLE void addFilesToCollection(const QStringList &paths, int collectionId);
    Q_INVOKABLE void removeFolder(const QString &path);
    Q_INVOKABLE QVariantList folderStats() const;
    Q_INVOKABLE void openFolder(const QString &path) const;
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

    // Off-GUI-thread CommandPalette search: executes normalized fuzzy book
    // scoring + read-only FTS5 index.sqlite passage search + read-only
    // annotations mark search on a dedicated background pool with atomic
    // generation cancellation. Emits paletteSearchReady(requestId, results).
    Q_INVOKABLE int searchPaletteAsync(const QString &query, int currentBookId = 0, int maxBooks = 12,
                                       int maxPassages = 8, int maxMarks = 8, int scopeBookId = 0,
                                       const QString &mode = QStringLiteral("all"),
                                       const QString &indexPath = {});
    Q_INVOKABLE void cancelPaletteSearch();

    // Canonical zero-copy BookRecord store accessors (shared with WorksModel).
    const QHash<int, BookRecord> &bookRecords() const { return m_books; }
    const BookRecord *bookRecord(int id) const {
        const auto it = m_books.constFind(id);
        return it != m_books.cend() ? &(*it) : nullptr;
    }

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
    void savePosition(int id, const QString &position, double progress, QObject *sourceSession = nullptr);
    void notifyMarksChanged(int bookId, QObject *sourceSession = nullptr);
    void addReadingTime(int id, int seconds, qint64 charsRead);
    QList<int> chapterLengths(int id) const;
    // Small per-book choices, such as the narrator's voice.
    QString bookSetting(int id, const QString &key) const;
    void setBookSetting(int id, const QString &key, const QString &value);
    void adjustHighlightCount(int bookId, int delta);
    void setChapterLengths(int id, const QList<int> &lengths);

    QSqlDatabase database() const { return QSqlDatabase::database(m_connection); }

    // Resolves a book's filesystem path from active Library instances on the
    // calling thread (used by BookSearch without depending on Reader).
    static QString resolveBookPath(int bookId, const QString &format = {}, const QString &title = {},
                                   const QString &author = {});

signals:
    void booksChanged();
    void bookChanged(int id);
    void bookUpdated(int id);
    void bookPositionChanged(int bookId, int chapter, double progress, QObject *sourceSession = nullptr);
    void marksChanged(int bookId, QObject *sourceSession = nullptr);
    void continueReadingChanged();
    void foldersChanged();
    void collectionsChanged();
    void readingStatsChanged();
    void scanningChanged();
    void scanProgressChanged();
    void readingSpeedChanged();
    void scanFinished(int added, int updated);
    void booksAdded(int firstBookId, int count, const QString &firstTitle, int targetCollectionId);
    void paletteSearchReady(int requestId, QVariantMap results);
    void problem(const QString &message);

private:
    struct StagedBook {
        BookRecord record;
        QString oldPath;
        QString fingerprint;
        bool updated = false;
    };
    struct ImportBatch {
        int firstBookId = 0;
        int count = 0;
        QString firstTitle;
        int targetCollectionId = 0;

        void record(int bookId, const QString &title, int collectionId) {
            if (bookId <= 0)
                return;
            if (firstBookId == 0) {
                firstBookId = bookId;
                firstTitle = title;
                targetCollectionId = collectionId;
            } else if (targetCollectionId != collectionId) {
                targetCollectionId = 0;
            }
            ++count;
        }
        void merge(const ImportBatch &other) {
            if (other.count <= 0)
                return;
            if (firstBookId == 0) {
                *this = other;
            } else {
                if (targetCollectionId != other.targetCollectionId)
                    targetCollectionId = 0;
                count += other.count;
            }
        }
        void clear() {
            firstBookId = 0;
            count = 0;
            firstTitle.clear();
            targetCollectionId = 0;
        }
    };

    struct PaletteBookEntry {
        int id = 0;
        QString title;
        QString sortTitle;
        QString author;
        QString series;
        double seriesIndex = 0.0;
        QString format;
        QString cover;
        QString editionLine;
        double progress = 0.0;
        bool finished = false;
        qint64 openedAt = 0;
        qint64 addedAt = 0;
    };
    struct PaletteNormalizedEntry {
        QString normTitle;
        QString normMeta;
        QString normFull;
    };
    struct PaletteSnapshot {
        QList<PaletteBookEntry> books;
        QHash<int, int> indexById;
        mutable std::once_flag normOnce;
        mutable QList<PaletteNormalizedEntry> normalized;
    };
    struct PendingPaletteDelivery {
        int requestId = 0;
        QVariantMap payload;
    };

    bool openDatabase(const QString &dbPath, bool readOnly);
    bool rejectIfReadOnly();
    void refreshArticles();
    bool isArticleRecord(const BookRecord &record) const;
    void syncWorksCollections();
    void flushNotificationsNow();
    void addUrlsImpl(const QList<QUrl> &urls, int explicitCollectionId);
    void enrichBookMap(QVariantMap &map, int id) const;
    bool storeInDb(const ScannedBook &book, QList<StagedBook> &staged);
    void applyStaged(const QList<StagedBook> &staged);
    void flushPending();
    void markListed(const QStringList &paths, const QStringList &folders);
    void changed(bool statsDirty = true, bool collectionsDirty = true);
    std::shared_ptr<const PaletteSnapshot> paletteSnapshot();
    void invalidatePaletteSnapshot();
    void drainPaletteDeliveries();

    QString m_dataDirectory;
    QString m_canonicalDataDirectory;
    QString m_cacheDirectory;
    QString m_coverDirectory;
    QString m_dbPath;
    QString m_connection;
    QString m_error;
    bool m_readOnly = false;
    QHash<int, BookRecord> m_books;
    QHash<QString, int> m_pathToId;
    QHash<int, QString> m_fingerprints;
    QMultiHash<QString, int> m_fingerprintToIds;
    QHash<int, LibraryCollectionRow> m_collections;
    QHash<int, QSet<int>> m_collectionBooks;
    QHash<int, QSet<int>> m_bookCollections;
    QSet<int> m_downloadedBooks;
    QSet<int> m_audiobookBooks;
    QSet<int> m_articleSettingIds;
    QSet<int> m_articles;
    QHash<int, QString> m_sourceUrls;
    QHash<int, QString> m_voiceNames;
    QHash<int, int> m_highlightCounts;
    QStringList m_folders;
    QStringList m_queuedFiles;
    QHash<QString, int> m_pendingFileCollections;
    QHash<QString, int> m_pendingFolderCollections;
    ImportBatch m_importBatch;
    Scanner *m_scanner = nullptr;
    WorksModel *m_works = nullptr;
    QList<ScannedBook> m_pending;
    QTimer m_flush;
    QTimer m_notify;
    bool m_worksDirty = false;
    bool m_booksNotifyDirty = false;
    bool m_continueNotifyDirty = false;
    bool m_collectionsNotifyDirty = false;
    bool m_statsNotifyDirty = false;
    bool m_scanning = false;
    int m_scanDone = 0;
    int m_scanTotal = 0;
    int m_added = 0;
    int m_updated = 0;
    double m_charsPerMinute = 1300;
    std::shared_ptr<const PaletteSnapshot> m_paletteSnapshot;
    QThreadPool m_palettePool;
    std::atomic<int> m_paletteGeneration{0};
    std::atomic_bool m_paletteShuttingDown{false};
    std::mutex m_paletteDeliveryMutex;
    std::vector<PendingPaletteDelivery> m_paletteDeliveries;
};
