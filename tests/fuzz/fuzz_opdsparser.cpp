#include "online/opdsparser.h"

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <cstdint>

namespace {
constexpr size_t kMaxInputBytes = 256 * 1024;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > kMaxInputBytes)
        return 0;

    static const QUrl baseUrl(QStringLiteral("https://catalog.example.org/opds/v1/root.xml"));
    const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));

    // 1. Format auto-detection and generic entry point.
    (void)OpdsParser::detectFormat(raw);
    (void)OpdsParser::detectFormat(raw, QStringLiteral("application/atom+xml;profile=opds-catalog"));
    (void)OpdsParser::detectFormat(raw, QStringLiteral("application/opds+json"));

    const OpdsFeed autoFeed = OpdsParser::parse(raw, baseUrl);
    (void)autoFeed.isValid();
    (void)autoFeed.toVariantMap();

    // 2. Direct OPDS 1.2 Atom/XML and OPDS 2.0 JSON parsers regardless of sniffing.
    const OpdsFeed atomFeed = OpdsParser::parseAtom(raw, baseUrl);
    if (!atomFeed.entries.isEmpty())
        (void)atomFeed.entries.first().toVariantMap();

    const OpdsFeed jsonFeed = OpdsParser::parseOpds2(raw, baseUrl);
    if (!jsonFeed.entries.isEmpty())
        (void)jsonFeed.entries.first().toVariantMap();

    // 3. OpenSearch description template extraction and expansion.
    const QString tmpl = OpdsParser::parseOpenSearchTemplate(raw, baseUrl);
    if (!tmpl.isEmpty())
        (void)OpdsParser::expandSearchTemplate(tmpl, QStringLiteral("moby & dick <whale>"));

    // 4. URL resolution, scheme guard, and HTML summary stripping.
    const QString asText = QString::fromUtf8(raw);
    const QString resolved = OpdsParser::resolveHref(baseUrl, asText.left(512), (size & 1u) != 0);
    (void)OpdsParser::isAllowedHttpUrl(QUrl(resolved));
    (void)OpdsParser::expandSearchTemplate(asText.left(512), asText.mid(512, 128));
    (void)OpdsParser::stripHtmlToPlainText(asText);

    return 0;
}
