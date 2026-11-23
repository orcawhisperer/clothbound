#include "aiusage.h"

#include <QSqlQuery>
#include <QVariantMap>

AiUsageLedger::AiUsageLedger(const QString &connection) : m_connection(connection) {}

const QList<AiUsageLedger::Price> &AiUsageLedger::prices() {
    // Standard tier, USD per million tokens, from Google's pricing page
    // (read 2026-11-23). Later entries for a model replace earlier ones from
    // their date on.
    static const QList<Price> table{
        {"gemini-3.8-flash-tts", QDate(2026, 1, 1), 0.50, 9.00, 0},
        {"gemini-3.8-flash-tts", QDate(2027, 1, 1), 1.00, 18.00, 0},
        {"gemini-3.8-flash-lite-tts", QDate(2026, 1, 1), 0.50, 6.00, 0},
        {"gemini-3.8-flash-lite-tts", QDate(2027, 1, 1), 1.00, 12.00, 0},
        {"gemini-4-hydrogen", QDate(2026, 1, 1), 1.50, 7.50, 0.075},
        {"gemini-4-neon", QDate(2026, 1, 1), 3.00, 15.00, 0.15},
        {"gemini-4-carbon", QDate(2026, 1, 1), 2.00, 10.00, 0.10},
        {"gemini-4-carbon", QDate(2026, 12, 1), 4.00, 20.00, 0.20},
        {"gemini-3.5-flash-lite", QDate(2026, 1, 1), 0.30, 2.50, 0.03},
        {"gemini-3.1-flash-lite", QDate(2026, 1, 1), 0.25, 1.50, 0.025},
        {"gemini-nano-banana-2.1", QDate(2026, 1, 1), 1.50, 30.00, 0},
        // Nano Banana 2.1, as the API names it: $30 per million image tokens.
        {"gemini-3.1-flash-image", QDate(2026, 1, 1), 0.50, 30.00, 0},
        {"gemini-embedding-2", QDate(2026, 1, 1), 0.20, 0, 0},
    };
    return table;
}

double AiUsageLedger::cost(const QString &model, const Ai::Usage &usage, const QDate &day, bool flex) {
    const Price *price = nullptr;
    for (const Price &candidate : prices())
        if (model == QLatin1String(candidate.model) && candidate.from <= day &&
            (!price || candidate.from > price->from))
            price = &candidate;
    if (!price)
        return 0;
    const qint64 cached = qMin(usage.cachedTokens, usage.inputTokens);
    const double dollars = ((usage.inputTokens - cached) * price->input + cached * price->cached +
                            (usage.outputTokens + usage.thoughtTokens) * price->output) / 1e6;
    return flex ? dollars / 2 : dollars;
}

double AiUsageLedger::listeningCost(const QString &model, double minutes, const QDate &day, bool flex) {
    Ai::Usage usage;
    usage.outputTokens = qint64(minutes * 60 * 25);
    // About 4 characters a token and 900 characters a minute of narration.
    usage.inputTokens = qint64(minutes * 900 / 4);
    return cost(model, usage, day, flex);
}

bool AiUsageLedger::migrate() {
    QSqlQuery query(database());
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS ai_usage (id INTEGER PRIMARY KEY, at INTEGER NOT NULL, provider TEXT NOT NULL,"
            " model TEXT NOT NULL, feature TEXT NOT NULL, input INTEGER NOT NULL, output INTEGER NOT NULL,"
            " cached INTEGER NOT NULL, thought INTEGER NOT NULL, audio INTEGER NOT NULL, flex INTEGER NOT NULL,"
            " cost REAL NOT NULL)")))
        return false;
    return query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS ai_usage_at ON ai_usage (at)"));
}

void AiUsageLedger::record(const QString &provider, const QString &model, const QString &feature,
                           const Ai::Usage &usage, bool flex, const QDateTime &at) {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("INSERT INTO ai_usage (at, provider, model, feature, input, output, cached, thought,"
                                 " audio, flex, cost) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const QVariant &value :
         QVariantList{at.toSecsSinceEpoch(), provider, model, feature.isEmpty() ? QStringLiteral("other") : feature,
                      usage.inputTokens, usage.outputTokens, usage.cachedTokens, usage.thoughtTokens,
                      usage.audioTokens, flex ? 1 : 0, cost(model, usage, at.date(), flex)})
        query.addBindValue(value);
    query.exec();
}

double AiUsageLedger::spentSince(const QDate &day) const {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("SELECT COALESCE(SUM(cost), 0) FROM ai_usage WHERE at >= ?"));
    query.addBindValue(QDateTime(day, QTime(0, 0)).toSecsSinceEpoch());
    query.exec();
    return query.next() ? query.value(0).toDouble() : 0;
}

QVariantList AiUsageLedger::monthByFeature(const QDate &today) const {
    QSqlQuery query(database());
    query.prepare(QStringLiteral("SELECT feature, SUM(cost), COUNT(*), SUM(audio) FROM ai_usage WHERE at >= ?"
                                 " GROUP BY feature ORDER BY SUM(cost) DESC"));
    query.addBindValue(QDateTime(QDate(today.year(), today.month(), 1), QTime(0, 0)).toSecsSinceEpoch());
    query.exec();
    QVariantList rows;
    while (query.next())
        rows.append(QVariantMap{{"feature", query.value(0)},
                                {"cost", query.value(1).toDouble()},
                                {"requests", query.value(2).toInt()},
                                {"audioMinutes", query.value(3).toDouble() / 25.0 / 60.0}});
    return rows;
}
