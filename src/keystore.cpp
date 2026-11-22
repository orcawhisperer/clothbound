// libsecret pulls in GLib, whose headers use "signals" as a name; Qt's macro
// of that name must not be defined while they're read.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

#include "keystore.h"

#include <QCoreApplication>

namespace {
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

QString SecretKeyStore::load(const QString &provider) {
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

bool SecretKeyStore::save(const QString &provider, const QString &key, QString *message) {
    GError *error = nullptr;
    const QByteArray label = QCoreApplication::translate("KeyStore", "Dogear: %1 API key").arg(provider).toUtf8();
    const gboolean ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.constData(),
                                                   key.toUtf8().constData(), nullptr, &error, "provider",
                                                   provider.toUtf8().constData(), nullptr);
    if (error) {
        if (message)
            *message = QString::fromUtf8(error->message);
        g_error_free(error);
    }
    return ok;
}

bool SecretKeyStore::remove(const QString &provider) {
    GError *error = nullptr;
    const gboolean ok = secret_password_clear_sync(schema(), nullptr, &error, "provider", provider.toUtf8().constData(), nullptr);
    if (error)
        g_error_free(error);
    return ok;
}

QString SecretKeyStore::where() const { return QCoreApplication::translate("KeyStore", "your keyring"); }
