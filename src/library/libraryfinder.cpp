#include "library/libraryfinder.h"

#include <QDir>
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

LibraryFinder::FormatCounts LibraryFinder::countByFormat(const QString &folder, int depth, int cap) {
    FormatCounts counts;
    if (depth <= 0 || cap <= 0)
        return counts;
    const QDir root(folder);
    if (!root.exists())
        return counts;

    // Iterative depth-bounded traversal so deep trees (node_modules, .git,
    // deeply nested archives) are never descended past `depth`.
    struct Frame {
        QString dirPath;
        int level;
    };
    QList<Frame> stack;
    stack.append({QDir::cleanPath(root.absolutePath()), 0});
    while (!stack.isEmpty() && counts.total() < cap) {
        const Frame frame = stack.takeLast();
        const QDir dir(frame.dirPath);
        const QFileInfoList entries =
            dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
        for (const QFileInfo &entry : entries) {
            if (counts.total() >= cap)
                break;
            if (entry.isDir()) {
                if (entry.isSymLink() || frame.level + 1 >= depth)
                    continue;
                if (entry.fileName() == QLatin1String("node_modules"))
                    continue;
                stack.append({entry.absoluteFilePath(), frame.level + 1});
            } else if (entry.isFile()) {
                const QString name = entry.fileName();
                if (name.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
                    ++counts.pdf;
                else if (name.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive))
                    ++counts.epub;
            }
        }
    }
    return counts;
}

int LibraryFinder::countBooks(const QString &folder, int depth, int cap) {
    return countByFormat(folder, depth, cap).total();
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
    struct Hit {
        QString path;
        FormatCounts counts;
    };
    QList<Hit> hits;
    for (const QString &candidate : candidates) {
        if (std::any_of(watched.cbegin(), watched.cend(), [&](const QString &w) { return inside(candidate, w); }))
            continue;
        const FormatCounts counts = countByFormat(candidate);
        if (counts.total() > 0)
            hits.append({candidate, counts});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit &a, const Hit &b) { return a.counts.total() > b.counts.total(); });
    QVariantList list;
    for (const Hit &hit : std::as_const(hits))
        list.append(QVariantMap{{"path", hit.path},
                                {"label", label(hit.path, home)},
                                {"count", hit.counts.total()},
                                {"epubCount", hit.counts.epub},
                                {"pdfCount", hit.counts.pdf}});
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
