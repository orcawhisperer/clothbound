#pragma once

#include <QHash>
#include <QMutex>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <memory>

class EpubBook;

// Serves the reading surface and the open books to the web view, all from
// one origin, dogear://local, so the surface can lay out and annotate the
// chapters it shows:
//   dogear://local/app/…              the surface itself, from the binary
//   dogear://local/book/<token>/<path> a file inside an open book's ZIP
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

    // Whether a chapter can be served as XML (XHTML); books that aren't
    // well-formed are served as HTML instead, which browsers forgive.
    static bool isWellFormed(const QByteArray &document);

private:
    QMutex m_mutex;
    QHash<QString, std::shared_ptr<EpubBook>> m_books;
    QHash<QString, bool> m_wellFormed;
};

// Books never reach the network: everything but dogear:, data: and blob:
// is refused, so a book can't fetch fonts, images, or trackers from the web.
class OfflineInterceptor : public QWebEngineUrlRequestInterceptor {
    Q_OBJECT

public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override;
};
