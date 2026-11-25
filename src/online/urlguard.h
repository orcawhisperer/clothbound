#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>

class QAbstractSocket;
class QTimer;

struct PinnedHttpOptions {
    QUrl url;
    bool allowLocalNetwork = false;
    qint64 maxBytes = 10ll * 1024 * 1024;
    int maxRedirects = 5;
    int timeoutMs = 30000;
    QByteArray userAgent;
    QByteArray acceptHeader;

    // Optional per-hop URL rewrite (e.g. Standard Ebooks ?source=download).
    std::function<QUrl(const QUrl &hopUrl)> rewriteHopUrl;

    // Optional per-hop extra headers (e.g. Authorization for same-origin only).
    std::function<QList<QPair<QByteArray, QByteArray>>(const QUrl &hopUrl)> headersForHop;

    // Optional callback once non-redirect response headers arrive.
    // May adjust *maxBytesInOut (e.g. book cap upgrade); return false to abort.
    std::function<bool(int statusCode, const QUrl &finalUrl,
                       const QList<QPair<QByteArray, QByteArray>> &headers,
                       qint64 contentLength, qint64 *maxBytesInOut,
                       QString *abortErrorOut)> onHeaders;

    // Optional streaming body sink. When set, decoded body chunks are passed here
    // instead of being buffered in PinnedHttpResponse::body. Return false to abort.
    std::function<bool(const QByteArray &chunk, qint64 wireBytesTotal,
                       qint64 decodedBytesTotal, QString *abortErrorOut)> onBodyChunk;
};

struct PinnedHttpResponse {
    bool ok = false;
    bool sizeExceeded = false;
    bool abortedBySink = false;
    bool tooManyRedirects = false;
    bool blockedByGuard = false;
    int httpStatus = 0;
    QUrl finalUrl;
    QString contentType;
    QString contentDisposition;
    qint64 contentLength = -1;
    qint64 bytesReceived = 0;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    QString error;

    QByteArray headerValue(const QByteArray &name) const;
};

// Single-resolution IP-pinned HTTP/1.1 & HTTPS request using QTcpSocket/QSslSocket.
// Eliminates DNS rebinding TOCTOU by validating all resolved IPs once per hop and
// connecting directly to a validated QHostAddress with QNetworkProxy::NoProxy,
// explicit Host header, and QSslSocket::setPeerVerifyName(host) for TLS SNI & cert verification.
class PinnedHttpRequest : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(PinnedHttpResponse)>;

    explicit PinnedHttpRequest(PinnedHttpOptions options, Callback callback, QObject *parent = nullptr);
    ~PinnedHttpRequest() override;

    void start();
    void abort();
    bool isFinished() const { return m_finished; }

private:
    enum class ContentEncoding {
        Identity,
        Gzip,
        Deflate,
    };

    enum class ChunkState {
        SizeLine,
        Data,
        DataCrlf,
        Trailers,
        Done,
    };

    struct StreamInflator;

    void startHop(const QUrl &url, int redirectCount);
    void connectToNextValidatedAddress();
    void sendHttpRequest();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onSocketError(int socketError);
    void resetTransferTimeout();
    void detachSocket();

    bool parseHeadersIfReady();
    bool processBodyBuffer();
    bool deliverWireBytes(const char *data, qsizetype len);
    bool emitDecodedBytes(const char *data, qsizetype len);
    void finishInflatorAndSucceed();
    void succeed();
    void fail(const QString &error, bool sizeExceeded = false, bool tooManyRedirects = false,
              bool blockedByGuard = false, bool abortedBySink = false);

    PinnedHttpOptions m_options;
    Callback m_callback;

    bool m_started = false;
    bool m_finished = false;
    int m_lookupId = -1;
    QPointer<QAbstractSocket> m_socket;
    QTimer *m_timeoutTimer = nullptr;

    QUrl m_currentUrl;
    int m_redirectCount = 0;
    QString m_currentHost;
    quint16 m_currentPort = 80;
    bool m_currentIsHttps = false;
    QList<QHostAddress> m_resolvedAddresses;
    qsizetype m_nextAddressIndex = 0;
    bool m_requestSent = false;

    qint64 m_maxBytes = 0;
    bool m_headersParsed = false;
    int m_httpStatus = 0;
    QList<QPair<QByteArray, QByteArray>> m_responseHeaders;
    QString m_contentType;
    QString m_contentDisposition;
    qint64 m_declaredContentLength = -1;
    bool m_isChunked = false;
    ContentEncoding m_encoding = ContentEncoding::Identity;
    std::unique_ptr<StreamInflator> m_inflator;

    ChunkState m_chunkState = ChunkState::SizeLine;
    qint64 m_chunkRemaining = 0;
    qint64 m_trailerBytes = 0;

    QByteArray m_rawBuffer;
    QByteArray m_body;
    qint64 m_wireBytesReceived = 0;
    qint64 m_decodedBytesReceived = 0;
};

// Strict URL and SSRF validator for Clothbound's online-to-offline ingestion.
// Blocks non-HTTP(S) schemes, credentials on public URLs, local hostnames,
// private/link-local/loopback/CGNAT/ULA/IMDS IP literals, and DNS names
// resolving to any non-global address unless `allowLocalNetwork` is true.
class UrlGuard {
public:
    static constexpr int maxRedirects = 5;
    static constexpr qint64 maxHtmlBytes = 10ll * 1024 * 1024;
    static constexpr qint64 maxBookBytes = 200ll * 1024 * 1024;
    static constexpr qint64 maxImageBytes = 8ll * 1024 * 1024;
    static constexpr int maxArticleImages = 24;
    static constexpr qint64 maxTotalImageBytes = 25ll * 1024 * 1024;

    static bool isAllowedUrl(const QUrl &url, bool allowLocalNetwork, QString *reason = nullptr);
    static bool isPrivateHostAddress(const QHostAddress &addr);
    static bool validateResolvedHost(const QUrl &url, bool allowLocalNetwork, QString *reason = nullptr);

    // Parses a host string if it is an IP literal (IPv4, IPv6, bracketed IPv6,
    // trailing-dot IP literals, or legacy numeric/octal/hex IPv4).
    // Returns a null QHostAddress for DNS names.
    static QHostAddress parseHostLiteral(const QString &host);

    // Returns true when `host` / `url` refers strictly to loopback (127.0.0.0/8, ::1, localhost).
    static bool isLoopbackHost(const QString &host);
    static bool isLoopbackUrl(const QUrl &url);

    // Returns true when `url` targets a self-hosted loopback/RFC1918/LAN server
    // (excluding cloud metadata / IMDS endpoints).
    static bool isLocalNetworkUrl(const QUrl &url);

    // Asynchronous and synchronous IP-pinned HTTP/HTTPS fetchers.
    static PinnedHttpRequest *fetchPinned(PinnedHttpOptions options, PinnedHttpRequest::Callback callback,
                                          QObject *parent = nullptr);
    static PinnedHttpResponse fetchPinnedSync(PinnedHttpOptions options,
                                              const std::atomic_bool *cancelFlag = nullptr);

    // Thread-safe test-only hooks for deterministic offline DNS rebinding and SSRF tests.
    using HostResolverHook = std::function<QList<QHostAddress>(const QString &host, QString *errorOut)>;
    using ConnectAddressHook = std::function<QHostAddress(const QHostAddress &validatedAddress)>;
    static void setHostResolverForTests(HostResolverHook hook);
    static void setConnectAddressOverrideForTests(ConnectAddressHook hook);
};
