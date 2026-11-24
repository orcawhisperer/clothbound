#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <memory>
#include <vector>

// One piece of markup. Names are lowercased and keep their prefix
// ("dc:title", "epub:type"), so books' XML and HTML read the same way.
struct HtmlToken {
    enum Type { StartTag, EndTag, Text, End };
    Type type = End;
    QString name;
    QList<QPair<QString, QString>> attributes;
    bool selfClosing = false;
    QString text;

    QString localName() const { return name.mid(name.indexOf(':') + 1); }
    QString attribute(const QString &name) const;
};

// A forgiving tokenizer for book markup. EPUB chapters are meant to be XHTML,
// but real ones carry HTML entities (&nbsp;), stray ampersands, and
// unbalanced tags; a strict XML reader gives up on those, so this reads them
// the way a browser would, as far as the text and the structure go.
class HtmlTokenizer {
public:
    explicit HtmlTokenizer(const QString &source) : m_source(source) {}

    HtmlToken next();

    // &amp;, &#8217;, &#x2019;, and the HTML named entities; unknown names are
    // left as written, as browsers do.
    static QString decodeEntities(QStringView text);
    // Bytes to text: a byte-order mark, then a declared encoding (XML
    // declaration or meta charset), then UTF-8, then Windows-1252. Line breaks
    // are normalized to \n, as both XML and HTML parsers do.
    static QString decodeDocument(const QByteArray &bytes);

private:
    HtmlToken readTag();
    HtmlToken readText();

    QString m_source;
    qsizetype m_pos = 0;
    QString m_rawTextEnd;
};

// A small tree for the book's own XML: container.xml, the OPF package, the
// NCX, and the navigation document. Chapters are streamed, never built.
struct HtmlNode {
    HtmlNode() = default;
    ~HtmlNode();
    HtmlNode(const HtmlNode &) = delete;
    HtmlNode &operator=(const HtmlNode &) = delete;
    HtmlNode(HtmlNode &&) = default;
    HtmlNode &operator=(HtmlNode &&) = default;

    QString name;  // empty for text
    QList<QPair<QString, QString>> attributes;
    QString text;
    HtmlNode *parent = nullptr;
    std::vector<std::unique_ptr<HtmlNode>> children;

    bool isText() const { return name.isEmpty(); }
    QString localName() const { return name.mid(name.indexOf(':') + 1); }
    bool is(const QString &local) const { return !isText() && localName() == local; }
    // The attribute with exactly this name; a name without a prefix also
    // matches a prefixed one ("role" finds "opf:role").
    QString attribute(const QString &name) const;
    bool hasAttribute(const QString &name) const;
    QList<const HtmlNode *> elements(const QString &local) const;
    const HtmlNode *first(const QString &local) const;
    QList<const HtmlNode *> childElements(const QString &local = {}) const;
    QString textContent() const;
    // The text with runs of whitespace collapsed, for titles and labels.
    QString simplifiedText() const { return textContent().simplified(); }
};

namespace Html {
// Book XML closes every element itself, so <meta> and <link> there can have
// content; `html` applies HTML's empty elements (<br>, <img>) instead.
std::unique_ptr<HtmlNode> parse(const QString &source, bool html = false);
std::unique_ptr<HtmlNode> parse(const QByteArray &bytes, bool html = false);
bool isVoidElement(const QString &localName);
}
