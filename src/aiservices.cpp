#include "aiservices.h"

#include "keystore.h"

#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>

namespace {
QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
}

AiServices::AiServices(Ai::Provider *provider, KeyStore *keys, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_provider(provider), m_keys(keys), m_ledger(databaseConnection) {
    m_ledger.migrate();
    m_budget = settings().value(QStringLiteral("ai/budget"), 10.0).toDouble();
    // A key in the environment wins, so scripts and agents can bring their own.
    const QByteArray environment = qgetenv("GEMINI_API_KEY");
    if (provider->id() == QStringLiteral("gemini") && !environment.trimmed().isEmpty()) {
        provider->setKey(QString::fromUtf8(environment));
        m_fromEnvironment = true;
    } else if (keys) {
        provider->setKey(keys->load(provider->id()));
    }
    m_status = provider->hasKey() ? tr("Ready") : tr("No key yet");
}

QString AiServices::keyLocation() const {
    if (!hasKey())
        return {};
    return m_fromEnvironment ? tr("the GEMINI_API_KEY environment variable") : (m_keys ? m_keys->where() : QString());
}

QString AiServices::textModel() const {
    auto *model = m_provider->text();
    return model ? model->textModelName(false) : QString();
}

QString AiServices::speechModel() const {
    auto *model = m_provider->speech();
    return model ? model->speechModelName(false) : QString();
}

double AiServices::spentThisMonth() const {
    const QDate today = QDate::currentDate();
    return m_ledger.spentSince(QDate(today.year(), today.month(), 1));
}

void AiServices::setBudget(double budget) {
    budget = qMax(0.0, budget);
    if (qFuzzyCompare(budget, m_budget))
        return;
    m_budget = budget;
    settings().setValue(QStringLiteral("ai/budget"), budget);
    emit usageChanged();
}

QVariantList AiServices::usageThisMonth() const { return m_ledger.monthByFeature(); }

bool AiServices::fitsBudget(double cost) const { return m_budget <= 0 || spentThisMonth() + cost <= m_budget; }

double AiServices::listeningCost(double minutes, bool economy, bool flex) const {
    auto *model = m_provider->speech();
    return model ? AiUsageLedger::listeningCost(model->speechModelName(economy), minutes, QDate::currentDate(), flex) : 0;
}

void AiServices::setKey(const QString &key) {
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty())
        return;
    m_provider->setKey(trimmed);
    m_checking = true;
    m_status = tr("Checking the key…");
    emit changed();
    Ai::Reply *reply = m_provider->check();
    connect(reply, &Ai::Reply::finished, this, [this, reply, trimmed] {
        reply->deleteLater();
        m_checking = false;
        if (reply->error()) {
            // A key that doesn't work isn't kept, and the one before stays.
            m_provider->setKey(m_fromEnvironment ? QString::fromUtf8(qgetenv("GEMINI_API_KEY"))
                                                 : (m_keys ? m_keys->load(m_provider->id()) : QString()));
            m_connected = false;
            m_status = describe(reply->error().kind, reply->error().message);
            emit changed();
            emit keyChecked(false, m_status);
            return;
        }
        QString problem;
        const bool saved = m_keys && m_keys->save(m_provider->id(), trimmed, &problem);
        m_fromEnvironment = false;
        m_connected = true;
        m_status = saved ? tr("Connected") : tr("Connected for now; the key couldn't be saved in your keyring (%1).").arg(problem);
        emit changed();
        emit keyChecked(true, m_status);
    });
}

void AiServices::forgetKey() {
    if (m_keys)
        m_keys->remove(m_provider->id());
    m_provider->setKey({});
    m_fromEnvironment = false;
    m_connected = false;
    m_status = tr("No key yet");
    emit changed();
}

void AiServices::check() {
    if (!hasKey() || m_checking)
        return;
    m_checking = true;
    m_status = tr("Checking the key…");
    emit changed();
    Ai::Reply *reply = m_provider->check();
    connect(reply, &Ai::Reply::finished, this, [this, reply] {
        reply->deleteLater();
        m_checking = false;
        m_connected = !reply->error();
        m_status = reply->error() ? describe(reply->error().kind, reply->error().message) : tr("Connected");
        emit changed();
    });
}

void AiServices::track(Ai::Reply *reply, bool flex) {
    connect(reply, &Ai::Reply::finished, this, [this, reply, flex] {
        const Ai::Usage usage = reply->usage();
        if (usage.inputTokens || usage.outputTokens) {
            m_ledger.record(m_provider->id(), reply->model(), reply->feature(), usage, flex);
            emit usageChanged();
        }
        if (reply->error().kind == Ai::Error::BadKey && m_connected) {
            m_connected = false;
            m_status = describe(Ai::Error::BadKey);
            emit changed();
        }
    });
}

QString AiServices::describe(int errorKind, const QString &detail) const {
    switch (Ai::Error::Kind(errorKind)) {
    case Ai::Error::None:
        return {};
    case Ai::Error::NoKey:
        return tr("Add your Gemini key to use this.");
    case Ai::Error::BadKey:
        return tr("Gemini didn't accept the key. You can check it at aistudio.google.com/apikey.");
    case Ai::Error::Limited:
        return tr("Your key has reached Gemini's limit for now. It resets later; a key with billing has higher limits.");
    case Ai::Error::Budget:
        return tr("This would go past your monthly budget. You can raise it in Gemini settings.");
    case Ai::Error::Offline:
        return tr("Can't reach Gemini. Check your connection.");
    case Ai::Error::Refused:
        return tr("Gemini refused this request with your key.");
    case Ai::Error::Server:
        return tr("Gemini had a problem. Try again in a moment.");
    case Ai::Error::Unsupported:
        return tr("%1 can't do this.").arg(providerName());
    case Ai::Error::Cancelled:
        return tr("Cancelled.");
    case Ai::Error::Invalid:
        return detail.isEmpty() ? tr("Gemini couldn't handle this request.")
                                : tr("Gemini couldn't handle this request: %1").arg(detail);
    }
    return detail;
}
