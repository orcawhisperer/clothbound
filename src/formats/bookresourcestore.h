#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QMultiMap>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>

class EpubBook;

// WebEngine-free resolver for Clothbound's reading surface (/app/) and open EPUB
// books (/book/<token>/... and root-relative /... subresources). Shared by
// Linux's QWebEngineUrlSchemeHandler (BookScheme) and macOS's WKURLSchemeHandler
// without pulling in Qt6::WebEngineCore or Qt6::Quick.
//
// Thread-safe: registerBook(), unregisterBook(), and resolve() may be called
// concurrently from any thread.
class BookResourceStore {
public:
    struct ResolvedResource {
        int status = 404;
        QByteArray contentType;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        bool ok() const { return status >= 200 && status < 300; }
    };

    BookResourceStore() = default;
    ~BookResourceStore() = default;

    BookResourceStore(const BookResourceStore &) = delete;
    BookResourceStore &operator=(const BookResourceStore &) = delete;

    // Registers an open book under a fresh 128-bit random token. If `book` is
    // null and `path` is non-empty, opens `path` as an EpubBook on demand.
    // Returns the token, or an empty string if no valid book was provided.
    QString registerBook(const QString &path, std::shared_ptr<EpubBook> book);
    QString registerBook(std::shared_ptr<EpubBook> book);
    void unregisterBook(const QString &token);
    bool contains(const QString &token) const;
    std::shared_ptr<EpubBook> bookForToken(const QString &token) const;

    static QString baseUrl(const QString &token);

    // Resolves a clothbound://local/... URL. For root-relative paths (e.g.
    // "/images/cover.jpg"), extracts the book token from `referer`; when
    // `referer` is empty (as in WebKit WKURLSchemeHandler subresources), falls
    // back cleanly to `fallbackToken`.
    ResolvedResource resolve(const QUrl &url, const QByteArray &referer = {},
                             const QString &fallbackToken = {}) const;

    // Cancellation-aware variant for background worker pools (e.g. BookScheme).
    ResolvedResource resolveCancellable(const QUrl &url, const QByteArray &referer,
                                        const QString &fallbackToken,
                                        const std::atomic_bool *cancelled) const;

    static ResolvedResource resolveAppRequest(const QString &path);
    static QString tokenFromReferer(const QByteArray &refererHeader);

    // Whether a chapter can be served as XML (XHTML); books that aren't
    // well-formed are served as HTML instead, which browsers forgive.
    static bool isWellFormed(const QByteArray &document);

    // Response security headers for the reading surface (/app/) and book files (/book/).
    static const QList<QPair<QByteArray, QByteArray>> &appHeaderList();
    static const QList<QPair<QByteArray, QByteArray>> &bookHeaderList();
    static const QMultiMap<QByteArray, QByteArray> &appHeaders();
    static const QMultiMap<QByteArray, QByteArray> &bookHeaders();
    static QByteArray headerValue(const QList<QPair<QByteArray, QByteArray>> &headers, QByteArrayView name);

private:
    ResolvedResource resolveBookResource(const std::shared_ptr<EpubBook> &book, const QString &token, QString entry,
                                         const std::atomic_bool *cancelled) const;

    mutable QMutex m_mutex;
    QHash<QString, std::shared_ptr<EpubBook>> m_books;
    mutable QHash<QString, bool> m_wellFormed;
};
