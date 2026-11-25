#include "online/webimporter.h"

#include "formats/bookname.h"
#include "formats/epubbook.h"
#include "library/library.h"
#include "online/articleextractor.h"
#include "online/epubwriter.h"
#include "online/urlguard.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <memory>

namespace {
constexpr int kRequestTimeoutMs = 30000;
constexpr int kMetadataTimeoutMs = 7000;
constexpr int kMaxConcurrentImages = 4;

QString decodeSimpleHtmlEntities(QString text) {
    if (!text.contains(QLatin1Char('&')))
        return text;
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#34;"), QLatin1String("\""));
    text.replace(QLatin1String("&apos;"), QLatin1String("'"));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

QString shortHashHex(const QString &input) {
    return QString::fromLatin1(QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
}

QString sanitizeFileStem(QString raw, int maxLen = 96) {
    raw = raw.simplified();
    static const QRegularExpression illegal(QStringLiteral(R"([/\\:*?"<>|\x00-\x1f]+)"));
    raw.replace(illegal, QStringLiteral(" "));
    raw = raw.simplified();
    while (raw.startsWith(QLatin1Char('.')))
        raw.remove(0, 1);
    while (raw.endsWith(QLatin1Char('.')) || raw.endsWith(QLatin1Char(' ')))
        raw.chop(1);
    if (raw.size() > maxLen)
        raw = raw.left(maxLen).trimmed();
    return raw;
}

QString slugify(const QString &raw, int maxLen = 64) {
    const QString norm = raw.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(qMin(int(norm.size()), maxLen));
    bool prevDash = true;
    for (const QChar c : norm) {
        if (c.unicode() < 128 && c.isLetterOrNumber()) {
            out.append(c.toLower());
            prevDash = false;
        } else if (!prevDash && (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_'))) {
            out.append(QLatin1Char('-'));
            prevDash = true;
        }
        if (out.size() >= maxLen)
            break;
    }
    while (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("article") : out;
}

QString parseContentDispositionFilename(const QString &header) {
    if (header.trimmed().isEmpty())
        return {};
    static const QRegularExpression extRe(QStringLiteral(R"(filename\*\s*=\s*(?:UTF-8|utf-8)''([^;\s]+))"));
    if (const auto m = extRe.match(header); m.hasMatch())
        return QUrl::fromPercentEncoding(m.captured(1).toUtf8()).trimmed();
    static const QRegularExpression quotedRe(QStringLiteral(R"re(filename\s*=\s*"([^"]+)")re"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (const auto m = quotedRe.match(header); m.hasMatch())
        return m.captured(1).trimmed();
    static const QRegularExpression bareRe(QStringLiteral(R"(filename\s*=\s*([^;\s]+))"),
                                           QRegularExpression::CaseInsensitiveOption);
    if (const auto m = bareRe.match(header); m.hasMatch())
        return m.captured(1).trimmed();
    return {};
}

bool looksLikePdf(const QByteArray &data) {
    const QByteArray head = data.left(32).trimmed();
    return head.startsWith("%PDF-");
}

bool looksLikeEpubBytes(const QByteArray &data) {
    if (data.size() < 40 || !data.startsWith("PK\x03\x04"))
        return false;
    const QByteArray probe = data.left(512);
    return probe.contains("mimetype") && probe.contains("application/epub+zip");
}

bool isBookContentType(const QString &contentType) {
    const QString ct = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    return ct == QLatin1String("application/pdf") || ct == QLatin1String("application/epub+zip") ||
           ct == QLatin1String("application/octet-stream");
}

QString detectImageFormat(const QString &contentType, const QByteArray &data, QString *extOut) {
    const QString ct = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (data.size() >= 3 && quint8(data[0]) == 0xFF && quint8(data[1]) == 0xD8 && quint8(data[2]) == 0xFF) {
        if (extOut)
            *extOut = QStringLiteral("jpg");
        return QStringLiteral("image/jpeg");
    }
    if (data.size() >= 8 && data.startsWith("\x89PNG\r\n\x1a\n")) {
        if (extOut)
            *extOut = QStringLiteral("png");
        return QStringLiteral("image/png");
    }
    if (data.size() >= 6 && (data.startsWith("GIF87a") || data.startsWith("GIF89a"))) {
        if (extOut)
            *extOut = QStringLiteral("gif");
        return QStringLiteral("image/gif");
    }
    if (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP") {
        if (extOut)
            *extOut = QStringLiteral("webp");
        return QStringLiteral("image/webp");
    }
    if (ct == QLatin1String("image/svg+xml")) {
        if (EpubWriter::isSafeSvg(data)) {
            if (extOut)
                *extOut = QStringLiteral("svg");
            return QStringLiteral("image/svg+xml");
        }
    }
    return {};
}

bool saveBytesAtomically(const QString &destPath, const QByteArray &bytes, QString *error) {
    const QFileInfo info(destPath);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create folder %1.").arg(info.absolutePath());
        return false;
    }
    QSaveFile file(destPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not write %1: %2").arg(destPath, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("Failed writing %1: %2").arg(destPath, file.errorString());
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Could not save %1: %2").arg(destPath, file.errorString());
        return false;
    }
    return true;
}

QByteArray headerValueFromList(const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &name) {
    for (const auto &[k, v] : headers) {
        if (k.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    }
    return {};
}
}  // namespace

WebImporter::WebImporter(Library *library, const QString &storageDir, QObject *parent)
    : QObject(parent),
      m_library(library),
      m_storageDir(storageDir.trimmed().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                                  : storageDir) {}

WebImporter::~WebImporter() {
    abortActiveOperations();
}

void WebImporter::setArxivEndpointsForTests(const QString &apiBaseUrl, const QString &pdfBaseUrl,
                                            const QString &absBaseUrl) {
    m_arxivApiBase = apiBaseUrl;
    m_arxivPdfBase = pdfBaseUrl;
    if (!absBaseUrl.isEmpty())
        m_arxivAbsBase = absBaseUrl;
}

QString WebImporter::downloadsDir() const { return QDir(m_storageDir).filePath(QStringLiteral("downloads")); }

QString WebImporter::articlesDir() const { return QDir(m_storageDir).filePath(QStringLiteral("articles")); }

QString WebImporter::parseArxivId(const QString &rawUrl) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Match modern arXiv IDs (YYMM.NNNN[N][vN]) and old-style (archive[.sub]/YYMMNNN[vN]).
    static const QRegularExpression idPattern(
        QStringLiteral(R"(^((?:\d{4}\.\d{4,5}|[a-z\-]+(?:\.[A-Z]{2})?/\d{7})(?:[vV]\d+)?)$)"));

    auto normalizeId = [](QString candidate) -> QString {
        candidate = candidate.trimmed();
        while (candidate.endsWith(QLatin1Char('/')))
            candidate.chop(1);
        if (candidate.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
            candidate.chop(4);
        const QRegularExpressionMatch m = idPattern.match(candidate);
        if (!m.hasMatch())
            return {};
        QString id = m.captured(1);
        const qsizetype vPos = id.lastIndexOf(QLatin1Char('V'));
        if (vPos > 0 && vPos + 1 < id.size() && id[vPos + 1].isDigit())
            id[vPos] = QLatin1Char('v');
        return id;
    };

    // 1. arxiv:2301.07041 / arXiv:hep-th/9901001
    static const QRegularExpression prefixRe(QStringLiteral(R"(^arxiv:\s*(.+)$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (const auto m = prefixRe.match(trimmed); m.hasMatch())
        return normalizeId(m.captured(1));

    // 2. arXiv and popular arXiv paper mirrors / viewers:
    //    - [www.|export.|browse.|ar5iv.labs.]arxiv.org/{abs,pdf,html,e-print,src}/<id>
    //    - [www.]alphaxiv.org/{abs,pdf,overview,html}/<id>
    //    - [www.]ar5iv.org/{abs,html,pdf}/<id>
    //    - [www.]arxiv-vanity.com/papers/<id>
    //    - [www.]huggingface.co/papers/<id>
    //    - papers.cool/arxiv/<id>
    const QUrl url(trimmed);
    if (!url.isValid())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    const QString host = url.host().toLower();
    const bool isArxivHost = (host == QLatin1String("arxiv.org") || host.endsWith(QLatin1String(".arxiv.org")));
    const bool isAlphaXiv = (host == QLatin1String("alphaxiv.org") || host.endsWith(QLatin1String(".alphaxiv.org")));
    const bool isAr5iv = (host == QLatin1String("ar5iv.org") || host.endsWith(QLatin1String(".ar5iv.org")));
    const bool isArxivVanity = (host == QLatin1String("arxiv-vanity.com") || host.endsWith(QLatin1String(".arxiv-vanity.com")));
    const bool isHuggingFace = (host == QLatin1String("huggingface.co") || host == QLatin1String("www.huggingface.co"));
    const bool isPapersCool = (host == QLatin1String("papers.cool") || host == QLatin1String("www.papers.cool"));

    if (isArxivHost || isAlphaXiv || isAr5iv) {
        static const QRegularExpression pathRe(
            QStringLiteral(R"(^/(?:abs|pdf|html|e-print|src|overview)/((?:\d{4}\.\d{4,5}|[a-z\-]+(?:\.[A-Z]{2})?/\d{7})(?:v\d+)?)(?:\.pdf)?(?:/.*)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = pathRe.match(url.path()); m.hasMatch())
            return normalizeId(m.captured(1));
        return {};
    }
    if (isArxivVanity || isHuggingFace) {
        static const QRegularExpression papersRe(
            QStringLiteral(R"(^/papers/((?:\d{4}\.\d{4,5})(?:v\d+)?)(?:\.pdf)?(?:/.*)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = papersRe.match(url.path()); m.hasMatch())
            return normalizeId(m.captured(1));
        return {};
    }
    if (isPapersCool) {
        static const QRegularExpression coolRe(
            QStringLiteral(R"(^/arxiv/((?:\d{4}\.\d{4,5}|[a-z\-]+(?:\.[A-Z]{2})?/\d{7})(?:v\d+)?)(?:\.pdf)?(?:/.*)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = coolRe.match(url.path()); m.hasMatch())
            return normalizeId(m.captured(1));
        return {};
    }
    return {};
}

QString WebImporter::detectUrlKind(const QString &rawUrl) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty())
        return {};

    if (!parseArxivId(trimmed).isEmpty())
        return QStringLiteral("arxiv");

    const QUrl url(trimmed);
    if (!url.isValid())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    const QString host = url.host().trimmed();
    if (host.isEmpty())
        return {};
    for (const QChar ch : host) {
        if (ch.isSpace() || ch.unicode() < 0x20 || ch == QLatin1Char('/') || ch == QLatin1Char('\\'))
            return {};
    }

    const QString path = url.path().toLower();
    if (path.endsWith(QLatin1String(".epub")) || path.endsWith(QLatin1String(".kepub")))
        return QStringLiteral("epub");
    if (path.endsWith(QLatin1String(".pdf")))
        return QStringLiteral("pdf");
    return QStringLiteral("article");
}

QVariantMap WebImporter::parseArxivAtom(const QByteArray &xml) {
    if (xml.trimmed().isEmpty())
        return {};

    QXmlStreamReader reader(xml);
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    reader.setEntityExpansionLimit(1);
#else
    reader.setEntityExpansionLimit(0);
#endif
    bool inEntry = false;
    bool inAuthor = false;
    QString title;
    QStringList authors;
    QString summary;
    QString published;
    QString entryId;
    QString pdfUrl;
    QStringList categories;

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD)
            return {};
        if (token == QXmlStreamReader::StartElement) {
            const QStringView name = reader.name();
            if (name == QLatin1String("entry")) {
                inEntry = true;
            } else if (inEntry) {
                if (name == QLatin1String("title")) {
                    title = reader.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                } else if (name == QLatin1String("summary")) {
                    summary = reader.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                } else if (name == QLatin1String("published")) {
                    published = reader.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                } else if (name == QLatin1String("id")) {
                    entryId = reader.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                } else if (name == QLatin1String("author")) {
                    inAuthor = true;
                } else if (inAuthor && name == QLatin1String("name")) {
                    const QString authorName = reader.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                    if (!authorName.isEmpty())
                        authors.append(authorName);
                } else if (name == QLatin1String("link")) {
                    const auto attrs = reader.attributes();
                    const QString linkTitle = attrs.value(QStringLiteral("title")).toString().toLower();
                    const QString linkType = attrs.value(QStringLiteral("type")).toString().toLower();
                    const QString href = attrs.value(QStringLiteral("href")).toString().trimmed();
                    if ((linkTitle == QLatin1String("pdf") || linkType == QLatin1String("application/pdf")) &&
                        !href.isEmpty() && pdfUrl.isEmpty()) {
                        pdfUrl = href;
                    }
                } else if (name == QLatin1String("category")) {
                    const QString term = reader.attributes().value(QStringLiteral("term")).toString().trimmed();
                    if (!term.isEmpty() && !categories.contains(term))
                        categories.append(term);
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView name = reader.name();
            if (name == QLatin1String("author"))
                inAuthor = false;
            else if (name == QLatin1String("entry"))
                break;  // Only need the primary entry.
        }
    }

    if (reader.hasError() || !inEntry)
        return {};
    if (title.compare(QLatin1String("Error"), Qt::CaseInsensitive) == 0 && authors.isEmpty())
        return {};

    int year = 0;
    static const QRegularExpression yearRe(QStringLiteral(R"(^(\d{4}))"));
    if (const auto m = yearRe.match(published); m.hasMatch())
        year = m.captured(1).toInt();

    QVariantMap out;
    out.insert(QStringLiteral("title"), title);
    out.insert(QStringLiteral("authors"), authors);
    out.insert(QStringLiteral("firstAuthor"), authors.isEmpty() ? QString() : authors.first());
    out.insert(QStringLiteral("abstract"), summary);
    out.insert(QStringLiteral("published"), published);
    out.insert(QStringLiteral("year"), year);
    out.insert(QStringLiteral("id"), entryId);
    out.insert(QStringLiteral("pdfUrl"), pdfUrl);
    out.insert(QStringLiteral("categories"), categories);
    return out;
}

QVariantMap WebImporter::parseCitationMetaHtml(const QByteArray &html) {
    if (html.trimmed().isEmpty())
        return {};
    // Only scan the document head / first 128 KB for Highwire/Google Scholar meta tags.
    const QString text = QString::fromUtf8(html.left(131072));
    static const QRegularExpression metaTagRe(QStringLiteral(R"(<meta\b([^>]+)/?>)"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrRe(
        QStringLiteral(R"re(\b([a-zA-Z_:][-a-zA-Z0-9_:.]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))re"));

    QString title;
    QStringList authors;
    QString abstractText;
    QString dateText;
    QString pdfUrl;
    QString arxivId;

    auto it = metaTagRe.globalMatch(text);
    while (it.hasNext()) {
        const QString attrsStr = it.next().captured(1);
        QString name;
        QString content;
        auto ait = attrRe.globalMatch(attrsStr);
        while (ait.hasNext()) {
            const auto am = ait.next();
            const QString key = am.captured(1).toLower();
            const QString val = !am.captured(2).isNull()   ? am.captured(2)
                                : !am.captured(3).isNull() ? am.captured(3)
                                                           : am.captured(4);
            if (key == QLatin1String("name") || key == QLatin1String("property"))
                name = val.trimmed().toLower();
            else if (key == QLatin1String("content"))
                content = decodeSimpleHtmlEntities(val).simplified();
        }
        if (name.isEmpty() || content.isEmpty())
            continue;
        if (name == QLatin1String("citation_title") && title.isEmpty()) {
            title = content;
        } else if (name == QLatin1String("citation_author")) {
            const QString cleanAuthor = BookName::displayAuthor(content);
            if (!cleanAuthor.isEmpty() && !authors.contains(cleanAuthor))
                authors.append(cleanAuthor);
        } else if (name == QLatin1String("citation_abstract") && abstractText.isEmpty()) {
            abstractText = content;
        } else if ((name == QLatin1String("citation_date") || name == QLatin1String("citation_publication_date") ||
                    name == QLatin1String("citation_online_date")) &&
                   dateText.isEmpty()) {
            dateText = content;
        } else if (name == QLatin1String("citation_pdf_url") && pdfUrl.isEmpty()) {
            pdfUrl = content;
        } else if (name == QLatin1String("citation_arxiv_id") && arxivId.isEmpty()) {
            arxivId = content;
        }
    }

    if (title.isEmpty() && pdfUrl.isEmpty() && arxivId.isEmpty())
        return {};

    int year = 0;
    static const QRegularExpression yearRe(QStringLiteral(R"(\b(19\d{2}|20\d{2})\b)"));
    if (const auto m = yearRe.match(dateText); m.hasMatch())
        year = m.captured(1).toInt();

    QVariantMap out;
    if (!title.isEmpty())
        out.insert(QStringLiteral("title"), title);
    if (!authors.isEmpty()) {
        out.insert(QStringLiteral("authors"), authors);
        out.insert(QStringLiteral("firstAuthor"), authors.first());
    }
    if (!abstractText.isEmpty())
        out.insert(QStringLiteral("abstract"), abstractText);
    if (!dateText.isEmpty())
        out.insert(QStringLiteral("published"), dateText);
    if (year > 0)
        out.insert(QStringLiteral("year"), year);
    if (!pdfUrl.isEmpty())
        out.insert(QStringLiteral("pdfUrl"), pdfUrl);
    if (!arxivId.isEmpty())
        out.insert(QStringLiteral("arxivId"), arxivId);
    return out;
}

QString WebImporter::clipboardUrl() const {
    auto *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!gui)
        return {};
    QClipboard *cb = gui->clipboard();
    if (!cb)
        return {};
    const QString candidate = cb->text().trimmed();
    if (candidate.isEmpty() || candidate.contains(QLatin1Char('\n')) || candidate.contains(QLatin1Char('\r')) ||
        candidate.size() > 2048)
        return {};
    return detectUrlKind(candidate).isEmpty() ? QString() : candidate;
}

void WebImporter::setBusy(bool busy) {
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void WebImporter::setStatus(const QString &status) {
    if (m_statusText == status)
        return;
    m_statusText = status;
    emit statusChanged();
}

void WebImporter::setProgress(double progress) {
    if (qFuzzyCompare(m_progress + 2.0, progress + 2.0))
        return;
    m_progress = progress;
    emit progressChanged();
}

void WebImporter::setLastError(const QString &error) {
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

void WebImporter::setDetectedKind(const QString &kind) {
    if (m_detectedKind == kind)
        return;
    m_detectedKind = kind;
    emit detectedKindChanged();
}

void WebImporter::abortActiveOperations() {
    ++m_generation;
    const auto requests = m_activeRequests;
    m_activeRequests.clear();
    for (const QPointer<PinnedHttpRequest> &req : requests) {
        if (req) {
            req->abort();
            req->deleteLater();
        }
    }
}

void WebImporter::completeSuccess(int bookId, const QString &title, const QString &kind, const QString &sourceUrl) {
    setProgress(1.0);
    setStatus(QStringLiteral("Added to library"));
    setLastError({});
    setBusy(false);
    emit finished(bookId, title, kind, sourceUrl);
}

void WebImporter::completeFailure(const QString &error) {
    abortActiveOperations();
    setProgress(0.0);
    setStatus({});
    setLastError(error);
    setBusy(false);
    emit failed(error);
}

void WebImporter::cancel() {
    if (!m_busy && m_activeRequests.isEmpty())
        return;
    abortActiveOperations();
    setProgress(0.0);
    setStatus({});
    setBusy(false);
    emit cancelled();
}

void WebImporter::importUrl(const QString &rawUrl, int targetCollectionId) {
    if (m_busy) {
        setLastError(QStringLiteral("Another import is already in progress."));
        emit failed(m_lastError);
        return;
    }

    const QString trimmed = rawUrl.trimmed();
    const QString kind = detectUrlKind(trimmed);
    setDetectedKind(kind);
    setLastError({});

    if (!m_library) {
        completeFailure(QStringLiteral("Library is not available."));
        return;
    }
    if (kind.isEmpty()) {
        completeFailure(QStringLiteral("Enter a valid http://, https://, or arXiv link."));
        return;
    }

    abortActiveOperations();
    const quint64 gen = m_generation;
    setBusy(true);
    setProgress(-1.0);

    if (kind == QLatin1String("arxiv")) {
        const QString arxivId = parseArxivId(trimmed);
        runArxivImport(arxivId, targetCollectionId, gen);
        return;
    }

    const QUrl url(trimmed);
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, m_allowLocalNetwork, &guardReason)) {
        completeFailure(guardReason);
        return;
    }

    runDirectOrArticleImport(url, kind, targetCollectionId, gen);
}

bool WebImporter::importUrlSync(const QString &rawUrl, int *bookIdOut, QString *kindOut, QString *errorOut,
                                int targetCollectionId) {
    bool ok = false;
    bool done = false;
    int bookId = 0;
    QString kind;
    QString error;

    QEventLoop loop;
    auto c1 = connect(this, &WebImporter::finished, &loop,
                      [&](int id, const QString &, const QString &k, const QString &) {
                          ok = true;
                          done = true;
                          bookId = id;
                          kind = k;
                          loop.quit();
                      });
    auto c2 = connect(this, &WebImporter::failed, &loop, [&](const QString &err) {
        ok = false;
        done = true;
        error = err;
        loop.quit();
    });
    auto c3 = connect(this, &WebImporter::cancelled, &loop, [&] {
        ok = false;
        done = true;
        error = QStringLiteral("Import cancelled.");
        loop.quit();
    });

    importUrl(rawUrl, targetCollectionId);
    if (!done)
        loop.exec();

    disconnect(c1);
    disconnect(c2);
    disconnect(c3);

    if (bookIdOut)
        *bookIdOut = bookId;
    if (kindOut)
        *kindOut = kind;
    if (errorOut)
        *errorOut = error;
    return ok;
}

void WebImporter::fetchToMemory(const QUrl &url, qint64 maxBytes, bool trackProgress, quint64 generation,
                                const QByteArray &acceptHeader, FetchCallback callback, int timeoutMs) {
    if (generation != m_generation)
        return;

    auto declaredLength = std::make_shared<qint64>(-1);
    auto bodyBuffer = std::make_shared<QByteArray>();

    PinnedHttpOptions opts;
    opts.url = url;
    opts.allowLocalNetwork = m_allowLocalNetwork;
    opts.maxBytes = maxBytes;
    opts.maxRedirects = UrlGuard::maxRedirects;
    opts.timeoutMs = timeoutMs > 0 ? timeoutMs : kRequestTimeoutMs;
    opts.userAgent = QByteArrayLiteral("Clothbound/" CLOTHBOUND_VERSION " (+https://clothbound.app)");
    opts.acceptHeader = acceptHeader;

    opts.onHeaders = [declaredLength](int, const QUrl &, const QList<QPair<QByteArray, QByteArray>> &,
                                      qint64 contentLength, qint64 *, QString *) {
        *declaredLength = contentLength;
        return true;
    };

    opts.onBodyChunk = [this, bodyBuffer, declaredLength, trackProgress, generation](
                           const QByteArray &chunk, qint64 wireBytes, qint64, QString *) {
        if (generation != m_generation)
            return false;
        bodyBuffer->append(chunk);
        if (trackProgress && *declaredLength > 0) {
            const double frac = double(wireBytes) / double(*declaredLength);
            setProgress(qBound(0.05, frac * 0.8, 0.8));
        }
        return true;
    };

    auto reqPtr = std::make_shared<QPointer<PinnedHttpRequest>>();
    PinnedHttpRequest *req = UrlGuard::fetchPinned(
        std::move(opts),
        [this, reqPtr, bodyBuffer, generation, cb = std::move(callback)](PinnedHttpResponse resp) mutable {
            if (*reqPtr) {
                m_activeRequests.removeOne(*reqPtr);
                (*reqPtr)->deleteLater();
            }
            if (generation != m_generation)
                return;

            FetchResult res;
            if (!resp.ok) {
                res.error = resp.error.isEmpty() ? QStringLiteral("Network request failed.") : resp.error;
                cb(std::move(res));
                return;
            }
            if (resp.httpStatus >= 400) {
                res.error = QStringLiteral("Server returned HTTP %1.").arg(resp.httpStatus);
                cb(std::move(res));
                return;
            }
            res.ok = true;
            res.finalUrl = resp.finalUrl;
            res.contentType = resp.contentType;
            res.contentDisposition = resp.contentDisposition;
            res.data = std::move(*bodyBuffer);
            cb(std::move(res));
        },
        this);
    *reqPtr = req;
    if (!req->isFinished())
        m_activeRequests.append(req);
}

void WebImporter::fetchDirectOrStream(const QUrl &url, qint64 initialMaxBytes, bool allowBookCapUpgrade,
                                      bool trackProgress, quint64 generation, const QByteArray &acceptHeader,
                                      const QString &forcedBookFormat, DestPathBuilder destPathBuilder,
                                      FetchCallback callback, int timeoutMs) {
    if (generation != m_generation)
        return;

    struct StreamState {
        QByteArray sniffBuf;
        QString finalFormat;
        QString destPath;
        QUrl finalUrl;
        QString contentType;
        QString contentDisposition;
        qint64 declaredLength = -1;
        bool isHttpSuccess = false;
        bool decidedAsNonBook = false;
        std::unique_ptr<QSaveFile> saveFile;
        DestPathBuilder destPathBuilder;

        ~StreamState() {
            if (saveFile) {
                saveFile->cancelWriting();
                saveFile.reset();
            }
        }
    };

    auto state = std::make_shared<StreamState>();
    state->destPathBuilder = std::move(destPathBuilder);

    auto tryStartDiskStream = [state, forcedBookFormat](QString *abortErrorOut) -> bool {
        if (state->saveFile || state->decidedAsNonBook)
            return true;

        const QString mime = state->contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        const QString finalPath = state->finalUrl.path().toLower();
        const bool isPdf = looksLikePdf(state->sniffBuf);
        const bool isEpub = !isPdf &&
                            (looksLikeEpubBytes(state->sniffBuf) ||
                             ((mime == QLatin1String("application/epub+zip") ||
                               finalPath.endsWith(QLatin1String(".epub")) ||
                               finalPath.endsWith(QLatin1String(".kepub"))) &&
                              state->sniffBuf.startsWith("PK\x03\x04")));

        QString detectedFormat;
        if (forcedBookFormat == QLatin1String("pdf")) {
            if (isPdf)
                detectedFormat = QStringLiteral("pdf");
        } else if (isPdf) {
            detectedFormat = QStringLiteral("pdf");
        } else if (isEpub) {
            detectedFormat = QStringLiteral("epub");
        }

        if (!detectedFormat.isEmpty()) {
            state->finalFormat = detectedFormat;
            state->destPath = state->destPathBuilder
                                  ? state->destPathBuilder(detectedFormat, state->finalUrl, state->contentDisposition)
                                  : QString();
            if (state->destPath.isEmpty()) {
                if (abortErrorOut)
                    *abortErrorOut = QStringLiteral("Could not determine destination path for download.");
                return false;
            }
            const QFileInfo info(state->destPath);
            if (!QDir().mkpath(info.absolutePath())) {
                if (abortErrorOut)
                    *abortErrorOut = QStringLiteral("Could not create folder %1.").arg(info.absolutePath());
                return false;
            }
            state->saveFile = std::make_unique<QSaveFile>(state->destPath);
            if (!state->saveFile->open(QIODevice::WriteOnly)) {
                if (abortErrorOut)
                    *abortErrorOut = QStringLiteral("Could not write %1: %2")
                                         .arg(state->destPath, state->saveFile->errorString());
                state->saveFile.reset();
                return false;
            }
            if (!state->sniffBuf.isEmpty() && state->saveFile->write(state->sniffBuf) != state->sniffBuf.size()) {
                if (abortErrorOut)
                    *abortErrorOut = QStringLiteral("Failed writing %1: %2")
                                         .arg(state->destPath, state->saveFile->errorString());
                state->saveFile->cancelWriting();
                state->saveFile.reset();
                return false;
            }
            state->sniffBuf.clear();
            return true;
        }

        if (!forcedBookFormat.isEmpty()) {
            if (abortErrorOut)
                *abortErrorOut = QStringLiteral("Downloaded file is not a valid %1 document.")
                                     .arg(forcedBookFormat.toUpper());
            return false;
        }

        state->decidedAsNonBook = true;
        if (qint64(state->sniffBuf.size()) > UrlGuard::maxHtmlBytes) {
            if (abortErrorOut)
                *abortErrorOut = QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                                     .arg(UrlGuard::maxHtmlBytes / (1024 * 1024));
            return false;
        }
        return true;
    };

    PinnedHttpOptions opts;
    opts.url = url;
    opts.allowLocalNetwork = m_allowLocalNetwork;
    opts.maxBytes = initialMaxBytes;
    opts.maxRedirects = UrlGuard::maxRedirects;
    opts.timeoutMs = timeoutMs > 0 ? timeoutMs : kRequestTimeoutMs;
    opts.userAgent = QByteArrayLiteral("Clothbound/" CLOTHBOUND_VERSION " (+https://clothbound.app)");
    opts.acceptHeader = acceptHeader;

    opts.onHeaders = [state, allowBookCapUpgrade](int statusCode, const QUrl &finalUrl,
                                                  const QList<QPair<QByteArray, QByteArray>> &headers,
                                                  qint64 contentLength, qint64 *maxBytesInOut, QString *) {
        state->finalUrl = finalUrl;
        state->contentType = QString::fromUtf8(headerValueFromList(headers, "Content-Type"));
        state->contentDisposition = QString::fromUtf8(headerValueFromList(headers, "Content-Disposition"));
        state->declaredLength = contentLength;
        state->isHttpSuccess = (statusCode >= 200 && statusCode < 300);
        if (allowBookCapUpgrade && isBookContentType(state->contentType) &&
            *maxBytesInOut < UrlGuard::maxBookBytes) {
            *maxBytesInOut = UrlGuard::maxBookBytes;
        }
        return true;
    };

    const qsizetype sniffThreshold = (forcedBookFormat == QLatin1String("pdf")) ? 32 : 512;
    opts.onBodyChunk = [this, state, tryStartDiskStream, sniffThreshold, trackProgress, generation](
                           const QByteArray &chunk, qint64 wireBytes, qint64, QString *abortErrorOut) {
        if (generation != m_generation)
            return false;
        if (!state->isHttpSuccess)
            return true;

        if (state->saveFile) {
            if (state->saveFile->write(chunk) != chunk.size()) {
                if (abortErrorOut)
                    *abortErrorOut = QStringLiteral("Failed writing %1: %2")
                                         .arg(state->destPath, state->saveFile->errorString());
                state->saveFile->cancelWriting();
                state->saveFile.reset();
                return false;
            }
        } else if (state->decidedAsNonBook) {
            if (qint64(state->sniffBuf.size()) + qint64(chunk.size()) > UrlGuard::maxHtmlBytes) {
                if (abortErrorOut)
                    *abortErrorOut = QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                                         .arg(UrlGuard::maxHtmlBytes / (1024 * 1024));
                return false;
            }
            state->sniffBuf.append(chunk);
        } else {
            state->sniffBuf.append(chunk);
            if (state->sniffBuf.size() >= sniffThreshold) {
                if (!tryStartDiskStream(abortErrorOut))
                    return false;
            }
        }

        if (trackProgress && state->declaredLength > 0) {
            const double frac = double(wireBytes) / double(state->declaredLength);
            setProgress(qBound(0.05, frac * 0.8, 0.8));
        }
        return true;
    };

    auto reqPtr = std::make_shared<QPointer<PinnedHttpRequest>>();
    PinnedHttpRequest *req = UrlGuard::fetchPinned(
        std::move(opts),
        [this, reqPtr, state, tryStartDiskStream, generation, cb = std::move(callback)](
            PinnedHttpResponse resp) mutable {
            if (*reqPtr) {
                m_activeRequests.removeOne(*reqPtr);
                (*reqPtr)->deleteLater();
            }
            if (generation != m_generation)
                return;

            if (!resp.ok) {
                if (state->saveFile) {
                    state->saveFile->cancelWriting();
                    state->saveFile.reset();
                }
                FetchResult res;
                res.error = resp.error.isEmpty() ? QStringLiteral("Network request failed.") : resp.error;
                cb(std::move(res));
                return;
            }
            if (resp.httpStatus >= 400) {
                if (state->saveFile) {
                    state->saveFile->cancelWriting();
                    state->saveFile.reset();
                }
                FetchResult res;
                res.error = QStringLiteral("Server returned HTTP %1.").arg(resp.httpStatus);
                cb(std::move(res));
                return;
            }

            if (!state->saveFile && !state->decidedAsNonBook && !state->sniffBuf.isEmpty()) {
                QString abortErr;
                if (!tryStartDiskStream(&abortErr)) {
                    FetchResult res;
                    res.error = abortErr.isEmpty() ? QStringLiteral("Invalid downloaded file.") : abortErr;
                    cb(std::move(res));
                    return;
                }
            }

            FetchResult res;
            res.ok = true;
            res.finalUrl = resp.finalUrl;
            res.contentType = resp.contentType;
            res.contentDisposition = resp.contentDisposition;

            if (state->saveFile) {
                if (!state->saveFile->commit()) {
                    res.ok = false;
                    res.error = QStringLiteral("Could not save %1: %2")
                                    .arg(state->destPath, state->saveFile->errorString());
                    state->saveFile.reset();
                    cb(std::move(res));
                    return;
                }
                state->saveFile.reset();
                res.streamedFilePath = state->destPath;
                res.streamedFormat = state->finalFormat;
            } else {
                res.data = std::move(state->sniffBuf);
            }
            cb(std::move(res));
        },
        this);
    *reqPtr = req;
    if (!req->isFinished())
        m_activeRequests.append(req);
}

QString WebImporter::computeDirectBookDestPath(const QString &format, const QUrl &finalUrl,
                                               const QString &contentDisposition,
                                               const QVariantMap &citationMeta,
                                               const QString &sourceUrlOverride) const {
    QString stem;
    const QString citeTitle = sanitizeFileStem(citationMeta.value(QStringLiteral("title")).toString(), 110);
    const QString citeAuthor = sanitizeFileStem(
        BookName::displayAuthor(citationMeta.value(QStringLiteral("firstAuthor")).toString()), 48);
    const int citeYear = citationMeta.value(QStringLiteral("year")).toInt();
    if (format == QLatin1String("pdf") && !citeTitle.isEmpty()) {
        stem = !citeAuthor.isEmpty() ? (citeAuthor + QStringLiteral(" - ") + citeTitle) : citeTitle;
        if (citeYear > 0)
            stem += QStringLiteral(" (%1)").arg(citeYear);
    }

    if (stem.isEmpty()) {
        QString rawName = parseContentDispositionFilename(contentDisposition);
        if (rawName.isEmpty())
            rawName = QFileInfo(QUrl::fromPercentEncoding(finalUrl.path().toUtf8())).fileName();

        stem = QFileInfo(rawName).completeBaseName();
        stem = sanitizeFileStem(stem, 96);
    }
    if (stem.isEmpty())
        stem = slugify(finalUrl.host(), 32) + QLatin1Char('-') + shortHashHex(finalUrl.toString());

    const QString ext = QLatin1Char('.') + format;
    const QString effectiveSourceUrl =
        !sourceUrlOverride.isEmpty() ? sourceUrlOverride : finalUrl.toString();

    auto isSameSource = [this, &effectiveSourceUrl](const QString &path) {
        if (!m_library)
            return false;
        const int existingId = m_library->idForPath(path);
        return existingId > 0 && m_library->bookSetting(existingId, QStringLiteral("source_url")) == effectiveSourceUrl;
    };

    QString destPath = QDir(downloadsDir()).filePath(stem + ext);
    if (!QFileInfo::exists(destPath) || isSameSource(destPath))
        return destPath;

    const QString hashedStem = stem + QLatin1Char('-') + shortHashHex(effectiveSourceUrl);
    destPath = QDir(downloadsDir()).filePath(hashedStem + ext);
    if (!QFileInfo::exists(destPath) || isSameSource(destPath))
        return destPath;

    for (int n = 2; n < 1000; ++n) {
        const QString candidate = QDir(downloadsDir()).filePath(QStringLiteral("%1-%2%3").arg(hashedStem).arg(n).arg(ext));
        if (!QFileInfo::exists(candidate) || isSameSource(candidate))
            return candidate;
    }
    return destPath;
}

QString WebImporter::computeArticleDestPath(const QString &title, const QString &fetchedUrl) const {
    const QString baseStem = slugify(title, 60) + QLatin1Char('-') + shortHashHex(fetchedUrl);
    const QString dir = articlesDir();

    auto isSameSource = [this, &fetchedUrl](const QString &path) {
        if (!m_library)
            return false;
        const int existingId = m_library->idForPath(path);
        return existingId > 0 && m_library->bookSetting(existingId, QStringLiteral("source_url")) == fetchedUrl;
    };

    QString destPath = QDir(dir).filePath(baseStem + QStringLiteral(".epub"));
    if (!QFileInfo::exists(destPath) || isSameSource(destPath))
        return destPath;

    for (int n = 2; n < 1000; ++n) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("%1-%2.epub").arg(baseStem).arg(n));
        if (!QFileInfo::exists(candidate) || isSameSource(candidate))
            return candidate;
    }
    return destPath;
}

void WebImporter::runArxivImport(const QString &arxivId, int targetCollectionId, quint64 generation) {
    setStatus(QStringLiteral("Fetching arXiv metadata for %1…").arg(arxivId));
    setProgress(0.1);

    const QUrl apiUrl(m_arxivApiBase + QString::fromLatin1(QUrl::toPercentEncoding(arxivId)));
    fetchToMemory(
        apiUrl, UrlGuard::maxHtmlBytes, false, generation,
        QByteArrayLiteral("application/atom+xml, application/xml;q=0.9, */*;q=0.1"),
        [this, arxivId, targetCollectionId, generation](FetchResult atomRes) {
            if (generation != m_generation)
                return;

            QVariantMap meta;
            if (atomRes.ok)
                meta = parseArxivAtom(atomRes.data);

            // export.arxiv.org occasionally rate-limits or times out; fall back
            // to CDN-cached arxiv.org/abs/<id> Highwire citation_* meta tags.
            if (meta.value(QStringLiteral("title")).toString().trimmed().isEmpty() && !m_arxivAbsBase.isEmpty()) {
                const QUrl absUrl(m_arxivAbsBase + arxivId);
                fetchToMemory(
                    absUrl, UrlGuard::maxHtmlBytes, false, generation,
                    QByteArrayLiteral("text/html, application/xhtml+xml;q=0.9, */*;q=0.1"),
                    [this, arxivId, targetCollectionId, generation](FetchResult absRes) {
                        if (generation != m_generation)
                            return;
                        QVariantMap fallbackMeta;
                        if (absRes.ok)
                            fallbackMeta = parseCitationMetaHtml(absRes.data);
                        downloadArxivPdf(arxivId, fallbackMeta, targetCollectionId, generation);
                    },
                    kMetadataTimeoutMs);
                return;
            }

            downloadArxivPdf(arxivId, meta, targetCollectionId, generation);
        },
        kMetadataTimeoutMs);
}

void WebImporter::downloadArxivPdf(const QString &arxivId, const QVariantMap &meta, int targetCollectionId,
                                   quint64 generation) {
    if (generation != m_generation)
        return;

    const QString paperTitle = meta.value(QStringLiteral("title")).toString().simplified();
    const QString firstAuthor = BookName::displayAuthor(meta.value(QStringLiteral("firstAuthor")).toString().simplified());
    const QString abstractText = meta.value(QStringLiteral("abstract")).toString().simplified();
    const int year = meta.value(QStringLiteral("year")).toInt();

    setStatus(paperTitle.isEmpty() ? QStringLiteral("Downloading arXiv %1 PDF…").arg(arxivId)
                                   : QStringLiteral("Downloading “%1”…").arg(paperTitle));
    setProgress(0.25);

    // Build clean filename matching BookName::parseFileName():
    // "<FirstAuthor> - <SafeTitle> (<Year>).pdf" or "<id>.pdf" fallback.
    QString stem;
    const QString safeTitle = sanitizeFileStem(paperTitle, 110);
    const QString safeAuthor = sanitizeFileStem(firstAuthor, 48);
    if (!safeTitle.isEmpty()) {
        stem = !safeAuthor.isEmpty() ? (safeAuthor + QStringLiteral(" - ") + safeTitle) : safeTitle;
        if (year > 0)
            stem += QStringLiteral(" (%1)").arg(year);
    } else {
        QString safeId = arxivId;
        safeId.replace(QLatin1Char('/'), QLatin1Char('_'));
        stem = sanitizeFileStem(safeId, 64);
    }
    if (stem.isEmpty())
        stem = QStringLiteral("arxiv-paper");
    const QString sourceUrl = QStringLiteral("https://arxiv.org/abs/") + arxivId;
    auto isSameSource = [this, &sourceUrl, &arxivId](const QString &path) {
        if (!m_library)
            return false;
        const int existingId = m_library->idForPath(path);
        if (existingId <= 0)
            return false;
        return m_library->bookSetting(existingId, QStringLiteral("source_url")) == sourceUrl ||
               m_library->bookSetting(existingId, QStringLiteral("arxiv_id")) == arxivId;
    };

    const QString dir = downloadsDir();
    QString plannedDestPath = QDir(dir).filePath(stem + QStringLiteral(".pdf"));
    if (QFileInfo::exists(plannedDestPath) && !isSameSource(plannedDestPath)) {
        QString safeId = arxivId;
        safeId.replace(QLatin1Char('/'), QLatin1Char('_'));
        const QString disambigStem = stem + QLatin1Char('-') + sanitizeFileStem(safeId, 32);
        plannedDestPath = QDir(dir).filePath(disambigStem + QStringLiteral(".pdf"));
        if (QFileInfo::exists(plannedDestPath) && !isSameSource(plannedDestPath)) {
            const QString hashedStem = stem + QLatin1Char('-') + shortHashHex(sourceUrl);
            plannedDestPath = QDir(dir).filePath(hashedStem + QStringLiteral(".pdf"));
            if (QFileInfo::exists(plannedDestPath) && !isSameSource(plannedDestPath)) {
                for (int n = 2; n < 1000; ++n) {
                    const QString candidate =
                        QDir(dir).filePath(QStringLiteral("%1-%2.pdf").arg(hashedStem).arg(n));
                    if (!QFileInfo::exists(candidate) || isSameSource(candidate)) {
                        plannedDestPath = candidate;
                        break;
                    }
                }
            }
        }
    }

    const QUrl pdfUrl(m_arxivPdfBase + arxivId + QStringLiteral(".pdf"));
    fetchDirectOrStream(
        pdfUrl, UrlGuard::maxBookBytes, false, true, generation,
        QByteArrayLiteral("application/pdf, */*;q=0.1"),
        QStringLiteral("pdf"),
        [plannedDestPath](const QString &, const QUrl &, const QString &) { return plannedDestPath; },
        [this, arxivId, sourceUrl, paperTitle, abstractText, targetCollectionId, generation](FetchResult pdfRes) {
            if (generation != m_generation)
                return;
            if (!pdfRes.ok) {
                completeFailure(pdfRes.error);
                return;
            }
            if (pdfRes.streamedFilePath.isEmpty()) {
                completeFailure(QStringLiteral("Downloaded arXiv file is not a valid PDF document."));
                return;
            }

            setStatus(QStringLiteral("Adding paper to library…"));
            setProgress(0.92);

            const int bookId = m_library->addBookFile(pdfRes.streamedFilePath);
            if (bookId <= 0) {
                completeFailure(m_library->errorString().isEmpty()
                                    ? QStringLiteral("Could not add downloaded PDF to the library.")
                                    : m_library->errorString());
                return;
            }

            if (targetCollectionId > 0)
                m_library->addToCollection(bookId, targetCollectionId);

            m_library->setBookSetting(bookId, QStringLiteral("source_url"), sourceUrl);
            m_library->setBookSetting(bookId, QStringLiteral("arxiv_id"), arxivId);
            if (!abstractText.isEmpty())
                m_library->setBookSetting(bookId, QStringLiteral("arxiv_abstract"), abstractText);

            const QString displayTitle =
                !paperTitle.isEmpty() ? paperTitle : m_library->record(bookId).title;
            completeSuccess(bookId, displayTitle, QStringLiteral("arxiv"), sourceUrl);
        });
}

void WebImporter::runDirectOrArticleImport(const QUrl &initialUrl, const QString &initialKind, int targetCollectionId,
                                           quint64 generation) {
    const bool expectBook = (initialKind == QLatin1String("epub") || initialKind == QLatin1String("pdf"));
    setStatus(expectBook ? QStringLiteral("Downloading %1…").arg(initialKind.toUpper())
                         : QStringLiteral("Fetching page…"));

    const qint64 initialCap = expectBook ? UrlGuard::maxBookBytes : UrlGuard::maxHtmlBytes;
    const QByteArray accept =
        expectBook ? QByteArrayLiteral("application/epub+zip, application/pdf, application/octet-stream;q=0.9, */*;q=0.1")
                   : QByteArrayLiteral("text/html, application/xhtml+xml;q=0.9, application/pdf;q=0.8, application/epub+zip;q=0.8, */*;q=0.1");

    fetchDirectOrStream(
        initialUrl, initialCap, true, true, generation, accept, {},
        [this](const QString &format, const QUrl &finalUrl, const QString &contentDisposition) {
            return computeDirectBookDestPath(format, finalUrl, contentDisposition);
        },
        [this, initialKind, targetCollectionId, generation](FetchResult res) {
            if (generation != m_generation)
                return;
            if (!res.ok) {
                completeFailure(res.error);
                return;
            }

            if (!res.streamedFilePath.isEmpty()) {
                setDetectedKind(res.streamedFormat);
                finishDirectBook(res.streamedFormat, res, targetCollectionId);
                return;
            }

            const QString mime = res.contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
            const QString finalPath = res.finalUrl.path().toLower();

            if (looksLikePdf(res.data)) {
                setDetectedKind(QStringLiteral("pdf"));
                finishDirectBook(QStringLiteral("pdf"), res, targetCollectionId);
                return;
            }

            if (looksLikeEpubBytes(res.data) ||
                ((mime == QLatin1String("application/epub+zip") || finalPath.endsWith(QLatin1String(".epub")) ||
                  finalPath.endsWith(QLatin1String(".kepub"))) &&
                 res.data.startsWith("PK\x03\x04"))) {
                setDetectedKind(QStringLiteral("epub"));
                finishDirectBook(QStringLiteral("epub"), res, targetCollectionId);
                return;
            }

            const bool isHtmlMime = mime == QLatin1String("text/html") ||
                                    mime == QLatin1String("application/xhtml+xml") ||
                                    (mime.isEmpty() && res.data.left(1024).toLower().contains("<html"));

            if (initialKind == QLatin1String("pdf") && !isHtmlMime) {
                completeFailure(QStringLiteral("The link did not return a valid PDF document."));
                return;
            }
            if (initialKind == QLatin1String("epub") && !isHtmlMime) {
                completeFailure(QStringLiteral("The link did not return a valid EPUB archive."));
                return;
            }

            if (isHtmlMime) {
                // Academic paper landing pages (OpenReview, ACL Anthology, PMLR, bioRxiv,
                // arXiv mirrors) advertise Highwire/Scholar citation_* meta tags pointing
                // directly to the paper PDF.
                const QVariantMap citation = parseCitationMetaHtml(res.data);
                const QString metaArxivId = parseArxivId(QStringLiteral("arxiv:") +
                                                         citation.value(QStringLiteral("arxivId")).toString());
                if (!metaArxivId.isEmpty()) {
                    setDetectedKind(QStringLiteral("arxiv"));
                    downloadArxivPdf(metaArxivId, citation, targetCollectionId, generation);
                    return;
                }
                const QString metaPdfStr = citation.value(QStringLiteral("pdfUrl")).toString().trimmed();
                if (!metaPdfStr.isEmpty()) {
                    const QUrl resolvedPdf = res.finalUrl.resolved(QUrl(metaPdfStr));
                    if (UrlGuard::isAllowedUrl(resolvedPdf, m_allowLocalNetwork)) {
                        const QString landingUrl = res.finalUrl.toString();
                        const QString paperTitle = citation.value(QStringLiteral("title")).toString().trimmed();
                        setDetectedKind(QStringLiteral("pdf"));
                        setStatus(paperTitle.isEmpty() ? QStringLiteral("Downloading paper PDF…")
                                                       : QStringLiteral("Downloading “%1”…").arg(paperTitle));
                        fetchDirectOrStream(
                            resolvedPdf, UrlGuard::maxBookBytes, false, true, generation,
                            QByteArrayLiteral("application/pdf, */*;q=0.1"),
                            QStringLiteral("pdf"),
                            [this, citation, landingUrl](const QString &format, const QUrl &finalUrl,
                                                         const QString &contentDisposition) {
                                return computeDirectBookDestPath(format, finalUrl, contentDisposition, citation,
                                                                 landingUrl);
                            },
                            [this, citation, landingUrl, targetCollectionId, generation, res](FetchResult pdfRes) {
                                if (generation != m_generation)
                                    return;
                                if (pdfRes.ok && !pdfRes.streamedFilePath.isEmpty()) {
                                    finishDirectBook(QStringLiteral("pdf"), pdfRes, targetCollectionId, citation,
                                                     landingUrl);
                                    return;
                                }
                                // Fall back to article extraction if the advertised PDF failed.
                                setDetectedKind(QStringLiteral("article"));
                                finishWebArticle(res, targetCollectionId, generation);
                            });
                        return;
                    }
                }
                setDetectedKind(QStringLiteral("article"));
                finishWebArticle(res, targetCollectionId, generation);
                return;
            }

            completeFailure(QStringLiteral("Unsupported content type (%1). Clothbound imports web articles, arXiv papers, EPUBs, and PDFs.")
                                .arg(mime.isEmpty() ? QStringLiteral("unknown") : mime));
        });
}

void WebImporter::finishDirectBook(const QString &format, const FetchResult &res, int targetCollectionId,
                                   const QVariantMap &citationMeta, const QString &sourceUrlOverride) {
    setStatus(QStringLiteral("Saving %1…").arg(format.toUpper()));
    setProgress(0.88);

    const QString effectiveSourceUrl =
        !sourceUrlOverride.isEmpty() ? sourceUrlOverride : res.finalUrl.toString();

    QString destPath = res.streamedFilePath;
    if (destPath.isEmpty()) {
        destPath = computeDirectBookDestPath(format, res.finalUrl, res.contentDisposition, citationMeta,
                                             sourceUrlOverride);
        QString saveErr;
        if (!saveBytesAtomically(destPath, res.data, &saveErr)) {
            completeFailure(saveErr);
            return;
        }
    }

    if (format == QLatin1String("epub")) {
        EpubBook check(destPath);
        if (!check.open()) {
            QFile::remove(destPath);
            completeFailure(check.errorString().isEmpty() ? QStringLiteral("Downloaded file is not a valid EPUB.")
                                                          : check.errorString());
            return;
        }
    }

    const int bookId = m_library->addBookFile(destPath);
    if (bookId <= 0) {
        completeFailure(m_library->errorString().isEmpty()
                            ? QStringLiteral("Could not add downloaded book to the library.")
                            : m_library->errorString());
        return;
    }

    if (targetCollectionId > 0)
        m_library->addToCollection(bookId, targetCollectionId);
    m_library->setBookSetting(bookId, QStringLiteral("source_url"), effectiveSourceUrl);
    const QString citeAbstract = citationMeta.value(QStringLiteral("abstract")).toString().trimmed();
    if (!citeAbstract.isEmpty())
        m_library->setBookSetting(bookId, QStringLiteral("arxiv_abstract"), citeAbstract);

    completeSuccess(bookId, m_library->record(bookId).title, format, effectiveSourceUrl);
}

void WebImporter::finishWebArticle(const FetchResult &res, int targetCollectionId, quint64 generation) {
    setStatus(QStringLiteral("Extracting article…"));
    setProgress(0.4);

    auto extracted = std::make_shared<ExtractedArticle>(ArticleExtractor::extract(res.data, res.finalUrl));
    if (extracted->bodyXhtml.isEmpty() || extracted->plainText.trimmed().isEmpty()) {
        completeFailure(QStringLiteral("Could not extract readable article content from this page."));
        return;
    }

    // Security invariant: derive output filename hash and source ownership strictly from
    // res.finalUrl (never untrusted <link rel="canonical"> / og:url) so a hostile page
    // cannot collide with or overwrite an existing saved article EPUB.
    const QString fetchedUrl = res.finalUrl.toString();

    auto finalizePackaging = [this, extracted, fetchedUrl, targetCollectionId, generation](
                                 QList<EpubImageAsset> bundledImages, QHash<QString, QString> pathMap) {
        if (generation != m_generation)
            return;

        setStatus(QStringLiteral("Packaging offline EPUB…"));
        setProgress(0.86);

        EpubArticle epub;
        epub.title = extracted->title;
        epub.author = extracted->author;
        epub.publisher = extracted->siteName;
        epub.publishedDate = extracted->publishedDate;
        epub.language = extracted->language;
        epub.sourceUrl = fetchedUrl;
        epub.description = extracted->description;
        epub.subjects = {QStringLiteral("Clothbound Article")};
        epub.readingMinutes = extracted->readingMinutes;
        epub.bodyXhtml = ArticleExtractor::finalizeImages(extracted->bodyXhtml, pathMap);
        epub.images = std::move(bundledImages);
        for (const ExtractedHeading &h : std::as_const(extracted->headings))
            epub.headings.append({h.id, h.title, h.level});

        const QString destPath = computeArticleDestPath(extracted->title, fetchedUrl);

        QString writeErr;
        if (!EpubWriter::writeArticle(destPath, epub, &writeErr)) {
            completeFailure(writeErr.isEmpty() ? QStringLiteral("Could not package article as EPUB.") : writeErr);
            return;
        }

        const int bookId = m_library->addBookFile(destPath);
        if (bookId <= 0) {
            completeFailure(m_library->errorString().isEmpty()
                                ? QStringLiteral("Could not add packaged article to the library.")
                                : m_library->errorString());
            return;
        }

        if (targetCollectionId > 0)
            m_library->addToCollection(bookId, targetCollectionId);

        m_library->setBookSetting(bookId, QStringLiteral("source_url"), fetchedUrl);
        m_library->setBookSetting(bookId, QStringLiteral("content_kind"), QStringLiteral("article"));

        completeSuccess(bookId, extracted->title, QStringLiteral("article"), fetchedUrl);
    };

    if (extracted->images.isEmpty()) {
        finalizePackaging({}, {});
        return;
    }

    // Fetch content images concurrently (bounded concurrency + total byte cap).
    struct ImageBatch {
        QList<ExtractedImage> queue;
        int nextIndex = 0;
        int completed = 0;
        int inFlight = 0;
        bool finished = false;
        bool pumping = false;
        qint64 totalBytes = 0;
        QList<EpubImageAsset> assets;
        QHash<QString, QString> pathMap;
        std::function<void(QList<EpubImageAsset>, QHash<QString, QString>)> onDone;
    };

    auto batch = std::make_shared<ImageBatch>();
    batch->queue = extracted->images.mid(0, UrlGuard::maxArticleImages);
    batch->onDone = std::move(finalizePackaging);

    setStatus(QStringLiteral("Downloading images (0 of %1)…").arg(batch->queue.size()));
    setProgress(0.45);

    auto tryFinish = [batch] {
        if (!batch->finished && batch->completed >= batch->queue.size() && batch->inFlight == 0) {
            batch->finished = true;
            auto onDone = std::move(batch->onDone);
            batch->onDone = {};
            if (onDone)
                onDone(std::move(batch->assets), std::move(batch->pathMap));
        }
    };

    auto pump = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakPump = pump;
    *pump = [this, batch, weakPump, tryFinish, generation] {
        if (generation != m_generation || batch->finished || batch->pumping)
            return;
        auto strongPump = weakPump.lock();
        if (!strongPump)
            return;
        batch->pumping = true;
        while (!batch->finished && batch->inFlight < kMaxConcurrentImages && batch->nextIndex < batch->queue.size()) {
            const int idx = batch->nextIndex++;
            const ExtractedImage img = batch->queue[idx];
            const qint64 remainingBudget = UrlGuard::maxTotalImageBytes - batch->totalBytes;
            if (remainingBudget <= 0) {
                ++batch->completed;
                continue;
            }
            ++batch->inFlight;
            const qint64 imgLimit = qMin(UrlGuard::maxImageBytes, remainingBudget);
            fetchToMemory(
                img.sourceUrl, imgLimit, false, generation,
                QByteArrayLiteral("image/webp, image/png, image/jpeg, image/gif, image/svg+xml;q=0.8, */*;q=0.1"),
                [this, batch, strongPump, tryFinish, img, generation](FetchResult imgRes) {
                    if (generation != m_generation || batch->finished) {
                        batch->onDone = {};
                        if (!batch->pumping)
                            *strongPump = nullptr;
                        return;
                    }
                    --batch->inFlight;
                    ++batch->completed;

                    if (imgRes.ok && !imgRes.data.isEmpty() &&
                        batch->totalBytes + qint64(imgRes.data.size()) <= UrlGuard::maxTotalImageBytes) {
                        QString ext;
                        const QString mediaType = detectImageFormat(imgRes.contentType, imgRes.data, &ext);
                        if (!mediaType.isEmpty()) {
                            const int assetNum = int(batch->assets.size()) + 1;
                            const QString finalLocal = QStringLiteral("images/img-%1.%2")
                                                           .arg(assetNum, 3, 10, QLatin1Char('0'))
                                                           .arg(ext);
                            batch->totalBytes += qint64(imgRes.data.size());
                            batch->assets.append({finalLocal, mediaType, std::move(imgRes.data)});
                            batch->pathMap.insert(img.localPath, finalLocal);
                        }
                    }

                    const double imgFrac = batch->queue.isEmpty()
                                               ? 1.0
                                               : double(batch->completed) / double(batch->queue.size());
                    setProgress(0.45 + imgFrac * 0.35);
                    setStatus(QStringLiteral("Downloading images (%1 of %2)…")
                                  .arg(batch->completed)
                                  .arg(batch->queue.size()));

                    if (batch->completed >= batch->queue.size() && batch->inFlight == 0) {
                        tryFinish();
                        if (!batch->pumping)
                            *strongPump = nullptr;
                    } else if (!batch->pumping) {
                        (*strongPump)();
                        if (batch->finished)
                            *strongPump = nullptr;
                    }
                });
        }
        batch->pumping = false;
        tryFinish();
    };

    (*pump)();
    if (batch->finished || generation != m_generation) {
        batch->onDone = {};
        *pump = nullptr;
    }
}
