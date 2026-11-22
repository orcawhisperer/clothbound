#pragma once

#include <QString>

// One book as the library stores it: what it is, and how far it's read.
struct BookRecord {
    int id = 0;
    QString path;
    QString format;
    qint64 size = 0;
    qint64 mtime = 0;
    QString title;
    QString sortTitle;
    QString author;
    QString sortAuthor;
    QString editor;
    QString publisher;
    int year = 0;
    QString language;
    QString description;
    QString subjects;
    QString isbn;
    QString series;
    double seriesIndex = 0;
    QString workKey;
    int chapters = 0;
    int pages = 0;
    qint64 length = 0;
    QString cover;
    QString problem;
    qint64 addedAt = 0;
    qint64 openedAt = 0;
    qint64 finishedAt = 0;
    double progress = 0;
    QString position;
    qint64 secondsRead = 0;
    bool missing = false;
    bool hidden = false;
    bool loose = false;

    bool visible() const { return !missing && !hidden; }
    // "ed. Parrinder · Penguin · 2005"
    QString editionLine() const;
};
