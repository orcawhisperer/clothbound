#pragma once

#include <QHostAddress>
#include <QString>
#include <QUrl>

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
    // or legacy numeric/octal/hex IPv4). Returns a null QHostAddress for DNS names.
    static QHostAddress parseHostLiteral(const QString &host);
};
