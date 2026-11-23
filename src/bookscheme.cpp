#include "bookscheme.h"

#include "epubbook.h"
#include "htmltokenizer.h"

#include <QBuffer>
#include <QFile>
#include <QMultiMap>
#include <QMutexLocker>
#include <QUrl>
#include <QUuid>
#include <QWebEngineUrlRequestJob>
#include <QXmlStreamEntityResolver>
#include <QXmlStreamReader>

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

// Security headers applied to every resource served out of an untrusted EPUB.
const QMultiMap<QByteArray, QByteArray> &bookHeaders() {
    static const QMultiMap<QByteArray, QByteArray> headers{
        {"X-Content-Type-Options", "nosniff"},
        {"Content-Security-Policy",
         "default-src 'self' data: blob:; script-src 'none'; object-src 'none'; "
         "base-uri 'self'; form-action 'none'; frame-src 'none'; frame-ancestors 'self'; "
         "style-src 'self' 'unsafe-inline' data: blob:; img-src 'self' data: blob:; "
         "font-src 'self' data: blob:; media-src 'self' data: blob:"},
    };
    return headers;
}

QString tokenFromReferer(const QWebEngineUrlRequestJob *job) {
    const auto headers = job->requestHeaders();
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare("referer", Qt::CaseInsensitive) != 0)
            continue;
        const QUrl referer(QString::fromUtf8(it.value()));
        if (referer.scheme() != QLatin1String("dogear") || referer.host() != QLatin1String("local"))
            return {};
        const QString refPath = referer.path(QUrl::FullyDecoded);
        if (!refPath.startsWith(QLatin1String("/book/")))
            return {};
        const QString rest = refPath.mid(6);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        return slash > 0 ? rest.left(slash) : QString();
    }
    return {};
}

// The surface's own files, compiled into the binary.
const QHash<QString, QPair<QString, QByteArray>> &appFiles() {
    static const QHash<QString, QPair<QString, QByteArray>> files{
        {"reader.html", {":/reader.html", "text/html"}},
        {"reader.js", {":/reader.js", "text/javascript"}},
        {"reader.css", {":/reader.css", "text/css"}},
        {"hyphenation.js", {":/hyphenation.js", "text/javascript"}},
        {"hyph-en-us.pat.txt", {":/hyphenation/hyph-en-us.pat.txt", "text/plain"}},
        {"hyph-en-us.hyp.txt", {":/hyphenation/hyph-en-us.hyp.txt", "text/plain"}},
        {"qwebchannel.js", {":/qtwebchannel/qwebchannel.js", "text/javascript"}},
        {"fonts/Literata.ttf", {":/fonts/Literata.ttf", "font/ttf"}},
        {"fonts/Literata-Italic.ttf", {":/fonts/Literata-Italic.ttf", "font/ttf"}},
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

BookScheme::BookScheme(QObject *parent) : QWebEngineUrlSchemeHandler(parent) {}

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

QString BookScheme::baseUrl(const QString &token) { return QStringLiteral("dogear://local/book/%1/").arg(token); }

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

void BookScheme::requestStarted(QWebEngineUrlRequestJob *job) {
    const QUrl url = job->requestUrl();
    if (url.host() != QStringLiteral("local")) {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }
    const QString path = url.path(QUrl::FullyDecoded);

    if (path.startsWith(QStringLiteral("/app/"))) {
        const auto file = appFiles().value(path.mid(5));
        QFile source(file.first);
        if (file.first.isEmpty() || !source.open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        reply(job, file.second, source.readAll());
        return;
    }

    QString token, entry;
    if (path.startsWith(QStringLiteral("/book/"))) {
        const QString rest = path.mid(6);
        const qsizetype slash = rest.indexOf('/');
        token = rest.left(slash);
        entry = slash < 0 ? QString() : rest.mid(slash + 1);
    } else if (path.startsWith(QLatin1Char('/'))) {
        // Root-relative links inside an EPUB (e.g. "/images/cover.jpg") resolve
        // against dogear://local/; map them safely back into the referring book.
        token = tokenFromReferer(job);
        entry = EpubBook::resolve(QString(), path.mid(1));
    }
    if (token.isEmpty() || entry.isEmpty()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    std::shared_ptr<EpubBook> book;
    {
        QMutexLocker lock(&m_mutex);
        book = m_books.value(token);
    }
    if (!book) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    QString type, error;
    QByteArray data = book->resource(entry, &type, &error);
    if (data.isNull() && !error.isEmpty()) {
        const QString normalized = EpubBook::resolve(QString(), entry);
        if (!normalized.isEmpty() && normalized != entry) {
            error.clear();
            data = book->resource(normalized, &type, &error);
            if (!data.isNull() || error.isEmpty())
                entry = normalized;
        }
    }
    if (data.isNull() && !error.isEmpty()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    if (type == QStringLiteral("application/xhtml+xml") || type == QStringLiteral("text/html")) {
        bool xml;
        {
            QMutexLocker lock(&m_mutex);
            const QString key = token + '/' + entry;
            auto it = m_wellFormed.constFind(key);
            xml = it != m_wellFormed.cend() ? *it : m_wellFormed.insert(key, isWellFormed(data)).value();
        }
        reply(job, xml ? "application/xhtml+xml" : "text/html", data, bookHeaders());
        return;
    }
    // Enforce CSP + nosniff on every book resource (including SVG, XML, MathML).
    reply(job, type.toUtf8(), data, bookHeaders());
}

void OfflineInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
    const QString scheme = info.requestUrl().scheme();
    if (scheme != QStringLiteral("dogear") && scheme != QStringLiteral("data") && scheme != QStringLiteral("blob") &&
        scheme != QStringLiteral("about"))
        info.block(true);
}
