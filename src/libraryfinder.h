#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

// Looks, once and lightly, for books where people usually keep them
// (~/Books, ~/Documents/Books, ~/Calibre Library, Documents, Downloads), so an
// empty library can offer "Found 42 books in ~/Books" instead of a file
// picker. It only counts EPUB and PDF files, never opens them, stops at a few
// levels deep and a few thousand files, and works off the interface thread.
class LibraryFinder : public QObject {
    Q_OBJECT
    // [{path, label, count, epubCount, pdfCount}], the folder with the most books first.
    Q_PROPERTY(QVariantList found READ found NOTIFY changed)
    Q_PROPERTY(bool searching READ searching NOTIFY changed)

public:
    struct FormatCounts {
        int epub = 0;
        int pdf = 0;
        int total() const { return epub + pdf; }
    };

    explicit LibraryFinder(QObject *parent = nullptr);

    QVariantList found() const { return m_found; }
    bool searching() const { return m_searching; }

    // Looks in the usual places, skipping folders already watched.
    Q_INVOKABLE void find(const QStringList &watched = {});

    // The places looked in, in order (for tests: a home of your choosing).
    static QStringList candidates(const QString &home, const QString &documents, const QString &downloads);
    // EPUB and PDF files under `folder`, at most `depth` levels down,
    // counting no further than `cap`.
    static int countBooks(const QString &folder, int depth = 4, int cap = 5000);
    static FormatCounts countByFormat(const QString &folder, int depth = 4, int cap = 5000);
    // "~/Books" for a folder in the home, the full path otherwise.
    static QString label(const QString &path, const QString &home);
    // The candidates holding books, not inside a watched folder, most first.
    static QVariantList search(const QStringList &candidates, const QStringList &watched, const QString &home);

signals:
    void changed();

private:
    QVariantList m_found;
    bool m_searching = false;
    int m_generation = 0;
};
