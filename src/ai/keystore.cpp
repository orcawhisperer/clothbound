#include <QtGlobal>

#if !defined(Q_OS_MACOS)

// libsecret pulls in GLib, whose headers use "signals" as a name; Qt's macro
// of that name must not be defined while they're read.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

#include "ai/keystore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QThreadPool>
#include <QtConcurrent>

namespace {
QThreadPool *keyringPool() {
    // A single-threaded pool of its own that quitting never waits for: a keyring
    // that doesn't answer mustn't keep Clothbound running after it's closed, and
    // maxThreadCount(1) keeps saveLater/removeLater/loadLater strictly FIFO.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(1);
        return p;
    }();
    return pool;
}

const SecretSchema *schema() {
    static const SecretSchema clothbound = {
        "app.clothbound.ApiKey",
        SECRET_SCHEMA_NONE,
        {{"provider", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SecretSchemaAttributeType(0)}},
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &clothbound;
}

// Legacy schema retained read-only for seamless key migration after the
// Dogear -> Clothbound rename, and cleared alongside the primary schema.
const SecretSchema *legacySchema() {
    static const SecretSchema legacy = {
        "org.omarchy.Dogear.ApiKey",
        SECRET_SCHEMA_NONE,
        {{"provider", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SecretSchemaAttributeType(0)}},
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &legacy;
}

QString lookupIn(const SecretSchema *target, const QString &provider) {
    GError *error = nullptr;
    gchar *secret = secret_password_lookup_sync(target, nullptr, &error, "provider",
                                                provider.toUtf8().constData(), nullptr);
    if (error) {
        qWarning("KeyStore: lookup for '%s' failed: %s", qPrintable(provider), error->message);
        g_clear_error(&error);
    }
    if (!secret)
        return {};
    const QString key = QString::fromUtf8(secret);
    secret_password_free(secret);
    return key;
}

bool clearIn(const SecretSchema *target, const QString &provider) {
    GError *error = nullptr;
    const gboolean ok = secret_password_clear_sync(target, nullptr, &error, "provider",
                                                   provider.toUtf8().constData(), nullptr);
    if (error) {
        qWarning("KeyStore: clear for '%s' failed: %s", qPrintable(provider), error->message);
        g_clear_error(&error);
    }
    return bool(ok);
}
}  // namespace

QFuture<QString> SecretKeyStore::loadLater(const QString &provider) {
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::lookup, provider);
}

QString SecretKeyStore::lookup(const QString &provider) {
    QString key = lookupIn(schema(), provider);
    if (!key.isEmpty())
        return key;
    key = lookupIn(legacySchema(), provider);
    if (!key.isEmpty()) {
        // Mirror into Clothbound's schema on first read so future lookups hit
        // the primary schema directly while leaving the legacy entry intact.
        const QByteArray label =
            QCoreApplication::translate("KeyStore", "Clothbound: %1 API key").arg(provider).toUtf8();
        store(provider, key, label);
    }
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
        qWarning("KeyStore: store for '%s' failed: %s", qPrintable(provider), error->message);
        g_clear_error(&error);
    }
    if (!ok && message.isEmpty())
        message = QCoreApplication::translate("KeyStore", "keyring unavailable");
    return {bool(ok), message};
}

bool SecretKeyStore::clear(const QString &provider) {
    const bool clearedPrimary = clearIn(schema(), provider);
    const bool clearedLegacy = clearIn(legacySchema(), provider);
    return clearedPrimary || clearedLegacy;
}

bool SecretKeyStore::save(const QString &provider, const QString &key, QString *message) {
    const QByteArray label =
        QCoreApplication::translate("KeyStore", "Clothbound: %1 API key").arg(provider).toUtf8();
    const auto [ok, err] = store(provider, key, label);
    if (message && !err.isEmpty())
        *message = err;
    return ok;
}

QFuture<QPair<bool, QString>> SecretKeyStore::saveLater(const QString &provider, const QString &key) {
    const QByteArray label =
        QCoreApplication::translate("KeyStore", "Clothbound: %1 API key").arg(provider).toUtf8();
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::store, provider, key, label);
}

bool SecretKeyStore::remove(const QString &provider) { return clear(provider); }

QFuture<bool> SecretKeyStore::removeLater(const QString &provider) {
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::clear, provider);
}

QString SecretKeyStore::where() const { return QCoreApplication::translate("KeyStore", "your keyring"); }

#endif  // !defined(Q_OS_MACOS)
