#include "epubbook.h"

#include "htmltokenizer.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

namespace {
const QString idpfObfuscation = QStringLiteral("http://www.idpf.org/2008/embedding");
const QString adobeObfuscation = QStringLiteral("http://ns.adobe.com/pdf/enc#RC");

// Book descriptions are often HTML, escaped or not; keep the paragraphs.
QString plainText(const QString &text) {
    if (!text.contains('<'))
        return text.simplified();
    static const QStringList blocks{"p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote"};
    QString out;
    HtmlTokenizer tokens(text);
    for (HtmlToken token = tokens.next(); token.type != HtmlToken::End; token = tokens.next()) {
        if (token.type == HtmlToken::Text)
            out += token.text;
        else if (blocks.contains(token.localName()))
            out += '\n';
    }
    QStringList paragraphs;
    for (const QString &line : out.split('\n')) {
        const QString simplified = line.simplified();
        if (!simplified.isEmpty())
            paragraphs.append(simplified);
    }
    return paragraphs.join(QStringLiteral("\n\n"));
}

bool isFont(const QString &path, const QString &mediaType) {
    static const QRegularExpression extension(R"(\.(ttf|otf|woff2?|ttc)$)", QRegularExpression::CaseInsensitiveOption);
    return mediaType.contains("font") || mediaType.contains("opentype") || extension.match(path).hasMatch();
}

QStringList tokens(const QString &text) { return text.split(' ', Qt::SkipEmptyParts); }
}

EpubBook::EpubBook(const QString &filePath) : m_zip(filePath) {}

bool EpubBook::fail(const QString &message) {
    m_error = message;
    return false;
}

const EpubItem *EpubBook::item(const QString &path) const {
    const auto it = m_manifest.constFind(path);
    return it == m_manifest.cend() ? nullptr : &*it;
}

bool EpubBook::open() {
    if (!m_zip.open())
        return fail(QStringLiteral("This isn't a readable EPUB (%1).").arg(m_zip.errorString()));

    // The container names the package; a book without one still usually has
    // a single .opf somewhere.
    QString opfPath;
    const QByteArray container = m_zip.read(QStringLiteral("META-INF/container.xml"));
    if (!container.isEmpty()) {
        const auto doc = Html::parse(container);
        for (const HtmlNode *rootfile : doc->elements("rootfile")) {
            const QString type = rootfile->attribute("media-type");
            if (type.isEmpty() || type == "application/oebps-package+xml") {
                opfPath = m_zip.resolve(rootfile->attribute("full-path"));
                if (!opfPath.isEmpty())
                    break;
            }
        }
    }
    if (opfPath.isEmpty())
        for (const QString &name : m_zip.names())
            if (name.endsWith(QStringLiteral(".opf"), Qt::CaseInsensitive)) {
                opfPath = name;
                break;
            }
    if (opfPath.isEmpty())
        return fail(QStringLiteral("This EPUB has no package file, so it can't be read."));

    readEncryption();
    if (!readPackage(opfPath))
        return false;
    if (m_spine.isEmpty())
        return fail(QStringLiteral("This EPUB has no chapters to read."));

    // Store DRM encrypts the text itself; font obfuscation only touches fonts.
    for (auto it = m_encryption.cbegin(); it != m_encryption.cend(); ++it) {
        const bool obfuscation = it.value() == idpfObfuscation || it.value() == adobeObfuscation;
        if (!obfuscation || !isFont(it.key(), mediaType(it.key())))
            m_drm = true;
    }
    for (const auto &item : std::as_const(m_spine))
        if (m_zip.encrypted(item.path))
            m_drm = true;
    return true;
}

void EpubBook::readEncryption() {
    const QByteArray bytes = m_zip.read(QStringLiteral("META-INF/encryption.xml"));
    if (bytes.isEmpty())
        return;
    const auto doc = Html::parse(bytes);
    for (const HtmlNode *data : doc->elements("encrypteddata")) {
        const HtmlNode *method = data->first("encryptionmethod");
        const HtmlNode *reference = data->first("cipherreference");
        if (!method || !reference)
            continue;
        // References are relative to the root of the container.
        const QString path = resolve(QString(), reference->attribute("uri"));
        const QString stored = m_zip.resolve(path);
        m_encryption.insert(stored.isEmpty() ? path : stored, method->attribute("algorithm"));
    }
}

bool EpubBook::readPackage(const QString &opfPath) {
    QString error;
    const QByteArray bytes = m_zip.read(opfPath, &error);
    if (bytes.isEmpty())
        return fail(QStringLiteral("The book's package file can't be read (%1).").arg(error));
    m_opfPath = opfPath;
    const auto doc = Html::parse(bytes);
    const HtmlNode *package = doc->first("package");
    if (!package)
        return fail(QStringLiteral("The book's package file is damaged."));
    m_version = package->attribute("version");
    m_uniqueIdentifier = package->attribute("unique-identifier");

    // EPUB 3 attaches roles and series positions to elements by id.
    QHash<QString, QList<QPair<QString, QString>>> refines;
    QString coverId, collectionId;
    const HtmlNode *metadata = package->first("metadata");
    if (metadata) {
        for (const HtmlNode *meta : metadata->elements("meta")) {
            const QString target = meta->attribute("refines");
            const QString property = meta->attribute("property");
            const QString name = meta->attribute("name");
            const QString content = meta->attribute("content");
            const QString text = meta->simplifiedText();
            if (!target.isEmpty() && !property.isEmpty())
                refines[target.startsWith('#') ? target.mid(1) : target].append({property, text});
            if (name == "cover")
                coverId = content;
            else if (name == "calibre:series")
                m_metadata.series = content.simplified();
            else if (name == "calibre:series_index")
                m_metadata.seriesIndex = content.toDouble();
            if (property == "rendition:layout" && text == "pre-paginated")
                m_fixedLayout = true;
            if (property == "belongs-to-collection" && target.isEmpty() && m_metadata.series.isEmpty()) {
                m_metadata.series = text;
                collectionId = meta->attribute("id");
            }
        }
        if (!collectionId.isEmpty())
            for (const auto &[property, value] : refines.value(collectionId))
                if (property == "group-position")
                    m_metadata.seriesIndex = value.toDouble();

        auto refined = [&](const HtmlNode *node, const QString &property) {
            for (const auto &[key, value] : refines.value(node->attribute("id")))
                if (key == property)
                    return value;
            return QString();
        };

        for (const HtmlNode *title : metadata->elements("title")) {
            if (!title->name.startsWith("dc:") && title->parent != metadata)
                continue;
            const QString text = title->simplifiedText();
            if (text.isEmpty())
                continue;
            const QString type = refined(title, "title-type");
            if (m_metadata.title.isEmpty() || type == "main")
                m_metadata.title = text;
            if (type == "main")
                break;
        }
        auto people = [&](const QString &element, bool creator) {
            for (const HtmlNode *person : metadata->elements(element)) {
                const QString name = person->simplifiedText();
                if (name.isEmpty())
                    continue;
                QString role = person->attribute("role").toLower();
                if (role.isEmpty())
                    role = refined(person, "role").toLower();
                if (role == "edt")
                    m_metadata.editors.append(name);
                else if ((role.isEmpty() && creator) || role == "aut")
                    m_metadata.authors.append(name);
                else if (role != "bkp" && role != "mrk")
                    m_metadata.contributors.append(name);
            }
        };
        people("creator", true);
        people("contributor", false);

        if (const HtmlNode *publisher = metadata->first("publisher"))
            m_metadata.publisher = publisher->simplifiedText();
        for (const HtmlNode *date : metadata->elements("date")) {
            const QString event = date->attribute("event").toLower();
            if (event == "modification" || event == "conversion" || event == "file-creation")
                continue;
            m_metadata.date = date->simplifiedText();
            break;
        }
        if (const HtmlNode *language = metadata->first("language"))
            m_metadata.language = language->simplifiedText();
        if (const HtmlNode *description = metadata->first("description"))
            m_metadata.description = plainText(description->textContent());
        for (const HtmlNode *subject : metadata->elements("subject")) {
            const QString text = subject->simplifiedText();
            if (!text.isEmpty() && !m_metadata.subjects.contains(text))
                m_metadata.subjects.append(text);
        }
        static const QRegularExpression isbnPattern(R"(^(?:urn:isbn:|isbn:?\s*)?((?:97[89])?\d{9}[\dXx])$)",
                                                    QRegularExpression::CaseInsensitiveOption);
        for (const HtmlNode *identifier : metadata->elements("identifier")) {
            const QString text = identifier->simplifiedText();
            if (text.isEmpty())
                continue;
            m_identifiers.append(text);
            if (identifier->attribute("id") == m_uniqueIdentifier || m_metadata.identifier.isEmpty())
                m_metadata.identifier = text;
            const auto isbn = isbnPattern.match(QString(text).remove('-').remove(' '));
            if (m_metadata.isbn.isEmpty() && isbn.hasMatch())
                m_metadata.isbn = isbn.captured(1).toUpper();
        }
    }

    QList<EpubItem> manifestOrder;
    if (const HtmlNode *manifest = package->first("manifest"))
        for (const HtmlNode *node : manifest->childElements("item")) {
            EpubItem item;
            item.id = node->attribute("id");
            const QString path = resolve(opfPath, node->attribute("href"));
            if (path.isEmpty())
                continue;
            const QString stored = m_zip.resolve(path);
            item.path = stored.isEmpty() ? path : stored;
            item.mediaType = node->attribute("media-type").trimmed().toLower();
            item.properties = node->attribute("properties");
            m_manifest.insert(item.path, item);
            m_idToPath.insert(item.id, item.path);
            manifestOrder.append(item);
        }

    QString ncxPath;
    if (const HtmlNode *spine = package->first("spine")) {
        ncxPath = m_idToPath.value(spine->attribute("toc"));
        m_rightToLeft = spine->attribute("page-progression-direction") == "rtl";
        for (const HtmlNode *ref : spine->childElements("itemref")) {
            const QString path = m_idToPath.value(ref->attribute("idref"));
            if (path.isEmpty() || !m_zip.contains(path))
                continue;
            EpubSpineItem item;
            item.id = ref->attribute("idref");
            item.path = path;
            item.mediaType = m_manifest.value(path).mediaType;
            item.linear = ref->attribute("linear") != "no";
            if (!m_spineIndex.contains(path)) {
                m_spineIndex.insert(path, int(m_spine.size()));
                m_spine.append(item);
            }
        }
    }

    QString guideCover;
    if (const HtmlNode *guide = package->first("guide"))
        for (const HtmlNode *reference : guide->childElements("reference"))
            if (reference->attribute("type").toLower() == "cover") {
                guideCover = m_zip.resolve(resolve(opfPath, reference->attribute("href")));
                break;
            }

    QString navPath;
    for (const auto &item : std::as_const(manifestOrder))
        if (tokens(item.properties).contains("nav")) {
            navPath = item.path;
            break;
        }
    if (!navPath.isEmpty())
        readNavigation(navPath);
    if (ncxPath.isEmpty())
        for (const auto &item : std::as_const(manifestOrder))
            if (item.mediaType == "application/x-dtbncx+xml") {
                ncxPath = item.path;
                break;
            }
    if (!ncxPath.isEmpty() && (m_toc.isEmpty() || m_pageList.isEmpty()))
        readNcx(ncxPath);
    for (auto &entry : m_toc)
        entry.spine = spineIndex(entry.path);
    for (auto &target : m_pageList)
        target.spine = spineIndex(target.path);

    // Covers, in order of how deliberately the book marks them.
    auto isImage = [](const EpubItem &item) { return item.mediaType.startsWith("image/"); };
    for (const auto &item : std::as_const(manifestOrder))
        if (tokens(item.properties).contains("cover-image") && isImage(item)) {
            m_coverPath = item.path;
            return true;
        }
    if (!coverId.isEmpty()) {
        QString path = m_idToPath.value(coverId);
        if (path.isEmpty())  // Some tools write the file name instead of the id.
            for (const auto &item : std::as_const(manifestOrder))
                if (item.path.endsWith(coverId)) {
                    path = item.path;
                    break;
                }
        if (!path.isEmpty()) {
            const EpubItem item = m_manifest.value(path);
            m_coverPath = isImage(item) ? path : firstImageIn(path);
            if (!m_coverPath.isEmpty())
                return true;
        }
    }
    if (!guideCover.isEmpty()) {
        m_coverPath = firstImageIn(guideCover);
        if (!m_coverPath.isEmpty())
            return true;
    }
    for (const auto &item : std::as_const(manifestOrder))
        if (isImage(item) && (item.id.contains("cover", Qt::CaseInsensitive) ||
                              QFileInfo(item.path).fileName().contains("cover", Qt::CaseInsensitive))) {
            m_coverPath = item.path;
            return true;
        }
    if (!m_spine.isEmpty())
        m_coverPath = firstImageIn(m_spine.first().path);
    return true;
}

void EpubBook::readNavigation(const QString &navPath) {
    const auto doc = Html::parse(m_zip.read(navPath));
    // A list item's label is its own link or span, never one of a nested list.
    std::function<const HtmlNode *(const HtmlNode *)> label = [&](const HtmlNode *node) -> const HtmlNode * {
        for (const HtmlNode *child : node->childElements()) {
            if (child->is("ol") || child->is("ul"))
                continue;
            if (child->is("a") || child->is("span"))
                return child;
            if (const HtmlNode *inner = label(child))
                return inner;
        }
        return nullptr;
    };
    std::function<void(const HtmlNode *, int)> walk = [&](const HtmlNode *list, int depth) {
        for (const HtmlNode *li : list->childElements("li")) {
            if (const HtmlNode *anchor = label(li)) {
                EpubTocEntry entry;
                entry.title = anchor->simplifiedText();
                if (entry.title.isEmpty())
                    entry.title = anchor->attribute("title").simplified();
                entry.depth = depth;
                if (anchor->is("a")) {
                    const QString path = resolve(navPath, anchor->attribute("href"), &entry.fragment);
                    const QString stored = m_zip.resolve(path);
                    entry.path = stored.isEmpty() ? path : stored;
                }
                if (!entry.title.isEmpty())
                    m_toc.append(entry);
            }
            for (const HtmlNode *nested : li->childElements())
                if (nested->is("ol") || nested->is("ul"))
                    walk(nested, depth + 1);
        }
    };
    for (const HtmlNode *nav : doc->elements("nav")) {
        const QString type = nav->attribute("epub:type") + ' ' + nav->attribute("role");
        const HtmlNode *list = nav->first("ol");
        if (!list)
            list = nav->first("ul");
        if (!list)
            continue;
        if ((type.contains("toc") || type.contains("doc-toc")) && m_toc.isEmpty()) {
            walk(list, 0);
        } else if (type.contains("page-list") || type.contains("doc-pagelist")) {
            for (const HtmlNode *a : list->elements("a")) {
                EpubPageTarget target;
                target.label = a->simplifiedText();
                const QString path = resolve(navPath, a->attribute("href"), &target.fragment);
                const QString stored = m_zip.resolve(path);
                target.path = stored.isEmpty() ? path : stored;
                if (!target.label.isEmpty() && !target.path.isEmpty())
                    m_pageList.append(target);
            }
        }
    }
}

void EpubBook::readNcx(const QString &ncxPath) {
    const auto doc = Html::parse(m_zip.read(ncxPath));
    auto labelOf = [](const HtmlNode *node) {
        const HtmlNode *label = node->childElements("navlabel").value(0);
        const HtmlNode *text = label ? label->first("text") : nullptr;
        return text ? text->simplifiedText() : QString();
    };
    auto targetOf = [&](const HtmlNode *node, QString *fragment) {
        const HtmlNode *content = node->childElements("content").value(0);
        if (!content)
            return QString();
        const QString path = resolve(ncxPath, content->attribute("src"), fragment);
        const QString stored = m_zip.resolve(path);
        return stored.isEmpty() ? path : stored;
    };
    if (m_toc.isEmpty())
        if (const HtmlNode *map = doc->first("navmap")) {
            std::function<void(const HtmlNode *, int)> walk = [&](const HtmlNode *parent, int depth) {
                for (const HtmlNode *point : parent->childElements("navpoint")) {
                    EpubTocEntry entry;
                    entry.title = labelOf(point);
                    entry.path = targetOf(point, &entry.fragment);
                    entry.depth = depth;
                    if (!entry.title.isEmpty())
                        m_toc.append(entry);
                    walk(point, depth + 1);
                }
            };
            walk(map, 0);
        }
    if (m_pageList.isEmpty())
        if (const HtmlNode *list = doc->first("pagelist"))
            for (const HtmlNode *target : list->elements("pagetarget")) {
                EpubPageTarget page;
                page.label = labelOf(target);
                page.path = targetOf(target, &page.fragment);
                if (!page.label.isEmpty() && !page.path.isEmpty())
                    m_pageList.append(page);
            }
}

QString EpubBook::firstImageIn(const QString &documentPath) const {
    const QByteArray bytes = m_zip.read(documentPath);
    if (bytes.isEmpty())
        return {};
    HtmlTokenizer tokens(HtmlTokenizer::decodeDocument(bytes));
    for (HtmlToken token = tokens.next(); token.type != HtmlToken::End; token = tokens.next()) {
        if (token.type != HtmlToken::StartTag)
            continue;
        QString source;
        if (token.localName() == "img")
            source = token.attribute("src");
        else if (token.localName() == "image")
            source = token.attribute("xlink:href").isEmpty() ? token.attribute("href") : token.attribute("xlink:href");
        if (source.isEmpty())
            continue;
        const QString path = m_zip.resolve(resolve(documentPath, source));
        if (!path.isEmpty())
            return path;
    }
    return {};
}

QString EpubBook::mediaType(const QString &path) const {
    const QString declared = m_manifest.value(path).mediaType;
    // Some tools write nonsense such as "application/application/x-font-ttf".
    static const QRegularExpression sane(R"(^[a-z]+/[a-z0-9.+\-]+$)");
    if (!declared.isEmpty() && sane.match(declared).hasMatch() && declared.count('/') == 1)
        return declared;
    return mediaTypeForPath(path);
}

QString EpubBook::mediaTypeForPath(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QHash<QString, QString> types{
        {"xhtml", "application/xhtml+xml"}, {"xht", "application/xhtml+xml"}, {"html", "application/xhtml+xml"},
        {"htm", "application/xhtml+xml"},   {"css", "text/css"},             {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},             {"png", "image/png"},            {"gif", "image/gif"},
        {"svg", "image/svg+xml"},           {"webp", "image/webp"},          {"avif", "image/avif"},
        {"ttf", "font/ttf"},                {"otf", "font/otf"},             {"woff", "font/woff"},
        {"woff2", "font/woff2"},            {"ncx", "application/x-dtbncx+xml"},
        {"opf", "application/oebps-package+xml"}, {"xml", "application/xml"}, {"mp3", "audio/mpeg"},
        {"m4a", "audio/mp4"},               {"mp4", "video/mp4"},            {"js", "text/javascript"},
        {"txt", "text/plain"}};
    return types.value(suffix, "application/octet-stream");
}

QByteArray EpubBook::resource(const QString &path, QString *type, QString *error) const {
    const QString stored = m_zip.resolve(path);
    if (type)
        *type = mediaType(stored.isEmpty() ? path : stored);
    QByteArray data = m_zip.read(stored.isEmpty() ? path : stored, error);
    if (!data.isEmpty() && m_encryption.contains(stored))
        data = deobfuscate(stored, data);
    return data;
}

QByteArray EpubBook::deobfuscate(const QString &path, QByteArray data) const {
    const QString method = m_encryption.value(path);
    if (method == idpfObfuscation) {
        // The key is the SHA-1 of the package's unique identifier, without
        // whitespace, applied to the first 1040 bytes.
        QString uid;
        for (const QString &identifier : m_identifiers)
            if (identifier == m_metadata.identifier)
                uid = identifier;
        uid.remove(QRegularExpression(QStringLiteral("[\\x20\\x09\\x0d\\x0a]")));
        const QByteArray key = QCryptographicHash::hash(uid.toUtf8(), QCryptographicHash::Sha1);
        for (qsizetype i = 0; i < qMin<qsizetype>(1040, data.size()); ++i)
            data[i] = char(data[i] ^ key[i % key.size()]);
    } else if (method == adobeObfuscation) {
        // Adobe's key is the 16 bytes of the book's UUID, over 1024 bytes.
        static const QRegularExpression uuid(
            R"(([0-9a-fA-F]{8})-?([0-9a-fA-F]{4})-?([0-9a-fA-F]{4})-?([0-9a-fA-F]{4})-?([0-9a-fA-F]{12}))");
        QByteArray key;
        QStringList candidates{m_metadata.identifier};
        candidates += m_identifiers;
        for (const QString &identifier : std::as_const(candidates)) {
            const auto match = uuid.match(identifier);
            if (match.hasMatch()) {
                key = QByteArray::fromHex(
                    (match.captured(1) + match.captured(2) + match.captured(3) + match.captured(4) + match.captured(5))
                        .toLatin1());
                break;
            }
        }
        if (key.size() == 16)
            for (qsizetype i = 0; i < qMin<qsizetype>(1024, data.size()); ++i)
                data[i] = char(data[i] ^ key[i % 16]);
    }
    return data;
}

QByteArray EpubBook::coverImage(QString *type) const {
    if (m_coverPath.isEmpty())
        return {};
    return resource(m_coverPath, type);
}

QString EpubBook::chapterText(int index) const {
    if (index < 0 || index >= m_spine.size())
        return {};
    return extractText(m_zip.read(m_spine[index].path));
}

QString EpubBook::extractText(const QByteArray &document) {
    const QString source = HtmlTokenizer::decodeDocument(document);
    auto collect = [&source](bool requireBody) {
        QString out;
        out.reserve(source.size() / 2);
        HtmlTokenizer tokens(source);
        bool body = !requireBody, head = false;
        int skip = 0;
        for (HtmlToken token = tokens.next(); token.type != HtmlToken::End; token = tokens.next()) {
            if (token.type == HtmlToken::Text) {
                if (body && !head && skip == 0)
                    out += token.text;
                continue;
            }
            const QString local = token.localName();
            const bool start = token.type == HtmlToken::StartTag;
            if (local == "body" && requireBody)
                body = start && !token.selfClosing;
            else if (local == "head")
                head = start && !token.selfClosing;
            else if ((local == "script" || local == "style" || local == "template") && !token.selfClosing)
                skip = qMax(0, skip + (start ? 1 : -1));
        }
        out.remove(QChar(0x00AD));
        return out;
    };
    // Browsers imply a body when a document has none, so count everything
    // outside the head then.
    const bool hasBody = source.contains(QStringLiteral("<body"), Qt::CaseInsensitive);
    return collect(hasBody);
}

QString EpubBook::resolve(const QString &base, const QString &href, QString *fragment) {
    QString target = href.trimmed();
    if (fragment)
        fragment->clear();
    const qsizetype hash = target.indexOf('#');
    if (hash >= 0) {
        if (fragment)
            *fragment = QUrl::fromPercentEncoding(target.mid(hash + 1).toUtf8());
        target.truncate(hash);
    }
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.\\-]*:"));
    if (scheme.match(target).hasMatch())
        return {};
    const qsizetype query = target.indexOf('?');
    if (query >= 0)
        target.truncate(query);
    target = QUrl::fromPercentEncoding(target.toUtf8());
    if (target.isEmpty())
        return base;
    QStringList parts;
    if (!target.startsWith('/')) {
        parts = base.split('/');
        parts.removeLast();
    }
    for (const QString &segment : target.split('/')) {
        if (segment.isEmpty() || segment == ".")
            continue;
        if (segment == "..") {
            if (!parts.isEmpty())
                parts.removeLast();
            continue;
        }
        parts.append(segment);
    }
    return parts.join('/');
}
