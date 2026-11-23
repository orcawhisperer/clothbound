// libsecret pulls in GLib, whose headers use "signals" as a name; Qt's macro
// of that name must not be defined while they're read.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

#include "ai/keystore.h"

#include <QCoreApplication>
#include <QThreadPool>
#include <QtConcurrent>

namespace {
QThreadPool *keyringPool() {
    // A pool of its own that quitting never waits for: a keyring that doesn't
    // answer mustn't keep Dogear running after it's closed.
    static QThreadPool *pool = new QThreadPool;
    return pool;
}

const SecretSchema *schema() {
    static const SecretSchema dogear = {
        "org.omarchy.Dogear.ApiKey",
        SECRET_SCHEMA_NONE,
        {{"provider", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SecretSchemaAttributeType(0)}},
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &dogear;
}
}

QFuture<QString> SecretKeyStore::loadLater(const QString &provider) {
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::lookup, provider);
}

QString SecretKeyStore::lookup(const QString &provider) {
    GError *error = nullptr;
    gchar *secret = secret_password_lookup_sync(schema(), nullptr, &error, "provider", provider.toUtf8().constData(), nullptr);
    if (error)
        g_error_free(error);
    if (!secret)
        return {};
    const QString key = QString::fromUtf8(secret);
    secret_password_free(secret);
    return key;
}

QPair<bool, QString> SecretKeyStore::store(const QString &provider, const QString &key, const QByteArray &label) {
    GError *error = nullptr;
    const gboolean ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.constData(),
                                                   key.toUtf8().constData(), nullptr, &error, "provider",
                                                   provider.toUtf8().constData(), nullptr);
    QString message;
    if (error) {
        message = QString::fromUtf8(error->message);
        g_error_free(error);
    }
    if (!ok && message.isEmpty())
        message = QCoreApplication::translate("KeyStore", "keyring unavailable");
    return {bool(ok), message};
}

bool SecretKeyStore::clear(const QString &provider) {
    GError *error = nullptr;
    const gboolean ok = secret_password_clear_sync(schema(), nullptr, &error, "provider", provider.toUtf8().constData(), nullptr);
    if (error)
        g_error_free(error);
    return bool(ok);
}

bool SecretKeyStore::save(const QString &provider, const QString &key, QString *message) {
    const QByteArray label = QCoreApplication::translate("KeyStore", "Dogear: %1 API key").arg(provider).toUtf8();
    const auto [ok, err] = store(provider, key, label);
    if (message && !err.isEmpty())
        *message = err;
    return ok;
}

QFuture<QPair<bool, QString>> SecretKeyStore::saveLater(const QString &provider, const QString &key) {
    const QByteArray label = QCoreApplication::translate("KeyStore", "Dogear: %1 API key").arg(provider).toUtf8();
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::store, provider, key, label);
}

bool SecretKeyStore::remove(const QString &provider) { return clear(provider); }

QFuture<bool> SecretKeyStore::removeLater(const QString &provider) {
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::clear, provider);
}

QString SecretKeyStore::where() const { return QCoreApplication::translate("KeyStore", "your keyring"); }
