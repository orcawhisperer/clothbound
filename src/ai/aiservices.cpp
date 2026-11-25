#include "ai/aiservices.h"

#include "ai/keystore.h"
#include "ai/localmodels.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <cmath>

namespace {
QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
}

AiServices::AiServices(Ai::Provider *provider, KeyStore *keys, const QString &databaseConnection, QObject *parent)
    : QObject(parent), m_provider(provider), m_keys(keys), m_ledger(databaseConnection) {
    m_ledger.migrate();
    QSettings s = settings();
    m_budget = qMax(0.0, s.value(QStringLiteral("ai/budget"), 10.0).toDouble());
    Q_ASSERT(m_budget >= 0.0 && std::isfinite(m_budget));
    m_unlimitedBudget = s.value(QStringLiteral("ai/unlimitedBudget"), false).toBool();
    m_preset = s.value(QStringLiteral("ai/preset")).toString();
    s.beginGroup(QStringLiteral("ai/route"));
    for (const QString &feature : s.childKeys()) {
        const QString route = s.value(feature).toString();
        if (!route.isEmpty())
            m_overrides.insert(feature, route);
    }
    s.endGroup();
    // A key in the environment wins, so scripts and agents can bring their own.
    const QByteArray environment = qgetenv("GEMINI_API_KEY");
    if (provider->id() == QStringLiteral("gemini") && !environment.trimmed().isEmpty()) {
        provider->setKey(QString::fromUtf8(environment));
        m_fromEnvironment = true;
    } else if (keys) {
        m_savedKey = keys->load(provider->id());
        provider->setKey(m_savedKey);
    }
    m_status = provider->hasKey() ? tr("Ready") : tr("No key yet");
}

void AiServices::lookUpKeyIn(KeyStore *keys) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_keys = keys;
    if (!keys || m_fromEnvironment)
        return;
    m_status = tr("Looking in your keyring…");
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        watcher->deleteLater();
        if (m_keyChangedHere)
            return;
        m_savedKey = watcher->result();
        m_provider->setKey(m_savedKey);
        m_status = m_provider->hasKey() ? tr("Ready") : tr("No key yet");
        emit changed();
    });
    watcher->setFuture(keys->loadLater(m_provider->id()));
}

QString AiServices::keyLocation() const {
    if (!hasKey())
        return {};
    return m_fromEnvironment ? tr("the GEMINI_API_KEY environment variable") : (m_keys ? m_keys->where() : QString());
}

QString AiServices::maskedKey() const {
    if (!hasKey())
        return {};
    const QString key = m_fromEnvironment ? QString::fromUtf8(qgetenv("GEMINI_API_KEY")).trimmed() : m_savedKey.trimmed();
    if (key.size() < 12)
        return QStringLiteral("••••••••");
    return key.left(4) + QStringLiteral("••••") + key.right(4);
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
    const QDate monthStart(today.year(), today.month(), 1);
    const qint64 dataVer = m_ledger.dataVersion();
    const qint64 localVer = m_ledger.localVersion();
    if (m_spentCache < 0 || m_spentMonth != monthStart || dataVer < 0 || dataVer != m_spentDataVersion ||
        localVer != m_spentLocalVersion) {
        m_spentMonth = monthStart;
        m_spentDataVersion = dataVer;
        m_spentLocalVersion = localVer;
        m_spentCache = m_ledger.spentSince(monthStart);
    }
    Q_ASSERT(m_spentCache >= 0.0 && std::isfinite(m_spentCache));
    return m_spentCache;
}

void AiServices::setBudget(double budget) {
    Q_ASSERT(QThread::currentThread() == thread());
    budget = qMax(0.0, budget);
    Q_ASSERT(budget >= 0.0 && std::isfinite(budget));
    if (qFuzzyCompare(budget, m_budget))
        return;
    m_budget = budget;
    settings().setValue(QStringLiteral("ai/budget"), budget);
    emit usageChanged();
}

void AiServices::setUnlimitedBudget(bool unlimited) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (unlimited == m_unlimitedBudget)
        return;
    m_unlimitedBudget = unlimited;
    settings().setValue(QStringLiteral("ai/unlimitedBudget"), unlimited);
    emit usageChanged();
}

QVariantList AiServices::usageThisMonth() const { return m_ledger.monthByFeature(); }

void AiServices::resetUsageThisMonth() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_ledger.clearMonth();
    m_spentCache = -1;
    emit usageChanged();
}

bool AiServices::fitsBudget(double cost) const {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(m_budget >= 0.0 && std::isfinite(m_budget));
    Q_ASSERT(m_reservedUsd >= 0.0 && std::isfinite(m_reservedUsd));
    if (m_unlimitedBudget)
        return true;
    if (m_budget <= 0)
        return false;
    return spentThisMonth() + m_reservedUsd + qMax(0.0, cost) <= m_budget;
}

double AiServices::listeningCost(double minutes, bool economy, bool flex) const {
    auto *model = m_provider->speech();
    return model ? AiUsageLedger::listeningCost(model->speechModelName(economy), minutes, QDate::currentDate(), flex) : 0;
}

void AiServices::setKey(const QString &key) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty())
        return;
    if (m_keyCheckReply)
        m_keyCheckReply->abort();
    const int generation = ++m_keyGeneration;
    m_provider->setKey(trimmed);
    m_keyChangedHere = true;
    m_checking = true;
    m_checkFailed = false;
    m_status = tr("Checking the key…");
    emit changed();
    Ai::Reply *reply = m_provider->check();
    m_keyCheckReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply, trimmed, generation] {
        reply->deleteLater();
        if (generation != m_keyGeneration)
            return;
        if (reply->error()) {
            m_checking = false;
            m_checkFailed = true;
            // A key that doesn't work isn't kept, and the one before stays.
            m_provider->setKey(m_fromEnvironment ? QString::fromUtf8(qgetenv("GEMINI_API_KEY")) : m_savedKey);
            m_connected = false;
            m_status = describe(reply->error().kind, reply->error().message);
            emit changed();
            emit keyChecked(false, m_status);
            return;
        }
        auto finish = [this, trimmed](bool saved, const QString &problem) {
            m_checking = false;
            m_checkFailed = false;
            if (saved)
                m_savedKey = trimmed;
            m_fromEnvironment = false;
            m_connected = true;
            m_status = saved ? tr("Connected") : tr("Connected for now; the key couldn't be saved in your keyring (%1).").arg(problem);
            emit changed();
            emit keyChecked(true, m_status);
        };
        if (!m_keys) {
            finish(false, tr("no keyring"));
            return;
        }
        auto *watcher = new QFutureWatcher<QPair<bool, QString>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, finish] {
            watcher->deleteLater();
            if (generation != m_keyGeneration)
                return;
            const auto [saved, problem] = watcher->result();
            finish(saved, problem);
        });
        watcher->setFuture(m_keys->saveLater(m_provider->id(), trimmed));
    });
}

void AiServices::forgetKey() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_keyGeneration;
    if (m_keyCheckReply)
        m_keyCheckReply->abort();
    m_checking = false;
    m_checkFailed = false;
    if (m_keys)
        (void)m_keys->removeLater(m_provider->id());
    m_savedKey.clear();
    m_keyChangedHere = true;
    m_provider->setKey({});
    m_fromEnvironment = false;
    m_connected = false;
    m_status = tr("No key yet");
    emit changed();
}

void AiServices::check() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!hasKey() || m_checking)
        return;
    const int generation = ++m_keyGeneration;
    m_checking = true;
    m_checkFailed = false;
    m_status = tr("Checking the key…");
    emit changed();
    Ai::Reply *reply = m_provider->check();
    m_keyCheckReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_keyGeneration)
            return;
        m_checking = false;
        m_connected = !reply->error();
        m_checkFailed = bool(reply->error());
        m_status = reply->error() ? describe(reply->error().kind, reply->error().message) : tr("Connected");
        emit changed();
    });
}

void AiServices::track(Ai::Reply *reply, bool flex, double estimatedCost) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!reply)
        return;
    // A reply belongs to the provider that made it (a local one costs nothing).
    const auto *maker = qobject_cast<Ai::Provider *>(reply->parent());
    const QString providerId = maker ? maker->id() : m_provider->id();
    const bool isCloud = (providerId != QLatin1String("local") && providerId != QLatin1String("builtin"));

    quint64 reservationId = 0;
    if (isCloud && !reply->isFinished()) {
        double reserve = estimatedCost;
        if (reserve < 0.0) {
            const QString feat = reply->feature();
            if (feat == QLatin1String("illustrate"))
                reserve = m_provider && m_provider->images() ? m_provider->images()->imageCost() : 0.035;
            else if (feat == QLatin1String("listen") || feat == QLatin1String("voice design"))
                reserve = 0.01;
            else if (feat == QLatin1String("embed"))
                reserve = 0.001;
            else
                reserve = 0.01;
        }
        if (reserve > 0.0) {
            Q_ASSERT(std::isfinite(reserve));
            reservationId = m_nextReservationId++;
            m_reservations.insert(reservationId, reserve);
            m_reservedUsd += reserve;
            Q_ASSERT(m_reservedUsd >= 0.0 && std::isfinite(m_reservedUsd));
        }
    }

    auto releaseReservation = [this, reservationId]() {
        if (reservationId == 0)
            return;
        auto it = m_reservations.find(reservationId);
        if (it == m_reservations.end())
            return;
        m_reservedUsd = qMax(0.0, m_reservedUsd - it.value());
        m_reservations.erase(it);
        if (m_reservations.isEmpty())
            m_reservedUsd = 0.0;
        Q_ASSERT(m_reservedUsd >= 0.0 && std::isfinite(m_reservedUsd));
    };

    if (reservationId != 0) {
        connect(reply, &QObject::destroyed, this, [releaseReservation]() { releaseReservation(); });
    }

    connect(reply, &Ai::Reply::finished, this, [this, reply, flex, providerId, releaseReservation] {
        releaseReservation();
        const Ai::Usage usage = reply->usage();
        if (usage.inputTokens || usage.outputTokens || usage.thoughtTokens || usage.audioTokens) {
            m_ledger.record(providerId, reply->model(), reply->feature(), usage, flex);
            m_spentCache = -1;
            emit usageChanged();
        }
        if (reply->error().kind == Ai::Error::BadKey && providerId == m_provider->id() && m_connected) {
            m_connected = false;
            m_checkFailed = true;
            m_status = describe(Ai::Error::BadKey);
            emit changed();
        }
    });
}

QString AiServices::describe(int errorKind, const QString &detail) const {
    return describeFor(m_provider, errorKind, detail);
}

QString AiServices::describeFor(const Ai::Provider *provider, int errorKind, const QString &detail) const {
    if (provider && provider->id() == QLatin1String("local")) {
        const QString who = provider->name();
        switch (Ai::Error::Kind(errorKind)) {
        case Ai::Error::None:
            return {};
        case Ai::Error::Offline:
            return tr("Can't reach %1. Make sure the model server is running.").arg(who);
        case Ai::Error::NotFound:
            return detail.isEmpty() ? tr("%1 doesn't have that model.").arg(who)
                                    : tr("%1 doesn't have that model: %2").arg(who, detail);
        case Ai::Error::BadKey:
        case Ai::Error::Refused:
            return tr("%1 rejected the request.").arg(who);
        case Ai::Error::Server:
            return detail.isEmpty() ? tr("%1 had a problem. Try again in a moment.").arg(who)
                                    : tr("%1 had a problem: %2").arg(who, detail);
        case Ai::Error::Unsupported:
            return tr("%1 can't do this.").arg(who);
        case Ai::Error::Cancelled:
            return tr("Cancelled.");
        case Ai::Error::Invalid:
            return detail.isEmpty() ? tr("%1 couldn't handle this request.").arg(who)
                                    : tr("%1 couldn't handle this request: %2").arg(who, detail);
        default:
            return detail.isEmpty() ? tr("%1 couldn't finish this request.").arg(who) : detail;
        }
    }
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
        return tr("This would go past your monthly budget. You can raise it in Intelligence settings.");
    case Ai::Error::Offline:
        return tr("Can't reach Gemini. Check your connection.");
    case Ai::Error::Refused:
        return tr("Gemini refused this request with your key.");
    case Ai::Error::Server:
        return tr("Gemini had a problem. Try again in a moment.");
    case Ai::Error::Unsupported:
        return tr("%1 can't do this.").arg(provider ? provider->name() : providerName());
    case Ai::Error::Cancelled:
        return tr("Cancelled.");
    case Ai::Error::NotFound:
        return tr("Gemini no longer has this. A voice that isn't used for a year is let go.");
    case Ai::Error::Invalid:
        return detail.isEmpty() ? tr("Gemini couldn't handle this request.")
                                : tr("Gemini couldn't handle this request: %1").arg(detail);
    }
    return detail;
}

// --- Routing -----------------------------------------------------------------

void AiServices::setLocalModels(LocalModels *local) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_localModels)
        disconnect(m_localModels, &LocalModels::contextWindowChanged, this, &AiServices::routingChanged);
    m_localModels = local;
    if (!local)
        return;
    connect(local, &LocalModels::contextWindowChanged, this, &AiServices::routingChanged);
    QSettings s = settings();
    const QString url = s.value(QStringLiteral("local/url")).toString();
    const QUrl parsed = LocalModels::normalizeServerUrl(QUrl(url));
    if (LocalModels::isHttpUrl(parsed)) {
        LocalModels::Server server;
        server.kind = LocalModels::kindFromId(s.value(QStringLiteral("local/kind")).toString());
        server.url = parsed;
        local->configure(server, s.value(QStringLiteral("local/text")).toString(),
                         s.value(QStringLiteral("local/embedding")).toString(), s.value(QStringLiteral("local/speech")).toString());
        local->setSpeechVoices({}, LocalModels::takesInstructions(local->speechModelName(false)));
        local->queryContextWindow();
    }
    emit routingChanged();
}

bool AiServices::isCareful(const QString &feature) {
    return feature == QLatin1String("explain") || feature == QLatin1String("recap") || feature == QLatin1String("ask");
}

QString AiServices::preset() const {
    if (!m_preset.isEmpty())
        return m_preset;
    // Until chosen: hybrid once a local model is set up, the cloud before.
    return hasLocal() ? QStringLiteral("hybrid") : QStringLiteral("cloud");
}

void AiServices::setPreset(const QString &preset) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!preset.isEmpty() && preset != QLatin1String("cloud") && preset != QLatin1String("hybrid") &&
        preset != QLatin1String("local"))
        return;
    if (preset == this->preset() && !m_preset.isEmpty())
        return;
    m_preset = preset;
    QSettings s = settings();
    if (preset.isEmpty())
        s.remove(QStringLiteral("ai/preset"));
    else
        s.setValue(QStringLiteral("ai/preset"), preset);
    emit routingChanged();
    emit changed();
}

QString AiServices::overrideFor(const QString &feature) const {
    return m_overrides.value(feature);
}

void AiServices::setOverride(const QString &feature, const QString &route) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (feature.isEmpty())
        return;
    if (!route.isEmpty() && route != QLatin1String("cloud") && route != QLatin1String("local"))
        return;
    if (m_overrides.value(feature) == route)
        return;
    QSettings s = settings();
    if (route.isEmpty()) {
        m_overrides.remove(feature);
        s.remove(QStringLiteral("ai/route/") + feature);
    } else {
        m_overrides.insert(feature, route);
        s.setValue(QStringLiteral("ai/route/") + feature, route);
    }
    emit routingChanged();
    emit changed();
}

bool AiServices::hasLocal() const { return m_localModels && m_localModels->text(); }
bool AiServices::localIsLoopback() const { return m_localModels && m_localModels->isLocal(); }
QString AiServices::localName() const { return m_localModels && m_localModels->isConfigured() ? m_localModels->name() : QString(); }
QString AiServices::localUrl() const {
    return m_localModels && m_localModels->isConfigured() ? m_localModels->server().url.toString() : QString();
}
QString AiServices::localTextModel() const { return m_localModels ? m_localModels->textModelName(false) : QString(); }
QString AiServices::localEmbeddingModel() const { return m_localModels ? m_localModels->embeddingModelName() : QString(); }
QString AiServices::localSpeechModel() const { return m_localModels ? m_localModels->speechModelName(false) : QString(); }
int AiServices::localContextWindow() const { return m_localModels ? m_localModels->reportedContextWindow() : 0; }

QString AiServices::friendlyModelTitle(const QString &model) {
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty())
        return {};
    static const QHash<QString, QString> known{
        {QStringLiteral("qwen3.5:2b"), QStringLiteral("Qwen3.5 2B")},
        {QStringLiteral("qwen3.5:4b"), QStringLiteral("Qwen3.5 4B")},
        {QStringLiteral("qwen3.5:9b"), QStringLiteral("Qwen3.5 9B")},
        {QStringLiteral("qwen3-embedding:0.6b"), QStringLiteral("Qwen3-Embedding 0.6B")},
        {QStringLiteral("embeddinggemma"), QStringLiteral("EmbeddingGemma")},
        {QStringLiteral("embeddinggemma:latest"), QStringLiteral("EmbeddingGemma")},
    };
    const QString lower = trimmed.toLower();
    if (known.contains(lower))
        return known.value(lower);
    if (lower.endsWith(QLatin1String(":latest")))
        return trimmed.left(trimmed.size() - 7);
    return trimmed;
}

QString AiServices::localModelsSummary() const {
    QStringList parts;
    for (const QString &raw : {localTextModel(), localEmbeddingModel(), localSpeechModel()}) {
        const QString title = friendlyModelTitle(raw);
        if (!title.isEmpty() && !parts.contains(title))
            parts.append(title);
    }
    return parts.join(QStringLiteral(" · "));
}

void AiServices::clearOverrides() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_overrides.isEmpty())
        return;
    m_overrides.clear();
    QSettings s = settings();
    s.remove(QStringLiteral("ai/route"));
    emit routingChanged();
    emit changed();
}

QString AiServices::routeFor(const QString &feature) const {
    const QString overrideRoute = overrideFor(feature);
    const QString mode = preset();
    if (feature == QLatin1String("illustrate")) {
        // Pictures are Gemini-only: never route to cloud in strict local mode or local override.
        if (overrideRoute == QLatin1String("local") || (mode == QLatin1String("local") && overrideRoute.isEmpty()))
            return {};
        return (m_provider->hasKey() && m_provider->images()) ? QStringLiteral("cloud") : QString();
    }
    if (feature == QLatin1String("search")) {
        Ai::Provider *provider = embedderProvider();
        if (!provider)
            return {};
        return provider == m_localModels ? QStringLiteral("local") : QStringLiteral("cloud");
    }
    const bool cloud = m_provider->hasKey() && m_provider->text();
    const bool local = hasLocal();
    QString wanted = overrideRoute;
    if (wanted.isEmpty()) {
        if (mode == QLatin1String("local"))
            wanted = QStringLiteral("local");
        else if (mode == QLatin1String("hybrid"))
            wanted = isCareful(feature) ? QStringLiteral("cloud") : QStringLiteral("local");
        else
            wanted = QStringLiteral("cloud");
    }
    // An explicit "local" choice (preset or per-feature override) never falls back to the cloud.
    if (wanted == QLatin1String("local") && (overrideRoute == QLatin1String("local") || mode == QLatin1String("local")))
        return local ? wanted : QString();
    if (wanted == QLatin1String("local"))
        return local ? wanted : (cloud ? QStringLiteral("cloud") : QString());
    return cloud ? wanted : (local ? QStringLiteral("local") : QString());
}

Ai::Provider *AiServices::textProviderFor(const QString &feature) const {
    const QString route = routeFor(feature);
    if (route == QLatin1String("local"))
        return m_localModels;
    return route.isEmpty() ? nullptr : m_provider;
}

Ai::TextModel *AiServices::textFor(const QString &feature) const {
    Ai::Provider *provider = textProviderFor(feature);
    return provider ? provider->text() : nullptr;
}

Ai::Provider *AiServices::embedderProvider() const {
    const bool local = m_localModels && m_localModels->embedder();
    const bool cloud = m_provider->hasKey() && m_provider->embedder();
    const QString overrideRoute = overrideFor(QStringLiteral("search"));
    const QString mode = preset();
    // Explicit "local" (preset or override) never sends book text to the cloud.
    if (overrideRoute == QLatin1String("local") || (mode == QLatin1String("local") && overrideRoute.isEmpty()))
        return local ? m_localModels : nullptr;
    if (overrideRoute == QLatin1String("cloud"))
        return cloud ? m_provider : (local ? m_localModels : nullptr);
    if (local && (mode == QLatin1String("hybrid") || !cloud))
        return m_localModels;
    return cloud ? m_provider : (local ? m_localModels : nullptr);
}

Ai::Embedder *AiServices::embedder() const {
    Ai::Provider *provider = embedderProvider();
    return provider ? provider->embedder() : nullptr;
}

QString AiServices::whereFor(const QString &feature) const {
    if (feature == QLatin1String("illustrate")) {
        if (routeFor(feature).isEmpty())
            return m_provider->hasKey() ? tr("Unavailable in This computer mode (pictures need Gemini).")
                                        : tr("Not available: add a Gemini key.");
        return tr("Sent to %1").arg(m_provider->name());
    }
    Ai::Provider *provider = feature == QLatin1String("search") ? embedderProvider() : textProviderFor(feature);
    if (!provider)
        return tr("Not available: add a Gemini key or a local model.");
    if (provider->isLocal())
        return tr("On this computer, with %1").arg(provider->name());
    if (provider == m_localModels)
        return tr("Sent to %1 (%2)").arg(provider->name(), m_localModels->server().url.host());
    return tr("Sent to %1").arg(provider->name());
}

void AiServices::findLocal(const QString &url) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto sameEndpoint = [](const QUrl &a, const QUrl &b) {
        auto canon = [](const QUrl &u) {
            const QUrl n = LocalModels::normalizeServerUrl(u);
            QString host = n.host().toLower();
            if (host == QLatin1String("localhost") || host == QLatin1String("localhost."))
                host = QStringLiteral("127.0.0.1");
            const int port = n.port(n.scheme().toLower() == QLatin1String("https") ? 443 : 80);
            return QStringLiteral("%1://%2:%3%4").arg(n.scheme().toLower(), host).arg(port).arg(n.path());
        };
        return canon(a) == canon(b);
    };
    const QString trimmed = url.trimmed();
    QList<QUrl> extra;
    m_requestedLocalUrl.clear();
    if (!trimmed.isEmpty()) {
        const QUrl candidate = QUrl::fromUserInput(trimmed);
        if (!LocalModels::isHttpUrl(candidate)) {
            m_localError = tr("Enter an http:// or https:// address, such as localhost:11434.");
            emit localServersChanged();
            return;
        }
        m_requestedLocalUrl = LocalModels::normalizeServerUrl(candidate);
        extra.append(m_requestedLocalUrl);
    }
    if (!localUrl().isEmpty()) {
        const QUrl current(localUrl());
        if (LocalModels::isHttpUrl(current))
            extra.append(current);
    }
    if (!m_finder) {
        m_finder = new ServerFinder(this);
        connect(m_finder, &ServerFinder::finished, this, [this, sameEndpoint] {
            m_localServers.clear();
            bool matchedRequested = !m_requestedLocalUrl.isValid();
            for (const LocalModels::Server &server : m_finder->servers()) {
                m_localServers.append(QVariantMap{{QStringLiteral("kind"), LocalModels::kindId(server.kind)},
                                                  {QStringLiteral("name"), LocalModels::kindName(server.kind)},
                                                  {QStringLiteral("url"), server.url.toString()},
                                                  {QStringLiteral("textModels"), server.textModels},
                                                  {QStringLiteral("embeddingModels"), server.embeddingModels},
                                                  {QStringLiteral("speechModels"), server.speechModels},
                                                  {QStringLiteral("contextWindow"), server.contextWindow}});
                if (m_requestedLocalUrl.isValid() && sameEndpoint(server.url, m_requestedLocalUrl))
                    matchedRequested = true;
                if (m_localModels && m_localModels->isConfigured() &&
                    sameEndpoint(server.url, m_localModels->server().url)) {
                    if (server.contextWindow > 0)
                        m_localModels->setContextWindow(server.contextWindow);
                    else if (!m_localModels->textModelName(false).isEmpty() && m_localModels->reportedContextWindow() <= 0)
                        m_localModels->queryContextWindow();
                }
            }
            if (!matchedRequested)
                m_localError = tr("No compatible model server answered at %1.").arg(m_requestedLocalUrl.toString());
            else
                m_localError.clear();
            m_requestedLocalUrl.clear();
            m_findingLocal = false;
            emit localServersChanged();
        });
    }
    m_localError.clear();
    m_findingLocal = true;
    emit localServersChanged();
    m_finder->find(extra);
}

void AiServices::testLocal() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_localModels || !m_localModels->isConfigured())
        return;
    if (m_localTestReply)
        m_localTestReply->abort();
    const int gen = ++m_localTestGen;
    m_testingLocal = true;
    m_localError.clear();
    m_localTestResult.clear();
    emit localServersChanged();
    m_localTestClock.start();
    Ai::Reply *reply = m_localModels->check();
    m_localTestReply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (gen != m_localTestGen)
            return;
        m_localTestReply.clear();
        m_testingLocal = false;
        if (reply->error()) {
            m_localError = describeFor(m_localModels, reply->error().kind, reply->error().message);
            m_localTestResult.clear();
        } else {
            const qint64 ms = qMax<qint64>(1, m_localTestClock.elapsed());
            m_localError.clear();
            m_localTestResult = tr("Connected · %1 ms").arg(ms);
            if (m_localModels && !m_localModels->textModelName(false).isEmpty())
                m_localModels->queryContextWindow();
        }
        emit localServersChanged();
        if (!reply->error() && !m_findingLocal)
            findLocal();
    });
}

void AiServices::useLocal(const QString &kind, const QString &url, const QString &textModel,
                          const QString &embeddingModel, const QString &speechModel) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_localModels)
        return;
    const QUrl parsed = LocalModels::normalizeServerUrl(QUrl(url.trimmed()));
    if (!LocalModels::isHttpUrl(parsed))
        return;
    ++m_localTestGen;
    if (m_localTestReply)
        m_localTestReply->abort();
    m_testingLocal = false;
    m_localError.clear();
    m_localTestResult.clear();
    QSettings s = settings();
    s.setValue(QStringLiteral("local/kind"), kind);
    s.setValue(QStringLiteral("local/url"), parsed.toString());
    s.setValue(QStringLiteral("local/text"), textModel);
    s.setValue(QStringLiteral("local/embedding"), embeddingModel);
    s.setValue(QStringLiteral("local/speech"), speechModel);
    LocalModels::Server server;
    server.kind = LocalModels::kindFromId(kind);
    server.url = parsed;
    for (const QVariant &entry : std::as_const(m_localServers)) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("url")).toString() == parsed.toString()) {
            server.contextWindow = map.value(QStringLiteral("contextWindow")).toInt();
            break;
        }
    }
    m_localModels->configure(server, textModel, embeddingModel, speechModel);
    m_localModels->setSpeechVoices({}, LocalModels::takesInstructions(speechModel));
    if (server.contextWindow <= 0 && !textModel.isEmpty())
        m_localModels->queryContextWindow();
    emit routingChanged();
    emit localServersChanged();
    emit changed();
}

void AiServices::forgetLocal() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_localTestGen;
    if (m_localTestReply)
        m_localTestReply->abort();
    m_testingLocal = false;
    m_localError.clear();
    m_localTestResult.clear();
    QSettings s = settings();
    s.remove(QStringLiteral("local"));
    if (m_localModels)
        m_localModels->configure({}, {});
    emit routingChanged();
    emit localServersChanged();
    emit changed();
}

Ai::Provider *AiServices::providerFor(const QString &voice) const {
    if (isBuiltinVoice(voice) && m_local)
        return m_local;
    if (isStudioVoice(voice) && m_localModels)
        return m_localModels;
    return m_provider;
}

Ai::SpeechSynthesizer *AiServices::speechFor(const QString &voice) const {
    if (isBuiltinVoice(voice))
        return m_local ? m_local->speech() : nullptr;
    if (isStudioVoice(voice))
        return m_localModels ? m_localModels->speech() : nullptr;
    return speech();
}
