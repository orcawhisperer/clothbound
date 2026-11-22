#pragma once

#include <QString>
#include <QStringList>

// What a collection's file name says, when it follows the common
// "Author - Title [ed. Editor] (Publisher, Year)" pattern.
struct FileNameParts {
    bool matched = false;
    QString author;
    QString title;
    QString editor;
    QString illustrator;
    QString note;
    QString publisher;
    int year = 0;
    bool authorIsEditor = false;
};

// Who and what a book is, reconciled from its own metadata and its file name.
struct BookIdentity {
    QString title;
    QString author;
    QString sortTitle;
    QString sortAuthor;
    QString editor;
    QString publisher;
    int year = 0;
    QString workKey;
};

// Book metadata in the wild is messy: titles and authors swapped, the
// author spelled four ways, "(Penguin Classics)" glued onto titles, files
// named for one book holding another. These helpers turn it into what a
// library should show, and into a key that groups editions of one work.
namespace BookName {
FileNameParts parseFileName(const QString &fileName);

// "Wells, H. G.", "H.G. Wells" and "HG Wells" all read "H. G. Wells".
QString displayAuthor(const QString &name);
// "H. G. Wells" sorts as "Wells, H. G.".
QString sortAuthor(const QString &displayName);
// File-name titles: "First Men in the Moon, The" reads "The First Men in
// the Moon", and the "_" that stood in for ":" turns back.
QString displayTitle(const QString &title);
// Metadata titles without "(Penguin Classics)" or a trailing "-- Author".
QString cleanTitle(const QString &title);
// Lowercase, without a leading article, for sorting.
QString sortTitle(const QString &title);
// Letters and digits only, lowercase, accents removed: for comparing.
QString comparable(const QString &text);
// "Volume 2", "Vol. 3", "Book IV" as a number, or 0.
int volumeNumber(const QString &title);
// Editions of one work share this: the author's surname, the title without
// article, subtitle or punctuation, and the volume.
QString workKey(const QString &author, const QString &title, int volume = 0);
bool looksLikeGarbageTitle(const QString &title);

BookIdentity identify(const QString &filePath, const QString &metadataTitle, const QStringList &metadataAuthors,
                      const QStringList &metadataEditors, const QString &metadataPublisher,
                      const QString &metadataDate);
}
