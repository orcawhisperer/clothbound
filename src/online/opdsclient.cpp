#include "online/opdsclient.h"

#include "ai/keystore.h"
#include "library/library.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>
#include <functional>

namespace {

constexpr qint64 kMaxFeedBytes = 10LL * 1024 * 1024;   // 10 MB
constexpr qint64 kMaxBookBytes = 200LL * 1024 * 1024;  // 200 MB
constexpr int kMaxRedirects = 5;
constexpr int kTransferTimeoutMs = 30000;

int effectivePort(const QUrl &url) {
    const int p = url.port(-1);
    if (p > 0)
        return p;
    return url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
}

bool isRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

QNetworkRequest makeBaseRequest(const QUrl &url, const QByteArray &accept) {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    // Avoid Qt 6 HTTP/2 half-closed stream errors on Open Library / Archive.org / HAProxy servers.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Clothbound/0.4.0 (OPDS)"));
    if (!accept.isEmpty())
        req.setRawHeader("Accept", accept);
    req.setTransferTimeout(kTransferTimeoutMs);
    return req;
}

QString detectBookMagicFormat(const QByteArray &header) {
    if (header.size() >= 5 && header.startsWith("%PDF-"))
        return QStringLiteral("pdf");
    if (header.size() >= 4 && header.at(0) == 'P' && header.at(1) == 'K' &&
        header.at(2) == '\x03' && header.at(3) == '\x04')
        return QStringLiteral("epub");
    return {};
}

QString friendlyNetworkError(QNetworkReply *reply, int httpStatus) {
    if (httpStatus == 403)
        return QObject::tr("Access to this catalog was forbidden (HTTP 403).");
    if (httpStatus == 404)
        return QObject::tr("This catalog section could not be found (HTTP 404).");
    if (httpStatus >= 500 && httpStatus < 600)
        return QObject::tr("The catalog server returned an error (HTTP %1).").arg(httpStatus);
    if (httpStatus >= 400)
        return QObject::tr("Catalog request failed (HTTP %1).").arg(httpStatus);
    if (reply && !reply->errorString().isEmpty())
        return reply->errorString();
    return QObject::tr("Could not connect to the catalog server.");
}

}  // namespace

OpdsClient::OpdsClient(Library *library, KeyStore *keyStore, const QString &storageDir,
                       const QString &settingsPath, QObject *parent)
    : QObject(parent),
      m_library(library),
      m_keyStore(keyStore),
      m_storageDir(storageDir.isEmpty()
                       ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                       : storageDir),
      m_settingsPath(settingsPath.isEmpty()
                         ? (QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                            QStringLiteral("/settings.ini"))
                         : settingsPath) {
    seedBuiltInCatalogs();
    loadSettings();
    loadExistingDownloadStates();
}

OpdsClient::~OpdsClient() {
    abortActiveFeedRequest();
    const auto downloads = m_activeDownloads.values();
    m_activeDownloads.clear();
    for (const auto &dl : downloads) {
        dl->cancelled = true;
        if (dl->reply)
            dl->reply->abort();
        if (dl->saveFile)
            dl->saveFile->cancelWriting();
    }
}

void OpdsClient::seedBuiltInCatalogs() {
    m_builtInCatalogs = {
        Catalog{
            QStringLiteral("standard-ebooks-new"),
            QStringLiteral("Standard Ebooks"),
            QStringLiteral("https://standardebooks.org/feeds/atom/new-releases"),
            QStringLiteral("Latest free public-domain EPUBs"),
            {},
            true,
            false,
            {},
        },
        Catalog{
            QStringLiteral("unglue-it"),
            QStringLiteral("Unglue.it"),
            QStringLiteral("https://unglue.it/api/opds/"),
            QStringLiteral("Creative Commons \u0026 open-access books"),
            {},
            true,
            false,
            {},
        },
        Catalog{
            QStringLiteral("open-library"),
            QStringLiteral("Open Library"),
            QStringLiteral("https://openlibrary.org/opds/"),
            QStringLiteral("Internet Archive OPDS 2.0 catalog"),
            {},
            true,
            false,
            {},
        },
        Catalog{
            QStringLiteral("standard-ebooks"),
            QStringLiteral("Standard Ebooks (Patron)"),
            QStringLiteral("https://standardebooks.org/feeds/opds"),
            QStringLiteral("Full catalog \u00B7 requires Patron email"),
            {},
            true,
            true,
            QStringLiteral("Enter your Standard Ebooks Patrons Circle email as username (leave password blank)."),
        },
    };
}

void OpdsClient::loadSettings() {
    if (m_settingsPath.isEmpty())
        return;

    QSettings settings(m_settingsPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("opds"));

    const QStringList hidden = settings.value(QStringLiteral("hiddenBuiltIns")).toStringList();
    m_hiddenBuiltIns = QSet<QString>(hidden.cbegin(), hidden.cend());

    const QByteArray usernamesRaw = settings.value(QStringLiteral("builtInUsernames")).toByteArray();
    if (!usernamesRaw.isEmpty()) {
        const QJsonObject obj = QJsonDocument::fromJson(usernamesRaw).object();
        for (Catalog &c : m_builtInCatalogs) {
            if (obj.contains(c.id))
                c.username = obj.value(c.id).toString().trimmed();
        }
    }

    m_customCatalogs.clear();
    const QByteArray customRaw = settings.value(QStringLiteral("customCatalogs")).toByteArray();
    if (!customRaw.isEmpty()) {
        const QJsonArray arr = QJsonDocument::fromJson(customRaw).array();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toString().trimmed();
            const QString name = o.value(QStringLiteral("name")).toString().trimmed();
            const QString url = o.value(QStringLiteral("url")).toString().trimmed();
            if (id.isEmpty() || !OpdsParser::isAllowedHttpUrl(QUrl(url)))
                continue;
            Catalog c;
            c.id = id;
            c.name = name.isEmpty() ? QUrl(url).host() : name;
            c.url = url;
            c.subtitle = o.value(QStringLiteral("subtitle")).toString().trimmed();
            if (c.subtitle.isEmpty())
                c.subtitle = QUrl(url).host();
            c.username = o.value(QStringLiteral("username")).toString().trimmed();
            c.builtIn = false;
            c.authOptional = true;
            m_customCatalogs.append(std::move(c));
        }
    }

    settings.endGroup();
}

void OpdsClient::saveSettings() const {
    if (m_settingsPath.isEmpty())
        return;

    QDir().mkpath(QFileInfo(m_settingsPath).absolutePath());
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("opds"));

    QStringList hidden = m_hiddenBuiltIns.values();
    hidden.sort();
    settings.setValue(QStringLiteral("hiddenBuiltIns"), hidden);

    QJsonObject builtInUsers;
    for (const Catalog &c : m_builtInCatalogs) {
        if (!c.username.isEmpty())
            builtInUsers.insert(c.id, c.username);
    }
    settings.setValue(QStringLiteral("builtInUsernames"),
                      QJsonDocument(builtInUsers).toJson(QJsonDocument::Compact));

    QJsonArray customArr;
    for (const Catalog &c : m_customCatalogs) {
        // Explicitly serialize non-secret metadata only; passwords live in KeyStore.
        QJsonObject o;
        o.insert(QStringLiteral("id"), c.id);
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("url"), c.url);
        o.insert(QStringLiteral("subtitle"), c.subtitle);
        if (!c.username.isEmpty())
            o.insert(QStringLiteral("username"), c.username);
        customArr.append(o);
    }
    settings.setValue(QStringLiteral("customCatalogs"),
                      QJsonDocument(customArr).toJson(QJsonDocument::Compact));

    settings.endGroup();
    settings.sync();
}

void OpdsClient::loadExistingDownloadStates() {
    if (!m_library)
        return;
    const QSqlDatabase db = m_library->database();
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT book_id, value FROM book_settings WHERE key = 'source_url'")))
        return;
    while (q.next()) {
        const int bookId = q.value(0).toInt();
        const QString sourceUrl = q.value(1).toString().trimmed();
        if (bookId <= 0 || sourceUrl.isEmpty())
            continue;
        const BookRecord rec = m_library->record(bookId);
        if (rec.id == bookId && rec.visible()) {
            m_downloadStates.insert(sourceUrl, QVariantMap{
                {QStringLiteral("status"), QStringLiteral("done")},
                {QStringLiteral("progress"), 1.0},
                {QStringLiteral("bookId"), bookId},
                {QStringLiteral("error"), QString()},
            });
        }
    }
}

bool OpdsClient::sameOrigin(const QUrl &a, const QUrl &b) {
    if (!a.isValid() || !b.isValid())
        return false;
    return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 &&
           a.host().compare(b.host(), Qt::CaseInsensitive) == 0 &&
           effectivePort(a) == effectivePort(b);
}

QVariantList OpdsClient::catalogs() const {
    QVariantList out;
    auto toMap = [](const Catalog &c) -> QVariantMap {
        return {
            {QStringLiteral("id"), c.id},
            {QStringLiteral("name"), c.name},
            {QStringLiteral("url"), c.url},
            {QStringLiteral("subtitle"), c.subtitle},
            {QStringLiteral("username"), c.username},
            {QStringLiteral("builtIn"), c.builtIn},
            {QStringLiteral("authOptional"), c.authOptional},
            {QStringLiteral("authHint"), c.authHint},
            {QStringLiteral("hasCredentials"), !c.username.isEmpty()},
        };
    };

    for (const Catalog &c : m_builtInCatalogs) {
        if (!m_hiddenBuiltIns.contains(c.id))
            out.append(toMap(c));
    }
    for (const Catalog &c : m_customCatalogs)
        out.append(toMap(c));
    return out;
}

QString OpdsClient::activeCatalogName() const {
    if (const Catalog *c = findCatalog(m_activeCatalogId))
        return c->name;
    return {};
}

QString OpdsClient::activeCatalogUsername() const {
    if (const Catalog *c = findCatalog(m_activeCatalogId))
        return c->username;
    return {};
}

QVariantList OpdsClient::breadcrumbs() const {
    QVariantList list;
    list.reserve(m_history.size());
    for (const Breadcrumb &b : m_history) {
        list.append(QVariantMap{
            {QStringLiteral("title"), b.title},
            {QStringLiteral("url"), b.url},
        });
    }
    return list;
}

OpdsClient::Catalog *OpdsClient::findCatalog(const QString &catalogId) {
    for (Catalog &c : m_builtInCatalogs)
        if (c.id == catalogId)
            return &c;
    for (Catalog &c : m_customCatalogs)
        if (c.id == catalogId)
            return &c;
    return nullptr;
}

const OpdsClient::Catalog *OpdsClient::findCatalog(const QString &catalogId) const {
    for (const Catalog &c : m_builtInCatalogs)
        if (c.id == catalogId)
            return &c;
    for (const Catalog &c : m_customCatalogs)
        if (c.id == catalogId)
            return &c;
    return nullptr;
}

QString OpdsClient::effectiveSearchUrl() const {
    return !m_searchUrl.isEmpty() ? m_searchUrl : m_catalogSearchUrl;
}

bool OpdsClient::effectiveSearchTemplated() const {
    return !m_searchUrl.isEmpty() ? m_searchTemplated : m_catalogSearchTemplated;
}

QString OpdsClient::secretKeyFor(const QString &catalogId) {
    return QStringLiteral("opds:") + catalogId;
}

QString OpdsClient::sanitizeSlug(const QString &raw, const QString &fallback, int maxLen) {
    QString out;
    out.reserve(qMin(raw.size(), maxLen));
    bool lastDash = false;
    for (const QChar ch : raw.toLower()) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
            (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
            out.append(ch);
            lastDash = false;
        } else if (!out.isEmpty() && !lastDash) {
            out.append(QLatin1Char('-'));
            lastDash = true;
        }
        if (out.size() >= maxLen)
            break;
    }
    while (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    return out.isEmpty() ? fallback : out;
}

QString OpdsClient::sanitizeFilename(const QString &raw, const QString &fallback, int maxLen) {
    QString clean;
    clean.reserve(qMin(raw.size(), maxLen));
    bool lastSpace = false;
    for (const QChar ch : raw) {
        if (ch.category() == QChar::Other_Control || ch == QLatin1Char('/') || ch == QLatin1Char('\\') ||
            ch == QLatin1Char(':') || ch == QLatin1Char('*') || ch == QLatin1Char('?') ||
            ch == QLatin1Char('"') || ch == QLatin1Char('<') || ch == QLatin1Char('>') ||
            ch == QLatin1Char('|')) {
            if (!clean.isEmpty() && !lastSpace) {
                clean.append(QLatin1Char(' '));
                lastSpace = true;
            }
            continue;
        }
        if (ch.isSpace()) {
            if (!clean.isEmpty() && !lastSpace) {
                clean.append(QLatin1Char(' '));
                lastSpace = true;
            }
            continue;
        }
        clean.append(ch);
        lastSpace = false;
        if (clean.size() >= maxLen)
            break;
    }
    clean = clean.trimmed();
    while (clean.startsWith(QLatin1Char('.')))
        clean.remove(0, 1);
    while (clean.endsWith(QLatin1Char('.')) || clean.endsWith(QLatin1Char(' ')))
        clean.chop(1);
    if (clean.isEmpty() || clean == QLatin1String(".") || clean == QLatin1String(".."))
        return fallback;
    return clean;
}

QString OpdsClient::addCatalog(const QString &name, const QString &url,
                               const QString &username, const QString &password) {
    const QString trimmedUrl = url.trimmed();
    const QUrl parsed(trimmedUrl);
    if (!OpdsParser::isAllowedHttpUrl(parsed)) {
        m_error = tr("Catalog URL must start with http:// or https://.");
        emit feedChanged();
        return {};
    }

    const QString trimmedName = name.trimmed().isEmpty() ? parsed.host() : name.trimmed();
    const QString trimmedUser = username.trimmed();

    QString hostSummary = parsed.host();
    if (parsed.port(-1) > 0)
        hostSummary += QLatin1Char(':') + QString::number(parsed.port());

    const QString baseSlug = sanitizeSlug(trimmedName, QStringLiteral("server"), 24);
    const QString uniqueSuffix = QUuid::createUuid().toString(QUuid::Id128).left(8);
    QString id = QStringLiteral("custom-%1-%2").arg(baseSlug, uniqueSuffix);
    while (findCatalog(id) != nullptr)
        id = QStringLiteral("custom-%1-%2").arg(baseSlug, QUuid::createUuid().toString(QUuid::Id128).left(8));

    Catalog cat;
    cat.id = id;
    cat.name = trimmedName;
    cat.url = parsed.toString(QUrl::FullyEncoded);
    cat.subtitle = hostSummary;
    cat.username = trimmedUser;
    cat.builtIn = false;
    cat.authOptional = true;
    m_customCatalogs.append(cat);

    saveSettings();

    m_passwordLoaded.insert(id);
    if (!password.isEmpty()) {
        m_passwordCache.insert(id, password);
        if (m_keyStore)
            m_keyStore->saveLater(secretKeyFor(id), password);
    } else {
        m_passwordCache.remove(id);
    }

    emit catalogsChanged();
    return id;
}

bool OpdsClient::updateCatalogCredentials(const QString &catalogId,
                                          const QString &username, const QString &password) {
    Catalog *cat = findCatalog(catalogId);
    if (!cat)
        return false;

    cat->username = username.trimmed();
    saveSettings();

    m_passwordLoaded.insert(catalogId);
    if (!password.isEmpty()) {
        m_passwordCache.insert(catalogId, password);
        if (m_keyStore)
            m_keyStore->saveLater(secretKeyFor(catalogId), password);
    } else {
        m_passwordCache.remove(catalogId);
        if (m_keyStore)
            m_keyStore->removeLater(secretKeyFor(catalogId));
    }

    if (catalogId == m_activeCatalogId && m_authRequired) {
        m_authRequired = false;
        emit feedChanged();
    }
    emit catalogsChanged();
    return true;
}

bool OpdsClient::removeCatalog(const QString &catalogId) {
    bool removed = false;
    for (qsizetype i = 0; i < m_customCatalogs.size(); ++i) {
        if (m_customCatalogs[i].id == catalogId) {
            m_customCatalogs.removeAt(i);
            removed = true;
            break;
        }
    }

    if (!removed) {
        for (Catalog &c : m_builtInCatalogs) {
            if (c.id == catalogId) {
                m_hiddenBuiltIns.insert(catalogId);
                c.username.clear();
                removed = true;
                break;
            }
        }
    }

    if (!removed)
        return false;

    m_passwordCache.remove(catalogId);
    m_passwordLoaded.remove(catalogId);
    if (m_keyStore)
        m_keyStore->removeLater(secretKeyFor(catalogId));

    saveSettings();

    if (m_activeCatalogId == catalogId) {
        abortActiveFeedRequest();
        m_activeCatalogId.clear();
        m_feedTitle.clear();
        m_history.clear();
        m_entries.clear();
        m_currentUrl.clear();
        m_nextUrl.clear();
        m_searchUrl.clear();
        m_searchTemplated = false;
        m_catalogSearchUrl.clear();
        m_catalogSearchTemplated = false;
        m_authRequired = false;
        m_authHint.clear();
        m_error.clear();
        emit feedChanged();
    }

    emit catalogsChanged();
    return true;
}

void OpdsClient::restoreBuiltInCatalogs() {
    if (m_hiddenBuiltIns.isEmpty())
        return;
    m_hiddenBuiltIns.clear();
    saveSettings();
    emit catalogsChanged();
}

void OpdsClient::abortActiveFeedRequest() {
    ++m_feedGeneration;
    if (m_activeFeedReply) {
        QPointer<QNetworkReply> r = m_activeFeedReply;
        m_activeFeedReply = nullptr;
        r->abort();
        r->deleteLater();
    }
    if (m_loading || m_loadingMore) {
        m_loading = false;
        m_loadingMore = false;
        emit loadingChanged();
    }
}

void OpdsClient::ensurePasswordLoaded(const QString &catalogId, std::function<void()> ready) {
    if (!m_keyStore || m_passwordLoaded.contains(catalogId)) {
        ready();
        return;
    }

    const quint64 gen = m_feedGeneration;
    QFuture<QString> future = m_keyStore->loadLater(secretKeyFor(catalogId));
    if (future.isFinished()) {
        const QString secret = future.result();
        m_passwordLoaded.insert(catalogId);
        if (!secret.isEmpty())
            m_passwordCache.insert(catalogId, secret);
        else
            m_passwordCache.remove(catalogId);
        ready();
        return;
    }

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, catalogId, gen, ready = std::move(ready)]() mutable {
                const QString secret = watcher->result();
                watcher->deleteLater();
                m_passwordLoaded.insert(catalogId);
                if (!secret.isEmpty())
                    m_passwordCache.insert(catalogId, secret);
                else
                    m_passwordCache.remove(catalogId);
                if (gen == m_feedGeneration)
                    ready();
            });
    watcher->setFuture(future);
}

void OpdsClient::applyAuthIfAllowed(QNetworkRequest &request, const QUrl &targetUrl,
                                    const QUrl &catalogOrigin, const QUrl &initialOrigin,
                                    const QString &catalogId) const {
    const Catalog *cat = findCatalog(catalogId);
    if (!cat)
        return;
    const QString user = cat->username;
    const QString pass = m_passwordCache.value(catalogId);
    if (user.isEmpty() && pass.isEmpty())
        return;

    // Security invariant: never transmit credentials to any origin other than the catalog's own.
    if (!sameOrigin(targetUrl, catalogOrigin))
        return;
    if (initialOrigin.isValid() && !sameOrigin(targetUrl, initialOrigin))
        return;

    const QByteArray token = (user + QLatin1Char(':') + pass).toUtf8().toBase64();
    request.setRawHeader("Authorization", "Basic " + token);
}

void OpdsClient::openCatalog(const QString &catalogId) {
    const Catalog *cat = findCatalog(catalogId);
    if (!cat) {
        m_error = tr("Selected catalog was not found.");
        emit feedChanged();
        return;
    }

    const QUrl url(cat->url);
    if (!OpdsParser::isAllowedHttpUrl(url)) {
        m_error = tr("Catalog URL is not a valid HTTP or HTTPS address.");
        emit feedChanged();
        return;
    }

    abortActiveFeedRequest();
    m_activeCatalogId = cat->id;
    m_feedTitle = cat->name;
    m_history = {Breadcrumb{cat->name, cat->url}};
    m_entries.clear();
    m_currentUrl = cat->url;
    m_nextUrl.clear();
    m_searchUrl.clear();
    m_searchTemplated = false;
    m_catalogSearchUrl.clear();
    m_catalogSearchTemplated = false;
    m_pendingSearchQuery.clear();
    m_pendingOpenSearchUrl.clear();
    m_authRequired = false;
    m_authHint = cat->authHint;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    const QString cid = cat->id;
    ensurePasswordLoaded(cid, [this, url]() { fetchFeed(url, false, 0, url); });
}

void OpdsClient::navigate(const QString &url, const QString &title) {
    const QUrl target(url.trimmed());
    if (!OpdsParser::isAllowedHttpUrl(target)) {
        m_error = tr("Only HTTP and HTTPS catalog links are supported.");
        emit feedChanged();
        return;
    }

    abortActiveFeedRequest();
    const QString normalized = target.toString(QUrl::FullyEncoded);
    m_history.append(Breadcrumb{title.trimmed().isEmpty() ? normalized : title.trimmed(), normalized});
    m_currentUrl = normalized;
    m_nextUrl.clear();
    m_pendingSearchQuery.clear();
    m_pendingOpenSearchUrl.clear();
    m_authRequired = false;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    ensurePasswordLoaded(m_activeCatalogId, [this, target]() { fetchFeed(target, false, 0, target); });
}

void OpdsClient::goBack() {
    if (m_history.size() <= 1)
        return;
    m_history.removeLast();
    const QUrl target(m_history.last().url);
    if (!OpdsParser::isAllowedHttpUrl(target))
        return;

    abortActiveFeedRequest();
    m_currentUrl = target.toString(QUrl::FullyEncoded);
    m_nextUrl.clear();
    m_pendingSearchQuery.clear();
    m_pendingOpenSearchUrl.clear();
    m_authRequired = false;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    ensurePasswordLoaded(m_activeCatalogId, [this, target]() { fetchFeed(target, false, 0, target); });
}

void OpdsClient::navigateBreadcrumb(int index) {
    if (index < 0 || index >= m_history.size() - 1)
        return;
    m_history.resize(index + 1);
    const QUrl target(m_history.last().url);
    if (!OpdsParser::isAllowedHttpUrl(target))
        return;

    abortActiveFeedRequest();
    m_currentUrl = target.toString(QUrl::FullyEncoded);
    m_nextUrl.clear();
    m_pendingSearchQuery.clear();
    m_pendingOpenSearchUrl.clear();
    m_authRequired = false;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    ensurePasswordLoaded(m_activeCatalogId, [this, target]() { fetchFeed(target, false, 0, target); });
}

void OpdsClient::search(const QString &query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty())
        return;
    const QString searchEndpoint = effectiveSearchUrl();
    if (searchEndpoint.isEmpty())
        return;

    if (effectiveSearchTemplated()) {
        const QString expanded = OpdsParser::expandSearchTemplate(searchEndpoint, trimmed);
        navigate(expanded, tr("Search: %1").arg(trimmed));
        return;
    }

    if (m_openSearchTemplates.contains(searchEndpoint)) {
        const QString expanded = OpdsParser::expandSearchTemplate(m_openSearchTemplates.value(searchEndpoint), trimmed);
        navigate(expanded, tr("Search: %1").arg(trimmed));
        return;
    }

    const QUrl osUrl(searchEndpoint);
    if (!OpdsParser::isAllowedHttpUrl(osUrl)) {
        m_error = tr("Catalog search link is not a valid HTTP or HTTPS URL.");
        emit feedChanged();
        return;
    }

    abortActiveFeedRequest();
    m_pendingSearchQuery = trimmed;
    m_pendingOpenSearchUrl = osUrl;
    m_authRequired = false;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    ensurePasswordLoaded(m_activeCatalogId,
                         [this, osUrl, trimmed]() { fetchOpenSearchAndExecute(osUrl, trimmed, 0, osUrl); });
}

void OpdsClient::loadMore() {
    if (m_nextUrl.isEmpty() || m_loading || m_loadingMore)
        return;
    const QUrl next(m_nextUrl);
    if (!OpdsParser::isAllowedHttpUrl(next))
        return;

    abortActiveFeedRequest();
    m_error.clear();
    m_loading = false;
    m_loadingMore = true;
    emit loadingChanged();

    ensurePasswordLoaded(m_activeCatalogId, [this, next]() { fetchFeed(next, true, 0, next); });
}

void OpdsClient::retry() {
    if (!m_pendingSearchQuery.isEmpty() && m_pendingOpenSearchUrl.isValid()) {
        const QUrl osUrl = m_pendingOpenSearchUrl;
        const QString query = m_pendingSearchQuery;
        abortActiveFeedRequest();
        m_authRequired = false;
        m_error.clear();
        m_loading = true;
        m_loadingMore = false;
        emit loadingChanged();
        emit feedChanged();
        ensurePasswordLoaded(m_activeCatalogId,
                             [this, osUrl, query]() { fetchOpenSearchAndExecute(osUrl, query, 0, osUrl); });
        return;
    }

    if (m_currentUrl.isEmpty()) {
        if (!m_activeCatalogId.isEmpty())
            openCatalog(m_activeCatalogId);
        return;
    }

    const QUrl target(m_currentUrl);
    if (!OpdsParser::isAllowedHttpUrl(target))
        return;

    abortActiveFeedRequest();
    m_authRequired = false;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    ensurePasswordLoaded(m_activeCatalogId, [this, target]() { fetchFeed(target, false, 0, target); });
}

void OpdsClient::fetchFeed(const QUrl &url, bool appendMode, int hops, const QUrl &initialOrigin) {
    if (!OpdsParser::isAllowedHttpUrl(url)) {
        m_loading = false;
        m_loadingMore = false;
        m_error = tr("Blocked unsupported URL scheme.");
        emit loadingChanged();
        emit feedChanged();
        return;
    }

    const Catalog *cat = findCatalog(m_activeCatalogId);
    const QUrl catalogOrigin = cat ? QUrl(cat->url) : url;

    QNetworkRequest req = makeBaseRequest(
        url, "application/opds+json, application/opds-publication+json, application/atom+xml;profile=opds-catalog, application/atom+xml, application/xml;q=0.9, */*;q=0.5");
    applyAuthIfAllowed(req, url, catalogOrigin, initialOrigin, m_activeCatalogId);

    const quint64 gen = m_feedGeneration;
    QNetworkReply *reply = m_nam.get(req);
    m_activeFeedReply = reply;

    auto buffer = std::make_shared<QByteArray>();
    auto sizeExceeded = std::make_shared<bool>(false);

    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, sizeExceeded]() {
        const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
        if (cl.isValid() && cl.toLongLong() > kMaxFeedBytes) {
            *sizeExceeded = true;
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::readyRead, this, [reply, buffer, sizeExceeded]() {
        const QByteArray chunk = reply->readAll();
        if (buffer->size() + chunk.size() > kMaxFeedBytes) {
            *sizeExceeded = true;
            reply->abort();
            return;
        }
        buffer->append(chunk);
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, appendMode, hops, initialOrigin, gen, buffer, sizeExceeded]() {
                if (m_activeFeedReply == reply)
                    m_activeFeedReply = nullptr;
                reply->deleteLater();
                if (gen != m_feedGeneration)
                    return;

                if (!*sizeExceeded && reply->isOpen()) {
                    const QByteArray tail = reply->readAll();
                    if (!tail.isEmpty()) {
                        if (buffer->size() + tail.size() > kMaxFeedBytes)
                            *sizeExceeded = true;
                        else
                            buffer->append(tail);
                    }
                }

                if (*sizeExceeded) {
                    m_loading = false;
                    m_loadingMore = false;
                    m_error = tr("Catalog feed exceeds the 10 MB size limit.");
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

                // Manual redirect handling (max 5 hops, strict scheme check, cross-origin auth stripped).
                if (isRedirectStatus(status)) {
                    const QByteArray loc = reply->rawHeader("Location").trimmed();
                    const QUrl nextUrl = url.resolved(QUrl::fromEncoded(loc));
                    if (loc.isEmpty() || !OpdsParser::isAllowedHttpUrl(nextUrl)) {
                        m_loading = false;
                        m_loadingMore = false;
                        m_error = tr("Catalog redirected to an unsupported address.");
                        emit loadingChanged();
                        emit feedChanged();
                        return;
                    }
                    if (hops >= kMaxRedirects) {
                        m_loading = false;
                        m_loadingMore = false;
                        m_error = tr("Too many redirects while loading this catalog.");
                        emit loadingChanged();
                        emit feedChanged();
                        return;
                    }
                    fetchFeed(nextUrl, appendMode, hops + 1, initialOrigin);
                    return;
                }

                // Clean HTTP 401 handling for inline sign-in card.
                if (status == 401) {
                    m_loading = false;
                    m_loadingMore = false;
                    m_authRequired = true;
                    m_error.clear();
                    const Catalog *activeCat = findCatalog(m_activeCatalogId);
                    if (activeCat && !activeCat->authHint.isEmpty()) {
                        m_authHint = activeCat->authHint;
                    } else if (activeCat) {
                        m_authHint = tr("Sign in to browse %1.").arg(activeCat->name);
                    } else {
                        m_authHint = tr("This catalog requires sign-in.");
                    }
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                if (reply->error() != QNetworkReply::NoError || (status > 0 && (status < 200 || status >= 300))) {
                    m_loading = false;
                    m_loadingMore = false;
                    m_error = friendlyNetworkError(reply, status);
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
                const OpdsFeed parsed = OpdsParser::parse(*buffer, url, contentType);
                m_loading = false;
                m_loadingMore = false;

                if (!parsed.isValid()) {
                    m_error = parsed.error.isEmpty() ? tr("Could not read this catalog feed.") : parsed.error;
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                m_authRequired = false;
                m_error.clear();

                if (!appendMode) {
                    if (!parsed.title.isEmpty()) {
                        m_feedTitle = parsed.title;
                        if (!m_history.isEmpty() && m_history.last().title == m_history.last().url)
                            m_history.last().title = parsed.title;
                    }
                    m_entries.clear();
                    m_entries.reserve(parsed.entries.size());
                    for (const OpdsEntry &e : parsed.entries)
                        m_entries.append(e.toVariantMap());

                    m_searchUrl = parsed.searchUrl;
                    m_searchTemplated = parsed.searchTemplated;
                    if (m_history.size() == 1 && !parsed.searchUrl.isEmpty()) {
                        m_catalogSearchUrl = parsed.searchUrl;
                        m_catalogSearchTemplated = parsed.searchTemplated;
                    }
                } else {
                    for (const OpdsEntry &e : parsed.entries)
                        m_entries.append(e.toVariantMap());
                }

                const QString reqEncoded = url.toString(QUrl::FullyEncoded);
                m_nextUrl = (!parsed.nextUrl.isEmpty() && parsed.nextUrl != reqEncoded) ? parsed.nextUrl : QString();

                emit loadingChanged();
                emit feedChanged();
            });
}

void OpdsClient::fetchOpenSearchAndExecute(const QUrl &openSearchUrl, const QString &query,
                                           int hops, const QUrl &initialOrigin) {
    if (!OpdsParser::isAllowedHttpUrl(openSearchUrl)) {
        m_loading = false;
        m_error = tr("Blocked unsupported search URL scheme.");
        emit loadingChanged();
        emit feedChanged();
        return;
    }

    const Catalog *cat = findCatalog(m_activeCatalogId);
    const QUrl catalogOrigin = cat ? QUrl(cat->url) : openSearchUrl;

    QNetworkRequest req = makeBaseRequest(
        openSearchUrl, "application/opensearchdescription+xml, application/xml, text/xml;q=0.9, */*;q=0.5");
    applyAuthIfAllowed(req, openSearchUrl, catalogOrigin, initialOrigin, m_activeCatalogId);

    const quint64 gen = m_feedGeneration;
    QNetworkReply *reply = m_nam.get(req);
    m_activeFeedReply = reply;

    auto buffer = std::make_shared<QByteArray>();
    auto sizeExceeded = std::make_shared<bool>(false);

    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, sizeExceeded]() {
        const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
        if (cl.isValid() && cl.toLongLong() > kMaxFeedBytes) {
            *sizeExceeded = true;
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::readyRead, this, [reply, buffer, sizeExceeded]() {
        const QByteArray chunk = reply->readAll();
        if (buffer->size() + chunk.size() > kMaxFeedBytes) {
            *sizeExceeded = true;
            reply->abort();
            return;
        }
        buffer->append(chunk);
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, openSearchUrl, query, hops, initialOrigin, gen, buffer, sizeExceeded]() {
                if (m_activeFeedReply == reply)
                    m_activeFeedReply = nullptr;
                reply->deleteLater();
                if (gen != m_feedGeneration)
                    return;

                if (!*sizeExceeded && reply->isOpen()) {
                    const QByteArray tail = reply->readAll();
                    if (!tail.isEmpty()) {
                        if (buffer->size() + tail.size() > kMaxFeedBytes)
                            *sizeExceeded = true;
                        else
                            buffer->append(tail);
                    }
                }

                if (*sizeExceeded) {
                    m_loading = false;
                    m_error = tr("OpenSearch description exceeds the 10 MB size limit.");
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (isRedirectStatus(status)) {
                    const QByteArray loc = reply->rawHeader("Location").trimmed();
                    const QUrl nextUrl = openSearchUrl.resolved(QUrl::fromEncoded(loc));
                    if (loc.isEmpty() || !OpdsParser::isAllowedHttpUrl(nextUrl) || hops >= kMaxRedirects) {
                        m_loading = false;
                        m_error = tr("Could not follow catalog search redirect.");
                        emit loadingChanged();
                        emit feedChanged();
                        return;
                    }
                    fetchOpenSearchAndExecute(nextUrl, query, hops + 1, initialOrigin);
                    return;
                }

                if (status == 401) {
                    m_loading = false;
                    m_authRequired = true;
                    m_error.clear();
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                if (reply->error() != QNetworkReply::NoError || (status > 0 && (status < 200 || status >= 300))) {
                    m_loading = false;
                    m_error = friendlyNetworkError(reply, status);
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                const QString tmpl = OpdsParser::parseOpenSearchTemplate(*buffer, openSearchUrl);
                if (tmpl.isEmpty()) {
                    m_loading = false;
                    m_error = tr("This catalog does not advertise a compatible OPDS search endpoint.");
                    emit loadingChanged();
                    emit feedChanged();
                    return;
                }

                m_openSearchTemplates.insert(initialOrigin.toString(QUrl::FullyEncoded), tmpl);
                const QString expanded = OpdsParser::expandSearchTemplate(tmpl, query);
                m_pendingSearchQuery.clear();
                m_pendingOpenSearchUrl.clear();
                navigate(expanded, tr("Search: %1").arg(query));
            });
}

void OpdsClient::downloadPublication(const QString &acquisitionUrl, const QString &title,
                                     const QString &author, const QString &format,
                                     int targetCollectionId) {
    const QString key = acquisitionUrl.trimmed();
    const QUrl url(key);
    if (!OpdsParser::isAllowedHttpUrl(url)) {
        const QString err = tr("Only HTTP and HTTPS book downloads are supported.");
        setDownloadState(key, QStringLiteral("error"), 0.0, 0, err);
        emit downloadFailed(key, err);
        return;
    }

    if (m_activeDownloads.contains(key))
        return;

    const Catalog *cat = findCatalog(m_activeCatalogId);

    auto dl = std::make_shared<ActiveDownload>();
    dl->acquisitionUrl = key;
    dl->currentUrl = key;
    dl->initialOrigin = url;
    dl->catalogOrigin = cat ? QUrl(cat->url) : url;
    dl->catalogId = m_activeCatalogId.isEmpty() ? QStringLiteral("catalog") : m_activeCatalogId;
    dl->catalogName = cat ? cat->name : url.host();
    dl->title = title.trimmed();
    dl->author = author.trimmed();
    dl->requestedFormat = format.trimmed().toLower();
    dl->targetCollectionId = targetCollectionId;

    m_activeDownloads.insert(key, dl);
    setDownloadState(key, QStringLiteral("downloading"), 0.0);

    ensurePasswordLoaded(dl->catalogId, [this, dl]() {
        if (!dl->cancelled && m_activeDownloads.value(dl->acquisitionUrl) == dl)
            startDownloadRequest(dl);
    });
}

void OpdsClient::cancelDownload(const QString &acquisitionUrl) {
    const QString key = acquisitionUrl.trimmed();
    const auto dl = m_activeDownloads.take(key);
    if (!dl)
        return;

    dl->cancelled = true;
    if (dl->reply) {
        dl->reply->abort();
        dl->reply->deleteLater();
        dl->reply = nullptr;
    }
    if (dl->saveFile) {
        dl->saveFile->cancelWriting();
        dl->saveFile.reset();
    }

    m_downloadStates.remove(key);
    emit downloadStatesChanged();
}

void OpdsClient::startDownloadRequest(const std::shared_ptr<ActiveDownload> &dl) {
    QUrl reqUrl(dl->currentUrl);
    if (!OpdsParser::isAllowedHttpUrl(reqUrl)) {
        const QString err = tr("Download redirected to an unsupported address.");
        m_activeDownloads.remove(dl->acquisitionUrl);
        setDownloadState(dl->acquisitionUrl, QStringLiteral("error"), 0.0, 0, err);
        emit downloadFailed(dl->acquisitionUrl, err);
        return;
    }

    // Standard Ebooks serves an HTML interstitial on /downloads/*.epub unless ?source= is present
    // (Open Library OPDS book manifests link the bare URL without ?source=download).
    const QString hostLower = reqUrl.host().toLower();
    if ((hostLower == QLatin1String("standardebooks.org") || hostLower == QLatin1String("www.standardebooks.org")) &&
        reqUrl.path().contains(QLatin1String("/downloads/"), Qt::CaseInsensitive) &&
        reqUrl.path().endsWith(QLatin1String(".epub"), Qt::CaseInsensitive)) {
        QUrlQuery q(reqUrl);
        if (!q.hasQueryItem(QStringLiteral("source"))) {
            q.addQueryItem(QStringLiteral("source"), QStringLiteral("download"));
            reqUrl.setQuery(q);
        }
    }

    QNetworkRequest req = makeBaseRequest(reqUrl, "application/epub+zip, application/pdf, application/octet-stream;q=0.8, */*;q=0.5");
    applyAuthIfAllowed(req, reqUrl, dl->catalogOrigin, dl->initialOrigin, dl->catalogId);

    dl->bytesReceived = 0;
    dl->bytesTotal = -1;
    dl->headerBuffer.clear();
    dl->verifiedFormat.clear();
    dl->sizeExceeded = false;
    dl->invalidMagic = false;
    dl->diskError = false;
    if (dl->saveFile) {
        dl->saveFile->cancelWriting();
        dl->saveFile.reset();
    }

    QNetworkReply *reply = m_nam.get(req);
    dl->reply = reply;

    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, dl]() {
        if (dl->cancelled)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300)
            return;
        const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
        if (cl.isValid()) {
            dl->bytesTotal = cl.toLongLong();
            if (dl->bytesTotal > kMaxBookBytes) {
                dl->sizeExceeded = true;
                reply->abort();
            }
        }
    });

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, dl]() {
        if (dl->cancelled)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300) {
            // Discard redirect/error HTML bodies without touching disk.
            reply->readAll();
            return;
        }

        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;

        dl->bytesReceived += chunk.size();
        if (dl->bytesReceived > kMaxBookBytes) {
            dl->sizeExceeded = true;
            reply->abort();
            return;
        }

        if (!dl->saveFile) {
            dl->headerBuffer.append(chunk);
            if (dl->headerBuffer.size() < 5)
                return;

            dl->verifiedFormat = detectBookMagicFormat(dl->headerBuffer);
            if (dl->verifiedFormat.isEmpty()) {
                dl->invalidMagic = true;
                reply->abort();
                return;
            }

            dl->destPath = buildDownloadPath(dl->catalogId, dl->title, dl->verifiedFormat, dl->acquisitionUrl);
            QDir().mkpath(QFileInfo(dl->destPath).absolutePath());
            dl->saveFile = std::make_unique<QSaveFile>(dl->destPath);
            if (!dl->saveFile->open(QIODevice::WriteOnly)) {
                dl->diskError = true;
                dl->saveFile.reset();
                reply->abort();
                return;
            }
            if (dl->saveFile->write(dl->headerBuffer) != dl->headerBuffer.size()) {
                dl->diskError = true;
                dl->saveFile->cancelWriting();
                dl->saveFile.reset();
                reply->abort();
                return;
            }
            dl->headerBuffer.clear();
        } else {
            if (dl->saveFile->write(chunk) != chunk.size()) {
                dl->diskError = true;
                dl->saveFile->cancelWriting();
                dl->saveFile.reset();
                reply->abort();
                return;
            }
        }

        if (dl->bytesTotal > 0) {
            const double p = std::clamp(double(dl->bytesReceived) / double(dl->bytesTotal), 0.0, 0.99);
            setDownloadState(dl->acquisitionUrl, QStringLiteral("downloading"), p);
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, dl]() {
        finishDownload(dl, reply);
    });
}

void OpdsClient::finishDownload(const std::shared_ptr<ActiveDownload> &dl, QNetworkReply *reply) {
    if (dl->reply == reply)
        dl->reply = nullptr;
    reply->deleteLater();

    if (dl->cancelled)
        return;

    auto fail = [&](const QString &message) {
        if (dl->saveFile) {
            dl->saveFile->cancelWriting();
            dl->saveFile.reset();
        }
        m_activeDownloads.remove(dl->acquisitionUrl);
        setDownloadState(dl->acquisitionUrl, QStringLiteral("error"), 0.0, 0, message);
        emit downloadFailed(dl->acquisitionUrl, message);
    };

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (status >= 200 && status < 300 && !dl->sizeExceeded && !dl->invalidMagic && !dl->diskError && reply->isOpen()) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            dl->bytesReceived += tail.size();
            if (dl->bytesReceived > kMaxBookBytes) {
                dl->sizeExceeded = true;
            } else if (!dl->saveFile) {
                dl->headerBuffer.append(tail);
                dl->verifiedFormat = detectBookMagicFormat(dl->headerBuffer);
                if (dl->verifiedFormat.isEmpty()) {
                    dl->invalidMagic = true;
                } else {
                    dl->destPath = buildDownloadPath(dl->catalogId, dl->title, dl->verifiedFormat, dl->acquisitionUrl);
                    QDir().mkpath(QFileInfo(dl->destPath).absolutePath());
                    dl->saveFile = std::make_unique<QSaveFile>(dl->destPath);
                    if (!dl->saveFile->open(QIODevice::WriteOnly) ||
                        dl->saveFile->write(dl->headerBuffer) != dl->headerBuffer.size()) {
                        dl->diskError = true;
                        dl->saveFile->cancelWriting();
                        dl->saveFile.reset();
                    } else {
                        dl->headerBuffer.clear();
                    }
                }
            } else if (dl->saveFile->write(tail) != tail.size()) {
                dl->diskError = true;
                dl->saveFile->cancelWriting();
                dl->saveFile.reset();
            }
        }
    }

    if (isRedirectStatus(status)) {
        if (dl->saveFile) {
            dl->saveFile->cancelWriting();
            dl->saveFile.reset();
        }
        const QByteArray loc = reply->rawHeader("Location").trimmed();
        const QUrl nextUrl = QUrl(dl->currentUrl).resolved(QUrl::fromEncoded(loc));
        if (loc.isEmpty() || !OpdsParser::isAllowedHttpUrl(nextUrl)) {
            fail(tr("Download redirected to an unsupported address."));
            return;
        }
        if (dl->hops >= kMaxRedirects) {
            fail(tr("Too many redirects while downloading this book."));
            return;
        }
        dl->hops += 1;
        dl->currentUrl = nextUrl.toString(QUrl::FullyEncoded);
        startDownloadRequest(dl);
        return;
    }

    if (status == 401) {
        if (dl->catalogId == m_activeCatalogId) {
            m_authRequired = true;
            emit feedChanged();
        }
        fail(tr("Sign in required to download from this catalog."));
        return;
    }

    if (dl->sizeExceeded) {
        fail(tr("Book exceeds the 200 MB download size limit."));
        return;
    }
    if (dl->invalidMagic) {
        fail(tr("Downloaded file is not a valid EPUB or PDF."));
        return;
    }
    if (dl->diskError) {
        fail(tr("Could not write the downloaded book to disk."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError || (status > 0 && (status < 200 || status >= 300))) {
        fail(friendlyNetworkError(reply, status));
        return;
    }

    // Flush any remaining tiny buffered payload (<5 bytes) or verify file was opened.
    if (!dl->saveFile) {
        fail(tr("Downloaded file is not a valid EPUB or PDF."));
        return;
    }

    if (!dl->saveFile->commit()) {
        fail(tr("Could not finalize the downloaded book file."));
        return;
    }
    dl->saveFile.reset();

    if (!m_library) {
        fail(tr("Library is unavailable."));
        return;
    }

    const int bookId = m_library->addBookFile(dl->destPath);
    if (bookId <= 0) {
        QFile::remove(dl->destPath);
        fail(m_library->errorString().isEmpty() ? tr("Could not add downloaded book to library.")
                                                : m_library->errorString());
        return;
    }

    if (dl->targetCollectionId > 0)
        m_library->addToCollection(bookId, dl->targetCollectionId);

    m_library->setBookSetting(bookId, QStringLiteral("source_url"), dl->acquisitionUrl);
    m_library->setBookSetting(bookId, QStringLiteral("opds_catalog"), dl->catalogName);

    m_activeDownloads.remove(dl->acquisitionUrl);
    setDownloadState(dl->acquisitionUrl, QStringLiteral("done"), 1.0, bookId);
    emit bookDownloaded(dl->acquisitionUrl, bookId, dl->title);
}

void OpdsClient::setDownloadState(const QString &acquisitionUrl, const QString &status,
                                  double progress, int bookId, const QString &error) {
    m_downloadStates.insert(acquisitionUrl, QVariantMap{
        {QStringLiteral("status"), status},
        {QStringLiteral("progress"), progress},
        {QStringLiteral("bookId"), bookId},
        {QStringLiteral("error"), error},
    });
    emit downloadStatesChanged();
}

QString OpdsClient::buildDownloadPath(const QString &catalogId, const QString &title,
                                      const QString &ext, const QString &acquisitionUrl) const {
    const QString catalogSlug = sanitizeSlug(catalogId, QStringLiteral("catalog"), 48);
    const QString dirPath = QDir(m_storageDir).filePath(QStringLiteral("downloads/") + catalogSlug);
    const QString baseName = sanitizeFilename(title, QStringLiteral("book"), 96);
    const QString suffix = QLatin1Char('.') + (ext == QLatin1String("pdf") ? QStringLiteral("pdf") : QStringLiteral("epub"));

    QString candidate = QDir(dirPath).filePath(baseName + suffix);
    if (!QFileInfo::exists(candidate))
        return candidate;

    // Reuse the existing path if it belongs to this exact OPDS acquisition URL.
    if (m_library) {
        const int existingId = m_library->idForPath(candidate);
        if (existingId > 0 && m_library->bookSetting(existingId, QStringLiteral("source_url")) == acquisitionUrl)
            return candidate;
    }

    const QString shortHash =
        QString::fromLatin1(QCryptographicHash::hash(acquisitionUrl.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
    candidate = QDir(dirPath).filePath(baseName + QLatin1Char('-') + shortHash + suffix);
    for (int n = 2; QFileInfo::exists(candidate) && n < 100; ++n)
        candidate = QDir(dirPath).filePath(QStringLiteral("%1-%2-%3%4").arg(baseName, shortHash).arg(n).arg(suffix));
    return candidate;
}
