#pragma once

#include <QHash>
#include <QMultiMap>
#include <QMutex>
#include <QUrl>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <memory>

class EpubBook;
class QQuickWebEngineProfile;

// Serves the reading surface and the open books to the web view, all from
// one origin, clothbound://local, so the surface can lay out and annotate the
// chapters it shows:
//   clothbound://local/app/…              the surface itself, from the binary
//   clothbound://local/book/<token>/<path> a file inside an open book's ZIP
// A book's token is random and dies with the book, so a page can only
// reach books that are open.
class BookScheme : public QWebEngineUrlSchemeHandler {
    Q_OBJECT

public:
    explicit BookScheme(QObject *parent = nullptr);

    QString add(const std::shared_ptr<EpubBook> &book);
    void remove(const QString &token);
    static QString baseUrl(const QString &token);

    void requestStarted(QWebEngineUrlRequestJob *job) override;

    struct ResolvedResource {
        bool ok = false;
        bool invalidUrl = false;
        QByteArray mimeType;
        QByteArray data;
        QMultiMap<QByteArray, QByteArray> headers;
    };
    ResolvedResource resolveRequest(const QUrl &url, const QByteArray &referer = {});

    // Whether a chapter can be served as XML (XHTML); books that aren't
    // well-formed are served as HTML instead, which browsers forgive.
    static bool isWellFormed(const QByteArray &document);

private:
    QMutex m_mutex;
    QHash<QString, std::shared_ptr<EpubBook>> m_books;
    QHash<QString, bool> m_wellFormed;
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
