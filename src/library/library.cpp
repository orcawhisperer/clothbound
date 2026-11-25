#include "library/library.h"

#include "library/librarymodel.h"
#include "library/libraryschema.h"
#include "library/readingtelemetry.h"
#include "library/scanner.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>

namespace {
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }

bool isUnder(const QString &path, const QString &folder) {
    return path.startsWith(folder.endsWith('/') ? folder : folder + '/');
}

QString canonical(const QString &path) {
    const QFileInfo info(path);
    const QString resolved = info.canonicalFilePath();
    return resolved.isEmpty() ? info.absoluteFilePath() : resolved;
}

QMutex &liveLibrariesMutex() {
    static QMutex mutex;
    return mutex;
}

QList<Library *> &liveLibraries() {
    static QList<Library *> instances;
    return instances;
}
}  // namespace

QString BookRecord::editionLine() const {
    QStringList parts;
    if (!editor.isEmpty())
        parts << QCoreApplication::translate("Library", "ed. %1").arg(editor);
    if (!publisher.isEmpty())
        parts << publisher;
    if (year > 0)
        parts << QString::number(year);
    return parts.join(QStringLiteral(" · "));
}

Library::Library(const QString &dataDirectory, const QString &cacheDirectory, QObject *parent)
    : QObject(parent),
      m_dataDirectory(dataDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                              : dataDirectory),
      m_coverDirectory((cacheDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                                 : cacheDirectory) +
                       QStringLiteral("/covers")),
      m_connection(QStringLiteral("library-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    {
        QMutexLocker lock(&liveLibrariesMutex());
        liveLibraries().append(this);
    }
    m_works = new WorksModel(this);
    m_scanner = new Scanner(m_coverDirectory, this);

    m_flush.setSingleShot(true);
    m_flush.setInterval(200);
    connect(&m_flush, &QTimer::timeout, this, &Library::flushPending);
    // Many changes in a burst (a scan, a shelf toggle) become one refresh.
    m_notify.setSingleShot(true);
    m_notify.setInterval(60);
    connect(&m_notify, &QTimer::timeout, this, &Library::flushNotificationsNow);

    connect(m_scanner, &Scanner::listed, this, &Library::markListed);
    connect(m_scanner, &Scanner::bookRead, this, [this](const ScannedBook &book) {
        m_pending.append(book);
        if (!m_flush.isActive())
            m_flush.start();
    });
    connect(m_scanner, &Scanner::progressChanged, this, [this](int done, int total) {
        m_scanDone = done;
        m_scanTotal = total;
        emit scanProgressChanged();
    });
    connect(m_scanner, &Scanner::finished, this, [this] {
        flushPending();
        m_queuedFiles.clear();
        m_pendingFileCollections.clear();
        m_pendingFolderCollections.clear();
        m_scanning = false;
        emit scanningChanged();
        const ImportBatch batch = m_importBatch;
        m_importBatch.clear();
        if (batch.count > 0)
            flushNotificationsNow();
        emit scanFinished(m_added, m_updated);
        if (batch.count > 0)
            emit booksAdded(batch.firstBookId, batch.count, batch.firstTitle, batch.targetCollectionId);
    });
}

Library::~Library() {
    {
        QMutexLocker lock(&liveLibrariesMutex());
        liveLibraries().removeOne(this);
    }
    m_scanner->cancel();
    LibrarySchema::closeConnection(m_connection);
}

bool Library::rejectIfReadOnly() {
    if (!m_readOnly)
        return false;
    m_error = tr("Library is open read-only.");
    return true;
}

bool Library::open() {
    Q_ASSERT(QThread::currentThread() == thread());
    return openDatabase(m_dataDirectory + QStringLiteral("/library.sqlite"), false);
}

bool Library::openReadOnly(const QString &dbPath) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString target = dbPath.isEmpty() ? (m_dataDirectory + QStringLiteral("/library.sqlite")) : dbPath;
    return openDatabase(target, true);
}

bool Library::openDatabase(const QString &dbPath, bool readOnly) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_error.clear();
    m_readOnly = readOnly;
    if (!readOnly && (!QDir().mkpath(m_dataDirectory) || !QDir().mkpath(m_coverDirectory))) {
        m_error = tr("Can't create %1.").arg(m_dataDirectory);
        return false;
    }
    m_canonicalDataDirectory = canonical(readOnly ? QFileInfo(dbPath).absolutePath() : m_dataDirectory);
    if (!LibrarySchema::openConnection(m_connection, dbPath, readOnly, &m_error))
        return false;

    QSqlDatabase db = database();
    LibrarySnapshot snap;
    LibrarySchema::loadAll(db, snap);
    m_folders = std::move(snap.folders);
    m_books = std::move(snap.books);
    m_pathToId = std::move(snap.pathToId);
    m_fingerprints = std::move(snap.fingerprints);
    m_fingerprintToIds = std::move(snap.fingerprintToIds);
    m_collections = std::move(snap.collections);
    m_collectionBooks = std::move(snap.collectionBooks);
    m_bookCollections = std::move(snap.bookCollections);
    m_downloadedBooks = std::move(snap.downloadedBooks);
    m_audiobookBooks = std::move(snap.audiobookBooks);
    m_articleSettingIds = std::move(snap.articleSettingIds);
    m_sourceUrls = std::move(snap.sourceUrls);
    m_voiceNames = std::move(snap.voiceNames);
    m_highlightCounts = std::move(snap.highlightCounts);

    refreshArticles();
    m_charsPerMinute = ReadingTelemetry::loadCharsPerMinute(db, ReadingTelemetry::kDefaultCharsPerMinute);
    m_works->setCollections(m_collectionBooks, false);
    m_works->setAudioSets(m_downloadedBooks, m_audiobookBooks);
    m_works->setArticleSet(m_articles);
    m_works->setBooks(m_books);
    return true;
}

int Library::schemaVersion() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return LibrarySchema::schemaVersion(database());
}

QString Library::resolveBookPath(int bookId, const QString &format, const QString &title, const QString &author) {
    if (bookId <= 0)
        return {};
    QThread *current = QThread::currentThread();
    QMutexLocker lock(&liveLibrariesMutex());
    const auto &libs = liveLibraries();
    QString bestPath;
    int bestScore = -1;
    qint64 bestOpened = -1;
    for (int i = int(libs.size()) - 1; i >= 0; --i) {
        Library *lib = libs[i];
        if (!lib || lib->thread() != current)
            continue;
        const auto it = lib->m_books.constFind(bookId);
        if (it == lib->m_books.cend() || it->path.isEmpty())
            continue;
        const BookRecord &rec = *it;
        int score = 1;
        if (!format.isEmpty()) {
            if (rec.format.compare(format, Qt::CaseInsensitive) != 0)
                continue;
            score += 4;
        }
        if (!title.isEmpty() && rec.title == title)
            score += 2;
        if (!author.isEmpty() && rec.author == author)
            score += 1;
        if (score > bestScore || (score == bestScore && rec.openedAt > bestOpened)) {
            bestScore = score;
            bestOpened = rec.openedAt;
            bestPath = rec.path;
        }
    }
    return bestPath;
}

void Library::syncWorksCollections() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_worksDirty) {
        m_worksDirty = false;
        m_works->setCollections(m_collectionBooks, false);
        m_works->setBooks(m_books);
    } else {
        m_works->setCollections(m_collectionBooks);
    }
}

void Library::flushNotificationsNow() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_notify.isActive())
        m_notify.stop();
    if (m_worksDirty) {
        m_worksDirty = false;
        m_works->setCollections(m_collectionBooks, false);
        m_works->setBooks(m_books);
    }
    const bool emitBooks = m_booksNotifyDirty;
    const bool emitContinue = m_continueNotifyDirty || emitBooks;
    const bool emitCollections = m_collectionsNotifyDirty;
    const bool emitStats = m_statsNotifyDirty;
    m_booksNotifyDirty = false;
    m_continueNotifyDirty = false;
    m_collectionsNotifyDirty = false;
    m_statsNotifyDirty = false;
    if (emitBooks)
        emit booksChanged();
    if (emitContinue)
        emit continueReadingChanged();
    if (emitCollections)
        emit collectionsChanged();
    if (emitStats)
        emit readingStatsChanged();
}

void Library::adjustHighlightCount(int bookId, int delta) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (bookId <= 0 || delta == 0)
        return;
    const int next = qMax(0, m_highlightCounts.value(bookId, 0) + delta);
    if (next == 0)
        m_highlightCounts.remove(bookId);
    else
        m_highlightCounts.insert(bookId, next);
}

bool Library::isArticleRecord(const BookRecord &record) const {
    if (m_articleSettingIds.contains(record.id))
        return true;
    if (record.subjects.contains(QStringLiteral("Clothbound Article"), Qt::CaseInsensitive))
        return true;
    const QString articlesDir = m_dataDirectory + QStringLiteral("/articles");
    if (isUnder(record.path, articlesDir))
        return true;
    const QString &canonicalData = m_canonicalDataDirectory;
    if (!canonicalData.isEmpty() && canonicalData != m_dataDirectory &&
        isUnder(record.path, canonicalData + QStringLiteral("/articles")))
        return true;
    return false;
}

void Library::refreshArticles() {
    m_articles.clear();
    for (const auto &record : std::as_const(m_books))
        if (isArticleRecord(record))
            m_articles.insert(record.id);
    if (m_works)
        m_works->setArticleSet(m_articles);
}

void Library::changed(bool statsDirty, bool collectionsDirty) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_worksDirty = true;
    m_booksNotifyDirty = true;
    if (statsDirty)
        m_statsNotifyDirty = true;
    if (collectionsDirty)
        m_collectionsNotifyDirty = true;
    if (!m_notify.isActive())
        m_notify.start();
}

int Library::bookCount() const {
    Q_ASSERT(QThread::currentThread() == thread());
    int count = 0;
    for (const auto &record : m_books)
        count += record.visible() ? 1 : 0;
    return count;
}

int Library::idForPath(const QString &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (const int id = m_pathToId.value(path, 0))
        return id;
    const QString resolved = canonical(path);
    return resolved == path ? 0 : m_pathToId.value(resolved, 0);
}

void Library::addFolder(const QString &path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || path.trimmed().isEmpty())
        return;
    const QString folder = canonical(path);
    if (!QFileInfo(folder).isDir()) {
        emit problem(tr("%1 isn't a folder.").arg(path));
        return;
    }
    if (!m_folders.contains(folder)) {
        QSqlDatabase db = database();
        if (!LibrarySchema::insertFolder(db, folder, now(), &m_error))
            return;
        m_folders.append(folder);
        emit foldersChanged();
    }
    rescan();
}

void Library::addUrls(const QList<QUrl> &urls) {
    Q_ASSERT(QThread::currentThread() == thread());
    addUrlsImpl(urls, -1);
}

void Library::addUrlsToCollection(const QList<QUrl> &urls, int collectionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    addUrlsImpl(urls, qMax(0, collectionId));
}

void Library::addFilesToCollection(const QStringList &paths, int collectionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &path : paths)
        if (!path.trimmed().isEmpty())
            urls.append(QUrl::fromLocalFile(path));
    addUrlsToCollection(urls, collectionId);
}

void Library::addUrlsImpl(const QList<QUrl> &urls, int explicitCollectionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return;
    const int fileTargetCol = (explicitCollectionId >= 0) ? explicitCollectionId : (m_works ? m_works->collectionId() : 0);
    const int validFileCol = (fileTargetCol > 0 && m_collections.contains(fileTargetCol)) ? fileTargetCol : 0;
    const int validFolderCol = (explicitCollectionId > 0 && m_collections.contains(explicitCollectionId)) ? explicitCollectionId : 0;

    QStringList folders, files;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = canonical(url.toLocalFile());
        const QFileInfo info(path);
        if (info.isDir()) {
            folders.append(path);
        } else if (BookFiles::isBook(path)) {
            files.append(path);
            if (const int id = idForPath(path); id && m_books[id].hidden)
                setHidden(id, false);
        }
    }
    if (folders.isEmpty() && files.isEmpty()) {
        emit problem(tr("Clothbound reads EPUB and PDF books."));
        return;
    }

    QSet<int> immediateIds;
    ImportBatch immediateBatch;
    bool needScan = false;
    for (const QString &file : std::as_const(files)) {
        const QFileInfo info(file);
        if (const int id = idForPath(file)) {
            if (validFileCol > 0)
                addToCollection(id, validFileCol);
            const BookRecord &record = m_books[id];
            if (record.size == info.size() && record.mtime == info.lastModified().toSecsSinceEpoch() && !record.missing) {
                if (!immediateIds.contains(id)) {
                    immediateIds.insert(id);
                    immediateBatch.record(id, record.title, validFileCol);
                }
                continue;
            }
        }
        if (validFileCol > 0)
            m_pendingFileCollections.insert(file, validFileCol);
        if (!m_queuedFiles.contains(file))
            m_queuedFiles.append(file);
        needScan = true;
    }

    bool foldersAdded = false;
    QSqlDatabase db = database();
    for (const QString &folder : std::as_const(folders)) {
        if (validFolderCol > 0) {
            m_pendingFolderCollections.insert(folder, validFolderCol);
            for (const BookRecord &record : std::as_const(m_books)) {
                if (record.visible() && isUnder(record.path, folder)) {
                    addToCollection(record.id, validFolderCol);
                    if (!immediateIds.contains(record.id)) {
                        immediateIds.insert(record.id);
                        immediateBatch.record(record.id, record.title, validFolderCol);
                    }
                }
            }
        }
        if (m_folders.contains(folder))
            continue;
        if (LibrarySchema::insertFolder(db, folder, now(), &m_error)) {
            m_folders.append(folder);
            foldersAdded = true;
        }
    }
    if (foldersAdded)
        emit foldersChanged();
    if (!folders.isEmpty())
        needScan = true;

    if (needScan) {
        if (immediateBatch.count > 0) {
            flushNotificationsNow();
            m_importBatch.merge(immediateBatch);
        }
        rescan();
    } else if (immediateBatch.count > 0) {
        flushNotificationsNow();
        emit booksAdded(immediateBatch.firstBookId, immediateBatch.count, immediateBatch.firstTitle,
                        immediateBatch.targetCollectionId);
    }
}

void Library::removeFolder(const QString &path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || path.trimmed().isEmpty())
        return;
    const QString folder = canonical(path);
    const qsizetype folderIndex = m_folders.indexOf(folder);
    if (folderIndex < 0)
        return;
    QStringList remaining = m_folders;
    remaining.removeAt(folderIndex);
    // Books leave with their folder but keep their reading state, in case
    // the folder comes back (an unplugged drive).
    QList<int> markedMissing;
    for (const auto &record : std::as_const(m_books)) {
        if (record.loose || !isUnder(record.path, folder))
            continue;
        bool elsewhere = false;
        for (const QString &other : std::as_const(remaining))
            elsewhere = elsewhere || isUnder(record.path, other);
        if (!elsewhere)
            markedMissing.append(record.id);
    }
    QSqlDatabase db = database();
    if (!LibrarySchema::removeFolderAndMarkMissing(db, folder, markedMissing, &m_error))
        return;
    m_folders = remaining;
    for (const int id : markedMissing)
        m_books[id].missing = true;
    emit foldersChanged();
    changed();
    if (m_scanning) {
        cancelScan();
        if (!m_folders.isEmpty())
            rescan();
    }
}

QVariantList Library::folderStats() const {
    Q_ASSERT(QThread::currentThread() == thread());
    QVariantList result;
    result.reserve(m_folders.size());
    for (const QString &folder : std::as_const(m_folders)) {
        int books = 0;
        for (const auto &record : std::as_const(m_books))
            if (record.visible() && isUnder(record.path, folder))
                ++books;
        const QFileInfo info(folder);
        const QString name = info.fileName().isEmpty() ? folder : info.fileName();
        result.append(QVariantMap{{QStringLiteral("path"), folder},
                                  {QStringLiteral("name"), name},
                                  {QStringLiteral("exists"), info.isDir()},
                                  {QStringLiteral("books"), books}});
    }
    return result;
}

void Library::openFolder(const QString &path) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (path.trimmed().isEmpty())
        return;
    const QFileInfo info(path);
    if (info.isDir())
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}

void Library::rescan() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return;
    QDir().mkpath(m_coverDirectory);
    QStringList loose = m_queuedFiles;
    QSet<QString> looseSet(loose.cbegin(), loose.cend());
    QHash<QString, QPair<qint64, qint64>> known;
    known.reserve(m_books.size());
    for (const auto &record : std::as_const(m_books)) {
        const bool coverMissing =
            !record.cover.isEmpty() && !QFileInfo::exists(m_coverDirectory + QLatin1Char('/') + record.cover);
        if (!coverMissing && (!m_fingerprints.value(record.id).isEmpty() || record.missing || !record.problem.isEmpty()))
            known.insert(record.path, {record.size, record.mtime});
        if (record.loose && !looseSet.contains(record.path)) {
            looseSet.insert(record.path);
            loose.append(record.path);
        }
    }
    m_added = m_updated = 0;
    m_scanDone = m_scanTotal = 0;
    m_scanning = true;
    emit scanningChanged();
    emit scanProgressChanged();
    m_scanner->scan(m_folders, loose, known);
}

void Library::cancelScan() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_scanning)
        return;
    m_scanner->cancel();
    m_flush.stop();
    flushPending();
    m_queuedFiles.clear();
    m_pendingFileCollections.clear();
    m_pendingFolderCollections.clear();
    m_importBatch.clear();
    m_scanning = false;
    emit scanningChanged();
    emit scanProgressChanged();
}

void Library::markListed(const QStringList &paths, const QStringList &folders) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return;
    const QSet<QString> present(paths.cbegin(), paths.cend());
    QList<QPair<int, bool>> updates;
    for (const auto &record : std::as_const(m_books)) {
        bool inScope = record.loose;
        for (const QString &folder : folders)
            inScope = inScope || isUnder(record.path, folder);
        if (!inScope)
            continue;
        const bool missing = !present.contains(record.path);
        if (missing != record.missing)
            updates.append({record.id, missing});
    }
    QSqlDatabase db = database();
    if (updates.isEmpty() || !LibrarySchema::updateMissingFlags(db, updates, &m_error))
        return;
    for (const auto &[id, missing] : updates)
        m_books[id].missing = missing;
    changed();
}

bool Library::storeInDb(const ScannedBook &book, QList<StagedBook> &staged) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_readOnly);
    const BookInfo &info = book.info;
    const BookIdentity &id = info.identity;
    QSqlDatabase db = database();

    int existing = idForPath(info.path);
    for (const StagedBook &entry : std::as_const(staged))
        if (entry.record.path == info.path)
            existing = entry.record.id;

    // If the file was moved or renamed, relink it to the missing/moved record
    // with the same content fingerprint so annotations, progress, reading_days,
    // book_settings and passage_vectors stay attached.
    bool relinked = false;
    QString oldPath;
    if (!existing && !book.fingerprint.isEmpty()) {
        for (auto it = m_fingerprintToIds.constFind(book.fingerprint);
             it != m_fingerprintToIds.cend() && it.key() == book.fingerprint; ++it) {
            const BookRecord candidate = m_books.value(it.value());
            if (candidate.id > 0 && (candidate.missing || !QFileInfo::exists(candidate.path))) {
                bool claimed = false;
                for (const StagedBook &entry : std::as_const(staged))
                    if (entry.record.id == candidate.id)
                        claimed = true;
                if (!claimed) {
                    existing = candidate.id;
                    oldPath = candidate.path;
                    relinked = true;
                    break;
                }
            }
        }
    }

    BookRecord record = existing ? m_books.value(existing) : BookRecord();
    for (const StagedBook &entry : std::as_const(staged))
        if (existing && entry.record.id == existing)
            record = entry.record;
    record.path = info.path;
    record.format = info.format;
    record.size = info.size;
    record.mtime = info.modified.toSecsSinceEpoch();
    record.title = id.title;
    record.sortTitle = id.sortTitle;
    record.author = id.author;
    record.sortAuthor = id.sortAuthor;
    record.editor = id.editor;
    record.publisher = id.publisher;
    record.year = id.year;
    record.language = info.language;
    record.description = info.description;
    record.subjects = info.subjects.join(QStringLiteral(", "));
    record.isbn = info.isbn;
    record.series = info.series;
    record.seriesIndex = info.seriesIndex;
    record.workKey = id.workKey;
    record.chapters = info.chapters;
    record.pages = info.pages;
    record.length = book.length;
    record.cover = book.coverFile;
    record.problem = info.problem;
    record.missing = false;
    if (!existing || relinked) {
        if (!existing)
            record.addedAt = now();
        bool underFolder = false;
        for (const QString &folder : std::as_const(m_folders))
            underFolder = underFolder || isUnder(record.path, folder);
        record.loose = !underFolder;
    }

    if (!LibrarySchema::upsertBook(db, record, existing, relinked, book.fingerprint, &m_error))
        return false;
    if (!book.chapterLengths.isEmpty()) {
        qint64 total = 0;
        if (!LibrarySchema::writeChapterLengths(db, record.id, book.chapterLengths, &total, &m_error))
            return false;
        if (total > 0)
            record.length = total;
    }
    staged.append({record, oldPath, book.fingerprint, existing != 0});
    return true;
}

void Library::applyStaged(const QList<StagedBook> &staged) {
    Q_ASSERT(QThread::currentThread() == thread());
    bool articlesChanged = false;
    for (const StagedBook &entry : staged) {
        Q_ASSERT(entry.record.id > 0);
        if (!entry.oldPath.isEmpty() && entry.oldPath != entry.record.path)
            m_pathToId.remove(entry.oldPath);
        m_books.insert(entry.record.id, entry.record);
        m_pathToId.insert(entry.record.path, entry.record.id);
        if (!entry.fingerprint.isEmpty()) {
            const QString prevFp = m_fingerprints.value(entry.record.id);
            if (!prevFp.isEmpty() && prevFp != entry.fingerprint)
                m_fingerprintToIds.remove(prevFp, entry.record.id);
            m_fingerprints.insert(entry.record.id, entry.fingerprint);
            if (!m_fingerprintToIds.contains(entry.fingerprint, entry.record.id))
                m_fingerprintToIds.insert(entry.fingerprint, entry.record.id);
        }
        const bool isArt = isArticleRecord(entry.record);
        if (isArt && !m_articles.contains(entry.record.id)) {
            m_articles.insert(entry.record.id);
            articlesChanged = true;
        } else if (!isArt && m_articles.contains(entry.record.id)) {
            m_articles.remove(entry.record.id);
            articlesChanged = true;
        }
        entry.updated ? ++m_updated : ++m_added;
    }
    if (articlesChanged && m_works)
        m_works->setArticleSet(m_articles);
}

void Library::flushPending() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_pending.isEmpty() || rejectIfReadOnly())
        return;
    QSqlDatabase db = database();
    LibrarySavepoint sp(db, "flush_pending");
    if (!sp.isActive())
        return;
    QList<StagedBook> staged;
    staged.reserve(m_pending.size());
    int failedCount = 0;
    // Isolate each book in a nested savepoint so a single failing insert/update
    // cannot roll back or silently discard the rest of the batch.
    for (const ScannedBook &book : std::as_const(m_pending)) {
        LibrarySavepoint bookSp(db, "flush_book");
        const qsizetype before = staged.size();
        if (bookSp.isActive() && storeInDb(book, staged) && bookSp.commit())
            continue;
        staged.resize(before);
        bookSp.rollback();
        ++failedCount;
    }
    if (!sp.commit()) {
        // Outer transaction failed (e.g., transient lock/I/O): preserve m_pending
        // rather than silently losing the batch.
        return;
    }
    if (failedCount > 0)
        qWarning("Library: %d book(s) failed to store during batch flush", failedCount);
    m_pending.clear();
    if (staged.isEmpty())
        return;
    applyStaged(staged);
    changed();
    for (const StagedBook &entry : std::as_const(staged)) {
        int colId = m_pendingFileCollections.take(entry.record.path);
        if (colId <= 0 && !m_pendingFolderCollections.isEmpty()) {
            for (auto it = m_pendingFolderCollections.cbegin(); it != m_pendingFolderCollections.cend(); ++it) {
                if (isUnder(entry.record.path, it.key())) {
                    colId = it.value();
                    break;
                }
            }
        }
        if (colId > 0 && m_collections.contains(colId))
            addToCollection(entry.record.id, colId);
        if (!entry.updated || colId > 0)
            m_importBatch.record(entry.record.id, entry.record.title, colId);
    }
}

int Library::addBookFile(const QString &path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return 0;
    const QString file = canonical(path);
    const QFileInfo info(file);
    if (!info.exists()) {
        m_error = tr("%1 doesn't exist.").arg(path);
        return 0;
    }
    if (!BookFiles::isBook(file)) {
        m_error = tr("Clothbound reads EPUB and PDF books.");
        return 0;
    }
    bool wasHidden = false;
    if (const int id = idForPath(file)) {
        if (m_books[id].hidden) {
            setHidden(id, false);
            wasHidden = true;
        }
        const BookRecord &record = m_books[id];
        if (record.size == info.size() && record.mtime == info.lastModified().toSecsSinceEpoch() && !record.missing) {
            if (wasHidden) {
                flushNotificationsNow();
                emit booksAdded(id, 1, record.title, 0);
            }
            return id;
        }
    }
    const ScannedBook scanned = Scanner::read(file, m_coverDirectory);
    QSqlDatabase db = database();
    LibrarySavepoint sp(db, "add_book");
    if (!sp.isActive())
        return 0;
    QList<StagedBook> staged;
    if (!storeInDb(scanned, staged) || !sp.commit())
        return 0;
    const bool isNew = !staged.isEmpty() && !staged.first().updated;
    applyStaged(staged);
    changed();
    const int id = idForPath(file);
    if (id > 0 && (isNew || wasHidden)) {
        flushNotificationsNow();
        emit booksAdded(id, 1, m_books.value(id).title, 0);
    }
    return id;
}

QVariantMap Library::toMap(const BookRecord &r) const {
    Q_ASSERT(QThread::currentThread() == thread());
    QVariantList colIds;
    const QSet<int> cols = m_bookCollections.value(r.id);
    for (const int cid : cols)
        colIds.append(cid);
    return {
        {"id", r.id},
        {"path", r.path},
        {"folder", QFileInfo(r.path).absolutePath()},
        {"format", r.format},
        {"size", double(r.size)},
        {"title", r.title},
        {"author", r.author},
        {"editor", r.editor},
        {"publisher", r.publisher},
        {"year", r.year},
        {"editionLine", r.editionLine()},
        {"language", r.language},
        {"description", r.description},
        {"subjects", r.subjects},
        {"isbn", r.isbn},
        {"series", r.series},
        {"seriesIndex", r.seriesIndex},
        {"chapters", r.chapters},
        {"pages", r.pages},
        {"length", double(r.length)},
        {"workKey", r.workKey},
        {"cover", coverUrl(r.id)},
        {"problem", r.problem},
        {"addedAt", double(r.addedAt)},
        {"openedAt", double(r.openedAt)},
        {"finishedAt", double(r.finishedAt)},
        {"finished", r.finishedAt > 0},
        {"progress", r.progress},
        {"secondsRead", double(r.secondsRead)},
        {"minutesLeft", minutesLeft(r.id)},
        {"missing", r.missing},
        {"collectionIds", colIds},
        {"wantToRead", r.wantAt > 0 && r.finishedAt == 0},
        {"listening", r.listenedAt > 0},
        {"downloaded", m_downloadedBooks.contains(r.id)},
        {"hasAudiobook", m_audiobookBooks.contains(r.id)},
        {"isArticle", m_articles.contains(r.id)},
        {"sourceUrl", m_sourceUrls.value(r.id)},
        {"chapterTitle", chapterTitleOf(r)},
        {"snippet", snippetOf(r)},
    };
}

// The chapter the reader was last in, saved with the position.
QString Library::chapterTitleOf(const BookRecord &r) {
    if (r.position.isEmpty())
        return {};
    return QJsonDocument::fromJson(r.position.toUtf8()).object().value(QStringLiteral("title")).toString();
}

// A short quote from the last-read page, saved alongside the position.
QString Library::snippetOf(const BookRecord &r) {
    if (r.position.isEmpty())
        return {};
    return QJsonDocument::fromJson(r.position.toUtf8())
        .object()
        .value(QStringLiteral("snippet"))
        .toString()
        .simplified();
}

void Library::enrichBookMap(QVariantMap &map, int id) const {
    map.insert(QStringLiteral("highlightCount"), m_highlightCounts.value(id, 0));
    map.insert(QStringLiteral("voiceName"), m_voiceNames.value(id));
}

void Library::setWantToRead(int id, bool want) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id) || (m_books[id].wantAt > 0) == want)
        return;
    const qint64 when = want ? now() : 0;
    QSqlDatabase db = database();
    if (!LibrarySchema::setBookIntColumn(db, id, "want_at", when, &m_error))
        return;
    m_books[id].wantAt = when;
    emit bookChanged(id);
    changed(false, false);
}

void Library::markListened(int id) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id) || m_books[id].listenedAt > 0)
        return;
    const qint64 when = now();
    QSqlDatabase db = database();
    if (!LibrarySchema::setBookIntColumn(db, id, "listened_at", when, &m_error))
        return;
    m_books[id].listenedAt = when;
    emit bookChanged(id);
    changed(false, false);
}

QVariantMap Library::shelfCounts() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return ReadingTelemetry::shelfCounts(m_books, m_articles, m_downloadedBooks, m_audiobookBooks, now());
}

QVariantMap Library::book(int id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_books.contains(id))
        return {};
    QVariantMap map = toMap(m_books.value(id));
    enrichBookMap(map, id);
    return map;
}

QVariantList Library::allBooks() const {
    Q_ASSERT(QThread::currentThread() == thread());
    QList<BookRecord> found;
    found.reserve(m_books.size());
    for (const auto &record : std::as_const(m_books))
        if (record.visible())
            found.append(record);
    std::sort(found.begin(), found.end(), [](const BookRecord &a, const BookRecord &b) {
        if (a.openedAt != b.openedAt)
            return a.openedAt > b.openedAt;
        if (a.addedAt != b.addedAt)
            return a.addedAt > b.addedAt;
        return a.sortTitle.localeAwareCompare(b.sortTitle) < 0;
    });
    QVariantList list;
    list.reserve(found.size());
    for (const auto &record : std::as_const(found))
        list.append(toMap(record));
    return list;
}

QVariantList Library::collections() const {
    Q_ASSERT(QThread::currentThread() == thread());
    QList<LibraryCollectionRow> sorted = m_collections.values();
    std::sort(sorted.begin(), sorted.end(), [](const LibraryCollectionRow &a, const LibraryCollectionRow &b) {
        const int c = a.name.localeAwareCompare(b.name);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    QVariantList out;
    out.reserve(sorted.size());
    for (const LibraryCollectionRow &col : std::as_const(sorted)) {
        int visibleCount = 0;
        const QSet<int> bookIds = m_collectionBooks.value(col.id);
        for (const int bookId : bookIds)
            if (m_books.contains(bookId) && m_books.value(bookId).visible())
                ++visibleCount;
        out.append(QVariantMap{{"id", col.id},
                               {"name", col.name},
                               {"count", visibleCount},
                               {"createdAt", double(col.createdAt)}});
    }
    return out;
}

int Library::createCollection(const QString &name) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return 0;
    const QString cleaned = name.trimmed();
    QSqlDatabase db = database();
    if (cleaned.isEmpty() || !db.isOpen())
        return 0;
    for (const LibraryCollectionRow &existing : std::as_const(m_collections))
        if (existing.name.compare(cleaned, Qt::CaseInsensitive) == 0)
            return existing.id;
    const qint64 createdAt = now();
    const int id = LibrarySchema::insertCollection(db, cleaned, createdAt, &m_error);
    if (id <= 0)
        return 0;
    m_collections.insert(id, LibraryCollectionRow{id, cleaned, createdAt});
    syncWorksCollections();
    emit collectionsChanged();
    return id;
}

bool Library::renameCollection(int id, const QString &name) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return false;
    const QString cleaned = name.trimmed();
    QSqlDatabase db = database();
    if (cleaned.isEmpty() || !m_collections.contains(id) || !db.isOpen())
        return false;
    for (const LibraryCollectionRow &existing : std::as_const(m_collections))
        if (existing.id != id && existing.name.compare(cleaned, Qt::CaseInsensitive) == 0)
            return false;
    if (m_collections[id].name == cleaned)
        return true;
    if (!LibrarySchema::renameCollection(db, id, cleaned, &m_error))
        return false;
    m_collections[id].name = cleaned;
    emit collectionsChanged();
    return true;
}

bool Library::deleteCollection(int id) {
    Q_ASSERT(QThread::currentThread() == thread());
    QSqlDatabase db = database();
    if (rejectIfReadOnly() || !m_collections.contains(id) || !db.isOpen())
        return false;
    if (!LibrarySchema::deleteCollection(db, id, &m_error))
        return false;
    const QSet<int> affected = m_collectionBooks.take(id);
    for (const int bookId : affected)
        m_bookCollections[bookId].remove(id);
    m_collections.remove(id);
    if (m_works->collectionId() == id)
        m_works->setCollectionId(0);
    syncWorksCollections();
    emit collectionsChanged();
    for (const int bookId : affected)
        emit bookChanged(bookId);
    return true;
}

bool Library::addToCollection(int bookId, int collectionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    QSqlDatabase db = database();
    if (rejectIfReadOnly() || !m_books.contains(bookId) || !m_collections.contains(collectionId) || !db.isOpen())
        return false;
    if (m_collectionBooks.value(collectionId).contains(bookId)) {
        if (m_worksDirty)
            syncWorksCollections();
        return true;
    }
    if (!LibrarySchema::addBookToCollection(db, bookId, collectionId, now(), &m_error))
        return false;
    m_collectionBooks[collectionId].insert(bookId);
    m_bookCollections[bookId].insert(collectionId);
    syncWorksCollections();
    emit collectionsChanged();
    emit bookChanged(bookId);
    return true;
}

bool Library::removeFromCollection(int bookId, int collectionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    QSqlDatabase db = database();
    if (rejectIfReadOnly() || !m_collections.contains(collectionId) || !db.isOpen())
        return false;
    if (!m_collectionBooks.value(collectionId).contains(bookId))
        return true;
    if (!LibrarySchema::removeBookFromCollection(db, bookId, collectionId, &m_error))
        return false;
    m_collectionBooks[collectionId].remove(bookId);
    m_bookCollections[bookId].remove(collectionId);
    syncWorksCollections();
    emit collectionsChanged();
    emit bookChanged(bookId);
    return true;
}

QVariantList Library::collectionsForBook(int bookId) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const QSet<int> ids = m_bookCollections.value(bookId);
    QList<LibraryCollectionRow> cols;
    cols.reserve(ids.size());
    for (const int id : ids)
        if (m_collections.contains(id))
            cols.append(m_collections.value(id));
    std::sort(cols.begin(), cols.end(), [](const LibraryCollectionRow &a, const LibraryCollectionRow &b) {
        const int c = a.name.localeAwareCompare(b.name);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    QVariantList out;
    out.reserve(cols.size());
    for (const LibraryCollectionRow &c : std::as_const(cols))
        out.append(QVariantMap{{"id", c.id}, {"name", c.name}});
    return out;
}

bool Library::isInCollection(int bookId, int collectionId) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_collectionBooks.value(collectionId).contains(bookId);
}

QVariantMap Library::readingStats() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return ReadingTelemetry::readingStats(database(), m_books, m_charsPerMinute);
}

QVariantMap Library::bookReadingStats(int id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return ReadingTelemetry::bookReadingStats(database(), m_books.value(id), m_charsPerMinute);
}

QVariantList Library::editions(const QString &workKey) const {
    Q_ASSERT(QThread::currentThread() == thread());
    QList<BookRecord> found;
    for (const auto &record : m_books)
        if (record.visible() && record.workKey == workKey)
            found.append(record);
    std::sort(found.begin(), found.end(), [](const BookRecord &a, const BookRecord &b) {
        if (a.year != b.year)
            return a.year > b.year;
        return a.publisher.localeAwareCompare(b.publisher) < 0;
    });
    QVariantList list;
    for (const auto &record : found)
        list.append(toMap(record));
    return list;
}

QVariantList Library::continueReading() const {
    Q_ASSERT(QThread::currentThread() == thread());
    QList<BookRecord> found;
    for (const auto &record : m_books)
        if (record.visible() && record.openedAt > 0 && record.finishedAt == 0 && record.progress < 0.995)
            found.append(record);
    const qsizetype take = qMin<qsizetype>(8, found.size());
    std::partial_sort(found.begin(), found.begin() + take, found.end(),
                      [](const BookRecord &a, const BookRecord &b) { return a.openedAt > b.openedAt; });
    QVariantList list;
    list.reserve(take);
    for (qsizetype i = 0; i < take; ++i) {
        QVariantMap map = toMap(found[i]);
        enrichBookMap(map, found[i].id);
        list.append(map);
    }
    return list;
}

void Library::setFinished(int id, bool finished) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id))
        return;
    const qint64 when = finished ? now() : 0;
    QSqlDatabase db = database();
    if (!LibrarySchema::setBookIntColumn(db, id, "finished_at", when, &m_error))
        return;
    m_books[id].finishedAt = when;
    emit bookChanged(id);
    changed();
}

void Library::setHidden(int id, bool hidden) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id))
        return;
    QSqlDatabase db = database();
    if (!LibrarySchema::setBookIntColumn(db, id, "hidden", hidden ? 1 : 0, &m_error))
        return;
    m_books[id].hidden = hidden;
    changed();
}

void Library::showInFolder(int id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const BookRecord record = m_books.value(id);
    if (record.path.isEmpty())
        return;
    // The file manager selects the file when it speaks FileManager1, as
    // Nautilus does; otherwise the folder just opens. Run asynchronously so
    // the UI thread never blocks waiting for D-Bus activation.
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    call.setArguments({QStringList{QUrl::fromLocalFile(record.path).toString()}, QString()});
    const QString folder = QFileInfo(record.path).absolutePath();
    const QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call, 1500);
    auto *watcher = new QDBusPendingCallWatcher(pending, const_cast<Library *>(this));
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, folder] {
        watcher->deleteLater();
        const QDBusPendingReply<> reply(*watcher);
        if (reply.isError())
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
}

QString Library::coverUrl(int id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const BookRecord record = m_books.value(id);
    if (record.cover.isEmpty())
        return {};
    return QUrl::fromLocalFile(m_coverDirectory + '/' + record.cover).toString() + QStringLiteral("?v=") +
           QString::number(record.mtime);
}

int Library::lastOpenedBook() const {
    Q_ASSERT(QThread::currentThread() == thread());
    int best = 0;
    qint64 when = 0;
    for (const auto &record : m_books)
        if (record.visible() && record.openedAt > when) {
            when = record.openedAt;
            best = record.id;
        }
    return best;
}

int Library::minutesLeft(int id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return ReadingTelemetry::minutesLeft(m_books.value(id), m_charsPerMinute);
}

void Library::markOpened(int id) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id))
        return;
    const qint64 when = now();
    QSqlDatabase db = database();
    if (!LibrarySchema::setBookIntColumn(db, id, "opened_at", when, &m_error))
        return;
    const bool firstOpen = m_books[id].openedAt == 0;
    m_books[id].openedAt = when;
    changed(firstOpen, false);
}

void Library::savePosition(int id, const QString &position, double progress) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id))
        return;
    progress = std::clamp(progress, 0.0, 1.0);
    QSqlDatabase db = database();
    if (!LibrarySchema::updateBookPosition(db, id, position, progress, &m_error))
        return;
    BookRecord &rec = m_books[id];
    const double prevProgress = rec.progress;
    rec.position = position;
    rec.progress = progress;
    // Update only the changed book in WorksModel and emit targeted signals
    // instead of firing booksChanged()/collectionsChanged()/readingStatsChanged()
    // storms on every page turn.
    m_works->updateBook(rec);
    emit bookUpdated(id);
    emit bookChanged(id);
    if (rec.visible() && rec.openedAt > 0 && rec.finishedAt == 0 && (prevProgress < 0.995 || progress < 0.995)) {
        m_continueNotifyDirty = true;
        if (!m_notify.isActive())
            m_notify.start();
    }
}

void Library::addReadingTime(int id, int seconds, qint64 charsRead) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly() || !m_books.contains(id) || seconds <= 0)
        return;
    QSqlDatabase db = database();
    const ReadingTelemetry::RecordResult result =
        ReadingTelemetry::recordReadingTime(db, id, seconds, charsRead, m_charsPerMinute);
    if (!result.ok) {
        if (!result.error.isEmpty())
            m_error = result.error;
        return;
    }
    m_books[id].secondsRead += seconds;
    emit bookChanged(id);
    emit readingStatsChanged();
    if (result.paceUpdated) {
        m_charsPerMinute = result.newCharsPerMinute;
        emit readingSpeedChanged();
    }
}

QString Library::bookSetting(int id, const QString &key) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return LibrarySchema::readBookSetting(database(), id, key);
}

void Library::setBookSetting(int id, const QString &key, const QString &value) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return;
    QSqlDatabase db = database();
    if (!LibrarySchema::writeBookSetting(db, id, key, value, &m_error))
        return;
    const QString norm = value.trimmed().toLower();
    const bool truthy = !norm.isEmpty() && norm != QLatin1String("0") && norm != QLatin1String("false");
    auto updateSet = [&](QSet<int> &set) {
        if (truthy == set.contains(id))
            return;
        if (truthy)
            set.insert(id);
        else
            set.remove(id);
        m_works->setAudioSets(m_downloadedBooks, m_audiobookBooks);
        emit bookChanged(id);
        changed(false, false);
    };
    if (key == QLatin1String("audio_downloaded")) {
        updateSet(m_downloadedBooks);
    } else if (key == QLatin1String("audiobook_saved")) {
        updateSet(m_audiobookBooks);
    } else if (key == QLatin1String("content_kind")) {
        const bool wantArticle = (norm == QLatin1String("article"));
        if (wantArticle)
            m_articleSettingIds.insert(id);
        else
            m_articleSettingIds.remove(id);
        const bool isArt = m_books.contains(id) ? isArticleRecord(m_books.value(id)) : wantArticle;
        const bool had = m_articles.contains(id);
        if (isArt != had) {
            if (isArt)
                m_articles.insert(id);
            else
                m_articles.remove(id);
            m_works->setArticleSet(m_articles);
            emit bookChanged(id);
            changed(false, false);
        }
    } else if (key == QLatin1String("source_url")) {
        const QString trimmed = value.trimmed();
        const QString prev = m_sourceUrls.value(id);
        if (trimmed != prev) {
            if (trimmed.isEmpty())
                m_sourceUrls.remove(id);
            else
                m_sourceUrls.insert(id, trimmed);
            emit bookChanged(id);
        }
    } else if (key == QLatin1String("voiceName")) {
        if (value.isEmpty())
            m_voiceNames.remove(id);
        else
            m_voiceNames.insert(id, value);
        emit bookChanged(id);
        changed(false, false);
    }
}

QList<int> Library::chapterLengths(int id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    return LibrarySchema::readChapterLengths(database(), id);
}

void Library::setChapterLengths(int id, const QList<int> &lengths) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rejectIfReadOnly())
        return;
    QSqlDatabase db = database();
    LibrarySavepoint sp(db, "chapter_lengths");
    if (!sp.isActive())
        return;
    qint64 total = 0;
    if (!LibrarySchema::writeChapterLengths(db, id, lengths, &total, &m_error) || !sp.commit())
        return;
    if (m_books.contains(id) && total > 0 && m_books[id].length != total)
        m_books[id].length = total;
}
