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
                const QString normalized = resolve(QString(), rootfile->attribute("full-path"));
                opfPath = m_zip.resolve(normalized.isEmpty() ? rootfile->attribute("full-path") : normalized);
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
            if (name.compare(QLatin1String("cover"), Qt::CaseInsensitive) == 0)
                coverId = content;
            else if (name.compare(QLatin1String("calibre:series"), Qt::CaseInsensitive) == 0)
                m_metadata.series = content.simplified();
            else if (name.compare(QLatin1String("calibre:series_index"), Qt::CaseInsensitive) == 0)
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
            if ((!m_uniqueIdentifier.isEmpty() && identifier->attribute("id") == m_uniqueIdentifier) ||
                m_metadata.identifier.isEmpty())
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
            const QString rawHref = node->attribute("href").trimmed();
            if (rawHref.isEmpty())
                continue;
            const QString path = resolve(opfPath, rawHref);
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
        m_rightToLeft = spine->attribute("page-progression-direction").trimmed().compare(QLatin1String("rtl"), Qt::CaseInsensitive) == 0;
        for (const HtmlNode *ref : spine->childElements("itemref")) {
            const QString path = m_idToPath.value(ref->attribute("idref"));
            if (path.isEmpty() || !m_zip.contains(path))
                continue;
            EpubSpineItem item;
            item.id = ref->attribute("idref");
            item.path = path;
            item.mediaType = m_manifest.value(path).mediaType;
            item.linear = ref->attribute("linear").trimmed().compare(QLatin1String("no"), Qt::CaseInsensitive) != 0;
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

    // Covers, in order of how deliberately the book marks them. SVG cover
    // wrappers often just embed a raster image via <image xlink:href=...>;
    // unwrap them so cover decoders don't miss the nested image inside the ZIP.
    auto isImage = [](const EpubItem &item) { return item.mediaType.startsWith(QLatin1String("image/")); };
    auto isRasterImage = [](const QString &type) {
        return type.startsWith(QLatin1String("image/")) && type != QLatin1String("image/svg+xml");
    };
    auto resolveCover = [&](const QString &path) -> QString {
        if (path.isEmpty())
            return {};
        const QString type = mediaType(path);
        if (isRasterImage(type))
            return path;
        const QString nested = firstImageIn(path);
        if (!nested.isEmpty())
            return nested;
        return type == QLatin1String("image/svg+xml") ? path : QString();
    };
    for (const auto &item : std::as_const(manifestOrder))
        if (tokens(item.properties).contains("cover-image") && isImage(item)) {
            m_coverPath = resolveCover(item.path);
            if (!m_coverPath.isEmpty())
                return true;
        }
    if (!coverId.isEmpty()) {
        QString path = m_idToPath.value(coverId);
        if (path.isEmpty())  // Some tools write the file name instead of the id.
            for (const auto &item : std::as_const(manifestOrder))
                if (item.path.compare(coverId, Qt::CaseInsensitive) == 0 ||
                    item.path.endsWith(QLatin1Char('/') + coverId, Qt::CaseInsensitive)) {
                    path = item.path;
                    break;
                }
        if (!path.isEmpty()) {
            m_coverPath = resolveCover(path);
            if (!m_coverPath.isEmpty())
                return true;
        }
    }
    if (!guideCover.isEmpty()) {
        m_coverPath = resolveCover(guideCover);
        if (!m_coverPath.isEmpty())
            return true;
    }
    for (const auto &item : std::as_const(manifestOrder))
        if (isImage(item) && (item.id.contains("cover", Qt::CaseInsensitive) ||
                              QFileInfo(item.path).fileName().contains("cover", Qt::CaseInsensitive))) {
            m_coverPath = resolveCover(item.path);
            if (!m_coverPath.isEmpty())
                return true;
        }
    if (!m_spine.isEmpty())
        m_coverPath = resolveCover(m_spine.first().path);
    return true;
}

void EpubBook::readNavigation(const QString &navPath) {
    // EPUB 3 navigation documents are HTML5; parse with HTML void elements so
    // unclosed <br>, <img> or <hr> don't swallow following <li> entries.
    const auto doc = Html::parse(m_zip.read(navPath), true);
    // A list item's label is its own link or span, never one of a nested list.
    auto label = [](const HtmlNode *root) -> const HtmlNode * {
        std::vector<const HtmlNode *> stack;
        const QList<const HtmlNode *> top = root->childElements();
        for (qsizetype i = top.size() - 1; i >= 0; --i)
            stack.push_back(top[i]);
        while (!stack.empty()) {
            const HtmlNode *child = stack.back();
            stack.pop_back();
            if (child->is("ol") || child->is("ul"))
                continue;
            if (child->is("a") || child->is("span"))
                return child;
            const QList<const HtmlNode *> kids = child->childElements();
            for (qsizetype i = kids.size() - 1; i >= 0; --i)
                stack.push_back(kids[i]);
        }
        return nullptr;
    };
    auto walk = [&](const HtmlNode *rootList) {
        std::vector<QPair<const HtmlNode *, int>> stack;
        const QList<const HtmlNode *> items = rootList->childElements("li");
        for (qsizetype i = items.size() - 1; i >= 0; --i)
            stack.push_back({items[i], 0});
        while (!stack.empty()) {
            const auto [li, depth] = stack.back();
            stack.pop_back();
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
            const QList<const HtmlNode *> nestedLists = li->childElements();
            for (qsizetype i = nestedLists.size() - 1; i >= 0; --i) {
                const HtmlNode *nested = nestedLists[i];
                if (!nested->is("ol") && !nested->is("ul"))
                    continue;
                const QList<const HtmlNode *> subItems = nested->childElements("li");
                for (qsizetype j = subItems.size() - 1; j >= 0; --j)
                    stack.push_back({subItems[j], depth + 1});
            }
        }
    };
    for (const HtmlNode *nav : doc->elements("nav")) {
        const QStringList typeTokens =
            tokens((nav->attribute("epub:type") + QLatin1Char(' ') + nav->attribute("type") + QLatin1Char(' ') +
                    nav->attribute("role"))
                       .toLower());
        const HtmlNode *list = nav->first("ol");
        if (!list)
            list = nav->first("ul");
        if (!list)
            continue;
        if ((typeTokens.contains("toc") || typeTokens.contains("doc-toc")) && m_toc.isEmpty()) {
            walk(list);
        } else if (typeTokens.contains("page-list") || typeTokens.contains("doc-pagelist")) {
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
            std::vector<QPair<const HtmlNode *, int>> stack;
            const QList<const HtmlNode *> top = map->childElements("navpoint");
            for (qsizetype i = top.size() - 1; i >= 0; --i)
                stack.push_back({top[i], 0});
            while (!stack.empty()) {
                const auto [point, depth] = stack.back();
                stack.pop_back();
                EpubTocEntry entry;
                entry.title = labelOf(point);
                entry.path = targetOf(point, &entry.fragment);
                entry.depth = depth;
                if (!entry.title.isEmpty())
                    m_toc.append(entry);
                const QList<const HtmlNode *> children = point->childElements("navpoint");
                for (qsizetype i = children.size() - 1; i >= 0; --i)
                    stack.push_back({children[i], depth + 1});
            }
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
    const QString type = m_spine[index].mediaType.isEmpty() ? mediaType(m_spine[index].path) : m_spine[index].mediaType;
    if ((type.startsWith(QLatin1String("image/")) && type != QLatin1String("image/svg+xml")) ||
        type.startsWith(QLatin1String("audio/")) || type.startsWith(QLatin1String("video/")) ||
        type == QLatin1String("application/octet-stream"))
        return {};
    return extractText(m_zip.read(m_spine[index].path));
}

QString EpubBook::extractText(const QByteArray &document) {
    const QString source = HtmlTokenizer::decodeDocument(document);
    // Single-pass extraction: track both text inside <body> and fallback text
    // outside <head> (for documents with no real <body> element), avoiding
    // false matches on "<body" inside comments or custom tags like <bodydata>.
    QString insideBody, outsideHead;
    insideBody.reserve(source.size() / 2);
    HtmlTokenizer tokens(source);
    bool sawBody = false, inBody = false, head = false;
    int skip = 0;
    for (HtmlToken token = tokens.next(); token.type != HtmlToken::End; token = tokens.next()) {
        if (token.type == HtmlToken::Text) {
            if (!head && skip == 0) {
                if (sawBody) {
                    if (inBody)
                        insideBody += token.text;
                } else {
                    outsideHead += token.text;
                }
            }
            continue;
        }
        const QString local = token.localName();
        const bool start = token.type == HtmlToken::StartTag;
        if (local == "body") {
            if (start) {
                if (!sawBody) {
                    sawBody = true;
                    outsideHead.clear();
                }
                inBody = !token.selfClosing;
            } else {
                inBody = false;
            }
        } else if (local == "head") {
            head = start && !token.selfClosing;
        } else if ((local == "script" || local == "style" || local == "template") && !token.selfClosing) {
            skip = qMax(0, skip + (start ? 1 : -1));
        }
    }
    QString &out = sawBody ? insideBody : outsideHead;
    out.remove(QChar(0x00AD));
    return out;
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
    if (scheme.match(target).hasMatch() || target.startsWith(QLatin1String("//")))
        return {};
    const qsizetype query = target.indexOf('?');
    if (query >= 0)
        target.truncate(query);
    target = QUrl::fromPercentEncoding(target.toUtf8());
    target.replace(QLatin1Char('\\'), QLatin1Char('/'));
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
