#pragma once

#include "ai.h"
#include "aiusage.h"

#include <QDate>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <memory>

class KeyStore;
class LocalModels;
class ServerFinder;

// Dogear's intelligence, as the window and the features see it: which
// provider serves each capability, its key, whether it works, and what it
// has cost. Features ask here for a text model or a voice; they never
// create providers. main.cpp decides which providers exist.
class AiServices : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY changed)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString providerName READ providerName CONSTANT)
    Q_PROPERTY(QString privacyNote READ privacyNote CONSTANT)
    Q_PROPERTY(QString keyLocation READ keyLocation NOTIFY changed)
    Q_PROPERTY(bool keyFromEnvironment READ keyFromEnvironment NOTIFY changed)
    Q_PROPERTY(QString textModel READ textModel NOTIFY changed)
    Q_PROPERTY(QString speechModel READ speechModel NOTIFY changed)
    Q_PROPERTY(bool canSpeak READ canSpeak NOTIFY changed)
    Q_PROPERTY(bool canDesignVoices READ canDesignVoices NOTIFY changed)
    Q_PROPERTY(double spentThisMonth READ spentThisMonth NOTIFY usageChanged)
    Q_PROPERTY(double budget READ budget WRITE setBudget NOTIFY usageChanged)
    Q_PROPERTY(bool unlimitedBudget READ unlimitedBudget WRITE setUnlimitedBudget NOTIFY usageChanged)
    Q_PROPERTY(QVariantList usageThisMonth READ usageThisMonth NOTIFY usageChanged)
    // Where text work runs: "cloud", "hybrid" or "local".
    Q_PROPERTY(QString preset READ preset WRITE setPreset NOTIFY routingChanged)
    Q_PROPERTY(bool hasLocal READ hasLocal NOTIFY routingChanged)
    Q_PROPERTY(bool localIsLoopback READ localIsLoopback NOTIFY routingChanged)
    Q_PROPERTY(QString localName READ localName NOTIFY routingChanged)
    Q_PROPERTY(QString localUrl READ localUrl NOTIFY routingChanged)
    Q_PROPERTY(QString localTextModel READ localTextModel NOTIFY routingChanged)
    Q_PROPERTY(QString localEmbeddingModel READ localEmbeddingModel NOTIFY routingChanged)
    Q_PROPERTY(QString localSpeechModel READ localSpeechModel NOTIFY routingChanged)
    // The servers found by findLocal(): {kind, name, url, textModels, embeddingModels, speechModels}.
    Q_PROPERTY(QVariantList localServers READ localServers NOTIFY localServersChanged)
    Q_PROPERTY(bool findingLocal READ findingLocal NOTIFY localServersChanged)

public:
    // The key is read from `keys` before this returns, as commands need it.
    AiServices(Ai::Provider *provider, KeyStore *keys, const QString &databaseConnection, QObject *parent = nullptr);

    // The window's way: the key arrives from `keys` when they answer, which
    // a keyring starting up (or waiting to be unlocked) can take a while to.
    void lookUpKeyIn(KeyStore *keys);

    Ai::Provider *provider() const { return m_provider; }
    Ai::TextModel *text() const { return m_provider->hasKey() ? m_provider->text() : nullptr; }
    Ai::SpeechSynthesizer *speech() const { return m_provider->hasKey() ? m_provider->speech() : nullptr; }

    // Built-in voices (made on this computer) beside the cloud provider.
    void setLocalSpeech(Ai::Provider *local) { m_local = local; }
    static bool isBuiltinVoice(const QString &voice) { return voice.startsWith(QStringLiteral("builtin:")); }
    // A Studio voice: made by the model server on this computer.
    static bool isStudioVoice(const QString &voice) { return voice.startsWith(QStringLiteral("studio:")); }
    // Made here (free, one at a time): built-in and Studio voices.
    static bool isOnThisComputer(const QString &voice) { return isBuiltinVoice(voice) || isStudioVoice(voice); }
    // Who speaks in a voice: the built-in voices for theirs, the local
    // server for Studio ones, the cloud for the rest.
    Ai::Provider *providerFor(const QString &voice) const;
    Ai::SpeechSynthesizer *speechFor(const QString &voice) const;
    Q_INVOKABLE bool canSpeakWith(const QString &voice) const {
        const Ai::SpeechSynthesizer *speaker = speechFor(voice);
        return speaker && speaker->canSpeak(voice);
    }
    Ai::VoiceDesigner *voiceDesigner() const { return m_provider->hasKey() ? m_provider->voiceDesigner() : nullptr; }
    // Pictures: Gemini's (making them on a computer needs a large graphics
    // card, and nothing local is wired up for it).
    Ai::ImageGenerator *images() const { return m_provider->hasKey() ? m_provider->images() : nullptr; }

    // --- Routing: which model does a feature's text work --------------------
    // Features: "director", "casting", "define", "translate", "whoswho",
    // "cast" (quick, structured work, fine on small local models) and
    // "explain", "recap", "ask" (careful work, Gemini's unless local).
    void setLocalModels(LocalModels *local);
    LocalModels *localModels() const { return m_localModels; }
    Ai::TextModel *textFor(const QString &feature) const;
    Ai::Provider *textProviderFor(const QString &feature) const;
    // Finding passages by meaning: the local embedder when routed there.
    Ai::Embedder *embedder() const;
    Ai::Provider *embedderProvider() const;
    // "cloud", "local", or "" when nothing can do it.
    Q_INVOKABLE QString routeFor(const QString &feature) const;
    // For the reader: where a feature's work happens, in a few words.
    Q_INVOKABLE QString whereFor(const QString &feature) const;
    // A feature's own route ("cloud", "local", or "" to follow the preset).
    Q_INVOKABLE QString overrideFor(const QString &feature) const;
    Q_INVOKABLE void setOverride(const QString &feature, const QString &route);
    static bool isCareful(const QString &feature);
    QString preset() const;
    void setPreset(const QString &preset);
    bool hasLocal() const;
    bool localIsLoopback() const;
    QString localName() const;
    QString localUrl() const;
    QString localTextModel() const;
    QString localEmbeddingModel() const;
    QString localSpeechModel() const;
    QVariantList localServers() const { return m_localServers; }
    bool findingLocal() const { return m_findingLocal; }
    // Looks for model servers on this computer (and at `url`, if given).
    Q_INVOKABLE void findLocal(const QString &url = {});
    // Uses a server and its models ("" for a role it won't have).
    Q_INVOKABLE void useLocal(const QString &kind, const QString &url, const QString &textModel,
                              const QString &embeddingModel, const QString &speechModel = {});
    Q_INVOKABLE void forgetLocal();

    bool hasKey() const { return m_provider->hasKey(); }
    bool checking() const { return m_checking; }
    bool connected() const { return m_connected; }
    QString status() const { return m_status; }
    QString providerName() const { return m_provider->name(); }
    QString privacyNote() const { return m_provider->privacyNote(); }
    QString keyLocation() const;
    bool keyFromEnvironment() const { return m_fromEnvironment; }
    QString textModel() const;
    QString speechModel() const;
    bool canSpeak() const { return speech() != nullptr; }
    bool canDesignVoices() const { return voiceDesigner() != nullptr; }
    double spentThisMonth() const;
    double budget() const { return m_budget; }
    void setBudget(double budget);
    bool unlimitedBudget() const { return m_unlimitedBudget; }
    void setUnlimitedBudget(bool unlimited);
    QVariantList usageThisMonth() const;

    // Checks a key with the provider, and keeps it in the keyring if it works.
    Q_INVOKABLE void setKey(const QString &key);
    Q_INVOKABLE void forgetKey();
    Q_INVOKABLE void check();
    // A sentence for the reader about what went wrong.
    Q_INVOKABLE QString describe(int errorKind, const QString &detail = {}) const;
    QString describeFor(const Ai::Provider *provider, int errorKind, const QString &detail = {}) const;
    Q_INVOKABLE double listeningCost(double minutes, bool economy = false, bool flex = false) const;
    Q_INVOKABLE bool fitsBudget(double cost) const;

    // Every request goes through here, so its use is counted when it ends.
    void track(Ai::Reply *reply, bool flex = false);
    AiUsageLedger &ledger() { return m_ledger; }

signals:
    void changed();
    void usageChanged();
    void keyChecked(bool ok, const QString &message);
    void routingChanged();
    void localServersChanged();

private:
    Ai::Provider *m_provider;
    Ai::Provider *m_local = nullptr;
    LocalModels *m_localModels = nullptr;
    ServerFinder *m_finder = nullptr;
    QVariantList m_localServers;
    bool m_findingLocal = false;
    KeyStore *m_keys;
    AiUsageLedger m_ledger;
    bool m_checking = false;
    bool m_connected = false;
    bool m_fromEnvironment = false;
    // The key as the keyring has it, to go back to when a new one fails.
    QString m_savedKey;
    // A key typed or forgotten here outranks a lookup still under way.
    bool m_keyChangedHere = false;
    int m_keyGeneration = 0;
    QPointer<Ai::Reply> m_keyCheckReply;
    QString m_status;
    double m_budget = 10;
    bool m_unlimitedBudget = false;
    mutable double m_spentCache = -1;
    mutable QDate m_spentMonth;
    QString m_preset;
    QHash<QString, QString> m_overrides;
};
