#include "online/opdsparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>

namespace {

QByteArray trimmedPayloadPrefix(const QByteArray &data) {
    qsizetype i = 0;
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        i = 3;
    }
    while (i < data.size()) {
        const char ch = data[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            ++i;
        else
            break;
    }
    return data.mid(i, 64);
}

QString baseMediaType(const QString &raw) {
    const qsizetype semi = raw.indexOf(QLatin1Char(';'));
    return (semi >= 0 ? raw.left(semi) : raw).trimmed().toLower();
}

bool isFeedMediaType(const QString &rawType) {
    const QString mt = baseMediaType(rawType);
    return mt == QLatin1String("application/atom+xml") ||
           mt == QLatin1String("application/opds+json") ||
           mt == QLatin1String("application/opds-publication+json");
}

bool isOpenSearchMediaType(const QString &rawType) {
    return baseMediaType(rawType) == QLatin1String("application/opensearchdescription+xml");
}

bool isDrmMediaType(const QString &rawType) {
    const QString mt = baseMediaType(rawType);
    return mt.contains(QLatin1String("adobe.adept")) ||
           mt.contains(QLatin1String("readium.lcp")) ||
           mt.contains(QLatin1String("drm"));
}

QString acquisitionFormatFor(const QString &rawType, const QString &href) {
    if (isDrmMediaType(rawType))
        return {};
    const QString mt = baseMediaType(rawType);
    if (mt == QLatin1String("application/epub+zip") || mt == QLatin1String("application/epub"))
        return QStringLiteral("epub");
    if (mt == QLatin1String("application/pdf"))
        return QStringLiteral("pdf");
    // Only infer from URL extension when MIME type is generic or omitted.
    if (mt.isEmpty() || mt == QLatin1String("application/octet-stream")) {
        const QString path = QUrl(href).path().toLower();
        if (path.endsWith(QLatin1String(".epub")))
            return QStringLiteral("epub");
        if (path.endsWith(QLatin1String(".pdf")))
            return QStringLiteral("pdf");
    }
    return {};
}

bool isAcquisitionRel(const QString &rel) {
    const QString r = rel.trimmed().toLower();
    return r.startsWith(QLatin1String("http://opds-spec.org/acquisition")) ||
           r.startsWith(QLatin1String("https://opds-spec.org/acquisition")) ||
           r == QLatin1String("acquisition") ||
           r == QLatin1String("enclosure");
}

bool isOpenAccessRel(const QString &rel, const QString &price) {
    if (!price.isEmpty())
        return false;
    const QString r = rel.trimmed().toLower();
    if (r.endsWith(QLatin1String("/open-access")))
        return true;
    if (r.endsWith(QLatin1String("/buy")) || r.endsWith(QLatin1String("/borrow")) ||
        r.endsWith(QLatin1String("/subscribe")) || r.endsWith(QLatin1String("/sample")))
        return false;
    return r == QLatin1String("http://opds-spec.org/acquisition") ||
           r == QLatin1String("https://opds-spec.org/acquisition") ||
           r == QLatin1String("acquisition") ||
           r == QLatin1String("enclosure");
}

bool isSampleRel(const QString &rel) {
    return rel.trimmed().toLower().endsWith(QLatin1String("/sample"));
}

bool isThumbnailRel(const QString &rel) {
    const QString r = rel.trimmed().toLower();
    return r == QLatin1String("http://opds-spec.org/image/thumbnail") ||
           r == QLatin1String("https://opds-spec.org/image/thumbnail") ||
           r == QLatin1String("http://opds-spec.org/thumbnail") ||
           r == QLatin1String("x-stanza-cover-image-thumbnail") ||
           r == QLatin1String("thumbnail");
}

bool isCoverImageRel(const QString &rel) {
    const QString r = rel.trimmed().toLower();
    return r == QLatin1String("http://opds-spec.org/image") ||
           r == QLatin1String("https://opds-spec.org/image") ||
           r == QLatin1String("http://opds-spec.org/cover") ||
           r == QLatin1String("cover") ||
           r == QLatin1String("x-stanza-cover-image");
}

bool isNavigationRel(const QString &rel) {
    const QString r = rel.trimmed().toLower();
    return r == QLatin1String("subsection") ||
           r == QLatin1String("http://opds-spec.org/sort/new") ||
           r == QLatin1String("http://opds-spec.org/sort/popular") ||
           r == QLatin1String("http://opds-spec.org/featured") ||
           r == QLatin1String("http://opds-spec.org/recommended") ||
           r == QLatin1String("http://opds-spec.org/shelf") ||
           r == QLatin1String("http://opds-spec.org/crawlable") ||
           r == QLatin1String("http://opds-spec.org/facet") ||
           r == QLatin1String("collection") ||
           r == QLatin1String("alternate");
}

int extractYear(const QString &text) {
    static const QRegularExpression yearRe(QStringLiteral("\\b([12]\\d{3})\\b"));
    const QRegularExpressionMatch m = yearRe.match(text);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

void sortAcquisitions(QList<OpdsLink> &acquisitions) {
    auto rank = [](const OpdsLink &link) -> int {
        int score = 0;
        if (link.format == QLatin1String("epub"))
            score += 100;
        else if (link.format == QLatin1String("pdf"))
            score += 50;
        if (link.openAccess)
            score += 20;
        if (isSampleRel(link.rel))
            score -= 30;
        return score;
    };
    std::stable_sort(acquisitions.begin(), acquisitions.end(),
                     [&](const OpdsLink &a, const OpdsLink &b) { return rank(a) > rank(b); });
}

QString decodeHtmlEntities(QString text) {
    if (!text.contains(QLatin1Char('&')))
        return text;

    // Decode numeric character references first.
    static const QRegularExpression hexRe(QStringLiteral("&#[xX]([0-9A-Fa-f]{1,6});"));
    qsizetype pos = 0;
    while (true) {
        const QRegularExpressionMatch m = hexRe.match(text, pos);
        if (!m.hasMatch())
            break;
        bool ok = false;
        const char32_t cp = m.captured(1).toUInt(&ok, 16);
        if (ok && cp > 0 && cp <= 0x10FFFF) {
            const QString repl = QString::fromUcs4(&cp, 1);
            text.replace(m.capturedStart(), m.capturedLength(), repl);
            pos = m.capturedStart() + repl.size();
        } else {
            pos = m.capturedEnd();
        }
    }

    static const QRegularExpression decRe(QStringLiteral("&#([0-9]{1,7});"));
    pos = 0;
    while (true) {
        const QRegularExpressionMatch m = decRe.match(text, pos);
        if (!m.hasMatch())
            break;
        bool ok = false;
        const char32_t cp = m.captured(1).toUInt(&ok, 10);
        if (ok && cp > 0 && cp <= 0x10FFFF) {
            const QString repl = QString::fromUcs4(&cp, 1);
            text.replace(m.capturedStart(), m.capturedLength(), repl);
            pos = m.capturedStart() + repl.size();
        } else {
            pos = m.capturedEnd();
        }
    }

    text.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&apos;"), QLatin1String("'"));
    text.replace(QLatin1String("&lsquo;"), QString::fromUtf8("\xE2\x80\x98"));
    text.replace(QLatin1String("&rsquo;"), QString::fromUtf8("\xE2\x80\x99"));
    text.replace(QLatin1String("&ldquo;"), QString::fromUtf8("\xE2\x80\x9C"));
    text.replace(QLatin1String("&rdquo;"), QString::fromUtf8("\xE2\x80\x9D"));
    text.replace(QLatin1String("&mdash;"), QString::fromUtf8("\xE2\x80\x94"));
    text.replace(QLatin1String("&ndash;"), QString::fromUtf8("\xE2\x80\x93"));
    text.replace(QLatin1String("&hellip;"), QString::fromUtf8("\xE2\x80\xA6"));
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

QString readAtomTextElement(QXmlStreamReader &xml) {
    const QString type = xml.attributes().value(QLatin1String("type")).toString().trimmed().toLower();
    if (type == QLatin1String("xhtml")) {
        QString out;
        int depth = 1;
        while (!xml.atEnd() && depth > 0) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::StartElement) {
                ++depth;
                const QString name = xml.name().toString().toLower();
                if (name == QLatin1String("br") || name == QLatin1String("p") ||
                    name == QLatin1String("div") || name == QLatin1String("li")) {
                    if (!out.isEmpty() && !out.endsWith(QLatin1Char(' ')))
                        out.append(QLatin1Char(' '));
                }
            } else if (token == QXmlStreamReader::EndElement) {
                const QString name = xml.name().toString().toLower();
                if (name == QLatin1String("p") || name == QLatin1String("div") ||
                    name == QLatin1String("li")) {
                    if (!out.isEmpty() && !out.endsWith(QLatin1Char(' ')))
                        out.append(QLatin1Char(' '));
                }
                --depth;
            } else if (token == QXmlStreamReader::Characters) {
                out.append(xml.text());
            }
        }
        return OpdsParser::stripHtmlToPlainText(out);
    }

    const QString raw = xml.readElementText(QXmlStreamReader::IncludeChildElements);
    if (type == QLatin1String("html") || type == QLatin1String("text/html") ||
        raw.contains(QLatin1Char('<')) || raw.contains(QLatin1Char('&'))) {
        return OpdsParser::stripHtmlToPlainText(raw);
    }
    return raw.simplified();
}

void appendUnique(QStringList &list, const QString &value) {
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty() && !list.contains(trimmed))
        list.append(trimmed);
}

QString localizedJsonString(const QJsonValue &val) {
    if (val.isString())
        return val.toString().trimmed();
    if (val.isObject()) {
        const QJsonObject obj = val.toObject();
        for (const QString &preferred : {QStringLiteral("en"), QStringLiteral("en-US"), QStringLiteral("en-GB")}) {
            if (obj.value(preferred).isString()) {
                const QString s = obj.value(preferred).toString().trimmed();
                if (!s.isEmpty())
                    return s;
            }
        }
        for (auto it = obj.cbegin(); it != obj.cend(); ++it) {
            if (it.value().isString()) {
                const QString s = it.value().toString().trimmed();
                if (!s.isEmpty())
                    return s;
            }
        }
    }
    return {};
}

QStringList extractJsonNames(const QJsonValue &val) {
    QStringList names;
    auto handleOne = [&](const QJsonValue &item) {
        if (item.isString()) {
            appendUnique(names, item.toString());
        } else if (item.isObject()) {
            const QJsonObject obj = item.toObject();
            QString name = localizedJsonString(obj.value(QStringLiteral("name")));
            if (name.isEmpty())
                name = localizedJsonString(obj.value(QStringLiteral("title")));
            if (name.isEmpty())
                name = obj.value(QStringLiteral("code")).toString().trimmed();
            appendUnique(names, name);
        }
    };
    if (val.isArray()) {
        for (const QJsonValue &item : val.toArray())
            handleOne(item);
    } else {
        handleOne(val);
    }
    return names;
}

QStringList extractJsonRels(const QJsonObject &obj) {
    QStringList rels;
    const QJsonValue relVal = obj.value(QStringLiteral("rel"));
    if (relVal.isString()) {
        const QStringList parts = relVal.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &p : parts)
            rels.append(p.trimmed().toLower());
    } else if (relVal.isArray()) {
        for (const QJsonValue &item : relVal.toArray()) {
            if (item.isString()) {
                const QString r = item.toString().trimmed().toLower();
                if (!r.isEmpty())
                    rels.append(r);
            }
        }
    }
    return rels;
}

QJsonArray asJsonArray(const QJsonValue &val) {
    if (val.isArray())
        return val.toArray();
    if (val.isObject())
        return QJsonArray{val};
    return {};
}

bool isRasterImageType(const QString &rawType) {
    const QString mt = baseMediaType(rawType);
    return mt == QLatin1String("image/jpeg") || mt == QLatin1String("image/jpg") ||
           mt == QLatin1String("image/png") || mt == QLatin1String("image/webp") ||
           mt == QLatin1String("image/gif");
}

OpdsEntry parseOpds2Publication(const QJsonObject &pub, const QUrl &baseUrl) {
    OpdsEntry entry;
    const QJsonObject meta = pub.value(QStringLiteral("metadata")).toObject();

    entry.id = meta.value(QStringLiteral("identifier")).toString().trimmed();
    entry.title = OpdsParser::stripHtmlToPlainText(localizedJsonString(meta.value(QStringLiteral("title"))));
    entry.subtitle = OpdsParser::stripHtmlToPlainText(localizedJsonString(meta.value(QStringLiteral("subtitle"))));

    entry.authors = extractJsonNames(meta.value(QStringLiteral("author")));
    if (entry.authors.isEmpty())
        entry.authors = extractJsonNames(meta.value(QStringLiteral("contributor")));
    entry.author = entry.authors.join(QStringLiteral(", "));

    entry.summary = OpdsParser::stripHtmlToPlainText(localizedJsonString(meta.value(QStringLiteral("description"))));

    entry.published = meta.value(QStringLiteral("published")).toString().trimmed();
    if (entry.published.isEmpty())
        entry.published = meta.value(QStringLiteral("modified")).toString().trimmed();
    entry.year = extractYear(entry.published);

    const QJsonValue langVal = meta.value(QStringLiteral("language"));
    if (langVal.isString())
        entry.language = langVal.toString().trimmed();
    else if (langVal.isArray() && !langVal.toArray().isEmpty())
        entry.language = langVal.toArray().first().toString().trimmed();

    const QStringList publishers = extractJsonNames(meta.value(QStringLiteral("publisher")));
    entry.publisher = publishers.join(QStringLiteral(", "));
    entry.categories = extractJsonNames(meta.value(QStringLiteral("subject")));

    // Images: prefer thumbnail / reasonable cover raster images.
    struct ImageCandidate {
        QString url;
        QString type;
        int width = 0;
        bool explicitThumb = false;
        bool explicitCover = false;
        bool raster = false;
    };
    QList<ImageCandidate> candidates;
    const QJsonArray images = asJsonArray(pub.value(QStringLiteral("images")));
    for (const QJsonValue &imgVal : images) {
        const QJsonObject imgObj = imgVal.toObject();
        const QString href = OpdsParser::resolveHref(baseUrl, imgObj.value(QStringLiteral("href")).toString());
        if (href.isEmpty())
            continue;
        ImageCandidate c;
        c.url = href;
        c.type = imgObj.value(QStringLiteral("type")).toString().trimmed();
        c.width = imgObj.value(QStringLiteral("width")).toInt(0);
        c.raster = c.type.isEmpty() || isRasterImageType(c.type);
        const QStringList rels = extractJsonRels(imgObj);
        for (const QString &r : rels) {
            if (isThumbnailRel(r))
                c.explicitThumb = true;
            if (isCoverImageRel(r))
                c.explicitCover = true;
        }
        candidates.append(c);
    }

    if (!candidates.isEmpty()) {
        auto thumbRank = [](const ImageCandidate &c) -> int {
            int s = c.raster ? 100 : 0;
            if (c.explicitThumb)
                s += 50;
            if (c.explicitCover)
                s += 25;
            if (c.width > 0) {
                if (c.width >= 120 && c.width <= 480)
                    s += 40;
                else if (c.width > 480 && c.width <= 900)
                    s += 20;
                else
                    s += 10;
            }
            return s;
        };
        auto bestThumb = std::max_element(candidates.cbegin(), candidates.cend(),
                                          [&](const ImageCandidate &a, const ImageCandidate &b) {
                                              return thumbRank(a) < thumbRank(b);
                                          });
        entry.thumbnailUrl = bestThumb->url;
        entry.coverUrl = bestThumb->url;
        // If an explicit full cover exists separately from thumbnail, preserve coverUrl if thumbnail wasn't preferred.
        for (const ImageCandidate &c : candidates) {
            if (c.explicitCover && c.raster && !bestThumb->explicitThumb && c.width >= bestThumb->width) {
                entry.coverUrl = c.url;
                break;
            }
        }
    }

    // Acquisition and fallback links.
    const QJsonArray links = asJsonArray(pub.value(QStringLiteral("links")));
    for (const QJsonValue &linkVal : links) {
        const QJsonObject linkObj = linkVal.toObject();
        const QString rawHref = linkObj.value(QStringLiteral("href")).toString();
        const QString href = OpdsParser::resolveHref(baseUrl, rawHref);
        if (href.isEmpty())
            continue;
        const QString type = linkObj.value(QStringLiteral("type")).toString().trimmed();
        const QString title = linkObj.value(QStringLiteral("title")).toString().trimmed();
        const QStringList rels = extractJsonRels(linkObj);

        const QString fmt = acquisitionFormatFor(type, href);
        bool acqRel = false;
        QString chosenRel;
        for (const QString &r : rels) {
            if (isAcquisitionRel(r)) {
                acqRel = true;
                chosenRel = r;
                break;
            }
        }

        if (!fmt.isEmpty() && (acqRel || rels.isEmpty())) {
            OpdsLink link;
            link.href = href;
            link.rel = chosenRel.isEmpty() ? QStringLiteral("http://opds-spec.org/acquisition") : chosenRel;
            link.type = type;
            link.title = title;
            link.format = fmt;
            const QJsonObject props = linkObj.value(QStringLiteral("properties")).toObject();
            const QJsonObject priceObj = props.value(QStringLiteral("price")).toObject();
            if (!priceObj.isEmpty()) {
                const QString currency = priceObj.value(QStringLiteral("currency")).toString().trimmed();
                const QJsonValue val = priceObj.value(QStringLiteral("value"));
                const QString num = val.isString() ? val.toString().trimmed()
                                                   : (val.isDouble() ? QString::number(val.toDouble(), 'f', 2) : QString());
                if (!num.isEmpty())
                    link.price = currency.isEmpty() ? num : (currency + QLatin1Char(' ') + num);
            }
            link.openAccess = isOpenAccessRel(link.rel, link.price);
            entry.acquisitions.append(link);
        } else if (entry.navigationUrl.isEmpty() && isFeedMediaType(type)) {
            bool skip = false;
            for (const QString &r : rels) {
                if (r == QLatin1String("self") || r == QLatin1String("up") || r == QLatin1String("next"))
                    skip = true;
            }
            if (!skip) {
                entry.navigationUrl = href;
                entry.navigationType = type;
            }
        }
    }

    sortAcquisitions(entry.acquisitions);
    entry.isNavigation = entry.acquisitions.isEmpty() && !entry.navigationUrl.isEmpty();
    return entry;
}

OpdsEntry parseOpds2NavItem(const QJsonObject &navObj, const QUrl &baseUrl) {
    OpdsEntry entry;
    const QString href = OpdsParser::resolveHref(baseUrl, navObj.value(QStringLiteral("href")).toString());
    entry.title = OpdsParser::stripHtmlToPlainText(localizedJsonString(navObj.value(QStringLiteral("title"))));
    entry.navigationUrl = href;
    entry.navigationType = navObj.value(QStringLiteral("type")).toString().trimmed();
    const QJsonObject props = navObj.value(QStringLiteral("properties")).toObject();
    const int count = props.value(QStringLiteral("numberOfItems")).toInt(-1);
    if (count >= 0)
        entry.subtitle = QString::number(count) + (count == 1 ? QStringLiteral(" item") : QStringLiteral(" items"));
    entry.isNavigation = true;
    return entry;
}

}  // namespace

QVariantMap OpdsLink::toVariantMap() const {
    return {
        {QStringLiteral("href"), href},
        {QStringLiteral("rel"), rel},
        {QStringLiteral("type"), type},
        {QStringLiteral("title"), title},
        {QStringLiteral("format"), format},
        {QStringLiteral("price"), price},
        {QStringLiteral("openAccess"), openAccess},
        {QStringLiteral("templated"), templated},
    };
}

QVariantMap OpdsEntry::toVariantMap() const {
    QVariantList acqList;
    acqList.reserve(acquisitions.size());
    for (const OpdsLink &link : acquisitions)
        acqList.append(link.toVariantMap());

    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("subtitle"), subtitle},
        {QStringLiteral("author"), author},
        {QStringLiteral("authors"), authors},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("published"), published},
        {QStringLiteral("year"), year},
        {QStringLiteral("language"), language},
        {QStringLiteral("publisher"), publisher},
        {QStringLiteral("categories"), categories},
        {QStringLiteral("coverUrl"), coverUrl},
        {QStringLiteral("thumbnailUrl"), thumbnailUrl},
        {QStringLiteral("navigationUrl"), navigationUrl},
        {QStringLiteral("navigationType"), navigationType},
        {QStringLiteral("groupTitle"), groupTitle},
        {QStringLiteral("groupUrl"), groupUrl},
        {QStringLiteral("isNavigation"), isNavigation},
        {QStringLiteral("hasAcquisition"), !acquisitions.isEmpty()},
        {QStringLiteral("primaryAcquisitionUrl"), acquisitions.isEmpty() ? QString() : acquisitions.first().href},
        {QStringLiteral("primaryFormat"), acquisitions.isEmpty() ? QString() : acquisitions.first().format},
        {QStringLiteral("openAccess"), !acquisitions.isEmpty() && acquisitions.first().openAccess},
        {QStringLiteral("price"), acquisitions.isEmpty() ? QString() : acquisitions.first().price},
        {QStringLiteral("acquisitions"), acqList},
    };
}

QVariantMap OpdsGroup::toVariantMap() const {
    return {
        {QStringLiteral("title"), title},
        {QStringLiteral("selfUrl"), selfUrl},
        {QStringLiteral("entryCount"), entryCount},
    };
}

QVariantMap OpdsFeed::toVariantMap() const {
    QVariantList linkList;
    linkList.reserve(links.size());
    for (const OpdsLink &l : links)
        linkList.append(l.toVariantMap());

    QVariantList entryList;
    entryList.reserve(entries.size());
    for (const OpdsEntry &e : entries)
        entryList.append(e.toVariantMap());

    QVariantList groupList;
    groupList.reserve(groups.size());
    for (const OpdsGroup &g : groups)
        groupList.append(g.toVariantMap());

    const QString fmt = format == OpdsFormat::Opds1Atom ? QStringLiteral("opds1")
                      : format == OpdsFormat::Opds2Json ? QStringLiteral("opds2")
                                                        : QStringLiteral("unknown");

    return {
        {QStringLiteral("format"), fmt},
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("subtitle"), subtitle},
        {QStringLiteral("selfUrl"), selfUrl},
        {QStringLiteral("startUrl"), startUrl},
        {QStringLiteral("upUrl"), upUrl},
        {QStringLiteral("nextUrl"), nextUrl},
        {QStringLiteral("searchUrl"), searchUrl},
        {QStringLiteral("searchType"), searchType},
        {QStringLiteral("searchTemplated"), searchTemplated},
        {QStringLiteral("links"), linkList},
        {QStringLiteral("entries"), entryList},
        {QStringLiteral("groups"), groupList},
        {QStringLiteral("error"), error},
    };
}

bool OpdsParser::isAllowedHttpUrl(const QUrl &url) {
    if (!url.isValid() || url.isRelative())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    return !url.host().trimmed().isEmpty();
}

QString OpdsParser::resolveHref(const QUrl &baseUrl, const QString &href, bool preserveTemplate) {
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Reject explicitly dangerous schemes early (including before template substitution).
    static const QRegularExpression schemeRe(QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*):"));
    const QRegularExpressionMatch sm = schemeRe.match(trimmed);
    if (sm.hasMatch()) {
        const QString sch = sm.captured(1).toLower();
        if (sch != QLatin1String("http") && sch != QLatin1String("https"))
            return {};
    }

    QString working = trimmed;
    QList<QPair<QString, QString>> tokens;
    if (preserveTemplate || working.contains(QLatin1Char('{'))) {
        static const QRegularExpression tplRe(QStringLiteral("\\{[^{}]+\\}"));
        int idx = 0;
        qsizetype offset = 0;
        while (true) {
            const QRegularExpressionMatch m = tplRe.match(working, offset);
            if (!m.hasMatch())
                break;
            const QString placeholder = QStringLiteral("OPDSTPL%1END").arg(idx++);
            tokens.append({placeholder, m.captured(0)});
            working.replace(m.capturedStart(), m.capturedLength(), placeholder);
            offset = m.capturedStart() + placeholder.size();
        }
    }

    const QUrl rawUrl(working);
    QUrl resolved;
    if (baseUrl.isValid() && !baseUrl.isEmpty()) {
        resolved = baseUrl.resolved(rawUrl);
    } else {
        resolved = rawUrl;
    }

    if (!isAllowedHttpUrl(resolved))
        return {};

    QString out = resolved.toString(QUrl::FullyEncoded);
    for (const auto &[placeholder, original] : tokens)
        out.replace(placeholder, original);
    return out;
}

QString OpdsParser::stripHtmlToPlainText(const QString &html) {
    if (html.isEmpty())
        return {};

    QString text = html;
    static const QRegularExpression scriptStyleRe(
        QStringLiteral("<(script|style)\\b[^>]*>.*?</\\1\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    text.remove(scriptStyleRe);

    static const QRegularExpression commentRe(
        QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    text.remove(commentRe);

    static const QRegularExpression blockRe(
        QStringLiteral("</?(?:p|div|br|li|tr|h[1-6]|blockquote|section|article)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(blockRe, QStringLiteral(" "));

    static const QRegularExpression tagRe(QStringLiteral("</?[A-Za-z][A-Za-z0-9:_-]*(?:\\s+[^>]*)?/?>"));
    text.remove(tagRe);

    text = decodeHtmlEntities(text);
    // In case an entity-encoded HTML snippet was decoded into tags, strip one more pass.
    if (text.contains(QLatin1Char('<'))) {
        text.replace(blockRe, QStringLiteral(" "));
        text.remove(tagRe);
    }
    return text.simplified();
}

OpdsFormat OpdsParser::detectFormat(const QByteArray &data, const QString &contentType) {
    const QByteArray prefix = trimmedPayloadPrefix(data);
    if (prefix.isEmpty())
        return OpdsFormat::Unknown;

    const char first = prefix.at(0);
    const QString mt = baseMediaType(contentType);

    if ((mt.contains(QLatin1String("opds+json")) || mt.contains(QLatin1String("json"))) && first == '{')
        return OpdsFormat::Opds2Json;
    if ((mt.contains(QLatin1String("atom+xml")) || mt.contains(QLatin1String("xml"))) && first == '<')
        return OpdsFormat::Opds1Atom;

    if (first == '{')
        return OpdsFormat::Opds2Json;
    if (first == '<')
        return OpdsFormat::Opds1Atom;
    return OpdsFormat::Unknown;
}

OpdsFeed OpdsParser::parse(const QByteArray &data, const QUrl &baseUrl, const QString &contentType) {
    const OpdsFormat fmt = detectFormat(data, contentType);
    if (fmt == OpdsFormat::Opds2Json)
        return parseOpds2(data, baseUrl);
    if (fmt == OpdsFormat::Opds1Atom)
        return parseAtom(data, baseUrl);

    OpdsFeed invalid;
    invalid.error = QStringLiteral("This URL did not return a recognized OPDS catalog feed.");
    return invalid;
}

OpdsFeed OpdsParser::parseAtom(const QByteArray &xmlData, const QUrl &baseUrl) {
    OpdsFeed feed;
    feed.format = OpdsFormat::Opds1Atom;

    QXmlStreamReader xml(xmlData);
    bool foundRoot = false;
    bool rootIsHtml = false;

    auto readEntry = [&]() -> OpdsEntry {
        OpdsEntry entry;
        QString summaryText;
        QString contentText;
        QString issuedText;
        QString publishedText;
        bool hasPreferredNav = false;

        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("entry"))
                break;
            if (token != QXmlStreamReader::StartElement)
                continue;

            const QString name = xml.name().toString().toLower();
            if (name == QLatin1String("id")) {
                entry.id = xml.readElementText().trimmed();
            } else if (name == QLatin1String("title")) {
                entry.title = readAtomTextElement(xml);
            } else if (name == QLatin1String("subtitle")) {
                entry.subtitle = readAtomTextElement(xml);
            } else if (name == QLatin1String("summary")) {
                const QString s = readAtomTextElement(xml);
                if (!s.isEmpty())
                    summaryText = s;
            } else if (name == QLatin1String("content")) {
                const QString c = readAtomTextElement(xml);
                if (!c.isEmpty())
                    contentText = c;
            } else if (name == QLatin1String("author")) {
                QString authorName;
                while (!xml.atEnd()) {
                    const auto at = xml.readNext();
                    if (at == QXmlStreamReader::EndElement && xml.name() == QLatin1String("author"))
                        break;
                    if (at == QXmlStreamReader::StartElement && xml.name() == QLatin1String("name"))
                        authorName = xml.readElementText().trimmed();
                }
                appendUnique(entry.authors, authorName);
            } else if (name == QLatin1String("creator")) {
                appendUnique(entry.authors, xml.readElementText().trimmed());
            } else if (name == QLatin1String("issued") || name == QLatin1String("date")) {
                const QString d = xml.readElementText().trimmed();
                if (!d.isEmpty() && issuedText.isEmpty())
                    issuedText = d;
            } else if (name == QLatin1String("published")) {
                const QString d = xml.readElementText().trimmed();
                if (!d.isEmpty())
                    publishedText = d;
            } else if (name == QLatin1String("language")) {
                const QString l = xml.readElementText().trimmed();
                if (!l.isEmpty() && entry.language.isEmpty())
                    entry.language = l;
            } else if (name == QLatin1String("publisher")) {
                const QString p = xml.readElementText().trimmed();
                if (!p.isEmpty() && entry.publisher.isEmpty())
                    entry.publisher = p;
            } else if (name == QLatin1String("category")) {
                const QString label = xml.attributes().value(QLatin1String("label")).toString().trimmed();
                const QString term = xml.attributes().value(QLatin1String("term")).toString().trimmed();
                appendUnique(entry.categories, !label.isEmpty() ? label : term);
                xml.skipCurrentElement();
            } else if (name == QLatin1String("thumbnail")) {
                const QString rawUrl = xml.attributes().value(QLatin1String("url")).toString().trimmed();
                const QString resolved = resolveHref(baseUrl, rawUrl);
                if (!resolved.isEmpty() && entry.thumbnailUrl.isEmpty())
                    entry.thumbnailUrl = resolved;
                xml.skipCurrentElement();
            } else if (name == QLatin1String("link")) {
                const QXmlStreamAttributes attrs = xml.attributes();
                const QString relAttr = attrs.value(QLatin1String("rel")).toString().trimmed();
                const QString rawHref = attrs.value(QLatin1String("href")).toString().trimmed();
                const QString type = attrs.value(QLatin1String("type")).toString().trimmed();
                const QString title = attrs.value(QLatin1String("title")).toString().trimmed();

                // Read any nested <opds:price> child inside <link>.
                QString price;
                while (!xml.atEnd()) {
                    const auto lt = xml.readNext();
                    if (lt == QXmlStreamReader::EndElement && xml.name() == QLatin1String("link"))
                        break;
                    if (lt == QXmlStreamReader::StartElement &&
                        xml.name().toString().compare(QLatin1String("price"), Qt::CaseInsensitive) == 0) {
                        const QString currency =
                            xml.attributes().value(QLatin1String("currencycode")).toString().trimmed();
                        const QString amount = xml.readElementText().trimmed();
                        if (!amount.isEmpty())
                            price = currency.isEmpty() ? amount : (currency + QLatin1Char(' ') + amount);
                    }
                }

                const QString href = resolveHref(baseUrl, rawHref);
                if (href.isEmpty())
                    continue;

                const QStringList rels = relAttr.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                const QString primaryRel = rels.isEmpty() ? QString() : rels.first().toLower();

                bool matchedCover = false;
                for (const QString &r : rels) {
                    if (isThumbnailRel(r)) {
                        if (entry.thumbnailUrl.isEmpty())
                            entry.thumbnailUrl = href;
                        matchedCover = true;
                    } else if (isCoverImageRel(r)) {
                        if (entry.coverUrl.isEmpty())
                            entry.coverUrl = href;
                        matchedCover = true;
                    }
                }
                if (matchedCover)
                    continue;

                bool matchedAcq = false;
                QString acqRel;
                for (const QString &r : rels) {
                    if (isAcquisitionRel(r)) {
                        matchedAcq = true;
                        acqRel = r;
                        break;
                    }
                }
                if (matchedAcq) {
                    const QString fmt = acquisitionFormatFor(type, href);
                    if (!fmt.isEmpty()) {
                        OpdsLink link;
                        link.href = href;
                        link.rel = acqRel;
                        link.type = type;
                        link.title = title;
                        link.format = fmt;
                        link.price = price;
                        link.openAccess = isOpenAccessRel(acqRel, price);
                        entry.acquisitions.append(link);
                    }
                    continue;
                }

                // Navigation subsection link.
                if (isFeedMediaType(type)) {
                    const bool skip = (primaryRel == QLatin1String("self") || primaryRel == QLatin1String("up") ||
                                       primaryRel == QLatin1String("next") || primaryRel == QLatin1String("previous"));
                    if (!skip) {
                        bool isPreferred = false;
                        for (const QString &r : rels) {
                            if (isNavigationRel(r)) {
                                isPreferred = true;
                                break;
                            }
                        }
                        if (entry.navigationUrl.isEmpty() || (isPreferred && !hasPreferredNav)) {
                            entry.navigationUrl = href;
                            entry.navigationType = type;
                            hasPreferredNav = isPreferred;
                        }
                    }
                }
            } else {
                xml.skipCurrentElement();
            }
        }

        entry.author = entry.authors.join(QStringLiteral(", "));
        entry.summary = !summaryText.isEmpty() ? summaryText : contentText;
        entry.published = !issuedText.isEmpty() ? issuedText : publishedText;
        entry.year = extractYear(entry.published);

        if (entry.coverUrl.isEmpty())
            entry.coverUrl = entry.thumbnailUrl;
        if (entry.thumbnailUrl.isEmpty())
            entry.thumbnailUrl = entry.coverUrl;

        sortAcquisitions(entry.acquisitions);
        entry.isNavigation = entry.acquisitions.isEmpty() && !entry.navigationUrl.isEmpty();
        if (entry.isNavigation && entry.subtitle.isEmpty())
            entry.subtitle = entry.summary;

        return entry;
    };

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token != QXmlStreamReader::StartElement)
            continue;

        const QString rootName = xml.name().toString().toLower();
        if (!foundRoot) {
            foundRoot = true;
            if (rootName == QLatin1String("html")) {
                rootIsHtml = true;
                break;
            }
            if (rootName == QLatin1String("entry")) {
                feed.entries.append(readEntry());
                if (!feed.entries.isEmpty())
                    feed.title = feed.entries.first().title;
                break;
            }
            if (rootName != QLatin1String("feed")) {
                feed.format = OpdsFormat::Unknown;
                feed.error = QStringLiteral("This URL did not return an OPDS Atom feed.");
                return feed;
            }
            continue;
        }

        if (rootName == QLatin1String("id")) {
            feed.id = xml.readElementText().trimmed();
        } else if (rootName == QLatin1String("title")) {
            feed.title = readAtomTextElement(xml);
        } else if (rootName == QLatin1String("subtitle")) {
            feed.subtitle = readAtomTextElement(xml);
        } else if (rootName == QLatin1String("link")) {
            const QXmlStreamAttributes attrs = xml.attributes();
            const QString relAttr = attrs.value(QLatin1String("rel")).toString().trimmed();
            const QString rawHref = attrs.value(QLatin1String("href")).toString().trimmed();
            const QString type = attrs.value(QLatin1String("type")).toString().trimmed();
            const QString title = attrs.value(QLatin1String("title")).toString().trimmed();
            const bool templatedAttr =
                attrs.value(QLatin1String("templated")).toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
            xml.skipCurrentElement();

            const QStringList rels = relAttr.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const bool isSearch = std::any_of(rels.cbegin(), rels.cend(), [](const QString &r) {
                return r.compare(QLatin1String("search"), Qt::CaseInsensitive) == 0;
            });
            const bool hasTemplateBraces = rawHref.contains(QLatin1String("{searchTerms}"), Qt::CaseInsensitive) ||
                                           (isSearch && rawHref.contains(QLatin1Char('{')));
            const QString href = resolveHref(baseUrl, rawHref, isSearch || templatedAttr || hasTemplateBraces);
            if (href.isEmpty())
                continue;

            OpdsLink link;
            link.href = href;
            link.rel = relAttr;
            link.type = type;
            link.title = title;
            link.templated = templatedAttr || hasTemplateBraces;
            feed.links.append(link);

            for (const QString &r : rels) {
                const QString rl = r.toLower();
                if (rl == QLatin1String("self") && feed.selfUrl.isEmpty()) {
                    feed.selfUrl = href;
                } else if (rl == QLatin1String("next") && feed.nextUrl.isEmpty()) {
                    feed.nextUrl = href;
                } else if (rl == QLatin1String("start") && feed.startUrl.isEmpty()) {
                    feed.startUrl = href;
                } else if (rl == QLatin1String("up") && feed.upUrl.isEmpty()) {
                    feed.upUrl = href;
                } else if (rl == QLatin1String("search")) {
                    // Prefer direct templated search URLs; otherwise accept OpenSearch XML discovery links.
                    const bool directTemplate = link.templated && (type.isEmpty() || isFeedMediaType(type));
                    const bool openSearch = isOpenSearchMediaType(type);
                    if (directTemplate) {
                        feed.searchUrl = href;
                        feed.searchType = type;
                        feed.searchTemplated = true;
                    } else if (openSearch && !feed.searchTemplated) {
                        feed.searchUrl = href;
                        feed.searchType = type;
                        feed.searchTemplated = false;
                    } else if (feed.searchUrl.isEmpty() && isFeedMediaType(type)) {
                        feed.searchUrl = href;
                        feed.searchType = type;
                        feed.searchTemplated = link.templated;
                    }
                }
            }
        } else if (rootName == QLatin1String("entry")) {
            OpdsEntry entry = readEntry();
            if (!entry.title.isEmpty() || !entry.acquisitions.isEmpty() || !entry.navigationUrl.isEmpty())
                feed.entries.append(std::move(entry));
        } else {
            xml.skipCurrentElement();
        }
    }

    if (rootIsHtml) {
        feed.format = OpdsFormat::Unknown;
        feed.error = QStringLiteral("This URL returned a web page rather than an OPDS catalog.");
        return feed;
    }

    if (xml.hasError()) {
        feed.format = OpdsFormat::Unknown;
        feed.error = QStringLiteral("Could not parse OPDS XML feed: %1").arg(xml.errorString());
        return feed;
    }

    if (!foundRoot) {
        feed.format = OpdsFormat::Unknown;
        feed.error = QStringLiteral("OPDS catalog feed was empty.");
    }
    return feed;
}

OpdsFeed OpdsParser::parseOpds2(const QByteArray &jsonData, const QUrl &baseUrl) {
    OpdsFeed feed;
    feed.format = OpdsFormat::Opds2Json;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        feed.format = OpdsFormat::Unknown;
        feed.error = QStringLiteral("Could not parse OPDS 2.0 JSON feed: %1").arg(parseError.errorString());
        return feed;
    }

    const QJsonObject root = doc.object();
    const QJsonObject meta = root.value(QStringLiteral("metadata")).toObject();

    const bool hasOpdsStructure = !meta.isEmpty() || root.contains(QStringLiteral("navigation")) ||
                                  root.contains(QStringLiteral("publications")) ||
                                  root.contains(QStringLiteral("groups"));
    if (!hasOpdsStructure) {
        feed.format = OpdsFormat::Unknown;
        feed.error = QStringLiteral("This JSON document is not an OPDS 2.0 catalog.");
        return feed;
    }

    feed.id = meta.value(QStringLiteral("identifier")).toString().trimmed();
    feed.title = stripHtmlToPlainText(localizedJsonString(meta.value(QStringLiteral("title"))));
    feed.subtitle = stripHtmlToPlainText(localizedJsonString(meta.value(QStringLiteral("subtitle"))));
    if (feed.subtitle.isEmpty())
        feed.subtitle = stripHtmlToPlainText(localizedJsonString(meta.value(QStringLiteral("description"))));

    // Top-level links.
    const QJsonArray links = asJsonArray(root.value(QStringLiteral("links")));
    for (const QJsonValue &linkVal : links) {
        const QJsonObject linkObj = linkVal.toObject();
        const QStringList rels = extractJsonRels(linkObj);
        const bool templated = linkObj.value(QStringLiteral("templated")).toBool(false);
        const QString rawHref = linkObj.value(QStringLiteral("href")).toString().trimmed();
        const QString type = linkObj.value(QStringLiteral("type")).toString().trimmed();
        const QString title = localizedJsonString(linkObj.value(QStringLiteral("title")));
        const bool isSearch = rels.contains(QStringLiteral("search"));
        const bool hasTemplate = templated || rawHref.contains(QLatin1Char('{'));
        const QString href = resolveHref(baseUrl, rawHref, isSearch || hasTemplate);
        if (href.isEmpty())
            continue;

        OpdsLink link;
        link.href = href;
        link.rel = rels.join(QLatin1Char(' '));
        link.type = type;
        link.title = title;
        link.templated = hasTemplate;
        feed.links.append(link);

        for (const QString &rl : rels) {
            if (rl == QLatin1String("self") && feed.selfUrl.isEmpty()) {
                feed.selfUrl = href;
            } else if (rl == QLatin1String("next") && feed.nextUrl.isEmpty()) {
                feed.nextUrl = href;
            } else if (rl == QLatin1String("start") && feed.startUrl.isEmpty()) {
                feed.startUrl = href;
            } else if (rl == QLatin1String("up") && feed.upUrl.isEmpty()) {
                feed.upUrl = href;
            } else if (rl == QLatin1String("search")) {
                if (hasTemplate) {
                    feed.searchUrl = href;
                    feed.searchType = type;
                    feed.searchTemplated = true;
                } else if (isOpenSearchMediaType(type) && !feed.searchTemplated) {
                    feed.searchUrl = href;
                    feed.searchType = type;
                    feed.searchTemplated = false;
                } else if (feed.searchUrl.isEmpty()) {
                    feed.searchUrl = href;
                    feed.searchType = type;
                    feed.searchTemplated = hasTemplate;
                }
            }
        }
    }

    // Navigation entries.
    const QJsonArray navArray = asJsonArray(root.value(QStringLiteral("navigation")));
    for (const QJsonValue &navVal : navArray) {
        OpdsEntry entry = parseOpds2NavItem(navVal.toObject(), baseUrl);
        if (!entry.navigationUrl.isEmpty())
            feed.entries.append(std::move(entry));
    }

    // Publications.
    const QJsonArray pubArray = asJsonArray(root.value(QStringLiteral("publications")));
    for (const QJsonValue &pubVal : pubArray) {
        OpdsEntry entry = parseOpds2Publication(pubVal.toObject(), baseUrl);
        if (!entry.title.isEmpty() || !entry.acquisitions.isEmpty() || !entry.navigationUrl.isEmpty())
            feed.entries.append(std::move(entry));
    }

    // Groups: flatten inline navigation & publications cleanly with groupTitle/groupUrl,
    // and expose link-only groups as navigable subsections.
    const QJsonArray groupsArray = asJsonArray(root.value(QStringLiteral("groups")));
    for (const QJsonValue &groupVal : groupsArray) {
        const QJsonObject groupObj = groupVal.toObject();
        const QJsonObject groupMeta = groupObj.value(QStringLiteral("metadata")).toObject();
        const QString groupTitle = stripHtmlToPlainText(localizedJsonString(groupMeta.value(QStringLiteral("title"))));

        QString groupSelfUrl;
        const QJsonArray groupLinks = asJsonArray(groupObj.value(QStringLiteral("links")));
        for (const QJsonValue &glVal : groupLinks) {
            const QJsonObject glObj = glVal.toObject();
            const QStringList rels = extractJsonRels(glObj);
            const QString href = resolveHref(baseUrl, glObj.value(QStringLiteral("href")).toString());
            if (href.isEmpty())
                continue;
            if (rels.contains(QStringLiteral("self")) || rels.contains(QStringLiteral("subsection")) ||
                groupSelfUrl.isEmpty()) {
                groupSelfUrl = href;
                if (rels.contains(QStringLiteral("self")))
                    break;
            }
        }

        int addedInGroup = 0;
        const QJsonArray gNav = asJsonArray(groupObj.value(QStringLiteral("navigation")));
        for (const QJsonValue &nv : gNav) {
            OpdsEntry entry = parseOpds2NavItem(nv.toObject(), baseUrl);
            if (!entry.navigationUrl.isEmpty()) {
                entry.groupTitle = groupTitle;
                entry.groupUrl = groupSelfUrl;
                feed.entries.append(std::move(entry));
                ++addedInGroup;
            }
        }

        const QJsonArray gPubs = asJsonArray(groupObj.value(QStringLiteral("publications")));
        for (const QJsonValue &pv : gPubs) {
            OpdsEntry entry = parseOpds2Publication(pv.toObject(), baseUrl);
            if (!entry.title.isEmpty() || !entry.acquisitions.isEmpty() || !entry.navigationUrl.isEmpty()) {
                entry.groupTitle = groupTitle;
                entry.groupUrl = groupSelfUrl;
                feed.entries.append(std::move(entry));
                ++addedInGroup;
            }
        }

        if (addedInGroup == 0 && !groupSelfUrl.isEmpty() && !groupTitle.isEmpty()) {
            OpdsEntry navEntry;
            navEntry.title = groupTitle;
            navEntry.navigationUrl = groupSelfUrl;
            navEntry.isNavigation = true;
            const int numItems = groupMeta.value(QStringLiteral("numberOfItems")).toInt(-1);
            if (numItems >= 0) {
                navEntry.subtitle = QString::number(numItems) +
                                    (numItems == 1 ? QStringLiteral(" item") : QStringLiteral(" items"));
            }
            feed.entries.append(std::move(navEntry));
            ++addedInGroup;
        }

        if (!groupTitle.isEmpty() || !groupSelfUrl.isEmpty())
            feed.groups.append(OpdsGroup{groupTitle, groupSelfUrl, addedInGroup});
    }

    return feed;
}

QString OpdsParser::parseOpenSearchTemplate(const QByteArray &xmlData, const QUrl &baseUrl) {
    QXmlStreamReader xml(xmlData);
    QString bestTemplate;
    int bestRank = -1;

    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (xml.name().toString().compare(QLatin1String("Url"), Qt::CaseInsensitive) != 0)
            continue;

        const QXmlStreamAttributes attrs = xml.attributes();
        const QString rawTemplate = attrs.value(QLatin1String("template")).toString().trimmed();
        const QString type = attrs.value(QLatin1String("type")).toString().trimmed();
        const QString rel = attrs.value(QLatin1String("rel")).toString().trimmed().toLower();
        if (rawTemplate.isEmpty())
            continue;
        if (rel == QLatin1String("suggestions") || type.contains(QLatin1String("suggestions"), Qt::CaseInsensitive))
            continue;

        const QString mt = baseMediaType(type);
        int rank = -1;
        if (mt == QLatin1String("application/atom+xml")) {
            rank = type.contains(QLatin1String("opds-catalog"), Qt::CaseInsensitive) ? 100 : 90;
        } else if (mt == QLatin1String("application/opds+json")) {
            rank = 95;
        } else if (mt == QLatin1String("application/xml") || mt == QLatin1String("text/xml")) {
            rank = 50;
        } else if (mt.isEmpty()) {
            rank = 40;
        } else {
            // Skip text/html or non-feed OpenSearch endpoints.
            continue;
        }

        const QString resolved = resolveHref(baseUrl, rawTemplate, true);
        if (!resolved.isEmpty() && rank > bestRank) {
            bestRank = rank;
            bestTemplate = resolved;
        }
    }

    if (xml.hasError())
        return {};
    return bestTemplate;
}

QString OpdsParser::expandSearchTemplate(const QString &templateUrl, const QString &query) {
    if (templateUrl.trimmed().isEmpty())
        return {};
    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed()));
    QString out = templateUrl.trimmed();

    static const QRegularExpression termsRe(
        QStringLiteral("\\{(?:[A-Za-z0-9_.-]+:)?searchTerms\\??\\}"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rfc6570QueryRe(
        QStringLiteral("\\{([?&])([A-Za-z0-9_.,*-]+)\\}"));
    if (termsRe.match(out).hasMatch()) {
        out.replace(termsRe, encoded);
    } else if (const QRegularExpressionMatch m = rfc6570QueryRe.match(out); m.hasMatch()) {
        const QString op = m.captured(1);
        const QStringList vars = m.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        QString paramName;
        for (const QString &candidate :
             {QStringLiteral("query"), QStringLiteral("q"), QStringLiteral("search"), QStringLiteral("searchTerms")}) {
            if (vars.contains(candidate, Qt::CaseInsensitive)) {
                paramName = candidate;
                break;
            }
        }
        if (paramName.isEmpty() && !vars.isEmpty())
            paramName = vars.first();
        const QString replacement =
            encoded.isEmpty() ? QString() : (op + paramName + QLatin1Char('=') + encoded);
        out.replace(m.capturedStart(), m.capturedLength(), replacement);
    } else {
        // Fallback for non-templated search endpoints.
        out.append(out.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?'));
        out.append(QStringLiteral("query=") + encoded);
    }

    // Clear remaining optional OpenSearch or RFC 6570 placeholders like {startPage?} or {&page}.
    static const QRegularExpression optionalRe(QStringLiteral("\\{[^{}]+\\?\\}|\\{[?&][^{}]+\\}"));
    out.remove(optionalRe);
    return out;
}
