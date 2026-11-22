#pragma once

#include "bookinfo.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <atomic>
#include <memory>

// One book file as the scanner read it, ready for the library to store.
struct ScannedBook {
    BookInfo info;
    QString coverFile;  // in the cover cache, or empty
    QList<int> chapterLengths;
    qint64 length = 0;
};
Q_DECLARE_METATYPE(ScannedBook)

// Finds books under the library's folders and reads the new or changed ones
// on worker threads: metadata, a cover for the cache, and how much text
// each chapter holds. It never writes anywhere but the cover cache.
class Scanner : public QObject {
    Q_OBJECT

public:
    explicit Scanner(const QString &coverDirectory, QObject *parent = nullptr);
    ~Scanner() override;

    // `known` maps each stored path to the size and modification time it
    // was read at, so unchanged books are skipped.
    void scan(const QStringList &folders, const QStringList &files,
              const QHash<QString, QPair<qint64, qint64>> &known);
    void cancel();
    bool running() const { return m_running; }

    // Reads one book file: metadata, a cached cover, chapter lengths.
    static ScannedBook read(const QString &path, const QString &coverDirectory);
    static QString coverFileFor(const QString &bookPath);
    static constexpr int coverHeight = 900;

signals:
    // Every book path found under the folders, read or not.
    void listed(const QStringList &paths, const QStringList &folders);
    void bookRead(const ScannedBook &book);
    void progressChanged(int done, int total);
    void finished();

private:
    void readNext();

    QString m_coverDirectory;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    bool m_running = false;
    QStringList m_queue;
    int m_total = 0;
    int m_done = 0;
    int m_inFlight = 0;
    int m_generation = 0;
};
