#pragma once

#include "formats/bookresourcestore.h"

#include <QMultiMap>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class EpubBook;
class QQuickWebEngineProfile;

// Thin QWebEngineUrlSchemeHandler delegating to the WebEngine-free
// BookResourceStore so the reading surface and open books are served from one
// origin (clothbound://local):
//   clothbound://local/app/…              the surface itself, from the binary
//   clothbound://local/book/<token>/<path> a file inside an open book
// A book's token is random and dies with the book, so a page can only
// reach books that are open.
class BookScheme : public QWebEngineUrlSchemeHandler {
    Q_OBJECT

public:
    explicit BookScheme(QObject *parent = nullptr);
    ~BookScheme() override;

    QString add(const std::shared_ptr<EpubBook> &book);
    void remove(const QString &token);
    static QString baseUrl(const QString &token);

    BookResourceStore &store() { return m_store; }
    const BookResourceStore &store() const { return m_store; }

    void requestStarted(QWebEngineUrlRequestJob *job) override;

    struct ResolvedResource {
        bool ok = false;
        bool invalidUrl = false;
        QByteArray mimeType;
        QByteArray data;
        QMultiMap<QByteArray, QByteArray> headers;
    };
    ResolvedResource resolveRequest(const QUrl &url, const QByteArray &referer = {});
    // Resolves a request off the GUI thread on BookScheme's dedicated worker pool
    // and delivers the result back on BookScheme's thread, safely ignoring cancelled
    // or destroyed `context` objects.
    void resolveRequestAsync(const QUrl &url, const QByteArray &referer, QObject *context,
                             std::function<void(ResolvedResource)> callback);

    // Whether a chapter can be served as XML (XHTML); books that aren't
    // well-formed are served as HTML instead, which browsers forgive.
    static bool isWellFormed(const QByteArray &document);

    // Response security headers for the reading surface (/app/) and book files (/book/).
    static const QMultiMap<QByteArray, QByteArray> &appHeaders();
    static const QMultiMap<QByteArray, QByteArray> &bookHeaders();

private:
    struct PendingDelivery {
        QPointer<QObject> guard;
        bool hasContext = false;
        std::shared_ptr<std::atomic_bool> cancelled;
        std::function<void(ResolvedResource)> callback;
        ResolvedResource result;
    };

    static ResolvedResource fromStoreResult(BookResourceStore::ResolvedResource res);
    void drainDeliveries();

    BookResourceStore m_store;
    QThreadPool m_pool;
    std::atomic_bool m_shuttingDown{false};
    std::mutex m_deliveryMutex;
    std::vector<PendingDelivery> m_pendingDeliveries;
};

// Books never reach the network: everything but clothbound:, data: and blob:
// is refused, so a book can't fetch fonts, images, or trackers from the web.
class OfflineInterceptor : public QWebEngineUrlRequestInterceptor {
    Q_OBJECT

public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override;
};

// Defers QQuickWebEngineProfile::defaultProfile() (and Chromium's helper
// process startup) until the first EPUB reading surface is actually created,
// keeping Library-only and PDF-only sessions free of WebEngine startup & RAM.
class LazyWebProfile : public QObject {
    Q_OBJECT

public:
    LazyWebProfile(BookScheme *scheme, OfflineInterceptor *interceptor, QObject *parent = nullptr);
    ~LazyWebProfile() override;

    Q_INVOKABLE QQuickWebEngineProfile *ensure();

private:
    BookScheme *m_scheme = nullptr;
    OfflineInterceptor *m_interceptor = nullptr;
    QQuickWebEngineProfile *m_profile = nullptr;
};
