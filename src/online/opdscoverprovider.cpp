#include "online/opdscoverprovider.h"

#include "online/urlguard.h"

#include <QBuffer>
#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#include <QUrl>
#include <QUrlQuery>
#include <atomic>
#include <functional>

namespace {

class ImmediateOpdsCoverResponse : public QQuickImageResponse {
public:
    explicit ImmediateOpdsCoverResponse(QImage image, QString error = {})
        : m_image(std::move(image)), m_error(std::move(error)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

private:
    QImage m_image;
    QString m_error;
};

class OpdsCoverResponse : public QQuickImageResponse, public QRunnable {
public:
    OpdsCoverResponse(QString cacheKey, QUrl url, bool allowLocalNetwork, QSize requestedSize,
                      std::function<QImage(const QString &)> cacheGet,
                      std::function<void(const QString &, const QImage &)> cachePut)
        : m_cacheKey(std::move(cacheKey)),
          m_url(std::move(url)),
          m_allowLocalNetwork(allowLocalNetwork),
          m_requestedSize(requestedSize),
          m_cacheGet(std::move(cacheGet)),
          m_cachePut(std::move(cachePut)) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    void cancel() override { m_cancelled.store(true, std::memory_order_relaxed); }

    void run() override {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished();
            return;
        }
        if (m_cacheGet) {
            if (const QImage hit = m_cacheGet(m_cacheKey); !hit.isNull()) {
                m_image = hit;
                emit finished();
                return;
            }
        }

        QString guardReason;
        if (!UrlGuard::isAllowedUrl(m_url, m_allowLocalNetwork, &guardReason)) {
            m_error = guardReason;
            emit finished();
            return;
        }

        PinnedHttpOptions opts;
        opts.url = m_url;
        opts.allowLocalNetwork = m_allowLocalNetwork;
        opts.maxBytes = OpdsCoverProvider::maxCoverBytes;
        opts.maxRedirects = UrlGuard::maxRedirects;
        opts.timeoutMs = OpdsCoverProvider::defaultTimeoutMs;
        opts.userAgent = OpdsClient::userAgent();
        opts.acceptHeader = QByteArrayLiteral("image/webp, image/png, image/jpeg, image/gif, */*;q=0.1");

        const PinnedHttpResponse resp = UrlGuard::fetchPinnedSync(std::move(opts), &m_cancelled);
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished();
            return;
        }
        if (!resp.ok || resp.httpStatus < 200 || resp.httpStatus >= 300 || resp.body.isEmpty()) {
            m_error = resp.error.isEmpty() ? QStringLiteral("Could not load cover image.") : resp.error;
            emit finished();
            return;
        }

        // Reject SVG payloads in OPDS cover thumbnails (raster formats only).
        const QString ctLower = resp.contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        const QByteArray headLower = resp.body.left(256).trimmed().toLower();
        if (ctLower.contains(QLatin1String("svg")) || headLower.startsWith("<svg") ||
            headLower.contains("<svg")) {
            m_error = QStringLiteral("SVG cover thumbnails are not allowed.");
            emit finished();
            return;
        }

        QByteArray data = resp.body;
        QBuffer buf(&data);
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);

        const QByteArray fmt = reader.format().toLower();
        if (fmt == "svg" || fmt == "svgz" || !reader.canRead()) {
            m_error = QStringLiteral("Unsupported cover image format.");
            emit finished();
            return;
        }

        const QSize nativeSize = reader.size();
        if (!nativeSize.isValid() || nativeSize.width() <= 0 || nativeSize.height() <= 0 ||
            nativeSize.width() > OpdsCoverProvider::maxImageDimension ||
            nativeSize.height() > OpdsCoverProvider::maxImageDimension ||
            qint64(nativeSize.width()) * qint64(nativeSize.height()) > OpdsCoverProvider::maxImagePixels) {
            m_error = QStringLiteral("Cover image dimensions exceed safety limits.");
            emit finished();
            return;
        }

        const int maxW = (m_requestedSize.width() > 0) ? qMin(m_requestedSize.width(), 1024) : 512;
        const int maxH = (m_requestedSize.height() > 0) ? qMin(m_requestedSize.height(), 1024) : 768;
        if (nativeSize.width() > maxW || nativeSize.height() > maxH) {
            const QSize scaled = nativeSize.scaled(maxW, maxH, Qt::KeepAspectRatio);
            if (scaled.isValid() && !scaled.isEmpty())
                reader.setScaledSize(scaled);
        }

        const QImage decoded = reader.read();
        if (decoded.isNull() || decoded.width() > OpdsCoverProvider::maxImageDimension ||
            decoded.height() > OpdsCoverProvider::maxImageDimension) {
            m_error = QStringLiteral("Could not decode cover image.");
            emit finished();
            return;
        }

        if (!m_cancelled.load(std::memory_order_relaxed)) {
            if (m_cachePut)
                m_cachePut(m_cacheKey, decoded);
            m_image = decoded;
        }
        emit finished();
    }

private:
    QString m_cacheKey;
    QUrl m_url;
    bool m_allowLocalNetwork = false;
    QSize m_requestedSize;
    std::function<QImage(const QString &)> m_cacheGet;
    std::function<void(const QString &, const QImage &)> m_cachePut;
    std::atomic_bool m_cancelled{false};
    QImage m_image;
    QString m_error;
};

}  // namespace

OpdsCoverProvider::OpdsCoverProvider(const OpdsClient *client)
    : m_localRegistry(client ? client->m_localRegistry : nullptr),
      m_cache(std::make_shared<CoverCache>()) {
    m_pool.setMaxThreadCount(4);
}

OpdsCoverProvider::~OpdsCoverProvider() {
    m_pool.waitForDone(3000);
}

QString OpdsCoverProvider::imageUrl(const QString &coverUrl, const QString &catalogId) {
    return OpdsClient::formatCoverImageUrl(coverUrl, catalogId);
}

QQuickImageResponse *OpdsCoverProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // Supported formats:
    //   "<percent-encoded-url>?catalog=<percent-encoded-catalog-id>"
    //   "?url=<percent-encoded-url>&catalog=<percent-encoded-catalog-id>"
    const qsizetype qPos = id.indexOf(QLatin1Char('?'));
    const QString encodedPath = qPos >= 0 ? id.left(qPos) : id;
    const QUrlQuery query(qPos >= 0 ? id.mid(qPos + 1) : QString());

    const QString rawUrl = encodedPath.isEmpty()
                               ? QUrl::fromPercentEncoding(
                                     query.queryItemValue(QStringLiteral("url"), QUrl::FullyEncoded).toUtf8())
                               : QUrl::fromPercentEncoding(encodedPath.toUtf8());
    const QString catalogId = QUrl::fromPercentEncoding(
        query.queryItemValue(QStringLiteral("catalog"), QUrl::FullyEncoded).toUtf8());

    const QUrl url(rawUrl.trimmed());
    const bool allowLocal = m_localRegistry && m_localRegistry->allowsLocal(catalogId);

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, allowLocal, &guardReason))
        return new ImmediateOpdsCoverResponse({}, guardReason);

    const QString cacheKey = QStringLiteral("%1|%2|%3x%4")
                                 .arg(allowLocal ? QStringLiteral("1") : QStringLiteral("0"),
                                      url.toString(QUrl::FullyEncoded))
                                 .arg(requestedSize.width())
                                 .arg(requestedSize.height());
    if (const QImage hit = m_cache->get(cacheKey); !hit.isNull())
        return new ImmediateOpdsCoverResponse(hit);

    auto cache = m_cache;
    auto *response = new OpdsCoverResponse(
        cacheKey, url, allowLocal, requestedSize,
        [cache](const QString &k) { return cache->get(k); },
        [cache](const QString &k, const QImage &img) { cache->insert(k, img); });
    m_pool.start(response);
    return response;
}
