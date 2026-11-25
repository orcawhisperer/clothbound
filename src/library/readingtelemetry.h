#pragma once

#include "formats/bookrecord.h"

#include <QDate>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

// Reading telemetry, daily streak aggregation, pace learning, shelf counts, and
// per-book reading statistics backed by library.sqlite's reading_days and meta tables.
namespace ReadingTelemetry {
inline constexpr double kDefaultCharsPerMinute = 1300.0;

struct RecordResult {
    bool ok = false;
    bool paceUpdated = false;
    double newCharsPerMinute = kDefaultCharsPerMinute;
    QString error;
};

double loadCharsPerMinute(const QSqlDatabase &db, double fallback = kDefaultCharsPerMinute);

int minutesLeft(const BookRecord &record, double charsPerMinute);

// Atomically records elapsed reading time into `books.seconds_read` and
// `reading_days`, and updates learned pace (`meta.chars_per_minute`) when
// enough normal reading has accumulated.
RecordResult recordReadingTime(QSqlDatabase &db, int bookId, int seconds, qint64 charsRead,
                               double currentCharsPerMinute, const QDate &today = QDate::currentDate());

// Library-wide reading insights: status counts, totals, current & longest
// consecutive reading streaks, and 14-day daily activity chart.
QVariantMap readingStats(const QSqlDatabase &db, const QHash<int, BookRecord> &books, double charsPerMinute,
                         const QDate &today = QDate::currentDate());

// Per-book reading insights from `reading_days` and estimated completion time.
QVariantMap bookReadingStats(const QSqlDatabase &db, const BookRecord &record, double charsPerMinute);

// How many visible books each sidebar shelf holds.
QVariantMap shelfCounts(const QHash<int, BookRecord> &books, const QSet<int> &articles,
                        const QSet<int> &downloadedBooks, const QSet<int> &audiobookBooks, qint64 nowEpoch = -1);
}  // namespace ReadingTelemetry
