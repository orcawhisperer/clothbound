#include "formats/bookscheme.h"

#include "formats/epubbook.h"
#include "formats/tsanhandoff.h"

#include <QBuffer>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QUrl>
#include <QWebEngineUrlRequestJob>
#include <QtWebEngineQuick/QQuickWebEngineProfile>

namespace {

QByteArray refererHeader(const QWebEngineUrlRequestJob *job) {
    const auto headers = job->requestHeaders();
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        if (it.key().compare("referer", Qt::CaseInsensitive) == 0)
            return it.value();
    return {};
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

}  // namespace

BookScheme::BookScheme(QObject *parent) : QWebEngineUrlSchemeHandler(parent) {
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount(), 4));
}

BookScheme::~BookScheme() {
    m_shuttingDown.store(true, std::memory_order_release);
    m_pool.clear();
    m_pool.waitForDone();
}

QString BookScheme::add(const std::shared_ptr<EpubBook> &book) { return m_store.registerBook(book); }

void BookScheme::remove(const QString &token) { m_store.unregisterBook(token); }

QString BookScheme::baseUrl(const QString &token) { return BookResourceStore::baseUrl(token); }

const QMultiMap<QByteArray, QByteArray> &BookScheme::bookHeaders() { return BookResourceStore::bookHeaders(); }

const QMultiMap<QByteArray, QByteArray> &BookScheme::appHeaders() { return BookResourceStore::appHeaders(); }

bool BookScheme::isWellFormed(const QByteArray &document) { return BookResourceStore::isWellFormed(document); }

BookScheme::ResolvedResource BookScheme::fromStoreResult(BookResourceStore::ResolvedResource res) {
    ResolvedResource out;
    out.ok = res.ok();
    out.invalidUrl = (res.status == 400);
    if (!out.ok)
        return out;
    out.mimeType = std::move(res.contentType);
    out.data = std::move(res.body);
    for (const auto &[k, v] : res.headers)
        out.headers.insert(k, v);
    return out;
}

BookScheme::ResolvedResource BookScheme::resolveRequest(const QUrl &url, const QByteArray &referer) {
    return fromStoreResult(m_store.resolve(url, referer));
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
        {
            std::lock_guard<std::mutex> lock(m_deliveryMutex);
            m_pendingDeliveries.push_back(
                {guard, hasContext, std::move(cancelled), std::move(callback), std::move(res)});
        }
        QMetaObject::invokeMethod(this, &BookScheme::drainDeliveries, Qt::QueuedConnection);
    };

    if ((!url.scheme().isEmpty() && url.scheme() != QLatin1String("clothbound")) ||
        url.host() != QLatin1String("local")) {
        ResolvedResource invalid;
        invalid.invalidUrl = true;
        deliver(std::move(invalid));
        return;
    }

    const QString path = url.path(QUrl::FullyDecoded);
    if (path.startsWith(QLatin1String("/app/"))) {
        deliver(fromStoreResult(BookResourceStore::resolveAppRequest(path)));
        return;
    }

    // Fast-fail unknown tokens before dispatching to the worker pool.
    QString token;
    if (path.startsWith(QLatin1String("/book/"))) {
        const QString rest = path.mid(6);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        token = rest.left(slash);
        if (slash < 0 || rest.mid(slash + 1).isEmpty())
            token.clear();
    } else if (path.startsWith(QLatin1Char('/'))) {
        token = BookResourceStore::tokenFromReferer(referer);
    }
    if (token.isEmpty() || !m_store.contains(token)) {
        deliver(ResolvedResource{});
        return;
    }

    Clothbound::startPoolTask(
        &m_pool,
        [this, url, referer, cancelled, deliver = std::move(deliver)]() mutable {
            if (cancelled->load(std::memory_order_relaxed) || m_shuttingDown.load(std::memory_order_acquire))
                return;
            auto resolved = m_store.resolveCancellable(url, referer, {}, cancelled.get());
            if (m_shuttingDown.load(std::memory_order_acquire))
                return;
            deliver(fromStoreResult(std::move(resolved)));
        });
}

void BookScheme::drainDeliveries() {
    std::vector<PendingDelivery> batch;
    {
        std::lock_guard<std::mutex> lock(m_deliveryMutex);
        batch.swap(m_pendingDeliveries);
    }
    for (auto &item : batch) {
        if (item.cancelled->load(std::memory_order_relaxed) || (item.hasContext && !item.guard) ||
            m_shuttingDown.load(std::memory_order_acquire))
            continue;
        item.callback(std::move(item.result));
    }
}

void BookScheme::requestStarted(QWebEngineUrlRequestJob *job) {
    if (!job)
        return;
    const QUrl url = job->requestUrl();
    if ((!url.scheme().isEmpty() && url.scheme() != QLatin1String("clothbound")) ||
        url.host() != QLatin1String("local")) {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }
    const QString path = url.path(QUrl::FullyDecoded);
    if (path.startsWith(QLatin1String("/app/"))) {
        const ResolvedResource resolved = fromStoreResult(BookResourceStore::resolveAppRequest(path));
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
