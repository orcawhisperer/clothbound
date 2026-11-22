#include "htmltokenizer.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>

namespace {
const QHash<QString, char32_t> &namedEntities() {
    static const QHash<QString, char32_t> table = [] {
        QHash<QString, char32_t> t;
        // Latin-1, in code point order from U+00A0.
        static const char *const latin1[] = {
            "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf",
            "laquo", "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro",
            "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
            "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute",
            "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute",
            "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute",
            "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
            "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml", "eth", "ntilde",
            "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc",
            "uuml", "yacute", "thorn", "yuml"};
        for (int i = 0; i < 96; ++i)
            t.insert(QString::fromLatin1(latin1[i]), char32_t(0xA0 + i));
        // Greek, capitals then small letters, in code point order.
        static const char *const greekUpper[] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta",
                                                 "Eta", "Theta", "Iota", "Kappa", "Lambda", "Mu", "Nu", "Xi",
                                                 "Omicron", "Pi", "Rho", nullptr, "Sigma", "Tau", "Upsilon",
                                                 "Phi", "Chi", "Psi", "Omega"};
        static const char *const greekLower[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta",
                                                 "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi",
                                                 "omicron", "pi", "rho", "sigmaf", "sigma", "tau", "upsilon",
                                                 "phi", "chi", "psi", "omega"};
        for (int i = 0; i < 25; ++i) {
            if (greekUpper[i])
                t.insert(QString::fromLatin1(greekUpper[i]), char32_t(0x391 + i));
            t.insert(QString::fromLatin1(greekLower[i]), char32_t(0x3B1 + i));
        }
        const QList<QPair<const char *, char32_t>> rest = {
            {"quot", 34}, {"amp", 38}, {"apos", 39}, {"lt", 60}, {"gt", 62},
            {"OElig", 338}, {"oelig", 339}, {"Scaron", 352}, {"scaron", 353}, {"Yuml", 376},
            {"fnof", 402}, {"circ", 710}, {"tilde", 732}, {"thetasym", 977}, {"upsih", 978}, {"piv", 982},
            {"ensp", 8194}, {"emsp", 8195}, {"numsp", 8199}, {"puncsp", 8200}, {"thinsp", 8201},
            {"hairsp", 8202}, {"ZeroWidthSpace", 8203}, {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206},
            {"rlm", 8207}, {"hyphen", 8208}, {"dash", 8208}, {"ndash", 8211}, {"mdash", 8212},
            {"horbar", 8213}, {"lsquo", 8216}, {"rsquo", 8217}, {"sbquo", 8218}, {"ldquo", 8220},
            {"rdquo", 8221}, {"bdquo", 8222}, {"dagger", 8224}, {"Dagger", 8225}, {"bull", 8226},
            {"nldr", 8229}, {"hellip", 8230}, {"permil", 8240}, {"prime", 8242}, {"Prime", 8243},
            {"lsaquo", 8249}, {"rsaquo", 8250}, {"oline", 8254}, {"frasl", 8260}, {"euro", 8364},
            {"image", 8465}, {"weierp", 8472}, {"real", 8476}, {"trade", 8482}, {"alefsym", 8501},
            {"larr", 8592}, {"uarr", 8593}, {"rarr", 8594}, {"darr", 8595}, {"harr", 8596},
            {"crarr", 8629}, {"lArr", 8656}, {"uArr", 8657}, {"rArr", 8658}, {"dArr", 8659},
            {"hArr", 8660}, {"forall", 8704}, {"part", 8706}, {"exist", 8707}, {"empty", 8709},
            {"nabla", 8711}, {"isin", 8712}, {"notin", 8713}, {"ni", 8715}, {"prod", 8719},
            {"sum", 8721}, {"minus", 8722}, {"lowast", 8727}, {"radic", 8730}, {"prop", 8733},
            {"infin", 8734}, {"ang", 8736}, {"and", 8743}, {"or", 8744}, {"cap", 8745}, {"cup", 8746},
            {"int", 8747}, {"there4", 8756}, {"sim", 8764}, {"cong", 8773}, {"asymp", 8776},
            {"ne", 8800}, {"equiv", 8801}, {"le", 8804}, {"ge", 8805}, {"sub", 8834}, {"sup", 8835},
            {"nsub", 8836}, {"sube", 8838}, {"supe", 8839}, {"oplus", 8853}, {"otimes", 8855},
            {"perp", 8869}, {"sdot", 8901}, {"lceil", 8968}, {"rceil", 8969}, {"lfloor", 8970},
            {"rfloor", 8971}, {"lang", 9001}, {"rang", 9002}, {"loz", 9674}, {"spades", 9824},
            {"clubs", 9827}, {"hearts", 9829}, {"diams", 9830}, {"half", 189}, {"centerdot", 183},
        };
        for (const auto &[name, code] : rest)
            t.insert(QString::fromLatin1(name), code);
        return t;
    }();
    return table;
}

// Browsers read &#128;–&#159; as the Windows-1252 characters those bytes
// stood for (curly quotes, dashes), which is what the book meant.
char32_t windows1252(char32_t code) {
    static const char16_t map[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                     0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                     0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                     0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    return code >= 0x80 && code <= 0x9F ? map[code - 0x80] : code;
}

bool isNameStart(QChar c) { return c.isLetter() || c == '_' || c == ':'; }
bool isSpace(QChar c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }
}

QString HtmlToken::attribute(const QString &wanted) const {
    for (const auto &[key, value] : attributes)
        if (key == wanted)
            return value;
    return {};
}

QString HtmlTokenizer::decodeEntities(QStringView text) {
    const qsizetype amp = text.indexOf('&');
    if (amp < 0)
        return text.toString();
    QString out;
    out.reserve(text.size());
    out.append(text.left(amp));
    const auto &table = namedEntities();
    for (qsizetype i = amp; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c != '&') {
            out.append(c);
            continue;
        }
        // Entity names are short; looking only a little ahead keeps text full
        // of bare ampersands linear.
        qsizetype semicolon = -1;
        for (qsizetype j = i + 1; j < text.size() && j - i <= 40; ++j) {
            if (text[j] == ';') {
                semicolon = j;
                break;
            }
            if (!text[j].isLetterOrNumber() && text[j] != '#')
                break;
        }
        if (semicolon < 0) {
            out.append(c);
            continue;
        }
        const QStringView name = text.mid(i + 1, semicolon - i - 1);
        char32_t code = 0;
        bool found = false;
        if (name.startsWith('#')) {
            bool ok = false;
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const uint value = name.mid(hex ? 2 : 1).toUInt(&ok, hex ? 16 : 10);
            if (ok) {
                code = windows1252(value);
                if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                    code = 0xFFFD;
                found = true;
            }
        } else {
            const auto it = table.constFind(name.toString());
            if (it != table.cend()) {
                code = *it;
                found = true;
            }
        }
        if (!found) {
            out.append(c);
            continue;
        }
        out.append(QString::fromUcs4(&code, 1));
        i = semicolon;
    }
    return out;
}

QString HtmlTokenizer::decodeDocument(const QByteArray &bytes) {
    QString text;
    auto normalize = [](QString s) {
        if (s.contains('\r')) {
            s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            s.replace('\r', '\n');
        }
        return s;
    };
    if (bytes.startsWith("\xEF\xBB\xBF"))
        return normalize(QString::fromUtf8(bytes.mid(3)));
    if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        QStringDecoder utf16(bytes.startsWith("\xFF\xFE") ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        return normalize(utf16(bytes.mid(2)));
    }

    static const QRegularExpression declared(
        R"re((?:encoding|charset)\s*=\s*["']?\s*([A-Za-z0-9_.:\-]+))re");
    const QString head = QString::fromLatin1(bytes.left(1024));
    const auto match = declared.match(head);
    if (match.hasMatch()) {
        const QString name = match.captured(1).toLower();
        if (name != "utf-8" && name != "utf8") {
            QStringDecoder decoder(name.toLatin1().constData());
            if (decoder.isValid())
                return normalize(decoder(bytes));
        }
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    text = utf8(bytes);
    if (utf8.hasError()) {
        QStringDecoder cp1252("windows-1252");
        text = cp1252.isValid() ? cp1252(bytes) : QString::fromLatin1(bytes);
    }
    return normalize(text);
}

HtmlToken HtmlTokenizer::next() {
    const qsizetype length = m_source.size();
    if (m_pos >= length)
        return {};

    // Script and style hold raw text up to their own end tag.
    if (!m_rawTextEnd.isEmpty()) {
        HtmlToken token;
        token.type = HtmlToken::Text;
        const qsizetype end = m_source.indexOf(QStringLiteral("</") + m_rawTextEnd, m_pos, Qt::CaseInsensitive);
        const qsizetype stop = end < 0 ? length : end;
        token.text = m_source.mid(m_pos, stop - m_pos);
        m_pos = stop;
        m_rawTextEnd.clear();
        return token;
    }

    while (m_pos < length && m_source[m_pos] == '<') {
        const QStringView rest = QStringView(m_source).mid(m_pos);
        if (rest.startsWith(QStringLiteral("<!--"))) {
            const qsizetype end = m_source.indexOf(QStringLiteral("-->"), m_pos + 4);
            m_pos = end < 0 ? length : end + 3;
            continue;
        }
        if (rest.startsWith(QStringLiteral("<![CDATA["))) {
            const qsizetype end = m_source.indexOf(QStringLiteral("]]>"), m_pos + 9);
            HtmlToken token;
            token.type = HtmlToken::Text;
            token.text = m_source.mid(m_pos + 9, (end < 0 ? length : end) - m_pos - 9);
            m_pos = end < 0 ? length : end + 3;
            return token;
        }
        if (rest.startsWith(QStringLiteral("<!")) || rest.startsWith(QStringLiteral("<?"))) {
            const qsizetype end = m_source.indexOf('>', m_pos + 2);
            m_pos = end < 0 ? length : end + 1;
            continue;
        }
        if (rest.size() > 1 && (rest[1] == '/' || isNameStart(rest[1])))
            return readTag();
        break;  // A lone "<" in text, such as "a < b".
    }
    if (m_pos >= length)
        return {};
    return readText();
}

HtmlToken HtmlTokenizer::readText() {
    const qsizetype length = m_source.size();
    qsizetype end = m_pos;
    // Step over a leading "<" that didn't start a tag, then run to the next.
    if (m_source[end] == '<')
        ++end;
    while (end < length) {
        end = m_source.indexOf('<', end);
        if (end < 0) {
            end = length;
            break;
        }
        if (end + 1 < length && (m_source[end + 1] == '/' || m_source[end + 1] == '!' ||
                                 m_source[end + 1] == '?' || isNameStart(m_source[end + 1])))
            break;
        ++end;
    }
    HtmlToken token;
    token.type = HtmlToken::Text;
    token.text = decodeEntities(QStringView(m_source).mid(m_pos, end - m_pos));
    m_pos = end;
    return token;
}

HtmlToken HtmlTokenizer::readTag() {
    const qsizetype length = m_source.size();
    HtmlToken token;
    qsizetype i = m_pos + 1;
    if (m_source[i] == '/') {
        token.type = HtmlToken::EndTag;
        ++i;
    } else {
        token.type = HtmlToken::StartTag;
    }
    const qsizetype nameStart = i;
    while (i < length && !isSpace(m_source[i]) && m_source[i] != '>' && m_source[i] != '/')
        ++i;
    token.name = m_source.mid(nameStart, i - nameStart).toLower();

    while (i < length) {
        while (i < length && isSpace(m_source[i]))
            ++i;
        if (i >= length)
            break;
        if (m_source[i] == '>') {
            ++i;
            break;
        }
        if (m_source[i] == '/') {
            token.selfClosing = true;
            ++i;
            continue;
        }
        const qsizetype attrStart = i;
        while (i < length && !isSpace(m_source[i]) && m_source[i] != '=' && m_source[i] != '>' &&
               !(m_source[i] == '/' && i + 1 < length && m_source[i + 1] == '>'))
            ++i;
        const QString attrName = m_source.mid(attrStart, i - attrStart).toLower();
        while (i < length && isSpace(m_source[i]))
            ++i;
        QString value;
        if (i < length && m_source[i] == '=') {
            ++i;
            while (i < length && isSpace(m_source[i]))
                ++i;
            if (i < length && (m_source[i] == '"' || m_source[i] == '\'')) {
                const QChar quote = m_source[i];
                const qsizetype end = m_source.indexOf(quote, i + 1);
                const qsizetype stop = end < 0 ? length : end;
                value = decodeEntities(QStringView(m_source).mid(i + 1, stop - i - 1));
                i = stop + 1;
            } else {
                const qsizetype valueStart = i;
                while (i < length && !isSpace(m_source[i]) && m_source[i] != '>')
                    ++i;
                value = decodeEntities(QStringView(m_source).mid(valueStart, i - valueStart));
            }
        }
        if (!attrName.isEmpty() && token.type == HtmlToken::StartTag)
            token.attributes.append({attrName, value});
    }
    m_pos = qMin(i, length);
    if (token.type == HtmlToken::StartTag && !token.selfClosing) {
        const QString local = token.localName();
        if (local == "script" || local == "style")
            m_rawTextEnd = token.name;
    }
    return token;
}

QString HtmlNode::attribute(const QString &wanted) const {
    for (const auto &[key, value] : attributes)
        if (key == wanted)
            return value;
    if (!wanted.contains(':'))
        for (const auto &[key, value] : attributes)
            if (key.endsWith(':' + wanted))
                return value;
    return {};
}

bool HtmlNode::hasAttribute(const QString &wanted) const {
    for (const auto &[key, value] : attributes)
        if (key == wanted || (!wanted.contains(':') && key.endsWith(':' + wanted)))
            return true;
    return false;
}

QList<const HtmlNode *> HtmlNode::elements(const QString &local) const {
    QList<const HtmlNode *> found;
    std::vector<const HtmlNode *> stack;
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        stack.push_back(it->get());
    while (!stack.empty()) {
        const HtmlNode *node = stack.back();
        stack.pop_back();
        if (node->isText())
            continue;
        if (local.isEmpty() || node->localName() == local)
            found.append(node);
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return found;
}

const HtmlNode *HtmlNode::first(const QString &local) const {
    std::vector<const HtmlNode *> stack;
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        stack.push_back(it->get());
    while (!stack.empty()) {
        const HtmlNode *node = stack.back();
        stack.pop_back();
        if (node->isText())
            continue;
        if (node->localName() == local)
            return node;
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return nullptr;
}

QList<const HtmlNode *> HtmlNode::childElements(const QString &local) const {
    QList<const HtmlNode *> found;
    for (const auto &child : children)
        if (!child->isText() && (local.isEmpty() || child->localName() == local))
            found.append(child.get());
    return found;
}

QString HtmlNode::textContent() const {
    if (isText())
        return text;
    QString out;
    for (const auto &child : children)
        out += child->textContent();
    return out;
}

namespace Html {
bool isVoidElement(const QString &local) {
    static const QStringList voids{"area", "base", "br", "col", "embed", "hr", "img", "input",
                                   "link", "meta", "param", "source", "track", "wbr"};
    return voids.contains(local);
}

std::unique_ptr<HtmlNode> parse(const QString &source, bool html) {
    auto root = std::make_unique<HtmlNode>();
    root->name = "#document";
    std::vector<HtmlNode *> stack{root.get()};
    HtmlTokenizer tokenizer(source);
    for (HtmlToken token = tokenizer.next(); token.type != HtmlToken::End; token = tokenizer.next()) {
        HtmlNode *top = stack.back();
        if (token.type == HtmlToken::Text) {
            if (!top->children.empty() && top->children.back()->isText()) {
                top->children.back()->text += token.text;
                continue;
            }
            auto node = std::make_unique<HtmlNode>();
            node->text = token.text;
            node->parent = top;
            top->children.push_back(std::move(node));
        } else if (token.type == HtmlToken::StartTag) {
            auto node = std::make_unique<HtmlNode>();
            node->name = token.name;
            node->attributes = token.attributes;
            node->parent = top;
            HtmlNode *raw = node.get();
            top->children.push_back(std::move(node));
            if (!token.selfClosing && !(html && isVoidElement(token.localName())))
                stack.push_back(raw);
        } else if (token.type == HtmlToken::EndTag) {
            // Close the nearest open element of that name; an end tag with no
            // open match is ignored, as browsers do.
            for (auto it = stack.size(); it > 1; --it)
                if (stack[it - 1]->name == token.name || stack[it - 1]->localName() == token.localName()) {
                    stack.resize(it - 1);
                    break;
                }
        }
    }
    return root;
}

std::unique_ptr<HtmlNode> parse(const QByteArray &bytes, bool html) {
    return parse(HtmlTokenizer::decodeDocument(bytes), html);
}
}
