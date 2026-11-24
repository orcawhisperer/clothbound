#include "online/epubwriter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>
#include <limits>
#include <zlib.h>

namespace {
struct ZipEntryData {
    QString name;
    QByteArray data;
    bool deflate = true;
};

void put16(QByteArray &out, quint16 v) {
    out.append(char(v & 0xff));
    out.append(char((v >> 8) & 0xff));
}

void put32(QByteArray &out, quint32 v) {
    put16(out, quint16(v & 0xffff));
    put16(out, quint16((v >> 16) & 0xffff));
}

bool rawDeflate(const QByteArray &data, QByteArray *compressed) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;

    const uLong bound = deflateBound(&stream, uLong(data.size()));
    QByteArray out(qsizetype(bound) + 16, Qt::Uninitialized);
    stream.next_in = data.isEmpty() ? nullptr : reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());

    const int rc = deflate(&stream, Z_FINISH);
    const qsizetype produced = qsizetype(stream.total_out);
    deflateEnd(&stream);
    if (rc != Z_STREAM_END)
        return false;
    out.truncate(produced);
    *compressed = std::move(out);
    return true;
}

quint32 computeCrc32(const QByteArray &data) {
    uLong crc = crc32(0L, Z_NULL, 0);
    if (!data.isEmpty())
        crc = crc32(crc, reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size()));
    return quint32(crc);
}

void dosDateTime(const QDateTime &utc, quint16 *dosTime, quint16 *dosDate) {
    const QDate d = utc.date();
    const QTime t = utc.time();
    const int year = qBound(1980, d.year(), 2107);
    *dosTime = quint16(((t.hour() & 0x1f) << 11) | ((t.minute() & 0x3f) << 5) | ((t.second() / 2) & 0x1f));
    *dosDate = quint16((((year - 1980) & 0x7f) << 9) | ((d.month() & 0x0f) << 5) | (d.day() & 0x1f));
}

bool buildZip(const QList<ZipEntryData> &entries, QByteArray *archiveOut, QString *error) {
    QByteArray out;
    QByteArray central;
    quint16 dosTime = 0, dosDate = 0;
    dosDateTime(QDateTime::currentDateTimeUtc(), &dosTime, &dosDate);

    for (qsizetype i = 0; i < entries.size(); ++i) {
        const ZipEntryData &entry = entries[i];
        const QByteArray name = entry.name.toUtf8();
        if (name.isEmpty() || name.size() > 65535) {
            if (error)
                *error = QStringLiteral("Invalid ZIP entry path.");
            return false;
        }
        if (quint64(entry.data.size()) > std::numeric_limits<quint32>::max()) {
            if (error)
                *error = QStringLiteral("ZIP entry exceeds 4 GiB limit.");
            return false;
        }

        const bool isMimetype = (i == 0 && entry.name == QLatin1String("mimetype"));
        bool useDeflate = !isMimetype && entry.deflate;
        QByteArray payload;
        if (useDeflate) {
            if (!rawDeflate(entry.data, &payload)) {
                if (error)
                    *error = QStringLiteral("Failed to compress %1.").arg(entry.name);
                return false;
            }
        } else {
            payload = entry.data;
        }

        const quint32 crc = computeCrc32(entry.data);
        const quint64 localOffset = quint64(out.size());
        if (localOffset > std::numeric_limits<quint32>::max() ||
            quint64(payload.size()) > std::numeric_limits<quint32>::max()) {
            if (error)
                *error = QStringLiteral("Archive exceeds standard ZIP size limit.");
            return false;
        }

        // EPUB OCF 3.3 requires `mimetype` first, stored (method 0), 0 extra bytes,
        // no encryption and no data descriptor.
        const quint16 flags = isMimetype ? 0x0000 : 0x0800;  // UTF-8 filename flag for non-mimetype entries
        const quint16 method = useDeflate ? 8 : 0;
        const quint16 versionNeeded = useDeflate ? 20 : 10;

        // Local file header (30 bytes + name)
        put32(out, 0x04034b50u);
        put16(out, versionNeeded);
        put16(out, flags);
        put16(out, method);
        put16(out, dosTime);
        put16(out, dosDate);
        put32(out, crc);
        put32(out, quint32(payload.size()));
        put32(out, quint32(entry.data.size()));
        put16(out, quint16(name.size()));
        put16(out, 0);  // extra field length MUST be 0
        out.append(name);
        out.append(payload);

        // Central directory header (46 bytes + name)
        put32(central, 0x02014b50u);
        put16(central, 20);  // version made by
        put16(central, versionNeeded);
        put16(central, flags);
        put16(central, method);
        put16(central, dosTime);
        put16(central, dosDate);
        put32(central, crc);
        put32(central, quint32(payload.size()));
        put32(central, quint32(entry.data.size()));
        put16(central, quint16(name.size()));
        put16(central, 0);  // extra length
        put16(central, 0);  // comment length
        put16(central, 0);  // disk number start
        put16(central, 0);  // internal attrs
        put32(central, 0);  // external attrs
        put32(central, quint32(localOffset));
        central.append(name);
    }

    const quint64 centralOffset = quint64(out.size());
    if (centralOffset > std::numeric_limits<quint32>::max() ||
        quint64(central.size()) > std::numeric_limits<quint32>::max() || entries.size() > 65535) {
        if (error)
            *error = QStringLiteral("Archive directory exceeds standard ZIP limit.");
        return false;
    }

    out.append(central);

    // End of central directory record (22 bytes)
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, quint16(entries.size()));
    put16(out, quint16(entries.size()));
    put32(out, quint32(central.size()));
    put32(out, quint32(centralOffset));
    put16(out, 0);

    *archiveOut = std::move(out);
    return true;
}

QString sanitizeRelativePath(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    QStringList safe;
    for (const QString &seg : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (seg == QLatin1String(".") || seg == QLatin1String(".."))
            continue;
        safe.append(seg);
    }
    return safe.join(QLatin1Char('/'));
}

QStringList wrapTextLines(const QString &text, int maxCharsPerLine, int maxLines) {
    const QString cleaned = text.simplified();
    if (cleaned.isEmpty())
        return {};
    const QStringList words = cleaned.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList lines;
    QString current;
    for (qsizetype i = 0; i < words.size(); ++i) {
        const QString &word = words[i];
        if (current.isEmpty()) {
            current = word.size() > maxCharsPerLine ? word.left(maxCharsPerLine - 1) + QChar(0x2026) : word;
        } else if (current.size() + 1 + word.size() <= maxCharsPerLine) {
            current += QLatin1Char(' ') + word;
        } else {
            if (lines.size() + 1 == maxLines) {
                if (current.size() + 1 <= maxCharsPerLine)
                    current += QChar(0x2026);
                else
                    current = current.left(maxCharsPerLine - 1) + QChar(0x2026);
                lines.append(current);
                return lines;
            }
            lines.append(current);
            current = word.size() > maxCharsPerLine ? word.left(maxCharsPerLine - 1) + QChar(0x2026) : word;
        }
    }
    if (!current.isEmpty())
        lines.append(current);
    return lines;
}

QString displayHost(const QString &urlText) {
    const QUrl u(urlText);
    QString host = u.host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host = host.mid(4);
    return host;
}

QString buildArticleCss() {
    return QStringLiteral(
        "html { box-sizing: border-box; }\n"
        "body { margin: 0; padding: 0; line-height: 1.65; }\n"
        "article.clothbound-article { max-width: 38em; margin: 0 auto; }\n"
        "header.article-header { margin: 0 0 1.6em 0; }\n"
        "p.article-kicker { margin: 0 0 0.45em 0; font-size: 0.78em; letter-spacing: 0.1em; text-transform: uppercase; opacity: 0.7; text-indent: 0; }\n"
        "h1.article-title { margin: 0 0 0.45em 0; font-size: 1.75em; line-height: 1.18; font-weight: 600; }\n"
        "p.article-meta { margin: 0; font-size: 0.85em; opacity: 0.72; text-indent: 0; }\n"
        "hr.article-rule { border: 0; border-top: 1px solid currentColor; opacity: 0.2; margin: 1.25em 0 1.6em 0; }\n"
        "section.article-body p { margin: 0 0 1em 0; text-indent: 0; }\n"
        "section.article-body h1, section.article-body h2, section.article-body h3, section.article-body h4 { margin: 1.6em 0 0.55em 0; line-height: 1.25; }\n"
        "section.article-body h2 { font-size: 1.32em; }\n"
        "section.article-body h3 { font-size: 1.14em; }\n"
        "blockquote { margin: 1.2em 0; padding: 0.1em 0 0.1em 1.1em; border-left: 2px solid currentColor; opacity: 0.88; font-style: italic; }\n"
        "pre { margin: 1.1em 0; padding: 0.85em 1em; overflow-x: auto; border: 1px solid currentColor; border-radius: 4px; font-size: 0.86em; line-height: 1.5; white-space: pre-wrap; word-break: break-word; }\n"
        "code { font-family: ui-monospace, 'SFMono-Regular', Menlo, Consolas, monospace; font-size: 0.9em; }\n"
        "pre code { font-size: inherit; }\n"
        "figure { margin: 1.4em 0; text-align: center; }\n"
        "figcaption { margin-top: 0.5em; font-size: 0.84em; opacity: 0.72; }\n"
        "img { max-width: 100%; height: auto; display: block; margin: 0.8em auto; }\n"
        "ul, ol { margin: 0.8em 0 1.1em 0; padding-left: 1.5em; }\n"
        "li { margin: 0.3em 0; }\n"
        "table { width: 100%; border-collapse: collapse; margin: 1.2em 0; font-size: 0.92em; }\n"
        "th, td { padding: 0.45em 0.6em; border-bottom: 1px solid currentColor; text-align: left; vertical-align: top; }\n"
        "th { font-weight: 600; }\n"
        "hr { border: 0; border-top: 1px solid currentColor; opacity: 0.22; margin: 1.5em 0; }\n"
        "footer.article-source { margin-top: 2.2em; padding-top: 0.9em; border-top: 1px solid currentColor; opacity: 0.72; font-size: 0.82em; }\n"
        "footer.article-source p { margin: 0; text-indent: 0; word-break: break-all; }\n");
}
}  // namespace

QString EpubWriter::xmlEscape(QStringView text) {
    QString out;
    out.reserve(text.size() + 16);
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        // Strip XML 1.0 illegal control characters.
        if (u < 0x20 && u != 0x09 && u != 0x0A && u != 0x0D)
            continue;
        switch (u) {
        case '&': out.append(QLatin1String("&amp;")); break;
        case '<': out.append(QLatin1String("&lt;")); break;
        case '>': out.append(QLatin1String("&gt;")); break;
        case '"': out.append(QLatin1String("&quot;")); break;
        case '\'': out.append(QLatin1String("&#39;")); break;
        default: out.append(ch); break;
        }
    }
    return out;
}

QByteArray EpubWriter::buildCoverSvg(const QString &title, const QString &author, const QString &siteName) {
    struct ClothPair {
        const char *bg;
        const char *fg;
        const char *accent;
    };
    static constexpr ClothPair palette[] = {
        {"#1F3550", "#E9D8A6", "#C8A96A"},
        {"#6B2E2A", "#F1E4D0", "#D9A87E"},
        {"#2F4A3A", "#E6DCC3", "#A7C4A0"},
        {"#3D3350", "#E7DCC8", "#B9A5D6"},
        {"#22403F", "#E3D6B8", "#8AB8B2"},
        {"#50443A", "#EADFCB", "#C9AC88"},
        {"#2B2B36", "#D9C7A0", "#9E97B8"},
        {"#5B6B2E", "#F0E9CF", "#C4CF8A"},
        {"#7A3F55", "#F3E2E6", "#D99BB2"},
        {"#CDBE9A", "#2A251D", "#6E5A3C"},
        {"#B9C4C9", "#1E2A30", "#4A6572"},
        {"#8A5A2B", "#F7EBD6", "#E2BA82"},
    };

    const QByteArray digest = QCryptographicHash::hash((title + QLatin1Char('|') + siteName).toUtf8(), QCryptographicHash::Sha256);
    const quint32 pick = (quint8(digest[0]) | (quint32(quint8(digest[1])) << 8)) %
                         quint32(sizeof(palette) / sizeof(palette[0]));
    const ClothPair &cloth = palette[pick];

    const QString safeTitle = title.simplified().isEmpty() ? QStringLiteral("Saved Article") : title.simplified();
    const QString kicker = (siteName.simplified().isEmpty() ? QStringLiteral("WEB ARTICLE") : siteName.simplified()).toUpper();
    const QString safeAuthor = author.simplified().toUpper();

    const QStringList titleLines = wrapTextLines(safeTitle, 20, 5);
    const QStringList authorLines = wrapTextLines(safeAuthor, 28, 2);
    const QString trimmedKicker = kicker.size() > 30 ? kicker.left(29) + QChar(0x2026) : kicker;

    const int fontSize = titleLines.size() <= 2 ? 48 : (titleLines.size() <= 4 ? 42 : 36);
    const int lineStep = int(fontSize * 1.24);
    const int titleStartY = 230;
    const int ruleY = titleStartY + int(titleLines.size()) * lineStep + 26;

    QString svg;
    svg.reserve(2048);
    svg += QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"600\" height=\"900\" viewBox=\"0 0 600 900\">\n"
        "  <rect width=\"600\" height=\"900\" fill=\"%1\"/>\n"
        "  <rect x=\"0\" y=\"0\" width=\"24\" height=\"900\" fill=\"#000000\" fill-opacity=\"0.16\"/>\n"
        "  <rect x=\"24\" y=\"0\" width=\"2\" height=\"900\" fill=\"#FFFFFF\" fill-opacity=\"0.08\"/>\n"
        "  <rect x=\"48\" y=\"48\" width=\"504\" height=\"804\" fill=\"none\" stroke=\"%2\" stroke-opacity=\"0.22\" stroke-width=\"1.5\"/>\n"
        "  <text x=\"82\" y=\"122\" fill=\"%3\" font-family=\"Inter, Helvetica, Arial, sans-serif\" font-size=\"17\" font-weight=\"600\" letter-spacing=\"3\">%4</text>\n")
               .arg(QLatin1String(cloth.bg), QLatin1String(cloth.fg), QLatin1String(cloth.accent),
                    xmlEscape(trimmedKicker));

    for (qsizetype i = 0; i < titleLines.size(); ++i) {
        const int y = titleStartY + int(i) * lineStep;
        svg += QStringLiteral(
                   "  <text x=\"82\" y=\"%1\" fill=\"%2\" font-family=\"Literata, Georgia, 'Times New Roman', serif\" font-size=\"%3\" font-weight=\"600\">%4</text>\n")
                   .arg(y)
                   .arg(QLatin1String(cloth.fg))
                   .arg(fontSize)
                   .arg(xmlEscape(titleLines[i]));
    }

    svg += QStringLiteral("  <rect x=\"82\" y=\"%1\" width=\"116\" height=\"3\" fill=\"%2\" fill-opacity=\"0.78\"/>\n")
               .arg(qMin(ruleY, 690))
               .arg(QLatin1String(cloth.accent));

    if (!authorLines.isEmpty()) {
        const int baseAuthorY = 785 - int(authorLines.size() - 1) * 28;
        for (qsizetype i = 0; i < authorLines.size(); ++i) {
            svg += QStringLiteral(
                       "  <text x=\"82\" y=\"%1\" fill=\"%2\" fill-opacity=\"0.88\" font-family=\"Inter, Helvetica, Arial, sans-serif\" font-size=\"19\" font-weight=\"500\" letter-spacing=\"2.4\">%3</text>\n")
                       .arg(baseAuthorY + int(i) * 28)
                       .arg(QLatin1String(cloth.fg), xmlEscape(authorLines[i]));
        }
    }

    svg += QStringLiteral(
               "  <text x=\"518\" y=\"816\" text-anchor=\"end\" fill=\"%1\" fill-opacity=\"0.42\" font-family=\"Inter, Helvetica, Arial, sans-serif\" font-size=\"13\" letter-spacing=\"2\">CLOTHBOUND</text>\n"
               "</svg>\n")
               .arg(QLatin1String(cloth.fg));

    return svg.toUtf8();
}

bool EpubWriter::writeArticle(const QString &outputPath, const EpubArticle &article, QString *error) {
    if (error)
        error->clear();
    if (outputPath.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Output path is empty.");
        return false;
    }

    const QFileInfo outInfo(outputPath);
    if (!QDir().mkpath(outInfo.absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create directory %1.").arg(outInfo.absolutePath());
        return false;
    }

    const QString title = article.title.simplified().isEmpty() ? QStringLiteral("Untitled Article") : article.title.simplified();
    const QString author = article.author.simplified();
    const QString publisher = article.publisher.simplified().isEmpty() ? displayHost(article.sourceUrl) : article.publisher.simplified();
    const QString language = article.language.simplified().isEmpty() ? QStringLiteral("en") : article.language.simplified();
    const QString date = article.publishedDate.simplified();
    const QString sourceUrl = article.sourceUrl.trimmed();
    const QString description = article.description.simplified();
    const QString identifier = article.identifier.trimmed().isEmpty()
                                   ? QStringLiteral("urn:uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces)
                                   : article.identifier.trimmed();
    const QString modifiedUtc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));

    QStringList subjects;
    auto addSubject = [&subjects](const QString &raw) {
        const QString cleaned = raw.simplified();
        if (cleaned.isEmpty())
            return;
        for (const QString &existing : std::as_const(subjects))
            if (existing.compare(cleaned, Qt::CaseInsensitive) == 0)
                return;
        subjects.append(cleaned);
    };
    addSubject(QStringLiteral("Clothbound Article"));
    for (const QString &s : article.subjects)
        addSubject(s);

    // 1. META-INF/container.xml
    const QByteArray containerXml = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "  <rootfiles>\n"
        "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
        "  </rootfiles>\n"
        "</container>\n");

    // Sanitize image assets and deduplicate archive paths.
    QList<EpubImageAsset> cleanImages;
    QStringList seenImagePaths;
    for (const EpubImageAsset &img : article.images) {
        const QString rel = sanitizeRelativePath(img.path);
        if (rel.isEmpty() || img.data.isEmpty() || seenImagePaths.contains(rel))
            continue;
        seenImagePaths.append(rel);
        EpubImageAsset copy = img;
        copy.path = rel;
        if (copy.mediaType.trimmed().isEmpty())
            copy.mediaType = QStringLiteral("image/jpeg");
        cleanImages.append(std::move(copy));
    }

    // 2. OEBPS/content.opf
    QString opf;
    opf.reserve(2048);
    opf += QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"pub-id\" xml:lang=\"%1\">\n"
               "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
               "    <dc:identifier id=\"pub-id\">%2</dc:identifier>\n"
               "    <dc:title>%3</dc:title>\n")
               .arg(xmlEscape(language), xmlEscape(identifier), xmlEscape(title));
    if (!author.isEmpty()) {
        opf += QStringLiteral(
                   "    <dc:creator id=\"creator\">%1</dc:creator>\n"
                   "    <meta refines=\"#creator\" property=\"role\" scheme=\"marc:relators\">aut</meta>\n")
                   .arg(xmlEscape(author));
    }
    if (!publisher.isEmpty())
        opf += QStringLiteral("    <dc:publisher>%1</dc:publisher>\n").arg(xmlEscape(publisher));
    if (!date.isEmpty())
        opf += QStringLiteral("    <dc:date>%1</dc:date>\n").arg(xmlEscape(date));
    opf += QStringLiteral("    <dc:language>%1</dc:language>\n").arg(xmlEscape(language));
    if (!sourceUrl.isEmpty())
        opf += QStringLiteral("    <dc:source>%1</dc:source>\n").arg(xmlEscape(sourceUrl));
    if (!description.isEmpty())
        opf += QStringLiteral("    <dc:description>%1</dc:description>\n").arg(xmlEscape(description));
    for (const QString &subject : std::as_const(subjects))
        opf += QStringLiteral("    <dc:subject>%1</dc:subject>\n").arg(xmlEscape(subject));
    opf += QStringLiteral(
               "    <meta property=\"dcterms:modified\">%1</meta>\n"
               "    <meta name=\"cover\" content=\"cover-image\"/>\n"
               "  </metadata>\n"
               "  <manifest>\n"
               "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
               "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
               "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n"
               "    <item id=\"cover-image\" href=\"cover.svg\" media-type=\"image/svg+xml\" properties=\"cover-image\"/>\n"
               "    <item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/>\n")
               .arg(xmlEscape(modifiedUtc));

    for (qsizetype i = 0; i < cleanImages.size(); ++i) {
        const EpubImageAsset &img = cleanImages[i];
        opf += QStringLiteral("    <item id=\"img-%1\" href=\"%2\" media-type=\"%3\"/>\n")
                   .arg(i + 1)
                   .arg(xmlEscape(img.path), xmlEscape(img.mediaType));
    }

    opf += QStringLiteral(
        "  </manifest>\n"
        "  <spine toc=\"ncx\">\n"
        "    <itemref idref=\"article\"/>\n"
        "  </spine>\n"
        "</package>\n");

    // 3. OEBPS/nav.xhtml (EPUB 3 Navigation Document)
    QList<EpubHeading> validHeadings;
    for (const EpubHeading &h : article.headings) {
        if (!h.id.trimmed().isEmpty() && !h.title.simplified().isEmpty())
            validHeadings.append({h.id.trimmed(), h.title.simplified(), qBound(1, h.level, 6)});
    }

    QString nav;
    nav += QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE html>\n"
               "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"%1\" lang=\"%1\">\n"
               "<head>\n"
               "  <meta charset=\"utf-8\"/>\n"
               "  <title>Contents</title>\n"
               "  <link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
               "</head>\n"
               "<body>\n"
               "  <nav epub:type=\"toc\" id=\"toc\">\n"
               "    <h1>Contents</h1>\n"
               "    <ol>\n"
               "      <li>\n"
               "        <a href=\"article.xhtml\">%2</a>\n")
               .arg(xmlEscape(language), xmlEscape(title));
    if (!validHeadings.isEmpty()) {
        nav += QStringLiteral("        <ol>\n");
        for (const EpubHeading &h : std::as_const(validHeadings)) {
            nav += QStringLiteral("          <li><a href=\"article.xhtml#%1\">%2</a></li>\n")
                       .arg(xmlEscape(h.id), xmlEscape(h.title));
        }
        nav += QStringLiteral("        </ol>\n");
    }
    nav += QStringLiteral(
        "      </li>\n"
        "    </ol>\n"
        "  </nav>\n"
        "</body>\n"
        "</html>\n");

    // 4. OEBPS/toc.ncx (EPUB 2 compatibility)
    QString ncx;
    ncx += QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
               "  <head>\n"
               "    <meta name=\"dtb:uid\" content=\"%1\"/>\n"
               "    <meta name=\"dtb:depth\" content=\"%2\"/>\n"
               "    <meta name=\"dtb:totalPageCount\" content=\"0\"/>\n"
               "    <meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n"
               "  </head>\n"
               "  <docTitle><text>%3</text></docTitle>\n"
               "  <navMap>\n"
               "    <navPoint id=\"navPoint-1\" playOrder=\"1\">\n"
               "      <navLabel><text>%3</text></navLabel>\n"
               "      <content src=\"article.xhtml\"/>\n")
               .arg(xmlEscape(identifier), validHeadings.isEmpty() ? QStringLiteral("1") : QStringLiteral("2"),
                    xmlEscape(title));
    int playOrder = 2;
    for (const EpubHeading &h : std::as_const(validHeadings)) {
        ncx += QStringLiteral(
                   "      <navPoint id=\"navPoint-%1\" playOrder=\"%1\">\n"
                   "        <navLabel><text>%2</text></navLabel>\n"
                   "        <content src=\"article.xhtml#%3\"/>\n"
                   "      </navPoint>\n")
                   .arg(playOrder)
                   .arg(xmlEscape(h.title), xmlEscape(h.id));
        ++playOrder;
    }
    ncx += QStringLiteral(
        "    </navPoint>\n"
        "  </navMap>\n"
        "</ncx>\n");

    // 5. OEBPS/article.xhtml
    QStringList metaParts;
    if (!author.isEmpty())
        metaParts.append(author);
    if (!date.isEmpty())
        metaParts.append(date.left(10));
    if (article.readingMinutes > 0)
        metaParts.append(QStringLiteral("%1 min read").arg(article.readingMinutes));

    QString articleXhtml;
    articleXhtml.reserve(article.bodyXhtml.size() + 1024);
    articleXhtml += QStringLiteral(
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<!DOCTYPE html>\n"
                        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"%1\" lang=\"%1\">\n"
                        "<head>\n"
                        "  <meta charset=\"utf-8\"/>\n"
                        "  <title>%2</title>\n"
                        "  <link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
                        "</head>\n"
                        "<body>\n"
                        "  <article class=\"clothbound-article\">\n"
                        "    <header class=\"article-header\">\n")
                        .arg(xmlEscape(language), xmlEscape(title));
    if (!publisher.isEmpty())
        articleXhtml += QStringLiteral("      <p class=\"article-kicker\">%1</p>\n").arg(xmlEscape(publisher));
    articleXhtml += QStringLiteral("      <h1 class=\"article-title\">%1</h1>\n").arg(xmlEscape(title));
    if (!metaParts.isEmpty()) {
        articleXhtml += QStringLiteral("      <p class=\"article-meta\">%1</p>\n")
                            .arg(xmlEscape(metaParts.join(QStringLiteral(" \u00B7 "))));
    }
    articleXhtml += QStringLiteral(
        "    </header>\n"
        "    <hr class=\"article-rule\"/>\n"
        "    <section class=\"article-body\">\n");
    articleXhtml += article.bodyXhtml;
    articleXhtml += QStringLiteral("\n    </section>\n");
    if (!sourceUrl.isEmpty()) {
        articleXhtml += QStringLiteral(
                            "    <footer class=\"article-source\">\n"
                            "      <p>Saved from <a href=\"%1\">%2</a></p>\n"
                            "    </footer>\n")
                            .arg(xmlEscape(sourceUrl), xmlEscape(sourceUrl));
    }
    articleXhtml += QStringLiteral(
        "  </article>\n"
        "</body>\n"
        "</html>\n");

    // 6. OEBPS/cover.svg
    const QByteArray coverSvg = buildCoverSvg(title, author, publisher);

    QList<ZipEntryData> entries;
    entries.reserve(8 + cleanImages.size());
    // mimetype MUST be first and stored uncompressed (method 0).
    entries.append({QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"), false});
    entries.append({QStringLiteral("META-INF/container.xml"), containerXml, true});
    entries.append({QStringLiteral("OEBPS/content.opf"), opf.toUtf8(), true});
    entries.append({QStringLiteral("OEBPS/nav.xhtml"), nav.toUtf8(), true});
    entries.append({QStringLiteral("OEBPS/toc.ncx"), ncx.toUtf8(), true});
    entries.append({QStringLiteral("OEBPS/style.css"), buildArticleCss().toUtf8(), true});
    entries.append({QStringLiteral("OEBPS/cover.svg"), coverSvg, true});
    entries.append({QStringLiteral("OEBPS/article.xhtml"), articleXhtml.toUtf8(), true});

    for (const EpubImageAsset &img : std::as_const(cleanImages)) {
        const bool compressImg = img.mediaType == QLatin1String("image/svg+xml") || img.mediaType == QLatin1String("image/bmp");
        entries.append({QStringLiteral("OEBPS/") + img.path, img.data, compressImg});
    }

    QByteArray zipBytes;
    if (!buildZip(entries, &zipBytes, error))
        return false;

    QSaveFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not open %1 for writing: %2").arg(outputPath, outFile.errorString());
        return false;
    }
    if (outFile.write(zipBytes) != zipBytes.size()) {
        outFile.cancelWriting();
        if (error)
            *error = QStringLiteral("Failed writing %1: %2").arg(outputPath, outFile.errorString());
        return false;
    }
    if (!outFile.commit()) {
        if (error)
            *error = QStringLiteral("Failed committing %1: %2").arg(outputPath, outFile.errorString());
        return false;
    }
    return true;
}
