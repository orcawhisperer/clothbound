#include "online/urlguard.h"

#include <QAbstractSocket>
#include <QEventLoop>
#include <QHostInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QSslError>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <zlib.h>

namespace {
constexpr qsizetype kMaxHeaderBytes = 64 * 1024;
constexpr qsizetype kMaxChunkLineBytes = 4096;
constexpr qint64 kMaxTrailerBytes = 16 * 1024;
constexpr int kDefaultTimeoutMs = 30000;

QMutex g_testHookMutex;
UrlGuard::HostResolverHook g_resolverHook;
UrlGuard::ConnectAddressHook g_connectOverrideHook;

UrlGuard::HostResolverHook currentResolverHook() {
    QMutexLocker lock(&g_testHookMutex);
    return g_resolverHook;
}

UrlGuard::ConnectAddressHook currentConnectOverrideHook() {
    QMutexLocker lock(&g_testHookMutex);
    return g_connectOverrideHook;
}

bool setReason(QString *reason, const QString &message) {
    if (reason)
        *reason = message;
    return false;
}

bool isPrivateIpv4(quint32 ip) {
    // 0.0.0.0/8 (this network / unspecified)
    if ((ip & 0xff000000u) == 0x00000000u)
        return true;
    // 10.0.0.0/8 (RFC 1918)
    if ((ip & 0xff000000u) == 0x0a000000u)
        return true;
    // 100.64.0.0/10 (Shared Address Space / CGNAT, RFC 6598)
    if ((ip & 0xffc00000u) == 0x64400000u)
        return true;
    // 127.0.0.0/8 (Loopback)
    if ((ip & 0xff000000u) == 0x7f000000u)
        return true;
    // 169.254.0.0/16 (Link-local & cloud metadata IMDS 169.254.169.254)
    if ((ip & 0xffff0000u) == 0xa9fe0000u)
        return true;
    // 172.16.0.0/12 (RFC 1918)
    if ((ip & 0xfff00000u) == 0xac100000u)
        return true;
    // 192.0.0.0/24 (IETF Protocol Assignments)
    if ((ip & 0xffffff00u) == 0xc0000000u)
        return true;
    // 192.0.2.0/24 (TEST-NET-1)
    if ((ip & 0xffffff00u) == 0xc0000200u)
        return true;
    // 192.88.99.0/24 (6to4 Relay Anycast)
    if ((ip & 0xffffff00u) == 0xc0586300u)
        return true;
    // 192.168.0.0/16 (RFC 1918)
    if ((ip & 0xffff0000u) == 0xc0a80000u)
        return true;
    // 198.18.0.0/15 (Benchmarking)
    if ((ip & 0xfffe0000u) == 0xc6120000u)
        return true;
    // 198.51.100.0/24 (TEST-NET-2)
    if ((ip & 0xffffff00u) == 0xc6336400u)
        return true;
    // 203.0.113.0/24 (TEST-NET-3)
    if ((ip & 0xffffff00u) == 0xcb007100u)
        return true;
    // 224.0.0.0/4 (Multicast) and 240.0.0.0/4 (Reserved / Broadcast 255.255.255.255)
    if ((ip & 0xe0000000u) == 0xe0000000u)
        return true;
    return false;
}

bool isImdsAddress(const QHostAddress &addr) {
    if (addr.isNull())
        return false;
    bool ok = false;
    const quint32 ip = addr.toIPv4Address(&ok);
    if (ok && (ip & 0xffff0000u) == 0xa9fe0000u)
        return true;
    return false;
}

quint32 ipv4FromBytes(quint8 a, quint8 b, quint8 c, quint8 d) {
    return (quint32(a) << 24) | (quint32(b) << 16) | (quint32(c) << 8) | quint32(d);
}

// Parses inet_aton-style numeric IPv4 forms (decimal integer, hex 0x..., octal 0...,
// shorthand a.b / a.b.c) so SSRF bypasses like http://2130706433 or http://0177.0.0.1
// are caught before any DNS or socket call.
bool parseLegacyIpv4(const QString &host, quint32 *out) {
    if (host.isEmpty())
        return false;
    const QStringList parts = host.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    if (parts.isEmpty() || parts.size() > 4)
        return false;

    quint64 vals[4] = {0, 0, 0, 0};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString &p = parts[i];
        if (p.isEmpty())
            return false;
        int base = 10;
        QString digits = p;
        if (p.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
            base = 16;
            digits = p.mid(2);
            if (digits.isEmpty())
                return false;
        } else if (p.size() > 1 && p.startsWith(QLatin1Char('0'))) {
            base = 8;
            digits = p.mid(1);
        }
        bool ok = false;
        const quint64 v = digits.toULongLong(&ok, base);
        if (!ok)
            return false;
        vals[i] = v;
    }

    quint64 combined = 0;
    if (parts.size() == 1) {
        if (vals[0] > 0xffffffffull)
            return false;
        combined = vals[0];
    } else if (parts.size() == 2) {
        if (vals[0] > 0xffull || vals[1] > 0xffffffull)
            return false;
        combined = (vals[0] << 24) | vals[1];
    } else if (parts.size() == 3) {
        if (vals[0] > 0xffull || vals[1] > 0xffull || vals[2] > 0xffffull)
            return false;
        combined = (vals[0] << 24) | (vals[1] << 16) | vals[2];
    } else {
        if (vals[0] > 0xffull || vals[1] > 0xffull || vals[2] > 0xffull || vals[3] > 0xffull)
            return false;
        combined = (vals[0] << 24) | (vals[1] << 16) | (vals[2] << 8) | vals[3];
    }
    if (out)
        *out = quint32(combined);
    return true;
}

bool isBlockedLocalHostname(const QString &rawHost) {
    QString host = rawHost.toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty())
        return true;
    if (host == QLatin1String("localhost") || host == QLatin1String("localhost.localdomain") ||
        host == QLatin1String("ip6-localhost") || host == QLatin1String("ip6-loopback") ||
        host == QLatin1String("metadata") || host == QLatin1String("metadata.google.internal"))
        return true;
    if (host.endsWith(QLatin1String(".localhost")) || host.endsWith(QLatin1String(".local")) ||
        host.endsWith(QLatin1String(".localdomain")) || host.endsWith(QLatin1String(".internal")))
        return true;
    return false;
}

bool containsCrLfOrNul(const QByteArray &bytes) {
    for (const char ch : bytes) {
        if (ch == '\r' || ch == '\n' || ch == '\0')
            return true;
    }
    return false;
}

bool isRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

QString stripBracketsAndTrailingDots(QString host) {
    host = host.trimmed();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
        host = host.mid(1, host.size() - 2);
        while (host.endsWith(QLatin1Char('.')))
            host.chop(1);
    }
    return host;
}
}  // namespace

struct PinnedHttpRequest::StreamInflator {
    z_stream zs{};
    bool active = false;
    bool rawDeflateFallbackTried = false;
    bool finishedStream = false;
    ContentEncoding encoding = ContentEncoding::Identity;

    ~StreamInflator() {
        if (active)
            inflateEnd(&zs);
    }

    bool init(ContentEncoding enc) {
        encoding = enc;
        const int windowBits = (enc == ContentEncoding::Gzip) ? (15 + 32) : 15;
        if (inflateInit2(&zs, windowBits) != Z_OK)
            return false;
        active = true;
        return true;
    }

    bool resetToRawDeflate() {
        if (active) {
            inflateEnd(&zs);
            active = false;
        }
        zs = {};
        if (inflateInit2(&zs, -15) != Z_OK)
            return false;
        active = true;
        rawDeflateFallbackTried = true;
        return true;
    }
};

QByteArray PinnedHttpResponse::headerValue(const QByteArray &name) const {
    for (const auto &[k, v] : headers) {
        if (k.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    }
    return {};
}

PinnedHttpRequest::PinnedHttpRequest(PinnedHttpOptions options, Callback callback, QObject *parent)
    : QObject(parent),
      m_options(std::move(options)),
      m_callback(std::move(callback)),
      m_timeoutTimer(new QTimer(this)) {
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        if (m_finished)
            return;
        // If we timed out waiting to connect/handshake on one resolved IP and more
        // validated fallback addresses exist (e.g. blackholed IPv6 -> IPv4), fail over
        // cleanly instead of aborting the whole request.
        if (!m_requestSent && m_nextAddressIndex < m_resolvedAddresses.size()) {
            connectToNextValidatedAddress();
            return;
        }
        fail(QStringLiteral("Network request timed out."));
    });
}

PinnedHttpRequest::~PinnedHttpRequest() {
    abort();
}

void PinnedHttpRequest::start() {
    if (m_started || m_finished)
        return;
    m_started = true;
    startHop(m_options.url, 0);
}

void PinnedHttpRequest::abort() {
    if (m_finished)
        return;
    m_finished = true;
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
    if (m_timeoutTimer)
        m_timeoutTimer->stop();
    detachSocket();
    m_callback = nullptr;
}

void PinnedHttpRequest::detachSocket() {
    if (m_socket) {
        QAbstractSocket *sock = m_socket.data();
        m_socket = nullptr;
        sock->disconnect(this);
        sock->abort();
        sock->deleteLater();
    }
}

void PinnedHttpRequest::resetTransferTimeout() {
    if (m_finished || !m_timeoutTimer)
        return;
    const int ms = m_options.timeoutMs > 0 ? m_options.timeoutMs : kDefaultTimeoutMs;
    m_timeoutTimer->start(ms);
}

void PinnedHttpRequest::startHop(const QUrl &rawUrl, int redirectCount) {
    if (m_finished)
        return;

    detachSocket();
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }

    QUrl url = m_options.rewriteHopUrl ? m_options.rewriteHopUrl(rawUrl) : rawUrl;
    m_currentUrl = url;
    m_redirectCount = redirectCount;
    m_resolvedAddresses.clear();
    m_nextAddressIndex = 0;
    m_requestSent = false;

    m_maxBytes = m_options.maxBytes;
    m_headersParsed = false;
    m_httpStatus = 0;
    m_responseHeaders.clear();
    m_contentType.clear();
    m_contentDisposition.clear();
    m_declaredContentLength = -1;
    m_isChunked = false;
    m_encoding = ContentEncoding::Identity;
    m_inflator.reset();
    m_chunkState = ChunkState::SizeLine;
    m_chunkRemaining = 0;
    m_trailerBytes = 0;
    m_rawBuffer.clear();
    m_body.clear();
    m_wireBytesReceived = 0;
    m_decodedBytesReceived = 0;

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, m_options.allowLocalNetwork, &guardReason)) {
        fail(guardReason, false, false, true);
        return;
    }

    m_currentIsHttps = (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0);
    const int port = url.port(m_currentIsHttps ? 443 : 80);
    if (port < 1 || port > 65535) {
        fail(QStringLiteral("URL contains an invalid port."), false, false, true);
        return;
    }
    m_currentPort = quint16(port);
    m_currentHost = url.host(QUrl::FullyEncoded).trimmed();

    resetTransferTimeout();

    const QHostAddress literal = UrlGuard::parseHostLiteral(m_currentHost);
    if (!literal.isNull()) {
        if (!m_options.allowLocalNetwork && UrlGuard::isPrivateHostAddress(literal)) {
            fail(QStringLiteral("Private, loopback, and link-local IP addresses are blocked."), false, false, true);
            return;
        }
        m_resolvedAddresses = {literal};
        connectToNextValidatedAddress();
        return;
    }

    auto validateAndConnect = [this](const QList<QHostAddress> &addresses, const QString &errorStr) {
        if (m_finished)
            return;
        if (addresses.isEmpty()) {
            const QString detail = errorStr.isEmpty() ? QStringLiteral("host lookup failed") : errorStr;
            fail(QStringLiteral("Could not resolve host “%1” (%2).").arg(m_currentHost, detail));
            return;
        }
        if (!m_options.allowLocalNetwork) {
            for (const QHostAddress &addr : addresses) {
                if (UrlGuard::isPrivateHostAddress(addr)) {
                    fail(QStringLiteral("Host “%1” resolved to a blocked private or local network address.")
                             .arg(m_currentHost),
                         false, false, true);
                    return;
                }
            }
        }
        m_resolvedAddresses = addresses;
        // Prefer IPv4 first when mixed A/AAAA records exist so broken/blackholed
        // residential IPv6 routes never stall catalog fetches before IPv4 is tried.
        std::stable_sort(m_resolvedAddresses.begin(), m_resolvedAddresses.end(),
                         [](const QHostAddress &a, const QHostAddress &b) {
                             return (a.protocol() == QAbstractSocket::IPv4Protocol) &&
                                    (b.protocol() != QAbstractSocket::IPv4Protocol);
                         });
        connectToNextValidatedAddress();
    };

    if (const auto hook = currentResolverHook()) {
        QString errStr;
        const QList<QHostAddress> addrs = hook(m_currentHost, &errStr);
        validateAndConnect(addrs, errStr);
        return;
    }

    const QPointer<PinnedHttpRequest> guard(this);
    const QString lookupHost = m_currentHost;
    m_lookupId = QHostInfo::lookupHost(lookupHost, this, [guard, validateAndConnect](const QHostInfo &info) {
        if (!guard || guard->m_finished)
            return;
        guard->m_lookupId = -1;
        if (info.error() != QHostInfo::NoError) {
            validateAndConnect({}, info.errorString());
            return;
        }
        validateAndConnect(info.addresses(), {});
    });
}

void PinnedHttpRequest::connectToNextValidatedAddress() {
    if (m_finished)
        return;
    if (m_nextAddressIndex >= m_resolvedAddresses.size()) {
        fail(QStringLiteral("Could not connect to host “%1”.").arg(m_currentHost));
        return;
    }

    detachSocket();
    const QHostAddress validatedAddr = m_resolvedAddresses[m_nextAddressIndex++];
    QHostAddress connectAddr = validatedAddr;
    if (const auto hook = currentConnectOverrideHook())
        connectAddr = hook(validatedAddr);

    // Use a bounded connect+TLS timeout when additional validated fallback IPs remain
    // so one stalled endpoint falls over promptly instead of burning the full budget.
    if (m_nextAddressIndex < m_resolvedAddresses.size() && m_timeoutTimer) {
        const int fullMs = m_options.timeoutMs > 0 ? m_options.timeoutMs : kDefaultTimeoutMs;
        m_timeoutTimer->start(qMin(fullMs, 8000));
    } else {
        resetTransferTimeout();
    }

    if (m_currentIsHttps) {
        auto *ssl = new QSslSocket(this);
        ssl->setProxy(QNetworkProxy::NoProxy);
        ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);
        const QString verifyName = stripBracketsAndTrailingDots(m_currentHost);
        ssl->setPeerVerifyName(verifyName);
        m_socket = ssl;

        connect(ssl, &QSslSocket::encrypted, this, [this]() {
            if (m_finished)
                return;
            resetTransferTimeout();
            sendHttpRequest();
        });
        connect(ssl, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
            if (m_finished)
                return;
            const QString msg = errors.isEmpty() ? QStringLiteral("TLS certificate verification failed.")
                                                 : errors.first().errorString();
            fail(msg);
        });
        connect(ssl, &QIODevice::readyRead, this, &PinnedHttpRequest::onSocketReadyRead);
        connect(ssl, &QAbstractSocket::disconnected, this, &PinnedHttpRequest::onSocketDisconnected);
        connect(ssl, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError err) { onSocketError(int(err)); });

        // Connect directly to the validated IP literal while verifying the original DNS name via SNI/cert.
        ssl->connectToHostEncrypted(connectAddr.toString(), m_currentPort, verifyName);
    } else {
        auto *tcp = new QTcpSocket(this);
        tcp->setProxy(QNetworkProxy::NoProxy);
        m_socket = tcp;

        connect(tcp, &QAbstractSocket::connected, this, [this]() {
            if (m_finished)
                return;
            resetTransferTimeout();
            sendHttpRequest();
        });
        connect(tcp, &QIODevice::readyRead, this, &PinnedHttpRequest::onSocketReadyRead);
        connect(tcp, &QAbstractSocket::disconnected, this, &PinnedHttpRequest::onSocketDisconnected);
        connect(tcp, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError err) { onSocketError(int(err)); });

        // Connect directly to the validated QHostAddress (zero DNS resolution).
        tcp->connectToHost(connectAddr, m_currentPort);
    }
}

void PinnedHttpRequest::sendHttpRequest() {
    if (m_finished || !m_socket)
        return;

    QByteArray target = m_currentUrl.path(QUrl::FullyEncoded).toUtf8();
    if (target.isEmpty())
        target = QByteArrayLiteral("/");
    if (m_currentUrl.hasQuery()) {
        target.append('?');
        target.append(m_currentUrl.query(QUrl::FullyEncoded).toUtf8());
    }
    if (containsCrLfOrNul(target)) {
        fail(QStringLiteral("URL contains invalid control characters."), false, false, true);
        return;
    }

    QByteArray hostHeader = m_currentHost.toUtf8();
    if (hostHeader.contains(':') && !hostHeader.startsWith('['))
        hostHeader = '[' + hostHeader + ']';
    const bool nonDefaultPort = (m_currentIsHttps && m_currentPort != 443) ||
                                (!m_currentIsHttps && m_currentPort != 80);
    if (nonDefaultPort)
        hostHeader += ':' + QByteArray::number(m_currentPort);
    if (containsCrLfOrNul(hostHeader)) {
        fail(QStringLiteral("URL host contains invalid control characters."), false, false, true);
        return;
    }

    const QByteArray ua = m_options.userAgent.isEmpty()
                              ? QByteArrayLiteral("Clothbound/" CLOTHBOUND_VERSION " (+https://clothbound.app)")
                              : m_options.userAgent;

    QByteArray req;
    req.reserve(512);
    req += "GET " + target + " HTTP/1.1\r\n";
    req += "Host: " + hostHeader + "\r\n";
    if (!containsCrLfOrNul(ua))
        req += "User-Agent: " + ua + "\r\n";
    if (!m_options.acceptHeader.isEmpty() && !containsCrLfOrNul(m_options.acceptHeader))
        req += "Accept: " + m_options.acceptHeader + "\r\n";
    req += "Accept-Encoding: gzip, deflate\r\n";
    req += "Connection: close\r\n";

    if (m_options.headersForHop) {
        const auto extra = m_options.headersForHop(m_currentUrl);
        for (const auto &[k, v] : extra) {
            if (k.isEmpty() || containsCrLfOrNul(k) || containsCrLfOrNul(v))
                continue;
            if (k.compare("Host", Qt::CaseInsensitive) == 0 ||
                k.compare("Connection", Qt::CaseInsensitive) == 0 ||
                k.compare("Content-Length", Qt::CaseInsensitive) == 0 ||
                k.compare("Transfer-Encoding", Qt::CaseInsensitive) == 0)
                continue;
            req += k + ": " + v + "\r\n";
        }
    }
    req += "\r\n";

    m_requestSent = true;
    const qint64 written = m_socket->write(req);
    if (written < 0) {
        fail(m_socket->errorString().isEmpty() ? QStringLiteral("Failed to write HTTP request.")
                                               : m_socket->errorString());
        return;
    }
}

void PinnedHttpRequest::onSocketReadyRead() {
    if (m_finished || !m_socket)
        return;
    const QByteArray incoming = m_socket->readAll();
    if (incoming.isEmpty())
        return;
    resetTransferTimeout();
    m_rawBuffer.append(incoming);

    if (!m_headersParsed) {
        if (!parseHeadersIfReady())
            return;
        if (m_finished)
            return;
    }

    if (m_headersParsed && !m_rawBuffer.isEmpty())
        processBodyBuffer();
}

void PinnedHttpRequest::onSocketDisconnected() {
    if (m_finished)
        return;
    if (m_socket && m_socket->bytesAvailable() > 0)
        onSocketReadyRead();
    if (m_finished)
        return;

    if (!m_headersParsed) {
        fail(QStringLiteral("Connection closed before response headers were received."));
        return;
    }
    if (m_isChunked) {
        fail(QStringLiteral("Connection closed before chunked response completed."));
        return;
    }
    if (m_declaredContentLength >= 0 && m_wireBytesReceived < m_declaredContentLength) {
        fail(QStringLiteral("Connection closed before full response was received."));
        return;
    }
    finishInflatorAndSucceed();
}

void PinnedHttpRequest::onSocketError(int socketError) {
    if (m_finished)
        return;
    const auto err = static_cast<QAbstractSocket::SocketError>(socketError);
    if (err == QAbstractSocket::RemoteHostClosedError) {
        onSocketDisconnected();
        return;
    }
    // Retry next already-validated IP from the same DNS lookup if connection failed before sending.
    if (!m_requestSent && m_nextAddressIndex < m_resolvedAddresses.size()) {
        connectToNextValidatedAddress();
        return;
    }
    const QString msg = (m_socket && !m_socket->errorString().isEmpty())
                            ? m_socket->errorString()
                            : QStringLiteral("Network connection failed.");
    fail(msg);
}

bool PinnedHttpRequest::parseHeadersIfReady() {
    while (!m_headersParsed && !m_finished) {
        const qsizetype headerEnd = m_rawBuffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (m_rawBuffer.size() > kMaxHeaderBytes) {
                fail(QStringLiteral("Server response headers exceeded size limit."));
                return false;
            }
            return false;
        }
        if (headerEnd > kMaxHeaderBytes) {
            fail(QStringLiteral("Server response headers exceeded size limit."));
            return false;
        }

        const QByteArray headerBlock = m_rawBuffer.left(headerEnd);
        m_rawBuffer.remove(0, headerEnd + 4);

        const QList<QByteArray> lines = headerBlock.split('\n');
        if (lines.isEmpty()) {
            fail(QStringLiteral("Server returned an empty HTTP response."));
            return false;
        }

        const QByteArray statusLine = lines.first().trimmed();
        if (!statusLine.startsWith("HTTP/1.")) {
            fail(QStringLiteral("Server returned an unsupported HTTP response."));
            return false;
        }
        const qsizetype sp1 = statusLine.indexOf(' ');
        if (sp1 < 0) {
            fail(QStringLiteral("Server returned a malformed HTTP status line."));
            return false;
        }
        const QByteArray afterProto = statusLine.mid(sp1 + 1).trimmed();
        const qsizetype sp2 = afterProto.indexOf(' ');
        const QByteArray codeBytes = sp2 >= 0 ? afterProto.left(sp2) : afterProto;
        bool codeOk = false;
        const int statusCode = codeBytes.toInt(&codeOk);
        if (!codeOk || statusCode < 100 || statusCode > 599) {
            fail(QStringLiteral("Server returned an invalid HTTP status code."));
            return false;
        }

        // Skip 1xx interim responses (e.g. 100 Continue, 103 Early Hints).
        if (statusCode >= 100 && statusCode < 200 && statusCode != 101)
            continue;

        m_httpStatus = statusCode;
        m_responseHeaders.clear();
        m_declaredContentLength = -1;
        m_isChunked = false;
        m_encoding = ContentEncoding::Identity;
        m_contentType.clear();
        m_contentDisposition.clear();

        QByteArray locationHeader;
        bool hasContentLength = false;

        for (qsizetype i = 1; i < lines.size(); ++i) {
            QByteArray line = lines[i];
            if (line.endsWith('\r'))
                line.chop(1);
            if (line.isEmpty())
                continue;
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed();
            const QByteArray value = line.mid(colon + 1).trimmed();
            m_responseHeaders.append({name, value});

            if (name.compare("Location", Qt::CaseInsensitive) == 0 && locationHeader.isEmpty()) {
                locationHeader = value;
            } else if (name.compare("Content-Type", Qt::CaseInsensitive) == 0 && m_contentType.isEmpty()) {
                m_contentType = QString::fromUtf8(value);
            } else if (name.compare("Content-Disposition", Qt::CaseInsensitive) == 0 && m_contentDisposition.isEmpty()) {
                m_contentDisposition = QString::fromUtf8(value);
            } else if (name.compare("Transfer-Encoding", Qt::CaseInsensitive) == 0) {
                const QList<QByteArray> tokens = value.toLower().split(',');
                for (const QByteArray &tok : tokens) {
                    if (tok.trimmed() == "chunked")
                        m_isChunked = true;
                }
            } else if (name.compare("Content-Length", Qt::CaseInsensitive) == 0) {
                bool lenOk = false;
                const qint64 len = value.toLongLong(&lenOk);
                if (!lenOk || len < 0) {
                    fail(QStringLiteral("Server returned an invalid Content-Length header."));
                    return false;
                }
                if (hasContentLength && m_declaredContentLength != len) {
                    fail(QStringLiteral("Server returned conflicting Content-Length headers."));
                    return false;
                }
                hasContentLength = true;
                m_declaredContentLength = len;
            } else if (name.compare("Content-Encoding", Qt::CaseInsensitive) == 0) {
                const QByteArray enc = value.toLower();
                if (enc == "gzip" || enc == "x-gzip")
                    m_encoding = ContentEncoding::Gzip;
                else if (enc == "deflate")
                    m_encoding = ContentEncoding::Deflate;
            }
        }

        // RFC 9112: Transfer-Encoding: chunked overrides Content-Length.
        if (m_isChunked)
            m_declaredContentLength = -1;

        // Manual redirect handling: discard redirect body and re-validate next hop.
        if (isRedirectStatus(m_httpStatus)) {
            if (m_redirectCount >= m_options.maxRedirects) {
                fail(QStringLiteral("Too many redirects (more than %1).").arg(m_options.maxRedirects),
                     false, true);
                return false;
            }
            if (locationHeader.isEmpty()) {
                fail(QStringLiteral("Server sent a redirect without a target URL."));
                return false;
            }
            const QUrl target = QUrl::fromEncoded(locationHeader, QUrl::TolerantMode);
            const QUrl nextUrl = target.isRelative() ? m_currentUrl.resolved(target) : target;
            startHop(nextUrl, m_redirectCount + 1);
            return false;
        }

        m_headersParsed = true;

        if (m_options.onHeaders) {
            QString abortErr;
            if (!m_options.onHeaders(m_httpStatus, m_currentUrl, m_responseHeaders, m_declaredContentLength,
                                     &m_maxBytes, &abortErr)) {
                fail(abortErr.isEmpty() ? QStringLiteral("Request aborted.") : abortErr,
                     false, false, false, true);
                return false;
            }
        }

        if (m_declaredContentLength >= 0 && m_declaredContentLength > m_maxBytes) {
            fail(QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                     .arg(qMax<qint64>(1, m_maxBytes / (1024 * 1024))),
                 true);
            return false;
        }

        if (m_encoding != ContentEncoding::Identity) {
            m_inflator = std::make_unique<StreamInflator>();
            if (!m_inflator->init(m_encoding)) {
                fail(QStringLiteral("Could not initialize response decompressor."));
                return false;
            }
        }

        if (m_httpStatus == 204 || m_httpStatus == 304 || (!m_isChunked && m_declaredContentLength == 0)) {
            succeed();
            return false;
        }
    }
    return m_headersParsed;
}

bool PinnedHttpRequest::processBodyBuffer() {
    if (m_finished)
        return false;

    if (m_isChunked) {
        while (!m_finished) {
            if (m_chunkState == ChunkState::SizeLine) {
                const qsizetype eol = m_rawBuffer.indexOf("\r\n");
                if (eol < 0) {
                    if (m_rawBuffer.size() > kMaxChunkLineBytes) {
                        fail(QStringLiteral("Malformed chunked HTTP response."));
                        return false;
                    }
                    return true;
                }
                QByteArray sizeLine = m_rawBuffer.left(eol);
                m_rawBuffer.remove(0, eol + 2);
                const qsizetype semi = sizeLine.indexOf(';');
                if (semi >= 0)
                    sizeLine.truncate(semi);
                sizeLine = sizeLine.trimmed();
                bool ok = false;
                const quint64 sz = sizeLine.toULongLong(&ok, 16);
                if (!ok || sz > quint64(std::numeric_limits<qint64>::max() / 2)) {
                    fail(QStringLiteral("Malformed chunk size in HTTP response."));
                    return false;
                }
                if (sz == 0) {
                    m_chunkState = ChunkState::Trailers;
                } else {
                    m_chunkRemaining = qint64(sz);
                    m_chunkState = ChunkState::Data;
                }
            } else if (m_chunkState == ChunkState::Data) {
                if (m_rawBuffer.isEmpty())
                    return true;
                const qsizetype take = qsizetype(std::min<qint64>(qint64(m_rawBuffer.size()), m_chunkRemaining));
                if (!deliverWireBytes(m_rawBuffer.constData(), take))
                    return false;
                if (take == m_rawBuffer.size())
                    m_rawBuffer.clear();
                else
                    m_rawBuffer.remove(0, take);
                m_chunkRemaining -= take;
                if (m_chunkRemaining == 0)
                    m_chunkState = ChunkState::DataCrlf;
            } else if (m_chunkState == ChunkState::DataCrlf) {
                if (m_rawBuffer.size() < 2)
                    return true;
                if (m_rawBuffer[0] != '\r' || m_rawBuffer[1] != '\n') {
                    fail(QStringLiteral("Malformed chunk terminator in HTTP response."));
                    return false;
                }
                m_rawBuffer.remove(0, 2);
                m_chunkState = ChunkState::SizeLine;
            } else if (m_chunkState == ChunkState::Trailers) {
                if (m_rawBuffer.startsWith("\r\n")) {
                    m_rawBuffer.remove(0, 2);
                    m_chunkState = ChunkState::Done;
                    finishInflatorAndSucceed();
                    return false;
                }
                const qsizetype eol = m_rawBuffer.indexOf("\r\n");
                if (eol < 0) {
                    if (m_trailerBytes + m_rawBuffer.size() > kMaxTrailerBytes) {
                        fail(QStringLiteral("HTTP chunked trailers exceeded size limit."));
                        return false;
                    }
                    return true;
                }
                m_trailerBytes += eol + 2;
                if (m_trailerBytes > kMaxTrailerBytes) {
                    fail(QStringLiteral("HTTP chunked trailers exceeded size limit."));
                    return false;
                }
                m_rawBuffer.remove(0, eol + 2);
            } else {
                return false;
            }
        }
        return false;
    }

    if (m_declaredContentLength >= 0) {
        const qint64 remaining = m_declaredContentLength - m_wireBytesReceived;
        if (remaining <= 0) {
            finishInflatorAndSucceed();
            return false;
        }
        const qsizetype take = qsizetype(std::min<qint64>(qint64(m_rawBuffer.size()), remaining));
        if (take > 0) {
            const QByteArray chunk = (take == m_rawBuffer.size()) ? std::move(m_rawBuffer) : m_rawBuffer.left(take);
            if (take < m_rawBuffer.size())
                m_rawBuffer.remove(0, take);
            else
                m_rawBuffer.clear();
            if (!deliverWireBytes(chunk.constData(), chunk.size()))
                return false;
        }
        if (m_wireBytesReceived >= m_declaredContentLength) {
            finishInflatorAndSucceed();
            return false;
        }
        return true;
    }

    // Close-delimited HTTP response.
    if (!m_rawBuffer.isEmpty()) {
        QByteArray chunk = std::move(m_rawBuffer);
        m_rawBuffer.clear();
        return deliverWireBytes(chunk.constData(), chunk.size());
    }
    return true;
}

bool PinnedHttpRequest::deliverWireBytes(const char *data, qsizetype len) {
    if (len <= 0)
        return true;
    if (m_wireBytesReceived + qint64(len) > m_maxBytes) {
        fail(QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                 .arg(qMax<qint64>(1, m_maxBytes / (1024 * 1024))),
             true);
        return false;
    }
    m_wireBytesReceived += qint64(len);

    if (m_encoding == ContentEncoding::Identity || !m_inflator)
        return emitDecodedBytes(data, len);

    if (m_inflator->finishedStream)
        return true;

    constexpr qsizetype kOutBufSize = 32768;
    char outBuf[kOutBufSize];

    const char *inPtr = data;
    qsizetype inRemaining = len;

    while (inRemaining > 0 && !m_finished) {
        const uInt step = uInt(std::min<qsizetype>(inRemaining, qsizetype(std::numeric_limits<uInt>::max() / 2)));
        m_inflator->zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(inPtr));
        m_inflator->zs.avail_in = step;

        while (m_inflator->zs.avail_in > 0 && !m_finished) {
            m_inflator->zs.next_out = reinterpret_cast<Bytef *>(outBuf);
            m_inflator->zs.avail_out = uInt(kOutBufSize);
            const int rc = inflate(&m_inflator->zs, Z_NO_FLUSH);
            const qsizetype produced = kOutBufSize - qsizetype(m_inflator->zs.avail_out);
            if (produced > 0) {
                if (!emitDecodedBytes(outBuf, produced))
                    return false;
            }
            if (rc == Z_STREAM_END) {
                m_inflator->finishedStream = true;
                return true;
            }
            if (rc == Z_DATA_ERROR && m_encoding == ContentEncoding::Deflate &&
                !m_inflator->rawDeflateFallbackTried && m_decodedBytesReceived == 0) {
                // Some servers send raw RFC 1951 deflate without zlib header.
                if (!m_inflator->resetToRawDeflate()) {
                    fail(QStringLiteral("Failed to decompress HTTP response."));
                    return false;
                }
                m_inflator->zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(inPtr));
                m_inflator->zs.avail_in = step;
                continue;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                fail(QStringLiteral("Corrupted compressed HTTP response."));
                return false;
            }
            if (rc == Z_BUF_ERROR && produced == 0)
                break;
        }
        inPtr += step;
        inRemaining -= step;
    }
    return !m_finished;
}

bool PinnedHttpRequest::emitDecodedBytes(const char *data, qsizetype len) {
    if (len <= 0)
        return true;
    if (m_decodedBytesReceived + qint64(len) > m_maxBytes) {
        fail(QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                 .arg(qMax<qint64>(1, m_maxBytes / (1024 * 1024))),
             true);
        return false;
    }
    m_decodedBytesReceived += qint64(len);

    if (m_options.onBodyChunk) {
        QString abortErr;
        if (!m_options.onBodyChunk(QByteArray(data, len), m_wireBytesReceived, m_decodedBytesReceived, &abortErr)) {
            fail(abortErr.isEmpty() ? QStringLiteral("Request aborted.") : abortErr,
                 false, false, false, true);
            return false;
        }
    } else {
        m_body.append(data, len);
    }
    return true;
}

void PinnedHttpRequest::finishInflatorAndSucceed() {
    if (m_finished)
        return;
    if (m_inflator && m_inflator->active && !m_inflator->finishedStream && m_wireBytesReceived > 0) {
        constexpr qsizetype kOutBufSize = 32768;
        char outBuf[kOutBufSize];
        for (;;) {
            m_inflator->zs.next_in = nullptr;
            m_inflator->zs.avail_in = 0;
            m_inflator->zs.next_out = reinterpret_cast<Bytef *>(outBuf);
            m_inflator->zs.avail_out = uInt(kOutBufSize);
            const int rc = inflate(&m_inflator->zs, Z_FINISH);
            const qsizetype produced = kOutBufSize - qsizetype(m_inflator->zs.avail_out);
            if (produced > 0 && !emitDecodedBytes(outBuf, produced))
                return;
            if (rc == Z_STREAM_END) {
                m_inflator->finishedStream = true;
                break;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                fail(QStringLiteral("Truncated or corrupted compressed HTTP response."));
                return;
            }
            if (produced == 0)
                break;
        }
    }
    succeed();
}

void PinnedHttpRequest::succeed() {
    if (m_finished)
        return;
    m_finished = true;
    if (m_timeoutTimer)
        m_timeoutTimer->stop();
    detachSocket();

    PinnedHttpResponse resp;
    resp.ok = true;
    resp.httpStatus = m_httpStatus;
    resp.finalUrl = m_currentUrl;
    resp.contentType = m_contentType;
    resp.contentDisposition = m_contentDisposition;
    resp.contentLength = m_declaredContentLength;
    resp.bytesReceived = m_decodedBytesReceived;
    resp.headers = std::move(m_responseHeaders);
    resp.body = std::move(m_body);

    auto cb = std::move(m_callback);
    m_callback = nullptr;
    if (cb)
        cb(std::move(resp));
}

void PinnedHttpRequest::fail(const QString &error, bool sizeExceeded, bool tooManyRedirects,
                             bool blockedByGuard, bool abortedBySink) {
    if (m_finished)
        return;
    m_finished = true;
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
    if (m_timeoutTimer)
        m_timeoutTimer->stop();
    detachSocket();
    m_body.clear();
    m_rawBuffer.clear();

    PinnedHttpResponse resp;
    resp.ok = false;
    resp.sizeExceeded = sizeExceeded;
    resp.abortedBySink = abortedBySink;
    resp.tooManyRedirects = tooManyRedirects;
    resp.blockedByGuard = blockedByGuard;
    resp.httpStatus = m_httpStatus;
    resp.finalUrl = m_currentUrl;
    resp.contentType = m_contentType;
    resp.contentDisposition = m_contentDisposition;
    resp.contentLength = m_declaredContentLength;
    resp.bytesReceived = m_decodedBytesReceived;
    resp.headers = std::move(m_responseHeaders);
    resp.error = error;

    auto cb = std::move(m_callback);
    m_callback = nullptr;
    if (cb)
        cb(std::move(resp));
}

QHostAddress UrlGuard::parseHostLiteral(const QString &rawHost) {
    QString host = rawHost.trimmed();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty())
        return {};
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
        host = host.mid(1, host.size() - 2);
        while (host.endsWith(QLatin1Char('.')))
            host.chop(1);
    }
    // Strip IPv6 zone identifier (e.g. fe80::1%eth0) before parsing.
    const qsizetype percent = host.indexOf(QLatin1Char('%'));
    if (percent > 0 && host.contains(QLatin1Char(':')))
        host.truncate(percent);

    QHostAddress addr;
    if (addr.setAddress(host))
        return addr;

    quint32 legacyV4 = 0;
    if (parseLegacyIpv4(host, &legacyV4))
        return QHostAddress(legacyV4);
    return {};
}

bool UrlGuard::isLoopbackHost(const QString &rawHost) {
    QString host = rawHost.trimmed().toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty())
        return false;
    if (host == QLatin1String("localhost") || host == QLatin1String("localhost.localdomain") ||
        host == QLatin1String("ip6-localhost") || host == QLatin1String("ip6-loopback") ||
        host.endsWith(QLatin1String(".localhost")))
        return true;

    const QHostAddress addr = parseHostLiteral(host);
    if (addr.isNull())
        return false;
    if (addr.isLoopback())
        return true;
    bool ok = false;
    const quint32 v4 = addr.toIPv4Address(&ok);
    return ok && (v4 & 0xff000000u) == 0x7f000000u;
}

bool UrlGuard::isLoopbackUrl(const QUrl &url) {
    if (!url.isValid() || url.isEmpty())
        return false;
    return isLoopbackHost(url.host(QUrl::FullyEncoded));
}

bool UrlGuard::isLocalNetworkUrl(const QUrl &url) {
    if (!isAllowedUrl(url, true))
        return false;
    QString host = url.host(QUrl::FullyEncoded).trimmed().toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty())
        return false;

    // Never auto-classify cloud metadata endpoints as self-hosted local OPDS servers.
    if (host == QLatin1String("metadata") || host == QLatin1String("metadata.google.internal"))
        return false;

    const QHostAddress literal = parseHostLiteral(host);
    if (!literal.isNull()) {
        if (isImdsAddress(literal))
            return false;
        return isPrivateHostAddress(literal);
    }

    if (isBlockedLocalHostname(host))
        return true;
    if (host.endsWith(QLatin1String(".lan")) || host.endsWith(QLatin1String(".home")) ||
        host.endsWith(QLatin1String(".home.arpa")))
        return true;
    // Single-label LAN hostnames (e.g. http://calibre:8083/opds).
    if (!host.contains(QLatin1Char('.')))
        return true;
    return false;
}

bool UrlGuard::isPrivateHostAddress(const QHostAddress &addr) {
    if (addr.isNull())
        return true;
    if (addr.isLoopback() || addr.isLinkLocal() || addr.isMulticast() || addr.isBroadcast())
        return true;

    const auto proto = addr.protocol();
    if (proto == QAbstractSocket::IPv4Protocol) {
        bool ok = false;
        const quint32 ip = addr.toIPv4Address(&ok);
        return !ok || isPrivateIpv4(ip);
    }

    if (proto == QAbstractSocket::IPv6Protocol) {
        // Qt reports IPv4-mapped IPv6 addresses (e.g. ::ffff:127.0.0.1) as IPv6Protocol
        // while toIPv4Address(&ok) extracts the embedded IPv4 address.
        bool hasMappedV4 = false;
        const quint32 mappedV4 = addr.toIPv4Address(&hasMappedV4);
        if (hasMappedV4)
            return isPrivateIpv4(mappedV4);

        const Q_IPV6ADDR b = addr.toIPv6Address();
        const auto allZero = [&](int from, int count) {
            for (int i = from; i < from + count; ++i)
                if (b[i] != 0)
                    return false;
            return true;
        };

        // ::/128 (unspecified) and ::1/128 (loopback) and deprecated IPv4-compatible ::/96
        if (allZero(0, 12))
            return true;
        // IPv4-mapped ::ffff:0:0/96 (belt-and-suspenders alongside toIPv4Address)
        if (allZero(0, 10) && b[10] == 0xff && b[11] == 0xff)
            return isPrivateIpv4(ipv4FromBytes(b[12], b[13], b[14], b[15]));
        // IPv4-translated ::ffff:0:0:0/96
        if (allZero(0, 8) && b[8] == 0xff && b[9] == 0xff && b[10] == 0 && b[11] == 0)
            return isPrivateIpv4(ipv4FromBytes(b[12], b[13], b[14], b[15]));
        // NAT64 well-known prefix 64:ff9b::/96 -> inspect embedded IPv4
        if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && allZero(4, 8))
            return isPrivateIpv4(ipv4FromBytes(b[12], b[13], b[14], b[15]));
        // NAT64 local-use prefix 64:ff9b:1::/48
        if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && b[4] == 0x00 && b[5] == 0x01)
            return true;
        // 6to4 2002::/16 -> inspect embedded IPv4 in bytes 2..5
        if (b[0] == 0x20 && b[1] == 0x02)
            return isPrivateIpv4(ipv4FromBytes(b[2], b[3], b[4], b[5]));
        // Discard-only 100::/64
        if (b[0] == 0x01 && allZero(1, 7))
            return true;
        // Teredo 2001:0000::/32
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00)
            return true;
        // Benchmarking 2001:0002::/48
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x02 && b[4] == 0x00 && b[5] == 0x00)
            return true;
        // Documentation 2001:db8::/32
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
            return true;
        // Unique Local Addresses fc00::/7
        if ((b[0] & 0xfe) == 0xfc)
            return true;
        // Link-local fe80::/10
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
            return true;
        // Deprecated site-local fec0::/10
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0)
            return true;
        // Multicast ff00::/8
        if (b[0] == 0xff)
            return true;
        return false;
    }

    // Fail closed for any unknown protocol family.
    return true;
}

bool UrlGuard::isAllowedUrl(const QUrl &url, bool allowLocalNetwork, QString *reason) {
    if (reason)
        reason->clear();
    if (!url.isValid() || url.isEmpty())
        return setReason(reason, QStringLiteral("Invalid URL."));

    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return setReason(reason, QStringLiteral("Only http:// and https:// links are allowed."));

    const QString host = url.host(QUrl::FullyEncoded).trimmed();
    if (host.isEmpty())
        return setReason(reason, QStringLiteral("URL is missing a host name."));

    for (const QChar ch : host) {
        if (ch.isSpace() || ch.unicode() < 0x20 || ch == QLatin1Char('/') || ch == QLatin1Char('\\') ||
            ch == QLatin1Char('@'))
            return setReason(reason, QStringLiteral("URL contains an invalid host name."));
    }

    if (url.port() != -1 && (url.port() < 1 || url.port() > 65535))
        return setReason(reason, QStringLiteral("URL contains an invalid port."));

    const bool hasCredentials = !url.userName().isEmpty() || !url.password().isEmpty() ||
                                url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'));
    if (hasCredentials && !allowLocalNetwork)
        return setReason(reason, QStringLiteral("Credentials in URLs are not allowed for public links."));

    if (!allowLocalNetwork) {
        if (isBlockedLocalHostname(host))
            return setReason(reason, QStringLiteral("Local and internal host names are blocked."));

        const QHostAddress literal = parseHostLiteral(host);
        if (!literal.isNull() && isPrivateHostAddress(literal))
            return setReason(reason, QStringLiteral("Private, loopback, and link-local IP addresses are blocked."));
    }

    return true;
}

bool UrlGuard::validateResolvedHost(const QUrl &url, bool allowLocalNetwork, QString *reason) {
    if (!isAllowedUrl(url, allowLocalNetwork, reason))
        return false;
    if (allowLocalNetwork)
        return true;

    const QString host = url.host(QUrl::FullyEncoded).trimmed();
    const QHostAddress literal = parseHostLiteral(host);
    if (!literal.isNull()) {
        if (isPrivateHostAddress(literal))
            return setReason(reason, QStringLiteral("Private, loopback, and link-local IP addresses are blocked."));
        return true;
    }

    if (const auto hook = currentResolverHook()) {
        QString errStr;
        const QList<QHostAddress> addrs = hook(host, &errStr);
        if (addrs.isEmpty()) {
            const QString detail = errStr.isEmpty() ? QStringLiteral("host lookup failed") : errStr;
            return setReason(reason, QStringLiteral("Could not resolve host “%1” (%2).").arg(host, detail));
        }
        for (const QHostAddress &addr : addrs) {
            if (isPrivateHostAddress(addr))
                return setReason(reason, QStringLiteral("Host “%1” resolved to a blocked private or local network address.").arg(host));
        }
        return true;
    }

    const QHostInfo info = QHostInfo::fromName(host);
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        const QString detail = info.errorString().isEmpty() ? QStringLiteral("host lookup failed") : info.errorString();
        return setReason(reason, QStringLiteral("Could not resolve host “%1” (%2).").arg(host, detail));
    }

    for (const QHostAddress &addr : info.addresses()) {
        if (isPrivateHostAddress(addr))
            return setReason(reason, QStringLiteral("Host “%1” resolved to a blocked private or local network address.").arg(host));
    }
    return true;
}

PinnedHttpRequest *UrlGuard::fetchPinned(PinnedHttpOptions options, PinnedHttpRequest::Callback callback,
                                         QObject *parent) {
    auto *req = new PinnedHttpRequest(std::move(options), std::move(callback), parent);
    req->start();
    return req;
}

PinnedHttpResponse UrlGuard::fetchPinnedSync(PinnedHttpOptions options, const std::atomic_bool *cancelFlag) {
    PinnedHttpResponse result;
    if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
        result.error = QStringLiteral("Request cancelled.");
        return result;
    }

    bool done = false;
    QEventLoop loop;
    PinnedHttpRequest req(std::move(options), [&](PinnedHttpResponse resp) {
        result = std::move(resp);
        done = true;
        loop.quit();
    });

    QTimer cancelPoll;
    if (cancelFlag) {
        cancelPoll.setInterval(25);
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
            if (cancelFlag->load(std::memory_order_relaxed)) {
                req.abort();
                result = {};
                result.error = QStringLiteral("Request cancelled.");
                done = true;
                loop.quit();
            }
        });
        cancelPoll.start();
    }

    req.start();
    if (!done)
        loop.exec();
    cancelPoll.stop();
    return result;
}

void UrlGuard::setHostResolverForTests(HostResolverHook hook) {
    QMutexLocker lock(&g_testHookMutex);
    g_resolverHook = std::move(hook);
}

void UrlGuard::setConnectAddressOverrideForTests(ConnectAddressHook hook) {
    QMutexLocker lock(&g_testHookMutex);
    g_connectOverrideHook = std::move(hook);
}
