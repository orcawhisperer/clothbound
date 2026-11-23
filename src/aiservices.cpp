#include "aiservices.h"

#include "keystore.h"
#include "localmodels.h"

#include <QFutureWatcher>
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
        m_savedKey = keys->load(provider->id());
        provider->setKey(m_savedKey);
    }
    m_status = provider->hasKey() ? tr("Ready") : tr("No key yet");
}

void AiServices::lookUpKeyIn(KeyStore *keys) {
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
    m_keyChangedHere = true;
    m_checking = true;
    m_status = tr("Checking the key…");
    emit changed();
    Ai::Reply *reply = m_provider->check();
    connect(reply, &Ai::Reply::finished, this, [this, reply, trimmed] {
        reply->deleteLater();
        m_checking = false;
        if (reply->error()) {
            // A key that doesn't work isn't kept, and the one before stays.
            m_provider->setKey(m_fromEnvironment ? QString::fromUtf8(qgetenv("GEMINI_API_KEY")) : m_savedKey);
            m_connected = false;
            m_status = describe(reply->error().kind, reply->error().message);
            emit changed();
            emit keyChecked(false, m_status);
            return;
        }
        QString problem;
        const bool saved = m_keys && m_keys->save(m_provider->id(), trimmed, &problem);
        if (saved)
            m_savedKey = trimmed;
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
    m_savedKey.clear();
    m_keyChangedHere = true;
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
    // A reply belongs to the provider that made it (a local one costs nothing).
    const auto *maker = qobject_cast<Ai::Provider *>(reply->parent());
    const QString providerId = maker ? maker->id() : m_provider->id();
    connect(reply, &Ai::Reply::finished, this, [this, reply, flex, providerId] {
        const Ai::Usage usage = reply->usage();
        if (usage.inputTokens || usage.outputTokens) {
            m_ledger.record(providerId, reply->model(), reply->feature(), usage, flex);
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
    m_localModels = local;
    if (!local)
        return;
    QSettings s = settings();
    const QString url = s.value(QStringLiteral("local/url")).toString();
    if (!url.isEmpty()) {
        LocalModels::Server server;
        server.kind = LocalModels::kindFromId(s.value(QStringLiteral("local/kind")).toString());
        server.url = QUrl(url);
        local->configure(server, s.value(QStringLiteral("local/text")).toString(),
                         s.value(QStringLiteral("local/embedding")).toString(), s.value(QStringLiteral("local/speech")).toString());
        local->setSpeechVoices({}, LocalModels::takesInstructions(local->speechModelName(false)));
    }
    emit routingChanged();
}

bool AiServices::isCareful(const QString &feature) {
    return feature == QLatin1String("explain") || feature == QLatin1String("recap") || feature == QLatin1String("ask");
}

QString AiServices::preset() const {
    const QString chosen = settings().value(QStringLiteral("ai/preset")).toString();
    if (!chosen.isEmpty())
        return chosen;
    // Until chosen: hybrid once a local model is set up, the cloud before.
    return hasLocal() ? QStringLiteral("hybrid") : QStringLiteral("cloud");
}

void AiServices::setPreset(const QString &preset) {
    if (preset == this->preset())
        return;
    settings().setValue(QStringLiteral("ai/preset"), preset);
    emit routingChanged();
    emit changed();
}

QString AiServices::overrideFor(const QString &feature) const {
    return settings().value(QStringLiteral("ai/route/") + feature).toString();
}

void AiServices::setOverride(const QString &feature, const QString &route) {
    QSettings s = settings();
    if (route.isEmpty())
        s.remove(QStringLiteral("ai/route/") + feature);
    else
        s.setValue(QStringLiteral("ai/route/") + feature, route);
    emit routingChanged();
    emit changed();
}

bool AiServices::hasLocal() const { return m_localModels && m_localModels->text(); }
QString AiServices::localName() const { return m_localModels && m_localModels->isConfigured() ? m_localModels->name() : QString(); }
QString AiServices::localUrl() const {
    return m_localModels && m_localModels->isConfigured() ? m_localModels->server().url.toString() : QString();
}
QString AiServices::localTextModel() const { return m_localModels ? m_localModels->textModelName(false) : QString(); }
QString AiServices::localEmbeddingModel() const { return m_localModels ? m_localModels->embeddingModelName() : QString(); }
QString AiServices::localSpeechModel() const { return m_localModels ? m_localModels->speechModelName(false) : QString(); }

QString AiServices::routeFor(const QString &feature) const {
    const bool cloud = m_provider->hasKey() && m_provider->text();
    const bool local = hasLocal();
    QString wanted = overrideFor(feature);
    if (wanted.isEmpty()) {
        const QString mode = preset();
        if (mode == QLatin1String("local"))
            wanted = QStringLiteral("local");
        else if (mode == QLatin1String("hybrid"))
            wanted = isCareful(feature) ? QStringLiteral("cloud") : QStringLiteral("local");
        else
            wanted = QStringLiteral("cloud");
    }
    // "Local" means nothing leaves the computer: no falling back to the cloud.
    if (wanted == QLatin1String("local") && preset() == QLatin1String("local") && overrideFor(feature).isEmpty())
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
    const QString route = routeFor(QStringLiteral("search"));
    if (local && (route == QLatin1String("local") || !cloud))
        return m_localModels;
    if (preset() == QLatin1String("local") && overrideFor(QStringLiteral("search")).isEmpty())
        return nullptr;
    return cloud ? m_provider : nullptr;
}

Ai::Embedder *AiServices::embedder() const {
    Ai::Provider *provider = embedderProvider();
    return provider ? provider->embedder() : nullptr;
}

QString AiServices::whereFor(const QString &feature) const {
    Ai::Provider *provider = feature == QLatin1String("search") ? embedderProvider() : textProviderFor(feature);
    if (!provider)
        return tr("Not available: add a Gemini key or a local model.");
    if (provider->isLocal())
        return tr("On this computer, with %1").arg(provider->name());
    return tr("Sent to %1").arg(provider->name());
}

void AiServices::findLocal(const QString &url) {
    if (!m_finder) {
        m_finder = new ServerFinder(this);
        connect(m_finder, &ServerFinder::finished, this, [this] {
            m_localServers.clear();
            for (const LocalModels::Server &server : m_finder->servers())
                m_localServers.append(QVariantMap{{QStringLiteral("kind"), LocalModels::kindId(server.kind)},
                                                  {QStringLiteral("name"), LocalModels::kindName(server.kind)},
                                                  {QStringLiteral("url"), server.url.toString()},
                                                  {QStringLiteral("textModels"), server.textModels},
                                                  {QStringLiteral("embeddingModels"), server.embeddingModels},
                                                  {QStringLiteral("speechModels"), server.speechModels}});
            m_findingLocal = false;
            emit localServersChanged();
        });
    }
    m_findingLocal = true;
    emit localServersChanged();
    QList<QUrl> extra;
    if (!url.trimmed().isEmpty())
        extra.append(QUrl::fromUserInput(url.trimmed()));
    if (!localUrl().isEmpty())
        extra.append(QUrl(localUrl()));
    m_finder->find(extra);
}

void AiServices::useLocal(const QString &kind, const QString &url, const QString &textModel,
                          const QString &embeddingModel, const QString &speechModel) {
    if (!m_localModels)
        return;
    QSettings s = settings();
    s.setValue(QStringLiteral("local/kind"), kind);
    s.setValue(QStringLiteral("local/url"), url);
    s.setValue(QStringLiteral("local/text"), textModel);
    s.setValue(QStringLiteral("local/embedding"), embeddingModel);
    s.setValue(QStringLiteral("local/speech"), speechModel);
    LocalModels::Server server;
    server.kind = LocalModels::kindFromId(kind);
    server.url = QUrl(url);
    m_localModels->configure(server, textModel, embeddingModel, speechModel);
    m_localModels->setSpeechVoices({}, LocalModels::takesInstructions(speechModel));
    emit routingChanged();
    emit changed();
}

void AiServices::forgetLocal() {
    QSettings s = settings();
    s.remove(QStringLiteral("local"));
    if (m_localModels)
        m_localModels->configure({}, {});
    emit routingChanged();
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
