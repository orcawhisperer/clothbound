#include "formats/bookscheme.h"

#include "formats/epubbook.h"
#include "formats/htmltokenizer.h"

#include <QBuffer>
#include <QFile>
#include <QMetaObject>
#include <QMultiMap>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QWebEngineUrlRequestJob>
#include <QXmlStreamEntityResolver>
#include <QXmlStreamReader>
#include <QtWebEngineQuick/QQuickWebEngineProfile>

namespace {
// Resolves standard HTML/XHTML entities so QXmlStreamReader validates the entire
// chapter rather than stopping on the first &nbsp; or &mdash;.
class XhtmlEntityResolver : public QXmlStreamEntityResolver {
public:
    QString resolveUndeclaredEntity(const QString &name) override {
        const QString decoded = HtmlTokenizer::decodeEntities(QLatin1Char('&') + name + QLatin1Char(';'));
        return decoded.startsWith(QLatin1Char('&')) ? QString() : decoded;
    }
};

QString tokenFromReferer(const QByteArray &refererHeader) {
    if (refererHeader.isEmpty())
        return {};
    const QUrl referer(QString::fromUtf8(refererHeader));
    if (referer.scheme() != QLatin1String("clothbound") || referer.host() != QLatin1String("local"))
        return {};
    const QString refPath = referer.path(QUrl::FullyDecoded);
    if (!refPath.startsWith(QLatin1String("/book/")))
        return {};
    const QString rest = refPath.mid(6);
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    return slash > 0 ? rest.left(slash) : QString();
}

QByteArray refererHeader(const QWebEngineUrlRequestJob *job) {
    const auto headers = job->requestHeaders();
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        if (it.key().compare("referer", Qt::CaseInsensitive) == 0)
            return it.value();
    return {};
}

// The surface's own files, compiled into the binary.
const QHash<QString, QPair<QString, QByteArray>> &appFiles() {
    static const QHash<QString, QPair<QString, QByteArray>> files{
        {"reader.html", {":/reader.html", "text/html"}},
        {"reader.js", {":/reader.js", "text/javascript"}},
        {"reader.css", {":/reader.css", "text/css"}},
        {"hyphenation.js", {":/hyphenation.js", "text/javascript"}},
        {"dropcap.js", {":/dropcap.js", "text/javascript"}},
        {"hyph-en-us.pat.txt", {":/hyphenation/hyph-en-us.pat.txt", "text/plain"}},
        {"hyph-en-us.hyp.txt", {":/hyphenation/hyph-en-us.hyp.txt", "text/plain"}},
        {"qwebchannel.js", {":/qtwebchannel/qwebchannel.js", "text/javascript"}},
        {"fonts/Literata.ttf", {":/fonts/Literata.ttf", "font/ttf"}},
        {"fonts/Literata-Italic.ttf", {":/fonts/Literata-Italic.ttf", "font/ttf"}},
        {"fonts/Inter.ttf", {":/fonts/Inter.ttf", "font/ttf"}},
    };
    return files;
}

void reply(QWebEngineUrlRequestJob *job, const QByteArray &type, const QByteArray &data,
           const QMultiMap<QByteArray, QByteArray> &headers = {}) {
    auto *buffer = new QBuffer(job);
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    if (!headers.isEmpty())
        job->setAdditionalResponseHeaders(headers);
    job->reply(type, buffer);
}
}

BookScheme::BookScheme(QObject *parent) : QWebEngineUrlSchemeHandler(parent) {
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount(), 4));
}

BookScheme::~BookScheme() {
    m_shuttingDown.store(true, std::memory_order_release);
    m_pool.clear();
    m_pool.waitForDone();
}

QString BookScheme::add(const std::shared_ptr<EpubBook> &book) {
    const QString token = QUuid::createUuid().toString(QUuid::Id128);
    QMutexLocker lock(&m_mutex);
    m_books.insert(token, book);
    return token;
}

void BookScheme::remove(const QString &token) {
    QMutexLocker lock(&m_mutex);
    m_books.remove(token);
    const QString prefix = token + '/';
    for (auto it = m_wellFormed.begin(); it != m_wellFormed.end();)
        it = it.key().startsWith(prefix) ? m_wellFormed.erase(it) : std::next(it);
}

QString BookScheme::baseUrl(const QString &token) { return QStringLiteral("clothbound://local/book/%1/").arg(token); }

const QMultiMap<QByteArray, QByteArray> &BookScheme::bookHeaders() {
    static const QMultiMap<QByteArray, QByteArray> headers{
        {"X-Content-Type-Options", "nosniff"},
        {"Cache-Control", "private, max-age=3600"},
        {"Content-Security-Policy",
         "default-src 'self' data: blob:; script-src 'none'; object-src 'none'; "
         "base-uri 'self'; form-action 'none'; frame-src 'none'; frame-ancestors 'self'; "
         "style-src 'self' 'unsafe-inline' data: blob:; img-src 'self' data: blob:; "
         "font-src 'self' data: blob:; media-src 'self' data: blob:"},
    };
    return headers;
}

const QMultiMap<QByteArray, QByteArray> &BookScheme::appHeaders() {
    static const QMultiMap<QByteArray, QByteArray> headers{
        {"X-Content-Type-Options", "nosniff"},
        {"Cache-Control", "private, max-age=86400"},
        {"Content-Security-Policy",
         "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
         "img-src 'self' clothbound: data: blob:; font-src 'self' clothbound: data:; "
         "connect-src 'self' clothbound:; frame-src 'self' clothbound:; "
         "object-src 'none'; base-uri 'self'; form-action 'none'; frame-ancestors 'none'"},
    };
    return headers;
}

bool BookScheme::isWellFormed(const QByteArray &document) {
    QXmlStreamReader xml(document);
    XhtmlEntityResolver resolver;
    // Browsers preload XHTML named entities (&nbsp;, &mdash;, ...) in XML mode
    // only when an XHTML DTD is declared, not for bare HTML5 `<!DOCTYPE html>`.
    const QByteArray head = document.left(2048).toLower();
    if (head.contains("<!doctype") && (head.contains("xhtml") || head.contains("dtd")))
        xml.setEntityResolver(&resolver);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.hasError())
            return false;
    }
    return true;
}

BookScheme::ResolvedResource BookScheme::resolveAppRequest(const QString &path) {
    ResolvedResource result;
    const auto file = appFiles().value(path.mid(5));
    QFile source(file.first);
    if (file.first.isEmpty() || !source.open(QIODevice::ReadOnly))
        return result;
    result.ok = true;
    result.mimeType = file.second;
    result.data = source.readAll();
    result.headers = appHeaders();
    return result;
}

BookScheme::ResolvedResource BookScheme::resolveBookResource(const std::shared_ptr<EpubBook> &book,
                                                             const QString &token, QString entry,
                                                             const std::atomic_bool *cancelled) {
    ResolvedResource result;
    if (!book || (cancelled && cancelled->load(std::memory_order_relaxed)) ||
        m_shuttingDown.load(std::memory_order_acquire))
        return result;

    QString type, error;
    QByteArray data = book->resource(entry, &type, &error);
    if (data.isNull() && !error.isEmpty()) {
        if ((cancelled && cancelled->load(std::memory_order_relaxed)) || m_shuttingDown.load(std::memory_order_acquire))
            return result;
        const QString normalized = EpubBook::resolve(QString(), entry);
        if (!normalized.isEmpty() && normalized != entry) {
            error.clear();
            data = book->resource(normalized, &type, &error);
            if (!data.isNull() || error.isEmpty())
                entry = normalized;
        }
    }
    if (data.isNull() && !error.isEmpty())
        return result;
    if ((cancelled && cancelled->load(std::memory_order_relaxed)) || m_shuttingDown.load(std::memory_order_acquire))
        return result;

    result.ok = true;
    result.data = std::move(data);
    // Enforce CSP + nosniff on every book resource (including SVG, XML, MathML).
    result.headers = bookHeaders();
    if (type == QStringLiteral("application/xhtml+xml") || type == QStringLiteral("text/html")) {
        const QString key = token + QLatin1Char('/') + entry;
        bool cached = false;
        bool xml = false;
        {
            QMutexLocker lock(&m_mutex);
            const auto it = m_wellFormed.constFind(key);
            if (it != m_wellFormed.cend()) {
                xml = *it;
                cached = true;
            }
        }
        if (!cached) {
            if ((cancelled && cancelled->load(std::memory_order_relaxed)) ||
                m_shuttingDown.load(std::memory_order_acquire))
                return {};
            xml = isWellFormed(result.data);
            QMutexLocker lock(&m_mutex);
            if (m_books.contains(token))
                m_wellFormed.insert(key, xml);
        }
        result.mimeType = xml ? QByteArrayLiteral("application/xhtml+xml") : QByteArrayLiteral("text/html");
        return result;
    }
    result.mimeType = type.toUtf8();
    return result;
}

BookScheme::ResolvedResource BookScheme::resolveRequest(const QUrl &url, const QByteArray &referer) {
    ResolvedResource result;
    if (url.host() != QStringLiteral("local")) {
        result.invalidUrl = true;
        return result;
    }
    const QString path = url.path(QUrl::FullyDecoded);

    if (path.startsWith(QStringLiteral("/app/")))
        return resolveAppRequest(path);

    QString token, entry;
    if (path.startsWith(QStringLiteral("/book/"))) {
        const QString rest = path.mid(6);
        const qsizetype slash = rest.indexOf('/');
        token = rest.left(slash);
        entry = slash < 0 ? QString() : rest.mid(slash + 1);
    } else if (path.startsWith(QLatin1Char('/'))) {
        // Root-relative links inside an EPUB (e.g. "/images/cover.jpg") resolve
        // against clothbound://local/; map them safely back into the referring book.
        token = tokenFromReferer(referer);
        entry = EpubBook::resolve(QString(), path.mid(1));
    }
    if (token.isEmpty() || entry.isEmpty())
        return result;

    std::shared_ptr<EpubBook> book;
    {
        QMutexLocker lock(&m_mutex);
        book = m_books.value(token);
    }
    if (!book)
        return result;

    return resolveBookResource(book, token, std::move(entry));
}

void BookScheme::resolveRequestAsync(const QUrl &url, const QByteArray &referer, QObject *context,
                                     std::function<void(ResolvedResource)> callback) {
    if (!callback)
        return;

    const bool hasContext = (context != nullptr);
    QPointer<QObject> guard(context);
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    if (context) {
        QObject::connect(
            context, &QObject::destroyed, this,
            [cancelled] { cancelled->store(true, std::memory_order_relaxed); }, Qt::DirectConnection);
    }

    auto deliver = [this, guard, hasContext, cancelled, callback = std::move(callback)](ResolvedResource res) mutable {
        if (cancelled->load(std::memory_order_relaxed) || m_shuttingDown.load(std::memory_order_acquire))
            return;
        QMetaObject::invokeMethod(
            this,
            [guard, hasContext, cancelled, callback = std::move(callback), res = std::move(res)]() mutable {
                if (cancelled->load(std::memory_order_relaxed) || (hasContext && !guard))
                    return;
                callback(std::move(res));
            },
            Qt::QueuedConnection);
    };

    if (url.host() != QStringLiteral("local")) {
        ResolvedResource invalid;
        invalid.invalidUrl = true;
        deliver(std::move(invalid));
        return;
    }

    const QString path = url.path(QUrl::FullyDecoded);
    if (path.startsWith(QStringLiteral("/app/"))) {
        deliver(resolveAppRequest(path));
        return;
    }

    QString token, entry;
    if (path.startsWith(QStringLiteral("/book/"))) {
        const QString rest = path.mid(6);
        const qsizetype slash = rest.indexOf('/');
        token = rest.left(slash);
        entry = slash < 0 ? QString() : rest.mid(slash + 1);
    } else if (path.startsWith(QLatin1Char('/'))) {
        token = tokenFromReferer(referer);
        entry = EpubBook::resolve(QString(), path.mid(1));
    }
    if (token.isEmpty() || entry.isEmpty()) {
        deliver(ResolvedResource{});
        return;
    }

    std::shared_ptr<EpubBook> book;
    {
        QMutexLocker lock(&m_mutex);
        book = m_books.value(token);
    }
    if (!book) {
        deliver(ResolvedResource{});
        return;
    }

    m_pool.start([this, book = std::move(book), token = std::move(token), entry = std::move(entry), cancelled,
                  deliver = std::move(deliver)]() mutable {
        if (cancelled->load(std::memory_order_relaxed) || m_shuttingDown.load(std::memory_order_acquire))
            return;
        ResolvedResource resolved = resolveBookResource(book, token, std::move(entry), cancelled.get());
        deliver(std::move(resolved));
    });
}

void BookScheme::requestStarted(QWebEngineUrlRequestJob *job) {
    if (!job)
        return;
    const QUrl url = job->requestUrl();
    if (url.host() != QStringLiteral("local")) {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }
    const QString path = url.path(QUrl::FullyDecoded);
    if (path.startsWith(QStringLiteral("/app/"))) {
        const ResolvedResource resolved = resolveAppRequest(path);
        if (!resolved.ok) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        reply(job, resolved.mimeType, resolved.data, resolved.headers);
        return;
    }

    QPointer<QWebEngineUrlRequestJob> jobGuard(job);
    resolveRequestAsync(url, refererHeader(job), job, [jobGuard](ResolvedResource resolved) {
        if (!jobGuard)
            return;
        if (!resolved.ok) {
            jobGuard->fail(resolved.invalidUrl ? QWebEngineUrlRequestJob::UrlInvalid
                                               : QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        reply(jobGuard.data(), resolved.mimeType, resolved.data, resolved.headers);
    });
}

void OfflineInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
    const QString scheme = info.requestUrl().scheme();
    if (scheme != QStringLiteral("clothbound") && scheme != QStringLiteral("data") && scheme != QStringLiteral("blob") &&
        scheme != QStringLiteral("about"))
        info.block(true);
}

LazyWebProfile::LazyWebProfile(BookScheme *scheme, OfflineInterceptor *interceptor, QObject *parent)
    : QObject(parent), m_scheme(scheme), m_interceptor(interceptor) {}

LazyWebProfile::~LazyWebProfile() {
    if (m_profile) {
        m_profile->setUrlRequestInterceptor(nullptr);
        if (m_scheme)
            m_profile->removeUrlSchemeHandler(m_scheme);
        m_profile = nullptr;
    }
}

QQuickWebEngineProfile *LazyWebProfile::ensure() {
    if (!m_profile) {
        m_profile = QQuickWebEngineProfile::defaultProfile();
        if (m_profile) {
            if (m_scheme)
                m_profile->installUrlSchemeHandler("clothbound", m_scheme);
            if (m_interceptor)
                m_profile->setUrlRequestInterceptor(m_interceptor);
        }
    }
    return m_profile;
}
