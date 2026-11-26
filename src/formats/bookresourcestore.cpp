#include "formats/bookresourcestore.h"

#include "formats/epubbook.h"
#include "formats/htmltokenizer.h"

#include <QFile>
#include <QMutexLocker>
#include <QUuid>
#include <QXmlStreamEntityResolver>
#include <QXmlStreamReader>
#include <iterator>
#include <utility>

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

bool climbsAboveRoot(const QString &relativePath) {
    if (relativePath.contains(QChar('\0')))
        return true;
    int depth = 0;
    QString normalized = relativePath;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = normalized.split(QLatin1Char('/'));
    for (const QString &seg : segments) {
        if (seg.isEmpty() || seg == QLatin1String("."))
            continue;
        if (seg == QLatin1String("..")) {
            --depth;
            if (depth < 0)
                return true;
        } else {
            ++depth;
        }
    }
    return false;
}

bool isCancelled(const std::atomic_bool *cancelled) {
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

}  // namespace

QString BookResourceStore::registerBook(const QString &path, std::shared_ptr<EpubBook> book) {
    if (!book && !path.isEmpty()) {
        auto opened = std::make_shared<EpubBook>(path);
        if (!opened->open())
            return {};
        book = std::move(opened);
    }
    if (!book)
        return {};
    const QString token = QUuid::createUuid().toString(QUuid::Id128);
    QMutexLocker lock(&m_mutex);
    m_books.insert(token, std::move(book));
    return token;
}

QString BookResourceStore::registerBook(std::shared_ptr<EpubBook> book) {
    if (!book)
        return {};
    return registerBook(book->filePath(), std::move(book));
}

void BookResourceStore::unregisterBook(const QString &token) {
    QMutexLocker lock(&m_mutex);
    m_books.remove(token);
    const QString prefix = token + QLatin1Char('/');
    for (auto it = m_wellFormed.begin(); it != m_wellFormed.end();)
        it = it.key().startsWith(prefix) ? m_wellFormed.erase(it) : std::next(it);
}

void BookResourceStore::clear() {
    QMutexLocker lock(&m_mutex);
    m_books.clear();
    m_wellFormed.clear();
}

bool BookResourceStore::contains(const QString &token) const {
    QMutexLocker lock(&m_mutex);
    return m_books.contains(token);
}

std::shared_ptr<EpubBook> BookResourceStore::bookForToken(const QString &token) const {
    QMutexLocker lock(&m_mutex);
    return m_books.value(token);
}

QString BookResourceStore::baseUrl(const QString &token) {
    return QStringLiteral("clothbound://local/book/%1/").arg(token);
}

QString BookResourceStore::tokenFromReferer(const QByteArray &refererHeader) {
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

const QList<QPair<QByteArray, QByteArray>> &BookResourceStore::bookHeaderList() {
    static const QList<QPair<QByteArray, QByteArray>> headers{
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

const QList<QPair<QByteArray, QByteArray>> &BookResourceStore::appHeaderList() {
    static const QList<QPair<QByteArray, QByteArray>> headers{
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

const QMultiMap<QByteArray, QByteArray> &BookResourceStore::bookHeaders() {
    static const QMultiMap<QByteArray, QByteArray> headers = [] {
        QMultiMap<QByteArray, QByteArray> map;
        for (const auto &[k, v] : bookHeaderList())
            map.insert(k, v);
        return map;
    }();
    return headers;
}

const QMultiMap<QByteArray, QByteArray> &BookResourceStore::appHeaders() {
    static const QMultiMap<QByteArray, QByteArray> headers = [] {
        QMultiMap<QByteArray, QByteArray> map;
        for (const auto &[k, v] : appHeaderList())
            map.insert(k, v);
        return map;
    }();
    return headers;
}

QByteArray BookResourceStore::headerValue(const QList<QPair<QByteArray, QByteArray>> &headers, QByteArrayView name) {
    for (const auto &[k, v] : headers) {
        if (k.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    }
    return {};
}

bool BookResourceStore::isWellFormed(const QByteArray &document) {
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

BookResourceStore::ResolvedResource BookResourceStore::resolveAppRequest(const QString &path) {
    ResolvedResource result;
    const auto file = appFiles().value(path.mid(5));
    QFile source(file.first);
    if (file.first.isEmpty() || !source.open(QIODevice::ReadOnly))
        return result;
    result.status = 200;
    result.contentType = file.second;
    result.body = source.readAll();
    result.headers = appHeaderList();
    return result;
}

BookResourceStore::ResolvedResource BookResourceStore::resolveBookResource(const std::shared_ptr<EpubBook> &book,
                                                                           const QString &token, QString entry,
                                                                           const std::atomic_bool *cancelled) const {
    ResolvedResource result;
    if (!book || isCancelled(cancelled))
        return result;

    QString type, error;
    QByteArray data = book->resource(entry, &type, &error);
    if (data.isNull() && !error.isEmpty()) {
        if (isCancelled(cancelled))
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
    if (isCancelled(cancelled))
        return result;

    result.status = 200;
    result.body = std::move(data);
    // Enforce CSP + nosniff on every book resource (including SVG, XML, MathML).
    result.headers = bookHeaderList();
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
            if (isCancelled(cancelled))
                return {};
            xml = isWellFormed(result.body);
            QMutexLocker lock(&m_mutex);
            if (m_books.contains(token))
                m_wellFormed.insert(key, xml);
        }
        result.contentType = xml ? QByteArrayLiteral("application/xhtml+xml") : QByteArrayLiteral("text/html");
        return result;
    }
    result.contentType = type.toUtf8();
    return result;
}

BookResourceStore::ResolvedResource BookResourceStore::resolve(const QUrl &url, const QByteArray &referer,
                                                               const QString &fallbackToken) const {
    return resolveCancellable(url, referer, fallbackToken, nullptr);
}

BookResourceStore::ResolvedResource BookResourceStore::resolveCancellable(const QUrl &url, const QByteArray &referer,
                                                                          const QString &fallbackToken,
                                                                          const std::atomic_bool *cancelled) const {
    ResolvedResource result;
    if ((!url.scheme().isEmpty() && url.scheme() != QLatin1String("clothbound")) ||
        url.host() != QLatin1String("local")) {
        result.status = 400;
        return result;
    }
    const QString path = url.path(QUrl::FullyDecoded);

    if (path.startsWith(QLatin1String("/app/")))
        return resolveAppRequest(path);

    QString token, entry;
    if (path.startsWith(QLatin1String("/book/"))) {
        const QString rest = path.mid(6);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        token = rest.left(slash);
        entry = slash < 0 ? QString() : rest.mid(slash + 1);
        if (climbsAboveRoot(entry))
            return result;
    } else if (path.startsWith(QLatin1Char('/'))) {
        // Root-relative links inside an EPUB (e.g. "/images/cover.jpg") resolve
        // against clothbound://local/; map them safely back into the referring
        // book. WebKit strips Referer on WKURLSchemeHandler subresources, so
        // when Referer is empty we fall back to the session's `fallbackToken`.
        token = tokenFromReferer(referer);
        if (token.isEmpty() && referer.trimmed().isEmpty())
            token = fallbackToken;
        const QString rel = path.mid(1);
        if (climbsAboveRoot(rel))
            return result;
        entry = EpubBook::resolve(QString(), rel);
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

    return resolveBookResource(book, token, std::move(entry), cancelled);
}
