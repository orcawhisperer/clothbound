#include "bookscheme.h"

#include "epubbook.h"

#include <QBuffer>
#include <QFile>
#include <QMultiMap>
#include <QMutexLocker>
#include <QUrl>
#include <QUuid>
#include <QWebEngineUrlRequestJob>
#include <QXmlStreamReader>

namespace {
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
    while (!xml.atEnd()) {
        xml.readNext();
        // Entities like &nbsp; need the XHTML DTD, which browsers know and
        // this reader doesn't; they're no reason to give up on XML.
        if (xml.hasError() && xml.error() != QXmlStreamReader::NotWellFormedError)
            return false;
        if (xml.hasError()) {
            const QString message = xml.errorString();
            if (message.contains(QStringLiteral("Entity")) && message.contains(QStringLiteral("not declared")) &&
                document.contains("<!DOCTYPE"))
                return true;
            return false;
        }
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

    if (!path.startsWith(QStringLiteral("/book/"))) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    const QString rest = path.mid(6);
    const qsizetype slash = rest.indexOf('/');
    const QString token = rest.left(slash);
    const QString entry = slash < 0 ? QString() : rest.mid(slash + 1);
    std::shared_ptr<EpubBook> book;
    {
        QMutexLocker lock(&m_mutex);
        book = m_books.value(token);
    }
    if (!book || entry.isEmpty()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    QString type, error;
    const QByteArray data = book->resource(entry, &type, &error);
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
        // Book pages never run scripts, on top of the frame's sandbox.
        QMultiMap<QByteArray, QByteArray> headers;
        headers.insert("Content-Security-Policy", "script-src 'none'; object-src 'none'");
        reply(job, xml ? "application/xhtml+xml" : "text/html", data, headers);
        return;
    }
    reply(job, type.toUtf8(), data);
}

void OfflineInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
    const QString scheme = info.requestUrl().scheme();
    if (scheme != QStringLiteral("dogear") && scheme != QStringLiteral("data") && scheme != QStringLiteral("blob") &&
        scheme != QStringLiteral("about"))
        info.block(true);
}
