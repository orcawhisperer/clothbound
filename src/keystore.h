#pragma once

#include <QHash>
#include <QString>

// Where a provider's key is kept: the desktop's keyring, never a file.
class KeyStore {
public:
    virtual ~KeyStore() = default;
    virtual QString load(const QString &provider) = 0;
    virtual bool save(const QString &provider, const QString &key, QString *error = nullptr) = 0;
    virtual bool remove(const QString &provider) = 0;
    // "GNOME Keyring", for telling the reader where their key is.
    virtual QString where() const = 0;
};

// The Secret Service (GNOME Keyring, KWallet) through libsecret.
class SecretKeyStore : public KeyStore {
public:
    QString load(const QString &provider) override;
    bool save(const QString &provider, const QString &key, QString *error = nullptr) override;
    bool remove(const QString &provider) override;
    QString where() const override;
};

// Keys held for this run only, for tests and for desktops without a keyring.
class MemoryKeyStore : public KeyStore {
public:
    QString load(const QString &provider) override { return m_keys.value(provider); }
    bool save(const QString &provider, const QString &key, QString * = nullptr) override {
        m_keys.insert(provider, key);
        return true;
    }
    bool remove(const QString &provider) override { return m_keys.remove(provider) > 0; }
    QString where() const override { return QStringLiteral("memory"); }

private:
    QHash<QString, QString> m_keys;
};
