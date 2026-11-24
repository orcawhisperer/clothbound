#include "online/urlguard.h"

#include <QAbstractSocket>
#include <QHostInfo>

namespace {
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
}  // namespace

QHostAddress UrlGuard::parseHostLiteral(const QString &rawHost) {
    QString host = rawHost.trimmed();
    if (host.isEmpty())
        return {};
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
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
