#include "libraryfinder.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>

LibraryFinder::LibraryFinder(QObject *parent) : QObject(parent) {}

QStringList LibraryFinder::candidates(const QString &home, const QString &documents, const QString &downloads) {
    QStringList list{home + "/Books", home + "/books", home + "/Documents/Books", home + "/Calibre Library",
                     home + "/Ebooks", home + "/eBooks"};
    if (!documents.isEmpty())
        list.append(documents);
    if (!downloads.isEmpty())
        list.append(downloads);
    // The same folder reached two ways (a symlink, or Documents being ~)
    // is looked at once; the home itself is never searched wholesale.
    QStringList unique;
    QStringList seen;
    for (const QString &path : std::as_const(list)) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty() || seen.contains(canonical) || canonical == QFileInfo(home).canonicalFilePath())
            continue;
        seen.append(canonical);
        unique.append(path);
    }
    return unique;
}

int LibraryFinder::countBooks(const QString &folder, int depth, int cap) {
    const QDir root(folder);
    if (!root.exists())
        return 0;
    const int rootDepth = QDir::cleanPath(root.absolutePath()).count('/');
    int count = 0;
    QDirIterator it(root.absolutePath(), {"*.epub", "*.EPUB", "*.pdf", "*.PDF"}, QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && count < cap) {
        const QString path = it.next();
        if (QDir::cleanPath(QFileInfo(path).absolutePath()).count('/') - rootDepth >= depth)
            continue;
        ++count;
    }
    return count;
}

QString LibraryFinder::label(const QString &path, const QString &home) {
    const QString clean = QDir::cleanPath(path), cleanHome = QDir::cleanPath(home);
    if (clean.startsWith(cleanHome + '/'))
        return "~" + clean.mid(cleanHome.size());
    return clean;
}

QVariantList LibraryFinder::search(const QStringList &candidates, const QStringList &watched, const QString &home) {
    auto inside = [](const QString &path, const QString &folder) {
        const QString a = QFileInfo(path).canonicalFilePath(), b = QFileInfo(folder).canonicalFilePath();
        return !a.isEmpty() && !b.isEmpty() && (a == b || a.startsWith(b + '/') || b.startsWith(a + '/'));
    };
    struct Hit { QString path; int count; };
    QList<Hit> hits;
    for (const QString &candidate : candidates) {
        if (std::any_of(watched.cbegin(), watched.cend(), [&](const QString &w) { return inside(candidate, w); }))
            continue;
        if (const int count = countBooks(candidate))
            hits.append({candidate, count});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.count > b.count; });
    QVariantList list;
    for (const Hit &hit : std::as_const(hits))
        list.append(QVariantMap{{"path", hit.path}, {"label", label(hit.path, home)}, {"count", hit.count}});
    return list;
}

void LibraryFinder::find(const QStringList &watched) {
    const int generation = ++m_generation;
    m_searching = true;
    emit changed();
    const QString home = QDir::homePath();
    const QStringList places = candidates(home, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    auto *watcher = new QFutureWatcher<QVariantList>(this);
    connect(watcher, &QFutureWatcher<QVariantList>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        m_found = watcher->result();
        m_searching = false;
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([places, watched, home] { return search(places, watched, home); }));
}
