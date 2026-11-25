#pragma once

#include "ai/ai.h"

#include <QDate>
#include <QSqlDatabase>
#include <QVariantList>

// What the reader's key has been used for, and roughly what it cost, kept
// in the library database. Prices are data with the dates they apply from,
// since providers change them (Gemini's TTS prices double in 2027).
class AiUsageLedger {
public:
    struct Price {
        const char *model;
        QDate from;
        double input;   // USD per million tokens
        double output;
        double cached;
    };

    explicit AiUsageLedger(const QString &connection);

    bool migrate();
    void record(const QString &provider, const QString &model, const QString &feature, const Ai::Usage &usage,
                bool flex, const QDateTime &at = QDateTime::currentDateTime());
    double spentSince(const QDate &day) const;
    // One row per feature this month: {feature, cost, requests, audioMinutes}.
    QVariantList monthByFeature(const QDate &today = QDate::currentDate()) const;
    // Clears this month's local usage estimate rows (does not affect provider billing).
    void clearMonth(const QDate &today = QDate::currentDate());
    // SQLite PRAGMA data_version to detect commits from sibling CLI/MCP processes,
    // paired with a local write counter for same-connection ledger mutations.
    qint64 dataVersion() const;
    qint64 localVersion() const { return m_localVersion; }

    static const QList<Price> &prices();
    // What a request costs on a given day (local models cost nothing; unrecognized
    // cloud models use a conservative non-zero fallback so model rotations can't
    // bypass budget caps). Flex and batch work costs half.
    static double cost(const QString &model, const Ai::Usage &usage, const QDate &day, bool flex);
    static double costForProvider(const QString &provider, const QString &model, const Ai::Usage &usage,
                                  const QDate &day, bool flex);
    // Listening: Gemini speech uses 25 audio tokens a second.
    static double listeningCost(const QString &model, double minutes, const QDate &day, bool flex);

private:
    QSqlDatabase database() const { return QSqlDatabase::database(m_connection); }
    QString m_connection;
    qint64 m_localVersion = 0;
};
