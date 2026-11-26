#include "ai/keystore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QThreadPool>
#include <QtConcurrent>

#undef slots
#undef signals
#undef emit

#import <Foundation/Foundation.h>
#import <Security/Security.h>

// Compiled with ARC (-fobjc-arc) and linked against Security.framework and Foundation.framework.
#if !__has_feature(objc_arc)
#error "keystore_mac.mm must be compiled with ARC (-fobjc-arc)"
#endif

namespace {

NSString *const kKeychainService = @"app.clothbound.Reader";

QThreadPool *keyringPool() {
    // A single-threaded pool of its own that quitting never waits for: a locked
    // keychain prompt mustn't keep Clothbound running after it's closed, and
    // maxThreadCount(1) keeps saveLater/removeLater/loadLater strictly FIFO.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(1);
        return p;
    }();
    return pool;
}

QString securityErrorMessage(OSStatus status) {
    CFStringRef messageRef = SecCopyErrorMessageString(status, nullptr);
    if (!messageRef)
        return QStringLiteral("OSStatus %1").arg(int(status));
    const QString message = QString::fromCFString(messageRef);
    CFRelease(messageRef);
    return message;
}

}  // namespace

QFuture<QString> SecretKeyStore::loadLater(const QString &provider) {
    return QtConcurrent::run(keyringPool(), &SecretKeyStore::lookup, provider);
}

QString SecretKeyStore::lookup(const QString &provider) {
    if (provider.isEmpty())
        return {};

    @autoreleasepool {
        NSString *account = provider.toNSString();
        NSDictionary *query = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : kKeychainService,
            (__bridge id)kSecAttrAccount : account,
            (__bridge id)kSecReturnData : @YES,
            (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne,
        };

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
        if (status == errSecItemNotFound)
            return {};
        if (status != errSecSuccess) {
            const QString err = securityErrorMessage(status);
            qWarning("KeyStore: Keychain lookup for '%s' failed (%d): %s",
                     qPrintable(provider), int(status), qPrintable(err));
            if (result)
                CFRelease(result);
            return {};
        }
        if (!result)
            return {};
        if (CFGetTypeID(result) != CFDataGetTypeID()) {
            CFRelease(result);
            return {};
        }

        // Transfer CoreFoundation ownership cleanly into ARC.
        NSData *data = (__bridge_transfer NSData *)result;
        return QString::fromUtf8(static_cast<const char *>(data.bytes), qsizetype(data.length));
    }
}

QPair<bool, QString> SecretKeyStore::store(const QString &provider, const QString &key, const QByteArray &label) {
    if (provider.isEmpty())
        return {false, QCoreApplication::translate("KeyStore", "keyring unavailable")};

    @autoreleasepool {
        NSString *account = provider.toNSString();
        NSString *nsLabel = QString::fromUtf8(label).toNSString();
        QByteArray utf8 = key.toUtf8();
        NSData *secretData = [NSData dataWithBytes:utf8.constData() length:NSUInteger(utf8.size())];
        utf8.fill('\0');

        NSDictionary *addQuery = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : kKeychainService,
            (__bridge id)kSecAttrAccount : account,
            (__bridge id)kSecAttrLabel : nsLabel,
            (__bridge id)kSecValueData : secretData,
            (__bridge id)kSecAttrAccessible : (__bridge id)kSecAttrAccessibleAfterFirstUnlock,
        };

        OSStatus status = SecItemAdd((__bridge CFDictionaryRef)addQuery, nullptr);
        if (status == errSecDuplicateItem) {
            NSDictionary *matchQuery = @{
                (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
                (__bridge id)kSecAttrService : kKeychainService,
                (__bridge id)kSecAttrAccount : account,
            };
            NSDictionary *attributesToUpdate = @{
                (__bridge id)kSecAttrLabel : nsLabel,
                (__bridge id)kSecValueData : secretData,
                (__bridge id)kSecAttrAccessible : (__bridge id)kSecAttrAccessibleAfterFirstUnlock,
            };
            status = SecItemUpdate((__bridge CFDictionaryRef)matchQuery,
                                   (__bridge CFDictionaryRef)attributesToUpdate);
        }

        if (status == errSecSuccess)
            return {true, {}};

        QString message = securityErrorMessage(status);
        qWarning("KeyStore: Keychain store for '%s' failed (%d): %s",
                 qPrintable(provider), int(status), qPrintable(message));
        if (message.isEmpty())
            message = QCoreApplication::translate("KeyStore", "keyring unavailable");
        return {false, message};
    }
}

bool SecretKeyStore::clear(const QString &provider) {
    if (provider.isEmpty())
        return false;

    @autoreleasepool {
        NSString *account = provider.toNSString();
        NSDictionary *query = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : kKeychainService,
            (__bridge id)kSecAttrAccount : account,
        };
        const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query);
        if (status == errSecSuccess)
            return true;
        if (status != errSecItemNotFound) {
            const QString err = securityErrorMessage(status);
            qWarning("KeyStore: Keychain delete for '%s' failed (%d): %s",
                     qPrintable(provider), int(status), qPrintable(err));
        }
        return false;
    }
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

QString SecretKeyStore::where() const { return QStringLiteral("macOS Keychain"); }
