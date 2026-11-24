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
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
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
        const QByteArray head = data.left(256).trimmed().toLower();
        if (head.contains("<svg")) {
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

bool isRedirectStatus(int code) {
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
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
    for (const int lookupId : std::as_const(m_activeLookups))
        QHostInfo::abortHostLookup(lookupId);
    m_activeLookups.clear();

    const auto replies = m_activeReplies;
    m_activeReplies.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
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
    if (!m_busy && m_activeReplies.isEmpty() && m_activeLookups.isEmpty())
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

void WebImporter::fetchUrl(const QUrl &url, qint64 maxBytes, bool allowBookCapUpgrade, bool trackProgress,
                           int redirectCount, quint64 generation, const QByteArray &acceptHeader,
                           FetchCallback callback, int timeoutMs) {
    if (generation != m_generation)
        return;

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, m_allowLocalNetwork, &guardReason)) {
        FetchResult res;
        res.error = guardReason;
        callback(std::move(res));
        return;
    }

    if (!m_allowLocalNetwork) {
        const QString host = url.host(QUrl::FullyEncoded).trimmed();
        const QHostAddress literal = UrlGuard::parseHostLiteral(host);
        if (literal.isNull()) {
            auto cbPtr = std::make_shared<FetchCallback>(std::move(callback));
            int lookupId = 0;
            lookupId = QHostInfo::lookupHost(
                host, this,
                [this, url, maxBytes, allowBookCapUpgrade, trackProgress, redirectCount, generation, acceptHeader, cbPtr,
                 host, timeoutMs](const QHostInfo &info) {
                    m_activeLookups.removeOne(info.lookupId());
                    if (generation != m_generation)
                        return;
                    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
                        FetchResult res;
                        const QString detail =
                            info.errorString().isEmpty() ? QStringLiteral("host lookup failed") : info.errorString();
                        res.error = QStringLiteral("Could not resolve host “%1” (%2).").arg(host, detail);
                        (*cbPtr)(std::move(res));
                        return;
                    }
                    for (const QHostAddress &addr : info.addresses()) {
                        if (UrlGuard::isPrivateHostAddress(addr)) {
                            FetchResult res;
                            res.error =
                                QStringLiteral("Host “%1” resolved to a blocked private or local network address.")
                                    .arg(host);
                            (*cbPtr)(std::move(res));
                            return;
                        }
                    }
                    startHttpRequest(url, maxBytes, allowBookCapUpgrade, trackProgress, redirectCount, generation,
                                     acceptHeader, std::move(*cbPtr), timeoutMs);
                });
            m_activeLookups.append(lookupId);
            return;
        }
    }

    startHttpRequest(url, maxBytes, allowBookCapUpgrade, trackProgress, redirectCount, generation, acceptHeader,
                     std::move(callback), timeoutMs);
}

void WebImporter::startHttpRequest(const QUrl &url, qint64 maxBytes, bool allowBookCapUpgrade, bool trackProgress,
                                   int redirectCount, quint64 generation, const QByteArray &acceptHeader,
                                   FetchCallback callback, int timeoutMs) {
    if (generation != m_generation)
        return;

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setTransferTimeout(timeoutMs > 0 ? timeoutMs : kRequestTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Clothbound/0.4 (+https://clothbound.app)"));
    if (!acceptHeader.isEmpty())
        req.setRawHeader("Accept", acceptHeader);

    QNetworkReply *reply = m_nam.get(req);
    m_activeReplies.append(reply);

    struct State {
        QByteArray buffer;
        qint64 limit = 0;
        qint64 declaredLength = -1;
        bool tooLarge = false;
        FetchCallback cb;
    };
    auto state = std::make_shared<State>();
    state->limit = maxBytes;
    state->cb = std::move(callback);

    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, state, allowBookCapUpgrade, generation, this] {
        if (generation != m_generation)
            return;
        const QString ct = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (allowBookCapUpgrade && isBookContentType(ct) && state->limit < UrlGuard::maxBookBytes)
            state->limit = UrlGuard::maxBookBytes;
        const QVariant lenVar = reply->header(QNetworkRequest::ContentLengthHeader);
        if (lenVar.isValid()) {
            state->declaredLength = lenVar.toLongLong();
            if (state->declaredLength > state->limit) {
                state->tooLarge = true;
                reply->abort();
            }
        }
    });

    connect(reply, &QNetworkReply::readyRead, this, [reply, state, trackProgress, generation, this] {
        if (generation != m_generation || state->tooLarge)
            return;
        const QByteArray chunk = reply->readAll();
        if (qint64(state->buffer.size()) + qint64(chunk.size()) > state->limit) {
            state->tooLarge = true;
            state->buffer.clear();
            reply->abort();
            return;
        }
        state->buffer.append(chunk);
        if (trackProgress && state->declaredLength > 0) {
            const double frac = double(state->buffer.size()) / double(state->declaredLength);
            setProgress(qBound(0.05, frac * 0.8, 0.8));
        }
    });

    connect(reply, &QNetworkReply::finished, this,
            [reply, state, url, maxBytes, allowBookCapUpgrade, trackProgress, redirectCount, generation, acceptHeader,
             timeoutMs, this] {
                m_activeReplies.removeOne(reply);
                const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
                if (generation != m_generation)
                    return;

                if (state->tooLarge) {
                    FetchResult res;
                    res.error = QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                                    .arg(state->limit / (1024 * 1024));
                    state->cb(std::move(res));
                    return;
                }

                // Drain any final unread bytes.
                const QByteArray tail = reply->readAll();
                if (!tail.isEmpty()) {
                    if (qint64(state->buffer.size()) + qint64(tail.size()) > state->limit) {
                        FetchResult res;
                        res.error = QStringLiteral("Download exceeded the maximum allowed size (%1 MiB).")
                                        .arg(state->limit / (1024 * 1024));
                        state->cb(std::move(res));
                        return;
                    }
                    state->buffer.append(tail);
                }

                const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (isRedirectStatus(httpStatus)) {
                    if (redirectCount >= UrlGuard::maxRedirects) {
                        FetchResult res;
                        res.error = QStringLiteral("Too many redirects (more than %1).").arg(UrlGuard::maxRedirects);
                        state->cb(std::move(res));
                        return;
                    }
                    const QByteArray rawLoc = reply->rawHeader("Location").trimmed();
                    if (rawLoc.isEmpty()) {
                        FetchResult res;
                        res.error = QStringLiteral("Server sent a redirect without a target URL.");
                        state->cb(std::move(res));
                        return;
                    }
                    const QUrl target = QUrl::fromEncoded(rawLoc, QUrl::TolerantMode);
                    const QUrl nextUrl = target.isRelative() ? url.resolved(target) : target;
                    fetchUrl(nextUrl, maxBytes, allowBookCapUpgrade, trackProgress, redirectCount + 1, generation,
                             acceptHeader, std::move(state->cb), timeoutMs);
                    return;
                }

                if (reply->error() != QNetworkReply::NoError) {
                    FetchResult res;
                    if (httpStatus >= 400)
                        res.error = QStringLiteral("Server returned HTTP %1.").arg(httpStatus);
                    else
                        res.error = reply->errorString().isEmpty() ? QStringLiteral("Network request failed.")
                                                                   : reply->errorString();
                    state->cb(std::move(res));
                    return;
                }

                if (httpStatus >= 400) {
                    FetchResult res;
                    res.error = QStringLiteral("Server returned HTTP %1.").arg(httpStatus);
                    state->cb(std::move(res));
                    return;
                }

                FetchResult res;
                res.ok = true;
                res.finalUrl = url;
                res.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
                res.contentDisposition = QString::fromUtf8(reply->rawHeader("Content-Disposition"));
                res.data = std::move(state->buffer);
                state->cb(std::move(res));
            });
}

void WebImporter::runArxivImport(const QString &arxivId, int targetCollectionId, quint64 generation) {
    setStatus(QStringLiteral("Fetching arXiv metadata for %1…").arg(arxivId));
    setProgress(0.1);

    const QUrl apiUrl(m_arxivApiBase + QString::fromLatin1(QUrl::toPercentEncoding(arxivId)));
    fetchUrl(
        apiUrl, UrlGuard::maxHtmlBytes, false, false, 0, generation,
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
                fetchUrl(
                    absUrl, UrlGuard::maxHtmlBytes, false, false, 0, generation,
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

    const QUrl pdfUrl(m_arxivPdfBase + arxivId + QStringLiteral(".pdf"));
    fetchUrl(
        pdfUrl, UrlGuard::maxBookBytes, false, true, 0, generation,
        QByteArrayLiteral("application/pdf, */*;q=0.1"),
        [this, arxivId, paperTitle, firstAuthor, abstractText, year, targetCollectionId,
         generation](FetchResult pdfRes) {
            if (generation != m_generation)
                return;
            if (!pdfRes.ok) {
                completeFailure(pdfRes.error);
                return;
            }
            if (!looksLikePdf(pdfRes.data)) {
                completeFailure(QStringLiteral("Downloaded arXiv file is not a valid PDF document."));
                return;
            }

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

            const QString destPath = QDir(downloadsDir()).filePath(stem + QStringLiteral(".pdf"));
            QString saveErr;
            if (!saveBytesAtomically(destPath, pdfRes.data, &saveErr)) {
                completeFailure(saveErr);
                return;
            }

            setStatus(QStringLiteral("Adding paper to library…"));
            setProgress(0.92);

            const int bookId = m_library->addBookFile(destPath);
            if (bookId <= 0) {
                completeFailure(m_library->errorString().isEmpty()
                                    ? QStringLiteral("Could not add downloaded PDF to the library.")
                                    : m_library->errorString());
                return;
            }

            if (targetCollectionId > 0)
                m_library->addToCollection(bookId, targetCollectionId);

            const QString sourceUrl = QStringLiteral("https://arxiv.org/abs/") + arxivId;
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

    fetchUrl(
        initialUrl, initialCap, true, true, 0, generation, accept,
        [this, initialKind, targetCollectionId, generation](FetchResult res) {
            if (generation != m_generation)
                return;
            if (!res.ok) {
                completeFailure(res.error);
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
                        fetchUrl(
                            resolvedPdf, UrlGuard::maxBookBytes, false, true, 0, generation,
                            QByteArrayLiteral("application/pdf, */*;q=0.1"),
                            [this, citation, landingUrl, targetCollectionId, generation, res](FetchResult pdfRes) {
                                if (generation != m_generation)
                                    return;
                                if (pdfRes.ok && looksLikePdf(pdfRes.data)) {
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
        QString rawName = parseContentDispositionFilename(res.contentDisposition);
        if (rawName.isEmpty())
            rawName = QFileInfo(QUrl::fromPercentEncoding(res.finalUrl.path().toUtf8())).fileName();

        stem = QFileInfo(rawName).completeBaseName();
        stem = sanitizeFileStem(stem, 96);
    }
    if (stem.isEmpty())
        stem = slugify(res.finalUrl.host(), 32) + QLatin1Char('-') + shortHashHex(res.finalUrl.toString());

    const QString ext = QLatin1Char('.') + format;
    QString destPath = QDir(downloadsDir()).filePath(stem + ext);
    const QString effectiveSourceUrl =
        !sourceUrlOverride.isEmpty() ? sourceUrlOverride : res.finalUrl.toString();
    // Avoid overwriting a different file from another URL with the same generic filename.
    if (QFileInfo::exists(destPath)) {
        const int existingId = m_library->idForPath(destPath);
        const QString existingSrc = existingId > 0 ? m_library->bookSetting(existingId, QStringLiteral("source_url")) : QString();
        if (existingSrc != effectiveSourceUrl)
            destPath = QDir(downloadsDir()).filePath(stem + QLatin1Char('-') + shortHashHex(effectiveSourceUrl) + ext);
    }

    QString saveErr;
    if (!saveBytesAtomically(destPath, res.data, &saveErr)) {
        completeFailure(saveErr);
        return;
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

    const QString sourceUrl =
        extracted->canonicalUrl.isValid() ? extracted->canonicalUrl.toString() : res.finalUrl.toString();

    auto finalizePackaging = [this, extracted, sourceUrl, targetCollectionId, generation](
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
        epub.sourceUrl = sourceUrl;
        epub.description = extracted->description;
        epub.subjects = {QStringLiteral("Clothbound Article")};
        epub.readingMinutes = extracted->readingMinutes;
        epub.bodyXhtml = ArticleExtractor::finalizeImages(extracted->bodyXhtml, pathMap);
        epub.images = std::move(bundledImages);
        for (const ExtractedHeading &h : std::as_const(extracted->headings))
            epub.headings.append({h.id, h.title, h.level});

        const QString fileName = slugify(extracted->title, 60) + QLatin1Char('-') + shortHashHex(sourceUrl) +
                                 QStringLiteral(".epub");
        const QString destPath = QDir(articlesDir()).filePath(fileName);

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

        m_library->setBookSetting(bookId, QStringLiteral("source_url"), sourceUrl);
        m_library->setBookSetting(bookId, QStringLiteral("content_kind"), QStringLiteral("article"));

        completeSuccess(bookId, extracted->title, QStringLiteral("article"), sourceUrl);
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
            batch->onDone(std::move(batch->assets), std::move(batch->pathMap));
        }
    };

    auto pump = std::make_shared<std::function<void()>>();
    *pump = [this, batch, pump, tryFinish, generation] {
        if (generation != m_generation || batch->finished || batch->pumping)
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
            fetchUrl(
                img.sourceUrl, imgLimit, false, false, 0, generation,
                QByteArrayLiteral("image/webp, image/png, image/jpeg, image/gif, image/svg+xml;q=0.8, */*;q=0.1"),
                [this, batch, pump, tryFinish, img, generation](FetchResult imgRes) {
                    if (generation != m_generation || batch->finished)
                        return;
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

                    if (batch->completed >= batch->queue.size() && batch->inFlight == 0)
                        tryFinish();
                    else if (!batch->pumping)
                        (*pump)();
                });
        }
        batch->pumping = false;
        tryFinish();
    };

    (*pump)();
}
