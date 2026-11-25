#pragma once

#include "ai/ai.h"

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QVariantList>

class QThread;

// SQLite persistence layer for Companion & Illustrator: schema creation,
// forward migrations, conversation threads, spoiler-safe Who's Who / Glossary
// and Recap / Study Brief snapshots, reading mode preferences, shelf book
// discovery, and generated illustration records.
class CompanionStore {
public:
    struct Citation {
        int bookId = 0;
        QString bookTitle;
        int chapter = 0;
        qint64 offset = 0;
        QString label;
        QString excerpt;
    };

    struct Turn {
        bool fromUser = true;
        QString text;
        QList<Citation> sources;
        QString provenance;
        bool local = false;
        QString snipDataUrl;
        int chapter = -1;
        qint64 offset = -1;
    };

    struct ShelfBook {
        int id = 0;
        QString title;
        QString format;
        QString path;
    };

    struct WhosWhoSnapshot {
        int chapter = -1;
        qint64 offset = -1;
        QString whereLabel;
        bool isLocal = false;
        QVariantList people;
    };

    struct RecapSnapshot {
        int chapter = -1;
        qint64 offset = -1;
        QString whereLabel;
        bool isLocal = false;
        QString text;
    };

    struct PictureRecord {
        int id = 0;
        int bookId = 0;
        int chapter = 0;
        qint64 offset = 0;
        QString caption;
        QString prompt;
        QString file;
        qint64 created = 0;
        QString style;
        QString passage;
    };

    explicit CompanionStore(const QString &databaseConnection);

    void ensureTables();
    void ensurePictureTable();

    QString loadReadingMode(int bookId) const;
    void saveReadingMode(int bookId, const QString &mode);
    QList<ShelfBook> shelfBooksFor(int bookId, QString *labelOut = nullptr) const;

    QList<Turn> loadConversation(int bookId) const;
    bool saveConversation(int bookId, const QList<Turn> &turns);
    void clearConversation(int bookId);

    bool loadWhosWhoSnapshot(int bookId, const QString &mode, bool wholeStudy, int chapter, qint64 pageEnd,
                             qint64 pageStart, bool requireSameChapter, WhosWhoSnapshot *out) const;
    bool saveWhosWhoSnapshot(int bookId, const QString &mode, int chapter, qint64 offset, qint64 pageStart,
                             const QString &whereLabel, bool isLocal, const QVariantList &people);

    bool loadRecapSnapshot(int bookId, const QString &mode, bool wholeStudy, int chapter, qint64 pageEnd,
                           qint64 pageStart, bool requireSameChapter, RecapSnapshot *out) const;
    bool saveRecapSnapshot(int bookId, const QString &mode, int chapter, qint64 offset, qint64 pageStart,
                           const QString &whereLabel, bool isLocal, const QString &text);

    void clearBookHistory(int bookId);

    // Generated illustration persistence (illustrations table).
    QList<PictureRecord> loadPictures(int bookId) const;
    int insertPicture(const PictureRecord &record);
    bool findPicture(int id, int bookId, PictureRecord *out) const;
    QString deletePicture(int id);

    static QVariantList citationsToVariant(const QList<Citation> &citations);
    static QJsonArray citationsToJson(const QList<Citation> &citations);
    static QList<Citation> citationsFromJson(const QByteArray &json);

    static bool isTurnVisible(const Turn &turn, bool filterSpoiler, int readerChapter, qint64 readerPageEnd);
    static QList<Turn> filterVisibleTurns(const QList<Turn> &turns, bool filterSpoiler, int readerChapter,
                                          qint64 readerPageEnd);
    static QVariantList conversationToVariant(const QList<Turn> &turns, bool filterSpoiler, int readerChapter,
                                              qint64 readerPageEnd);
    static QList<Ai::Message> buildHistoryMessages(const QList<Turn> &turns, qint64 budget, bool filterSpoiler,
                                                   int readerChapter, qint64 readerPageEnd);

private:
    QString m_connection;
    const QThread *m_ownerThread = nullptr;
};
