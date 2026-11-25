#include "online/articleextractor.h"

#include "formats/htmltokenizer.h"
#include "online/epubwriter.h"
#include "online/urlguard.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <memory>
#include <vector>

namespace {
struct DomNode {
    QString tag;  // empty for text nodes
    QList<QPair<QString, QString>> attrs;
    QString text;
    DomNode *parent = nullptr;
    std::vector<std::unique_ptr<DomNode>> children;
    bool pruned = false;

    ~DomNode() {
        std::vector<std::unique_ptr<DomNode>> pending;
        pending.reserve(children.size());
        for (auto &c : children)
            pending.push_back(std::move(c));
        children.clear();
        while (!pending.empty()) {
            auto cur = std::move(pending.back());
            pending.pop_back();
            if (!cur)
                continue;
            for (auto &c : cur->children)
                pending.push_back(std::move(c));
            cur->children.clear();
        }
    }

    bool isText() const { return tag.isEmpty(); }

    QString attr(const QString &name) const {
        for (const auto &[k, v] : attrs)
            if (k == name)
                return v;
        return {};
    }

    bool hasAttr(const QString &name) const {
        for (const auto &[k, v] : attrs)
            if (k == name)
                return true;
        return false;
    }

    QString textContent(bool skipPruned = true) const {
        if (skipPruned && pruned)
            return {};
        if (isText())
            return text;
        QString out;
        std::vector<const DomNode *> stack;
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            stack.push_back(it->get());
        while (!stack.empty()) {
            const DomNode *n = stack.back();
            stack.pop_back();
            if (skipPruned && n->pruned)
                continue;
            if (n->isText()) {
                out += n->text;
                continue;
            }
            if (n->tag == QLatin1String("br")) {
                out += QLatin1Char(' ');
                continue;
            }
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(it->get());
        }
        return out;
    }
};

bool closesParagraph(const QString &tag) {
    static const QSet<QString> set{
        "address", "article", "aside", "blockquote", "details", "dialog", "dd", "div", "dl", "dt",
        "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
        "header", "hgroup", "hr", "li", "main", "nav", "ol", "p", "pre", "section", "table", "ul"};
    return set.contains(tag);
}

bool isRawDiscardTag(const QString &tag) {
    static const QSet<QString> set{
        "script", "style", "noscript", "template", "iframe", "object", "embed", "applet", "nav",
        "footer", "aside", "form", "button", "input", "select", "textarea", "label", "dialog",
        "menu", "menuitem", "canvas", "audio", "video", "source", "track", "map", "area", "svg",
        "head"};
    return set.contains(tag);
}

bool isBlockTag(const QString &tag) {
    static const QSet<QString> set{
        "p", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre", "ul", "ol", "li",
        "table", "thead", "tbody", "tr", "th", "td", "figure", "figcaption", "hr"};
    return set.contains(tag);
}

std::unique_ptr<DomNode> buildHtmlDom(const QString &source) {
    constexpr std::size_t maxDepth = 256;
    auto root = std::make_unique<DomNode>();
    root->tag = QStringLiteral("#document");
    std::vector<DomNode *> stack{root.get()};

    HtmlTokenizer tokenizer(source);
    for (HtmlToken tok = tokenizer.next(); tok.type != HtmlToken::End; tok = tokenizer.next()) {
        if (tok.type == HtmlToken::Text) {
            DomNode *top = stack.back();
            if (!top->children.empty() && top->children.back()->isText()) {
                top->children.back()->text += tok.text;
            } else {
                auto node = std::make_unique<DomNode>();
                node->text = tok.text;
                node->parent = top;
                top->children.push_back(std::move(node));
            }
            continue;
        }

        const QString local = tok.localName();
        if (tok.type == HtmlToken::StartTag) {
            // HTML5 implicit closures for messy real-world markup.
            if (closesParagraph(local) && stack.size() > 1 && stack.back()->tag == QLatin1String("p"))
                stack.pop_back();
            if (local == QLatin1String("li")) {
                for (std::size_t i = stack.size(); i > 1; --i) {
                    const QString &t = stack[i - 1]->tag;
                    if (t == QLatin1String("li")) {
                        stack.resize(i - 1);
                        break;
                    }
                    if (t == QLatin1String("ul") || t == QLatin1String("ol"))
                        break;
                }
            } else if (local == QLatin1String("dt") || local == QLatin1String("dd")) {
                if (stack.size() > 1 &&
                    (stack.back()->tag == QLatin1String("dt") || stack.back()->tag == QLatin1String("dd")))
                    stack.pop_back();
            } else if (local == QLatin1String("td") || local == QLatin1String("th")) {
                if (stack.size() > 1 &&
                    (stack.back()->tag == QLatin1String("td") || stack.back()->tag == QLatin1String("th")))
                    stack.pop_back();
            } else if (local == QLatin1String("tr")) {
                for (std::size_t i = stack.size(); i > 1; --i) {
                    const QString &t = stack[i - 1]->tag;
                    if (t == QLatin1String("tr")) {
                        stack.resize(i - 1);
                        break;
                    }
                    if (t == QLatin1String("table") || t == QLatin1String("thead") || t == QLatin1String("tbody"))
                        break;
                }
            }

            DomNode *top = stack.back();
            auto node = std::make_unique<DomNode>();
            node->tag = local;
            node->attrs = std::move(tok.attributes);
            node->parent = top;
            DomNode *raw = node.get();
            top->children.push_back(std::move(node));

            if (!tok.selfClosing && !Html::isVoidElement(local) && stack.size() < maxDepth)
                stack.push_back(raw);
        } else if (tok.type == HtmlToken::EndTag) {
            for (std::size_t i = stack.size(); i > 1; --i) {
                if (stack[i - 1]->tag == local) {
                    stack.resize(i - 1);
                    break;
                }
            }
        }
    }
    return root;
}

template <typename Fn>
void walkDom(DomNode *node, Fn &&fn) {
    std::vector<DomNode *> stack{node};
    while (!stack.empty()) {
        DomNode *cur = stack.back();
        stack.pop_back();
        if (!fn(cur))
            continue;
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it)
            stack.push_back(it->get());
    }
}

bool isSafeHttpUrl(const QUrl &url) {
    if (!url.isValid() || url.isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    return (scheme == QLatin1String("http") || scheme == QLatin1String("https")) && !url.host().isEmpty();
}

QUrl resolveSafeHttpUrl(const QUrl &baseUrl, const QString &raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QString lower = trimmed.toLower();
    if (lower.startsWith(QLatin1String("javascript:")) || lower.startsWith(QLatin1String("vbscript:")) ||
        lower.startsWith(QLatin1String("data:")) || lower.startsWith(QLatin1String("file:")) ||
        lower.startsWith(QLatin1String("blob:")))
        return {};
    QUrl candidate = QUrl(trimmed);
    if (candidate.isRelative())
        candidate = baseUrl.resolved(candidate);
    if (!isSafeHttpUrl(candidate))
        return {};
    return candidate;
}

QString cleanAuthorText(QString raw) {
    raw = raw.simplified();
    static const QRegularExpression byPrefix(QStringLiteral(R"(^(?:written\s+by|posted\s+by|author\s*:|by)\s+)"),
                                             QRegularExpression::CaseInsensitiveOption);
    raw.remove(byPrefix);
    raw = raw.trimmed();
    if (raw.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
        raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        return {};
    if (raw.size() > 140)
        return {};
    return raw;
}

QString stripSiteSuffix(const QString &rawTitle, const QString &siteName, const QString &host) {
    QString title = rawTitle.simplified();
    if (title.isEmpty())
        return title;

    static const QRegularExpression sepRe(QStringLiteral(R"(\s+(?:\||—|–|·|::|-)\s+)"));
    QList<QRegularExpressionMatch> matches;
    auto it = sepRe.globalMatch(title);
    while (it.hasNext())
        matches.append(it.next());
    if (matches.isEmpty())
        return title;

    const QRegularExpressionMatch &last = matches.last();
    const QString left = title.left(last.capturedStart()).trimmed();
    const QString right = title.mid(last.capturedEnd()).trimmed();
    if (left.size() < 8)
        return title;

    auto compactAlnum = [](const QString &s) {
        QString out;
        out.reserve(s.size());
        for (const QChar c : s)
            if (c.isLetterOrNumber())
                out.append(c.toLower());
        return out;
    };

    const QString rightLow = right.toLower();
    const QString rightCompact = compactAlnum(right);
    const QString siteLow = siteName.simplified().toLower();
    const QString siteCompact = compactAlnum(siteName);
    const QString hostLow = host.toLower();
    const QString hostBare = hostLow.startsWith(QLatin1String("www.")) ? hostLow.mid(4) : hostLow;
    const QString hostStem = compactAlnum(hostBare.section(QLatin1Char('.'), 0, 0));

    const bool matchesSite =
        (!siteLow.isEmpty() &&
         (rightLow == siteLow || rightLow.contains(siteLow) || siteLow.contains(rightLow) ||
          (!siteCompact.isEmpty() && rightCompact == siteCompact))) ||
        (!hostBare.isEmpty() && (rightLow == hostBare || rightLow.contains(hostBare))) ||
        (hostStem.size() >= 3 && (rightLow == hostStem || rightCompact == hostStem));
    const bool shortBrandSuffix =
        right.size() <= 32 && right.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() <= 4 && left.size() >= 15;

    if (matchesSite || (shortBrandSuffix && last.captured(0).trimmed() != QLatin1String("-")))
        return left;
    return title;
}

QString normalizeLanguage(QString lang) {
    lang = lang.simplified().replace(QLatin1Char('_'), QLatin1Char('-'));
    static const QRegularExpression validLang(QStringLiteral(R"(^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$)"));
    if (validLang.match(lang).hasMatch())
        return lang;
    return QStringLiteral("en");
}

struct JsonLdMeta {
    QString title;
    QString author;
    QString publisher;
    QString publishedDate;
    QString description;
    QString language;
    QString imageUrl;
};

QString extractJsonLdPersonName(const QJsonValue &val) {
    if (val.isString())
        return cleanAuthorText(val.toString());
    if (val.isObject()) {
        const QJsonObject obj = val.toObject();
        return cleanAuthorText(obj.value(QStringLiteral("name")).toString());
    }
    if (val.isArray()) {
        QStringList names;
        for (const QJsonValue &item : val.toArray()) {
            const QString n = extractJsonLdPersonName(item);
            if (!n.isEmpty() && !names.contains(n))
                names.append(n);
        }
        return names.join(QStringLiteral(", "));
    }
    return {};
}

QString extractJsonLdImage(const QJsonValue &val) {
    if (val.isString())
        return val.toString().trimmed();
    if (val.isObject())
        return val.toObject().value(QStringLiteral("url")).toString().trimmed();
    if (val.isArray()) {
        for (const QJsonValue &item : val.toArray()) {
            const QString url = extractJsonLdImage(item);
            if (!url.isEmpty())
                return url;
        }
    }
    return {};
}

void harvestJsonLdObject(const QJsonObject &obj, JsonLdMeta &out) {
    if (obj.contains(QStringLiteral("@graph")) && obj.value(QStringLiteral("@graph")).isArray()) {
        for (const QJsonValue &item : obj.value(QStringLiteral("@graph")).toArray())
            if (item.isObject())
                harvestJsonLdObject(item.toObject(), out);
    }

    auto hasArticleType = [](const QJsonValue &typeVal) {
        auto matches = [](const QString &t) {
            const QString low = t.toLower();
            return low.contains(QLatin1String("article")) || low.contains(QLatin1String("blogposting")) ||
                   low.contains(QLatin1String("newsarticle")) || low.contains(QLatin1String("report")) ||
                   low.contains(QLatin1String("webpage")) || low.contains(QLatin1String("creativework"));
        };
        if (typeVal.isString())
            return matches(typeVal.toString());
        if (typeVal.isArray()) {
            for (const QJsonValue &v : typeVal.toArray())
                if (v.isString() && matches(v.toString()))
                    return true;
        }
        return false;
    };

    if (!hasArticleType(obj.value(QStringLiteral("@type"))))
        return;

    if (out.title.isEmpty()) {
        const QString h = obj.value(QStringLiteral("headline")).toString().simplified();
        out.title = h.isEmpty() ? obj.value(QStringLiteral("name")).toString().simplified() : h;
    }
    if (out.author.isEmpty())
        out.author = extractJsonLdPersonName(obj.value(QStringLiteral("author")));
    if (out.publisher.isEmpty()) {
        const QJsonValue pub = obj.value(QStringLiteral("publisher"));
        if (pub.isObject())
            out.publisher = pub.toObject().value(QStringLiteral("name")).toString().simplified();
        else if (pub.isString())
            out.publisher = pub.toString().simplified();
    }
    if (out.publishedDate.isEmpty())
        out.publishedDate = obj.value(QStringLiteral("datePublished")).toString().simplified();
    if (out.description.isEmpty())
        out.description = obj.value(QStringLiteral("description")).toString().simplified();
    if (out.language.isEmpty())
        out.language = obj.value(QStringLiteral("inLanguage")).toString().simplified();
    if (out.imageUrl.isEmpty())
        out.imageUrl = extractJsonLdImage(obj.value(QStringLiteral("image")));
}

struct PageMeta {
    QString ogTitle;
    QString twitterTitle;
    QString htmlTitle;
    QString firstH1;
    QString metaAuthor;
    QString bylineAuthor;
    QString siteName;
    QString publishedDate;
    QString timeElementDate;
    QString description;
    QString canonicalHref;
    QString ogUrl;
    QString language;
    QString leadImageHref;
    QUrl baseUrl;
    JsonLdMeta jsonLd;
};

PageMeta extractMetadata(DomNode *root, const QUrl &pageUrl) {
    PageMeta meta;
    meta.baseUrl = pageUrl;

    walkDom(root, [&](DomNode *n) -> bool {
        if (n->isText())
            return false;

        if (n->tag == QLatin1String("html")) {
            const QString lang = n->attr(QStringLiteral("lang")).isEmpty() ? n->attr(QStringLiteral("xml:lang"))
                                                                           : n->attr(QStringLiteral("lang"));
            if (!lang.trimmed().isEmpty())
                meta.language = lang.trimmed();
        } else if (n->tag == QLatin1String("base")) {
            const QUrl resolved = resolveSafeHttpUrl(pageUrl, n->attr(QStringLiteral("href")));
            if (resolved.isValid())
                meta.baseUrl = resolved;
        } else if (n->tag == QLatin1String("title")) {
            if (meta.htmlTitle.isEmpty())
                meta.htmlTitle = n->textContent(false).simplified();
        } else if (n->tag == QLatin1String("link")) {
            const QString rel = n->attr(QStringLiteral("rel")).toLower();
            if (rel.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(QLatin1String("canonical")) &&
                meta.canonicalHref.isEmpty()) {
                meta.canonicalHref = n->attr(QStringLiteral("href")).trimmed();
            }
        } else if (n->tag == QLatin1String("meta")) {
            const QString prop = n->attr(QStringLiteral("property")).toLower().trimmed();
            const QString name = n->attr(QStringLiteral("name")).toLower().trimmed();
            const QString httpEquiv = n->attr(QStringLiteral("http-equiv")).toLower().trimmed();
            const QString key = !prop.isEmpty() ? prop : name;
            const QString content = n->attr(QStringLiteral("content")).simplified();
            if (content.isEmpty())
                return false;

            if (key == QLatin1String("og:title") && meta.ogTitle.isEmpty())
                meta.ogTitle = content;
            else if (key == QLatin1String("twitter:title") && meta.twitterTitle.isEmpty())
                meta.twitterTitle = content;
            else if ((key == QLatin1String("author") || key == QLatin1String("article:author") ||
                      key == QLatin1String("byl") || key == QLatin1String("parsely-author") ||
                      key == QLatin1String("dc.creator")) &&
                     meta.metaAuthor.isEmpty()) {
                const QString cleaned = cleanAuthorText(content);
                if (!cleaned.isEmpty())
                    meta.metaAuthor = cleaned;
            } else if ((key == QLatin1String("og:site_name") || key == QLatin1String("application-name")) &&
                       meta.siteName.isEmpty()) {
                meta.siteName = content;
            } else if ((key == QLatin1String("article:published_time") || key == QLatin1String("pubdate") ||
                        key == QLatin1String("publishdate") || key == QLatin1String("dc.date") ||
                        key == QLatin1String("dcterms.created") || key == QLatin1String("date")) &&
                       meta.publishedDate.isEmpty()) {
                meta.publishedDate = content;
            } else if ((key == QLatin1String("og:description") || key == QLatin1String("description") ||
                        key == QLatin1String("twitter:description")) &&
                       meta.description.isEmpty()) {
                meta.description = content;
            } else if (key == QLatin1String("og:url") && meta.ogUrl.isEmpty()) {
                meta.ogUrl = content;
            } else if ((key == QLatin1String("og:image") || key == QLatin1String("twitter:image") ||
                        key == QLatin1String("twitter:image:src")) &&
                       meta.leadImageHref.isEmpty()) {
                meta.leadImageHref = content;
            } else if (httpEquiv == QLatin1String("content-language") && meta.language.isEmpty()) {
                meta.language = content.section(QLatin1Char(','), 0, 0).trimmed();
            }
        } else if (n->tag == QLatin1String("script")) {
            const QString type = n->attr(QStringLiteral("type")).toLower();
            if (type.contains(QLatin1String("ld+json"))) {
                const QJsonDocument doc = QJsonDocument::fromJson(n->textContent(false).toUtf8());
                if (doc.isObject()) {
                    harvestJsonLdObject(doc.object(), meta.jsonLd);
                } else if (doc.isArray()) {
                    for (const QJsonValue &v : doc.array())
                        if (v.isObject())
                            harvestJsonLdObject(v.toObject(), meta.jsonLd);
                }
            }
            return false;
        } else if (n->tag == QLatin1String("h1")) {
            // Skip site-logo <h1> inside <nav>/<footer>/<aside>; prefer one inside <article>/<main>.
            bool insideChrome = false;
            bool insideMain = false;
            for (const DomNode *p = n->parent; p; p = p->parent) {
                if (p->tag == QLatin1String("nav") || p->tag == QLatin1String("footer") ||
                    p->tag == QLatin1String("aside") ||
                    p->attr(QStringLiteral("role")).compare(QLatin1String("navigation"), Qt::CaseInsensitive) == 0) {
                    insideChrome = true;
                    break;
                }
                if (p->tag == QLatin1String("article") || p->tag == QLatin1String("main") ||
                    p->attr(QStringLiteral("role")).compare(QLatin1String("main"), Qt::CaseInsensitive) == 0)
                    insideMain = true;
            }
            if (!insideChrome) {
                const QString t = n->textContent(false).simplified();
                if (t.size() >= 3 && t.size() <= 240 && (meta.firstH1.isEmpty() || insideMain))
                    meta.firstH1 = t;
            }
        } else if (n->tag == QLatin1String("time") && meta.timeElementDate.isEmpty()) {
            const QString dt = n->attr(QStringLiteral("datetime")).simplified();
            if (dt.size() >= 4)
                meta.timeElementDate = dt;
        } else if (meta.bylineAuthor.isEmpty()) {
            const QString rel = n->attr(QStringLiteral("rel")).toLower();
            const QString itemProp = n->attr(QStringLiteral("itemprop")).toLower();
            const QString clsId = (n->attr(QStringLiteral("class")) + QLatin1Char(' ') + n->attr(QStringLiteral("id"))).toLower();
            static const QRegularExpression bylineRe(QStringLiteral(R"(\b(?:byline|author-name|post-author|entry-author)\b)"));
            if (rel == QLatin1String("author") || itemProp == QLatin1String("author") || bylineRe.match(clsId).hasMatch()) {
                const QString candidate = cleanAuthorText(n->textContent(false));
                if (candidate.size() >= 2 && candidate.size() <= 80)
                    meta.bylineAuthor = candidate;
            }
        }
        return true;
    });

    return meta;
}

bool isHiddenElement(const DomNode *n) {
    if (n->hasAttr(QStringLiteral("hidden")))
        return true;
    if (n->attr(QStringLiteral("aria-hidden")).compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
        return true;
    const QString style = n->attr(QStringLiteral("style")).toLower();
    if (!style.isEmpty()) {
        static const QRegularExpression hiddenCss(QStringLiteral(R"((?:display\s*:\s*none|visibility\s*:\s*hidden))"));
        if (hiddenCss.match(style).hasMatch())
            return true;
    }
    return false;
}

void pruneNonContent(DomNode *root) {
    static const QSet<QString> blockedRoles{
        "navigation", "banner", "contentinfo", "complementary", "search", "dialog", "alertdialog", "menubar", "toolbar"};
    static const QRegularExpression hardNegative(
        QStringLiteral(R"((?:cookie|consent|gdpr|onetrust|cmp-|share-?bar|social-?share|share-?buttons|share-?links|)"
                       R"(comment-?section|comments-?list|comments-?wrapper|disqus|related-?posts|related-?articles|)"
                       R"(recommended-?posts|newsletter|subscribe-?box|signup-?form|paywall|ad-?slot|ad-?container|)"
                       R"(ad-?banner|advert|sponsored|outbrain|taboola|sidebar|breadcrumbs|cookie-?banner|cookie-?notice|)"
                       R"(popup-?modal|\bads\b|\bad-unit\b))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression softNegative(
        QStringLiteral(R"((?:banner|combx|comment|community|extra|foot|menu|modal|nav|pager|pagination|popup|)"
                       R"(promo|related|remark|rss|share|shoutbox|skyscraper|social|sponsor|subscribe|tags|tool|widget))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression positiveHint(
        QStringLiteral(R"((?:article|body|column|content|entry|essay|main|markdown|page|post|prose|story|text))"),
        QRegularExpression::CaseInsensitiveOption);

    walkDom(root, [&](DomNode *n) -> bool {
        if (n->isText())
            return false;
        if (n->tag == QLatin1String("#document") || n->tag == QLatin1String("html") || n->tag == QLatin1String("body"))
            return true;

        if (isRawDiscardTag(n->tag) || isHiddenElement(n)) {
            n->pruned = true;
            return false;
        }

        const QString role = n->attr(QStringLiteral("role")).toLower().trimmed();
        if (blockedRoles.contains(role)) {
            n->pruned = true;
            return false;
        }

        const QString marker = n->attr(QStringLiteral("id")) + QLatin1Char(' ') + n->attr(QStringLiteral("class"));
        if (!marker.trimmed().isEmpty()) {
            const bool isPrimaryLandmark = (n->tag == QLatin1String("article") || n->tag == QLatin1String("main") ||
                                            role == QLatin1String("main"));
            if (hardNegative.match(marker).hasMatch()) {
                // Only prune an <article> if it's clearly a nested "related-articles" card.
                if (!isPrimaryLandmark || marker.contains(QLatin1String("related"), Qt::CaseInsensitive)) {
                    n->pruned = true;
                    return false;
                }
            }
            if (!isPrimaryLandmark && softNegative.match(marker).hasMatch() && !positiveHint.match(marker).hasMatch()) {
                n->pruned = true;
                return false;
            }
        }
        return true;
    });
}

struct CandidateMetrics {
    int paragraphs = 0;
    int proseChars = 0;
    int totalChars = 0;
    int linkChars = 0;
    double paragraphScore = 0.0;
};

// Monoid summary of `node->textContent(true).simplified()` length + punctuation count
// so every subtree's simplified text length and punctuation count are computed bottom-up in O(N).
struct SimplifiedTextSummary {
    int len = 0;
    int punct = 0;
    bool hasNonSpace = false;
    bool leadingSpace = false;
    bool trailingSpace = false;

    static SimplifiedTextSummary fromText(QStringView text) {
        SimplifiedTextSummary s;
        if (text.isEmpty())
            return s;
        s.leadingSpace = text.first().isSpace();
        s.trailingSpace = text.last().isSpace();
        bool pendingSpace = false;
        for (const QChar c : text) {
            if (c.isSpace()) {
                if (s.hasNonSpace)
                    pendingSpace = true;
                continue;
            }
            if (pendingSpace) {
                ++s.len;
                pendingSpace = false;
            }
            s.hasNonSpace = true;
            ++s.len;
            if (c == QLatin1Char(',') || c == QLatin1Char('.') || c == QLatin1Char(';') ||
                c == QChar(0x3001) || c == QChar(0x3002) || c == QChar(0x060C))
                ++s.punct;
        }
        return s;
    }

    static SimplifiedTextSummary concat(const SimplifiedTextSummary &a, const SimplifiedTextSummary &b) {
        if (!a.hasNonSpace && !b.hasNonSpace)
            return {0, 0, false, a.leadingSpace || b.leadingSpace, a.trailingSpace || b.trailingSpace};
        if (!a.hasNonSpace)
            return {b.len, b.punct, true, a.leadingSpace || a.trailingSpace || b.leadingSpace, b.trailingSpace};
        if (!b.hasNonSpace)
            return {a.len, a.punct, true, a.leadingSpace, a.trailingSpace || b.leadingSpace || b.trailingSpace};
        const bool gap = a.trailingSpace || b.leadingSpace;
        return {a.len + (gap ? 1 : 0) + b.len, a.punct + b.punct, true, a.leadingSpace, b.trailingSpace};
    }
};

const DomNode *selectPrimaryContainer(DomNode *root) {
    DomNode *body = nullptr;
    walkDom(root, [&](DomNode *n) -> bool {
        if (!n->isText() && n->tag == QLatin1String("body")) {
            body = n;
            return false;
        }
        return true;
    });
    DomNode *searchRoot = body ? body : root;
    if (!searchRoot || searchRoot->pruned)
        return searchRoot;

    // Build flat top-down BFS layout so every parent precedes its contiguous children in
    // exact left-to-right document order; reverse iteration then computes all metrics bottom-up in O(N).
    struct VisitItem {
        const DomNode *node = nullptr;
        std::size_t firstChild = 0;
        std::size_t childCount = 0;
        int depth = 0;
    };
    std::vector<VisitItem> order;
    order.reserve(256);
    order.push_back({searchRoot, 0, 0, 0});
    for (std::size_t i = 0; i < order.size(); ++i) {
        const DomNode *n = order[i].node;
        if (n->pruned || n->isText())
            continue;
        const std::size_t first = order.size();
        for (const auto &child : n->children) {
            if (!child->pruned)
                order.push_back({child.get(), 0, 0, order[i].depth + 1});
        }
        order[i].firstChild = first;
        order[i].childCount = order.size() - first;
    }

    struct NodeMemo {
        CandidateMetrics metrics;
        SimplifiedTextSummary text;
    };
    std::vector<NodeMemo> memo(order.size());

    for (std::size_t i = order.size(); i-- > 0;) {
        const VisitItem &item = order[i];
        const DomNode *n = item.node;
        NodeMemo &cur = memo[i];
        if (n->isText()) {
            cur.text = SimplifiedTextSummary::fromText(n->text);
            cur.metrics.totalChars = cur.text.len;
            continue;
        }
        if (n->tag == QLatin1String("br")) {
            cur.text = {0, 0, false, true, true};
            continue;
        }

        for (std::size_t c = item.firstChild; c < item.firstChild + item.childCount; ++c) {
            const NodeMemo &cm = memo[c];
            cur.text = SimplifiedTextSummary::concat(cur.text, cm.text);
            cur.metrics.paragraphs += cm.metrics.paragraphs;
            cur.metrics.proseChars += cm.metrics.proseChars;
            cur.metrics.totalChars += cm.metrics.totalChars;
            cur.metrics.linkChars += cm.metrics.linkChars;
            cur.metrics.paragraphScore += cm.metrics.paragraphScore;
        }

        if (n->tag == QLatin1String("a")) {
            cur.metrics.linkChars = cur.metrics.totalChars;
        } else if (n->tag == QLatin1String("p")) {
            const int len = cur.text.len;
            if (len >= 20) {
                ++cur.metrics.paragraphs;
                cur.metrics.proseChars += len;
                const int punct = qMin(cur.text.punct, 8);
                cur.metrics.paragraphScore += 1.5 + punct * 0.8 + qMin(len / 85.0, 4.5);
            }
        } else if (n->tag == QLatin1String("blockquote") || n->tag == QLatin1String("pre") ||
                   n->tag == QLatin1String("li")) {
            const int len = cur.text.len;
            if (len >= 30) {
                cur.metrics.proseChars += len;
                cur.metrics.paragraphScore += 0.65 * (1.0 + qMin(len / 110.0, 3.0));
            }
        }
    }

    static const QRegularExpression posClass(
        QStringLiteral(R"((?:article|body|content|entry|essay|main|markdown|post|prose|story|text))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression negClass(
        QStringLiteral(R"((?:combx|comment|footer|footnote|masthead|media|meta|outbrain|promo|related|scroll|share|shoutbox|sidebar|sponsor|widget))"),
        QRegularExpression::CaseInsensitiveOption);

    struct ScoredCandidate {
        const DomNode *node = nullptr;
        double score = -1e9;
        int proseChars = 0;
        int depth = 0;
    };
    QList<ScoredCandidate> candidates;

    for (std::size_t i = 0; i < order.size(); ++i) {
        const DomNode *n = order[i].node;
        const int depth = order[i].depth;
        if (n->pruned || n->isText())
            continue;

        const QString role = n->attr(QStringLiteral("role")).toLower().trimmed();
        const bool isCandidate = (n->tag == QLatin1String("article") || n->tag == QLatin1String("main") ||
                                  role == QLatin1String("main") || n->tag == QLatin1String("section") ||
                                  n->tag == QLatin1String("div") || n->tag == QLatin1String("body"));
        if (!isCandidate)
            continue;

        const CandidateMetrics &m = memo[i].metrics;
        const int effectiveChars = qMax(m.proseChars, m.totalChars);
        if (effectiveChars < 60)
            continue;

        double base = 0.0;
        if (n->tag == QLatin1String("article"))
            base = 30.0;
        else if (n->tag == QLatin1String("main") || role == QLatin1String("main"))
            base = 26.0;
        else if (n->tag == QLatin1String("section"))
            base = 8.0;
        else if (n->tag == QLatin1String("div"))
            base = 5.0;

        const QString marker = n->attr(QStringLiteral("id")) + QLatin1Char(' ') + n->attr(QStringLiteral("class"));
        if (posClass.match(marker).hasMatch())
            base += 20.0;
        if (negClass.match(marker).hasMatch())
            base -= 25.0;

        double contentScore = m.paragraphScore;
        if (m.paragraphs == 0 && m.totalChars >= 120)
            contentScore += qMin(m.totalChars / 95.0, 12.0);
        contentScore += qMin(m.proseChars / 220.0, 18.0);

        const double linkDensity = m.totalChars > 0 ? double(m.linkChars) / double(m.totalChars) : 1.0;
        double finalScore = (base + contentScore) * (1.0 - qMin(linkDensity, 0.95));
        if (n->tag == QLatin1String("body"))
            finalScore *= 0.78;
        // Slightly reward deeper focused containers when they capture the same prose.
        finalScore += qMin(depth, 10) * 0.4;

        candidates.append({n, finalScore, effectiveChars, depth});
    }

    if (candidates.isEmpty())
        return searchRoot;

    const ScoredCandidate *best = &candidates.first();
    for (const ScoredCandidate &c : std::as_const(candidates)) {
        if (c.score > best->score)
            best = &c;
    }

    // If the top candidate is a huge wrapper that wraps a single focused <article>
    // or <main> holding >= 75% of its prose, prefer the tighter child container.
    for (const ScoredCandidate &c : std::as_const(candidates)) {
        if (c.node == best->node || c.depth <= best->depth)
            continue;
        if (c.node->tag == QLatin1String("article") || c.node->tag == QLatin1String("main") ||
            c.node->attr(QStringLiteral("role")).compare(QLatin1String("main"), Qt::CaseInsensitive) == 0) {
            if (c.proseChars >= int(best->proseChars * 0.75) && c.score >= best->score * 0.85) {
                const DomNode *anc = c.node->parent;
                while (anc && anc != best->node)
                    anc = anc->parent;
                if (anc == best->node)
                    best = &c;
            }
        }
    }

    return best->node;
}

bool isTrackingPixel(const DomNode *imgNode, const QUrl &resolvedUrl) {
    auto parsePx = [](QString v) -> int {
        v = v.trimmed().toLower();
        if (v.endsWith(QLatin1String("px")))
            v.chop(2);
        bool ok = false;
        const int n = v.toInt(&ok);
        return ok ? n : -1;
    };
    const int w = parsePx(imgNode->attr(QStringLiteral("width")));
    const int h = parsePx(imgNode->attr(QStringLiteral("height")));
    if ((w >= 0 && w <= 2) || (h >= 0 && h <= 2))
        return true;

    const QString full = resolvedUrl.toString().toLower();
    static const QRegularExpression trackerRe(
        QStringLiteral(R"((?:1x1|pixel\.(?:gif|png)|spacer\.gif|beacon|doubleclick\.net|google-analytics\.com/collect|/tr\?|bat\.bing\.com|facebook\.com/tr))"));
    return trackerRe.match(full).hasMatch();
}

QString guessImageExtension(const QUrl &url) {
    const QString path = url.path().toLower();
    if (path.endsWith(QLatin1String(".png")))
        return QStringLiteral("png");
    if (path.endsWith(QLatin1String(".webp")))
        return QStringLiteral("webp");
    if (path.endsWith(QLatin1String(".gif")))
        return QStringLiteral("gif");
    if (path.endsWith(QLatin1String(".svg")))
        return QStringLiteral("svg");
    return QStringLiteral("jpg");
}

struct SanitizeContext {
    QUrl baseUrl;
    QString articleTitle;
    bool strippedLeadingH1 = false;
    QList<ExtractedImage> images;
    QHash<QString, QString> imageUrlToLocal;
    QList<ExtractedHeading> headings;
    QString plainText;

    QString registerImage(const QUrl &url, const QString &alt) {
        const QString key = url.toString();
        const auto existing = imageUrlToLocal.constFind(key);
        if (existing != imageUrlToLocal.cend())
            return *existing;
        if (images.size() >= UrlGuard::maxArticleImages)
            return {};
        const int num = int(images.size()) + 1;
        const QString ext = guessImageExtension(url);
        const QString localPath =
            QStringLiteral("images/img-%1.%2").arg(num, 3, 10, QLatin1Char('0')).arg(ext);
        imageUrlToLocal.insert(key, localPath);
        images.append({url, localPath, alt.simplified()});
        return localPath;
    }
};

QString normalizeInlineText(const QString &raw, bool preserveWhitespace) {
    if (preserveWhitespace)
        return EpubWriter::xmlEscape(raw);
    QString out;
    out.reserve(raw.size());
    bool prevSpace = false;
    for (const QChar c : raw) {
        if (c.isSpace()) {
            if (!prevSpace) {
                out.append(QLatin1Char(' '));
                prevSpace = true;
            }
        } else {
            out.append(c);
            prevSpace = false;
        }
    }
    return EpubWriter::xmlEscape(out);
}

bool hasMeaningfulXhtmlContent(const QString &fragment) {
    if (fragment.contains(QLatin1String("<img ")) || fragment.contains(QLatin1String("<hr/>")))
        return true;
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    QString plain = fragment;
    plain.remove(tagRe);
    return !plain.trimmed().isEmpty();
}

QString sanitizeNode(const DomNode *node, SanitizeContext &ctx, bool inPre, bool inParagraph, bool atBlockRoot);

QString sanitizeChildren(const DomNode *node, SanitizeContext &ctx, bool inPre, bool inParagraph, bool atBlockRoot) {
    QString out;
    QString pendingInline;

    auto flushInline = [&] {
        const QString trimmed = pendingInline.trimmed();
        if (!trimmed.isEmpty()) {
            if (atBlockRoot && !inParagraph && !inPre)
                out += QStringLiteral("<p>") + trimmed + QStringLiteral("</p>\n");
            else
                out += pendingInline;
        }
        pendingInline.clear();
    };

    for (const auto &childPtr : node->children) {
        const DomNode *child = childPtr.get();
        if (child->pruned)
            continue;

        if (!child->isText() && isBlockTag(child->tag)) {
            flushInline();
            out += sanitizeNode(child, ctx, inPre, inParagraph, atBlockRoot);
        } else {
            const QString piece = sanitizeNode(child, ctx, inPre, inParagraph || atBlockRoot, false);
            if (!piece.isEmpty()) {
                if (atBlockRoot && !inPre && !inParagraph) {
                    // Check if unwrapped container produced block elements directly.
                    if (piece.startsWith(QLatin1String("<p>")) || piece.startsWith(QLatin1String("<h")) ||
                        piece.startsWith(QLatin1String("<blockquote")) || piece.startsWith(QLatin1String("<pre")) ||
                        piece.startsWith(QLatin1String("<ul")) || piece.startsWith(QLatin1String("<ol")) ||
                        piece.startsWith(QLatin1String("<table")) || piece.startsWith(QLatin1String("<figure")) ||
                        piece.startsWith(QLatin1String("<hr"))) {
                        flushInline();
                        out += piece;
                    } else {
                        pendingInline += piece;
                    }
                } else {
                    out += piece;
                }
            }
        }
    }
    flushInline();
    return out;
}

QString sanitizeNode(const DomNode *node, SanitizeContext &ctx, bool inPre, bool inParagraph, bool atBlockRoot) {
    if (!node || node->pruned)
        return {};

    if (node->isText()) {
        const QString escaped = normalizeInlineText(node->text, inPre);
        if (!node->text.simplified().isEmpty()) {
            if (!ctx.plainText.isEmpty() && !ctx.plainText.endsWith(QLatin1Char(' ')))
                ctx.plainText.append(QLatin1Char(' '));
            ctx.plainText.append(node->text.simplified());
        }
        return escaped;
    }

    QString tag = node->tag;
    if (isRawDiscardTag(tag))
        return {};

    // Normalize common semantic equivalents.
    if (tag == QLatin1String("b"))
        tag = QStringLiteral("strong");
    else if (tag == QLatin1String("i"))
        tag = QStringLiteral("em");
    else if (tag == QLatin1String("s") || tag == QLatin1String("strike"))
        tag = QStringLiteral("del");
    else if (tag == QLatin1String("tfoot"))
        tag = QStringLiteral("tbody");

    // Void elements
    if (tag == QLatin1String("br"))
        return QStringLiteral("<br/>");
    if (tag == QLatin1String("hr"))
        return QStringLiteral("<hr/>\n");

    if (tag == QLatin1String("img")) {
        QString rawSrc = node->attr(QStringLiteral("src")).trimmed();
        if (rawSrc.isEmpty() || rawSrc.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
            for (const char *lazyAttr : {"data-src", "data-original", "data-lazy-src", "data-url"}) {
                const QString altSrc = node->attr(QLatin1String(lazyAttr)).trimmed();
                if (!altSrc.isEmpty() && !altSrc.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
                    rawSrc = altSrc;
                    break;
                }
            }
        }
        const QUrl resolved = resolveSafeHttpUrl(ctx.baseUrl, rawSrc);
        if (!resolved.isValid() || isTrackingPixel(node, resolved))
            return {};
        const QString alt = node->attr(QStringLiteral("alt")).simplified();
        const QString localPath = ctx.registerImage(resolved, alt);
        if (localPath.isEmpty())
            return {};
        return QStringLiteral("<img src=\"%1\" alt=\"%2\"/>")
            .arg(EpubWriter::xmlEscape(localPath), EpubWriter::xmlEscape(alt));
    }

    if (tag == QLatin1String("a")) {
        const QString inner = sanitizeChildren(node, ctx, inPre, true, false);
        if (inner.trimmed().isEmpty())
            return {};
        const QString rawHref = node->attr(QStringLiteral("href")).trimmed();
        if (rawHref.startsWith(QLatin1Char('#'))) {
            static const QRegularExpression safeFrag(QStringLiteral(R"(^#[A-Za-z0-9_.\-:]+$)"));
            if (safeFrag.match(rawHref).hasMatch())
                return QStringLiteral("<a href=\"%1\">%2</a>").arg(EpubWriter::xmlEscape(rawHref), inner);
            return inner;
        }
        const QUrl resolved = resolveSafeHttpUrl(ctx.baseUrl, rawHref);
        if (!resolved.isValid())
            return inner;  // Strip unsafe href (javascript:, data:, file:, etc.) while keeping readable text.
        return QStringLiteral("<a href=\"%1\">%2</a>").arg(EpubWriter::xmlEscape(resolved.toString()), inner);
    }

    // Headings h1..h6
    if (tag.size() == 2 && tag[0] == QLatin1Char('h') && tag[1] >= QLatin1Char('1') && tag[1] <= QLatin1Char('6')) {
        const int level = tag[1].digitValue();
        const QString plain = node->textContent(true).simplified();
        if (plain.isEmpty())
            return {};
        // Avoid duplicating the article's main title rendered in <header>.
        if (level == 1 && !ctx.strippedLeadingH1 &&
            plain.compare(ctx.articleTitle, Qt::CaseInsensitive) == 0) {
            ctx.strippedLeadingH1 = true;
            return {};
        }
        const QString inner = sanitizeChildren(node, ctx, false, true, false).trimmed();
        if (inner.isEmpty())
            return {};
        const QString id = QStringLiteral("sec-%1").arg(ctx.headings.size() + 1);
        ctx.headings.append({id, plain, level});
        return QStringLiteral("<%1 id=\"%2\">%3</%1>\n").arg(tag, id, inner);
    }

    // Code blocks
    if (tag == QLatin1String("pre")) {
        const QString inner = sanitizeChildren(node, ctx, true, true, false);
        if (inner.trimmed().isEmpty())
            return {};
        if (inner.trimmed().startsWith(QLatin1String("<code>")) && inner.trimmed().endsWith(QLatin1String("</code>")))
            return QStringLiteral("<pre>%1</pre>\n").arg(inner);
        return QStringLiteral("<pre><code>%1</code></pre>\n").arg(inner);
    }

    // Paragraphs
    if (tag == QLatin1String("p")) {
        const QString inner = sanitizeChildren(node, ctx, inPre, true, false).trimmed();
        if (!hasMeaningfulXhtmlContent(inner))
            return {};
        if (inParagraph)
            return inner + QStringLiteral("<br/>");
        return QStringLiteral("<p>%1</p>\n").arg(inner);
    }

    // Block containers
    static const QSet<QString> allowedBlocks{"blockquote", "ul", "ol", "li", "table", "thead", "tbody", "tr", "figure", "figcaption"};
    if (allowedBlocks.contains(tag)) {
        const bool childBlockRoot = (tag == QLatin1String("blockquote"));
        const QString inner = sanitizeChildren(node, ctx, inPre, false, childBlockRoot).trimmed();
        if (!hasMeaningfulXhtmlContent(inner))
            return {};
        return QStringLiteral("<%1>%2</%1>\n").arg(tag, inner);
    }

    // Table cells
    if (tag == QLatin1String("th") || tag == QLatin1String("td")) {
        const QString inner = sanitizeChildren(node, ctx, inPre, true, false).trimmed();
        QString attrs;
        for (const char *spanAttr : {"colspan", "rowspan"}) {
            bool ok = false;
            const int n = node->attr(QLatin1String(spanAttr)).trimmed().toInt(&ok);
            if (ok && n > 1 && n <= 20)
                attrs += QStringLiteral(" %1=\"%2\"").arg(QLatin1String(spanAttr)).arg(n);
        }
        return QStringLiteral("<%1%2>%3</%1>").arg(tag, attrs, inner);
    }

    // Inline formatting tags
    static const QSet<QString> allowedInline{"em", "strong", "sup", "sub", "del", "code"};
    if (allowedInline.contains(tag)) {
        const QString inner = sanitizeChildren(node, ctx, inPre, true, false);
        if (!hasMeaningfulXhtmlContent(inner))
            return {};
        return QStringLiteral("<%1>%2</%1>").arg(tag, inner);
    }

    // Definition list elements
    if (tag == QLatin1String("dt")) {
        const QString inner = sanitizeChildren(node, ctx, inPre, true, false).trimmed();
        return inner.isEmpty() ? QString() : QStringLiteral("<p><strong>%1</strong></p>\n").arg(inner);
    }
    if (tag == QLatin1String("dd")) {
        const QString inner = sanitizeChildren(node, ctx, inPre, true, false).trimmed();
        return inner.isEmpty() ? QString() : QStringLiteral("<p>%1</p>\n").arg(inner);
    }

    // Unwrap structural/presentational containers (div, section, article, main, span, header, picture, etc.)
    return sanitizeChildren(node, ctx, inPre, inParagraph, atBlockRoot);
}

int countWords(const QString &text) {
    int words = 0;
    bool inWord = false;
    for (const QChar c : text) {
        if (c.isLetterOrNumber()) {
            if (!inWord) {
                ++words;
                inWord = true;
            }
        } else {
            inWord = false;
        }
    }
    return words;
}
}  // namespace

ExtractedArticle ArticleExtractor::extract(const QByteArray &htmlBytes, const QUrl &pageUrl) {
    return extract(HtmlTokenizer::decodeDocument(htmlBytes), pageUrl);
}

ExtractedArticle ArticleExtractor::extract(const QString &html, const QUrl &pageUrl) {
    ExtractedArticle result;
    const auto dom = buildHtmlDom(html);

    // 1. Metadata extraction (before pruning <head> / <script type="application/ld+json">).
    const PageMeta meta = extractMetadata(dom.get(), pageUrl);

    QString hostName = pageUrl.host().toLower();
    if (hostName.startsWith(QLatin1String("www.")))
        hostName = hostName.mid(4);

    result.siteName = !meta.siteName.isEmpty() ? meta.siteName
                    : !meta.jsonLd.publisher.isEmpty() ? meta.jsonLd.publisher
                                                       : hostName;

    // Title priority: og:title -> twitter:title -> JSON-LD -> <h1> -> trimmed <title>
    if (!meta.ogTitle.isEmpty())
        result.title = stripSiteSuffix(meta.ogTitle, result.siteName, pageUrl.host());
    else if (!meta.twitterTitle.isEmpty())
        result.title = stripSiteSuffix(meta.twitterTitle, result.siteName, pageUrl.host());
    else if (!meta.jsonLd.title.isEmpty())
        result.title = meta.jsonLd.title;
    else if (!meta.firstH1.isEmpty())
        result.title = meta.firstH1;
    else if (!meta.htmlTitle.isEmpty())
        result.title = stripSiteSuffix(meta.htmlTitle, result.siteName, pageUrl.host());

    if (result.title.isEmpty())
        result.title = !hostName.isEmpty() ? hostName : QStringLiteral("Untitled Article");

    result.author = !meta.metaAuthor.isEmpty() ? meta.metaAuthor
                  : !meta.jsonLd.author.isEmpty() ? meta.jsonLd.author
                                                  : meta.bylineAuthor;
    result.publishedDate = !meta.publishedDate.isEmpty() ? meta.publishedDate
                         : !meta.jsonLd.publishedDate.isEmpty() ? meta.jsonLd.publishedDate
                                                                : meta.timeElementDate;
    result.description = !meta.description.isEmpty() ? meta.description : meta.jsonLd.description;
    result.language = normalizeLanguage(!meta.language.isEmpty() ? meta.language : meta.jsonLd.language);

    const QUrl canonical = resolveSafeHttpUrl(meta.baseUrl, !meta.canonicalHref.isEmpty() ? meta.canonicalHref : meta.ogUrl);
    result.canonicalUrl = canonical.isValid() ? canonical : pageUrl;

    const QString leadImgRaw = !meta.leadImageHref.isEmpty() ? meta.leadImageHref : meta.jsonLd.imageUrl;
    result.leadImageUrl = resolveSafeHttpUrl(meta.baseUrl, leadImgRaw);

    // 2. Strip non-content subtrees and select the primary article container.
    pruneNonContent(dom.get());
    const DomNode *primary = selectPrimaryContainer(dom.get());

    // 3. Sanitize selected container into strictly well-formed XHTML.
    SanitizeContext ctx;
    ctx.baseUrl = meta.baseUrl.isValid() ? meta.baseUrl : pageUrl;
    ctx.articleTitle = result.title;

    result.bodyXhtml = sanitizeChildren(primary, ctx, false, false, true).trimmed();
    result.images = std::move(ctx.images);
    result.headings = std::move(ctx.headings);
    result.plainText = ctx.plainText.simplified();
    result.wordCount = countWords(result.plainText);
    result.readingMinutes = result.wordCount > 0 ? qMax(1, int(std::lround(result.wordCount / 220.0))) : 0;

    return result;
}

QString ArticleExtractor::finalizeImages(const QString &bodyXhtml, const QHash<QString, QString> &downloadedPathMap) {
    static const QRegularExpression imgTagRe(QStringLiteral(R"re(<img\s+src="([^"]+)"\s+alt="([^"]*)"\s*/>)re"));
    QString out;
    out.reserve(bodyXhtml.size());
    qsizetype lastPos = 0;

    auto it = imgTagRe.globalMatch(bodyXhtml);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(bodyXhtml).mid(lastPos, m.capturedStart() - lastPos);
        const QString origPath = HtmlTokenizer::decodeEntities(m.captured(1));
        const auto found = downloadedPathMap.constFind(origPath);
        if (found != downloadedPathMap.cend() && !found->isEmpty()) {
            out += QStringLiteral("<img src=\"%1\" alt=\"%2\"/>")
                       .arg(EpubWriter::xmlEscape(*found), m.captured(2));
        }
        lastPos = m.capturedEnd();
    }
    out += QStringView(bodyXhtml).mid(lastPos);

    // Clean up any empty <figure> (or figure left with only a caption and no image) or empty <p>.
    static const QRegularExpression emptyFigure(
        QStringLiteral(R"(<figure>\s*(?:<figcaption>[\s\S]*?</figcaption>\s*)?</figure>\s*)"));
    static const QRegularExpression emptyPara(QStringLiteral(R"(<p>\s*</p>\s*)"));
    out.remove(emptyFigure);
    out.remove(emptyPara);
    return out.trimmed();
}
