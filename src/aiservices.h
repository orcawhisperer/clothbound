#pragma once

#include "ai.h"
#include "aiusage.h"

#include <QObject>
#include <QVariantList>
#include <memory>

class KeyStore;

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
    Q_PROPERTY(QVariantList usageThisMonth READ usageThisMonth NOTIFY usageChanged)

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
    // Who speaks in a voice: the built-in voices for theirs, the cloud for the rest.
    Ai::Provider *providerFor(const QString &voice) const {
        return isBuiltinVoice(voice) && m_local ? m_local : m_provider;
    }
    Ai::SpeechSynthesizer *speechFor(const QString &voice) const {
        if (isBuiltinVoice(voice))
            return m_local ? m_local->speech() : nullptr;
        return speech();
    }
    Q_INVOKABLE bool canSpeakWith(const QString &voice) const {
        const Ai::SpeechSynthesizer *speaker = speechFor(voice);
        return speaker && speaker->canSpeak(voice);
    }
    Ai::VoiceDesigner *voiceDesigner() const { return m_provider->hasKey() ? m_provider->voiceDesigner() : nullptr; }

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
    QVariantList usageThisMonth() const;

    // Checks a key with the provider, and keeps it in the keyring if it works.
    Q_INVOKABLE void setKey(const QString &key);
    Q_INVOKABLE void forgetKey();
    Q_INVOKABLE void check();
    // A sentence for the reader about what went wrong.
    Q_INVOKABLE QString describe(int errorKind, const QString &detail = {}) const;
    Q_INVOKABLE double listeningCost(double minutes, bool economy = false, bool flex = false) const;
    Q_INVOKABLE bool fitsBudget(double cost) const;

    // Every request goes through here, so its use is counted when it ends.
    void track(Ai::Reply *reply, bool flex = false);
    AiUsageLedger &ledger() { return m_ledger; }

signals:
    void changed();
    void usageChanged();
    void keyChecked(bool ok, const QString &message);

private:
    Ai::Provider *m_provider;
    Ai::Provider *m_local = nullptr;
    KeyStore *m_keys;
    AiUsageLedger m_ledger;
    bool m_checking = false;
    bool m_connected = false;
    bool m_fromEnvironment = false;
    // The key as the keyring has it, to go back to when a new one fails.
    QString m_savedKey;
    // A key typed or forgotten here outranks a lookup still under way.
    bool m_keyChangedHere = false;
    QString m_status;
    double m_budget = 10;
};
