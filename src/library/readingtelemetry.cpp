#include "library/readingtelemetry.h"

#include "library/librarymodel.h"
#include "library/libraryschema.h"

#include <QDateTime>
#include <QLocale>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariantList>
#include <algorithm>
#include <cmath>

namespace ReadingTelemetry {

double loadCharsPerMinute(const QSqlDatabase &db, double fallback) {
    if (!db.isOpen())
        return fallback;
    QSqlQuery speed(db);
    if (speed.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'chars_per_minute'")) && speed.next()) {
        const double loaded = speed.value(0).toDouble();
        if (loaded > 0.0)
            return loaded;
    }
    return fallback;
}

int minutesLeft(const BookRecord &record, double charsPerMinute) {
    if (record.length <= 0 || charsPerMinute <= 0.0)
        return 0;
    const double remaining = std::clamp(1.0 - record.progress, 0.0, 1.0);
    return qMax(1, int(std::ceil(record.length * remaining / charsPerMinute)));
}

RecordResult recordReadingTime(QSqlDatabase &db, int bookId, int seconds, qint64 charsRead,
                               double currentCharsPerMinute, const QDate &today) {
    RecordResult out;
    out.newCharsPerMinute = currentCharsPerMinute;
    if (bookId <= 0 || seconds <= 0 || !db.isOpen())
        return out;

    LibrarySavepoint sp(db, "reading_time");
    if (!sp.isActive()) {
        out.error = db.lastError().text();
        return out;
    }

    auto fail = [&](const QSqlQuery &q) {
        out.error = q.lastError().text();
        qWarning("Library: %s", qPrintable(out.error));
        return out;
    };

    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE books SET seconds_read = seconds_read + ? WHERE id = ?"));
    query.addBindValue(seconds);
    query.addBindValue(bookId);
    if (!query.exec())
        return fail(query);

    QSqlQuery day(db);
    day.prepare(QStringLiteral("INSERT INTO reading_days (book_id, day, seconds) VALUES (?, ?, ?) "
                               "ON CONFLICT (book_id, day) DO UPDATE SET seconds = seconds + excluded.seconds"));
    day.addBindValue(bookId);
    day.addBindValue(today.toString(Qt::ISODate));
    day.addBindValue(seconds);
    if (!day.exec())
        return fail(day);

    // The reader's own pace, learned from real reading: characters passed
    // per minute, once there's enough of it to trust, within human bounds.
    double pacedSeconds = 0.0, pacedChars = 0.0;
    double newPace = currentCharsPerMinute;
    bool paceUpdated = false;
    {
        QSqlQuery readTotals(db);
        if (!readTotals.exec(QStringLiteral("SELECT key, value FROM meta WHERE key IN ('paced_seconds', 'paced_chars')")))
            return fail(readTotals);
        while (readTotals.next())
            (readTotals.value(0).toString() == QLatin1String("paced_seconds") ? pacedSeconds : pacedChars) =
                readTotals.value(1).toDouble();
    }
    // A jump (a search hit, the contents) isn't reading.
    if (charsRead > 0 && charsRead < qint64(seconds) * 120) {
        pacedSeconds += seconds;
        pacedChars += double(charsRead);
        QSqlQuery save(db);
        save.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES ('paced_seconds', ?), "
                                    "('paced_chars', ?), ('chars_per_minute', ?)"));
        newPace = pacedSeconds >= 600.0 ? std::clamp(pacedChars / pacedSeconds * 60.0, 400.0, 4000.0)
                                        : currentCharsPerMinute;
        save.addBindValue(pacedSeconds);
        save.addBindValue(pacedChars);
        save.addBindValue(newPace);
        if (!save.exec())
            return fail(save);
        paceUpdated = newPace != currentCharsPerMinute;
    }

    if (!sp.commit()) {
        out.error = db.lastError().text();
        return out;
    }
    out.ok = true;
    out.paceUpdated = paceUpdated;
    out.newCharsPerMinute = newPace;
    return out;
}

QVariantMap readingStats(const QSqlDatabase &db, const QHash<int, BookRecord> &books, double charsPerMinute,
                         const QDate &today) {
    int totalBooks = 0, finishedBooks = 0, readingBooks = 0, unreadBooks = 0;
    qint64 totalSeconds = 0;
    for (const BookRecord &r : std::as_const(books)) {
        if (!r.visible())
            continue;
        ++totalBooks;
        totalSeconds += r.secondsRead;
        if (r.finishedAt > 0)
            ++finishedBooks;
        else if (r.openedAt > 0)
            ++readingBooks;
        else
            ++unreadBooks;
    }
    Q_ASSERT(finishedBooks + readingBooks + unreadBooks == totalBooks);

    QHash<QDate, qint64> byDay;
    QList<QDate> activeDates;
    qint64 daysSecondsSum = 0;
    if (db.isOpen()) {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT day, SUM(seconds) FROM reading_days WHERE seconds > 0 GROUP BY day ORDER BY day ASC"))) {
            while (q.next()) {
                const QDate d = QDate::fromString(q.value(0).toString(), Qt::ISODate);
                const qint64 sec = q.value(1).toLongLong();
                if (d.isValid() && sec > 0) {
                    byDay.insert(d, sec);
                    activeDates.append(d);
                    daysSecondsSum += sec;
                }
            }
        }
    }
    totalSeconds = qMax(totalSeconds, daysSecondsSum);

    const qint64 todaySeconds = byDay.value(today, 0);
    qint64 weekSeconds = 0;
    for (int i = 0; i < 7; ++i)
        weekSeconds += byDay.value(today.addDays(-i), 0);

    // Current streak: counts backwards from today if read today, else from yesterday.
    int streakDays = 0;
    QDate cursor = byDay.contains(today) ? today : today.addDays(-1);
    while (byDay.contains(cursor)) {
        ++streakDays;
        cursor = cursor.addDays(-1);
    }

    // Longest consecutive reading streak ever recorded.
    int longestStreak = 0;
    int run = 0;
    QDate prev;
    for (const QDate &d : std::as_const(activeDates)) {
        run = (prev.isValid() && prev.daysTo(d) == 1) ? (run + 1) : 1;
        longestStreak = qMax(longestStreak, run);
        prev = d;
    }
    Q_ASSERT(streakDays <= longestStreak);

    // Last 14 days for the sparkline / activity bar chart.
    QVariantList recentDays;
    recentDays.reserve(14);
    for (int i = 13; i >= 0; --i) {
        const QDate d = today.addDays(-i);
        recentDays.append(QVariantMap{{QStringLiteral("day"), d.toString(Qt::ISODate)},
                                      {QStringLiteral("shortLabel"), QLocale().toString(d, QStringLiteral("ddd"))},
                                      {QStringLiteral("seconds"), double(byDay.value(d, 0))}});
    }

    return {
        {QStringLiteral("totalBooks"), totalBooks},
        {QStringLiteral("finishedBooks"), finishedBooks},
        {QStringLiteral("readingBooks"), readingBooks},
        {QStringLiteral("unreadBooks"), unreadBooks},
        {QStringLiteral("totalSeconds"), double(totalSeconds)},
        {QStringLiteral("todaySeconds"), double(todaySeconds)},
        {QStringLiteral("weekSeconds"), double(weekSeconds)},
        {QStringLiteral("streakDays"), streakDays},
        {QStringLiteral("longestStreak"), longestStreak},
        {QStringLiteral("activeDays"), int(activeDates.size())},
        {QStringLiteral("charsPerMinute"), charsPerMinute},
        {QStringLiteral("wordsPerMinute"), qMax(50, int(std::round(charsPerMinute / 5.0)))},
        {QStringLiteral("recentDays"), recentDays},
    };
}

QVariantMap bookReadingStats(const QSqlDatabase &db, const BookRecord &record, double charsPerMinute) {
    if (record.id == 0)
        return {};
    int daysRead = 0;
    QString firstDay, lastDay;
    if (db.isOpen()) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT COUNT(*), MIN(day), MAX(day) FROM reading_days WHERE book_id = ? AND seconds > 0"));
        q.addBindValue(record.id);
        if (q.exec() && q.next()) {
            daysRead = q.value(0).toInt();
            firstDay = q.value(1).toString();
            lastDay = q.value(2).toString();
        }
    }
    const int totalEstimatedMinutes =
        (record.length > 0 && charsPerMinute > 0.0) ? qMax(1, int(std::ceil(record.length / charsPerMinute))) : 0;
    return {
        {QStringLiteral("bookId"), record.id},
        {QStringLiteral("secondsRead"), double(record.secondsRead)},
        {QStringLiteral("daysRead"), daysRead},
        {QStringLiteral("firstDay"), firstDay},
        {QStringLiteral("lastDay"), lastDay},
        {QStringLiteral("averageSecondsPerDay"), daysRead > 0 ? double(record.secondsRead) / daysRead : 0.0},
        {QStringLiteral("minutesLeft"), minutesLeft(record, charsPerMinute)},
        {QStringLiteral("estimatedTotalMinutes"), totalEstimatedMinutes},
    };
}

QVariantMap shelfCounts(const QHash<int, BookRecord> &books, const QSet<int> &articles,
                        const QSet<int> &downloadedBooks, const QSet<int> &audiobookBooks, qint64 nowEpoch) {
    int all = 0, reading = 0, recent = 0, want = 0, finished = 0, listening = 0, epub = 0, pdf = 0, articleCount = 0,
        downloaded = 0, audiobooks = 0;
    const qint64 epoch = nowEpoch >= 0 ? nowEpoch : QDateTime::currentSecsSinceEpoch();
    const qint64 recentCutoff = epoch - WorksModel::recentWindowSecs;
    for (const auto &r : std::as_const(books)) {
        if (!r.visible())
            continue;
        ++all;
        const bool isArt = articles.contains(r.id);
        if (isArt)
            ++articleCount;
        if (r.format.compare(QLatin1String("epub"), Qt::CaseInsensitive) == 0 && !isArt)
            ++epub;
        else if (r.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
            ++pdf;
        if (r.addedAt > 0 && r.addedAt >= recentCutoff)
            ++recent;
        if (r.finishedAt > 0)
            ++finished;
        else if (r.openedAt > 0)
            ++reading;
        if (r.wantAt > 0 && r.finishedAt == 0)
            ++want;
        if (r.listenedAt > 0)
            ++listening;
        if (downloadedBooks.contains(r.id))
            ++downloaded;
        if (audiobookBooks.contains(r.id))
            ++audiobooks;
    }
    return {{QStringLiteral("all"), all},
            {QStringLiteral("reading"), reading},
            {QStringLiteral("recent"), recent},
            {QStringLiteral("want"), want},
            {QStringLiteral("finished"), finished},
            {QStringLiteral("listening"), listening},
            {QStringLiteral("epub"), epub},
            {QStringLiteral("pdf"), pdf},
            {QStringLiteral("articles"), articleCount},
            {QStringLiteral("downloaded"), downloaded},
            {QStringLiteral("audiobooks"), audiobooks}};
}

}  // namespace ReadingTelemetry
