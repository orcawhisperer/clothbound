#include "online/opdsclient.h"

#include "ai/keystore.h"
#include "library/library.h"
#include "online/urlguard.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QRunnable>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <atomic>
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

QByteArray opdsUserAgent() {
    return QByteArrayLiteral("Clothbound/" CLOTHBOUND_VERSION " (OPDS)");
}

QString detectBookMagicFormat(const QByteArray &header) {
    if (header.size() >= 5 && header.startsWith("%PDF-"))
        return QStringLiteral("pdf");
    if (header.size() >= 4 && header.at(0) == 'P' && header.at(1) == 'K' &&
        header.at(2) == '\x03' && header.at(3) == '\x04')
        return QStringLiteral("epub");
    return {};
}

QString friendlyHttpError(const PinnedHttpResponse &resp) {
    const int httpStatus = resp.httpStatus;
    if (httpStatus == 403)
        return QObject::tr("Access to this catalog was forbidden (HTTP 403).");
    if (httpStatus == 404)
        return QObject::tr("This catalog section could not be found (HTTP 404).");
    if (httpStatus >= 500 && httpStatus < 600)
        return QObject::tr("The catalog server returned an error (HTTP %1).").arg(httpStatus);
    if (httpStatus >= 400)
        return QObject::tr("Catalog request failed (HTTP %1).").arg(httpStatus);
    if (!resp.error.isEmpty())
        return resp.error;
    return QObject::tr("Could not connect to the catalog server.");
}

QUrl rewriteStandardEbooksDownloadUrl(QUrl reqUrl) {
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
    return reqUrl;
}

class ImmediateOpdsCoverResponse : public QQuickImageResponse {
public:
    explicit ImmediateOpdsCoverResponse(QImage image, QString error = {})
        : m_image(std::move(image)), m_error(std::move(error)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

private:
    QImage m_image;
    QString m_error;
};

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
                         : settingsPath),
      m_localRegistry(std::make_shared<LocalCatalogRegistry>()) {
    seedBuiltInCatalogs();
    loadSettings();
    syncLocalRegistry();
    loadExistingDownloadStates();
}

OpdsClient::~OpdsClient() {
    abortActiveFeedRequest();
    const auto downloads = m_activeDownloads.values();
    m_activeDownloads.clear();
    for (const auto &dl : downloads) {
        dl->cancelled = true;
        if (dl->request) {
            dl->request->abort();
            dl->request->deleteLater();
            dl->request = nullptr;
        }
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
            false,
            false,
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
            false,
            false,
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
            false,
            false,
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
            false,
            false,
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
            const QUrl parsed(url);
            if (id.isEmpty() || !OpdsParser::isAllowedHttpUrl(parsed))
                continue;
            Catalog c;
            c.id = id;
            c.name = name.isEmpty() ? parsed.host() : name;
            c.url = url;
            c.subtitle = o.value(QStringLiteral("subtitle")).toString().trimmed();
            if (c.subtitle.isEmpty())
                c.subtitle = parsed.host();
            c.username = o.value(QStringLiteral("username")).toString().trimmed();
            c.builtIn = false;
            c.authOptional = true;
            c.allowLocalNetwork = o.contains(QStringLiteral("allowLocalNetwork"))
                                      ? o.value(QStringLiteral("allowLocalNetwork")).toBool(false)
                                      : UrlGuard::isLocalNetworkUrl(parsed);
            c.allowInsecureHttpAuth = o.value(QStringLiteral("allowInsecureHttpAuth")).toBool(false);
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
        o.insert(QStringLiteral("allowLocalNetwork"), c.allowLocalNetwork);
        if (c.allowInsecureHttpAuth)
            o.insert(QStringLiteral("allowInsecureHttpAuth"), true);
        customArr.append(o);
    }
    settings.setValue(QStringLiteral("customCatalogs"),
                      QJsonDocument(customArr).toJson(QJsonDocument::Compact));

    settings.endGroup();
    settings.sync();
}

void OpdsClient::syncLocalRegistry() {
    if (!m_localRegistry)
        return;
    QSet<QString> ids;
    for (const Catalog &c : m_builtInCatalogs) {
        if (c.allowLocalNetwork)
            ids.insert(c.id);
    }
    for (const Catalog &c : m_customCatalogs) {
        if (c.allowLocalNetwork)
            ids.insert(c.id);
    }
    m_localRegistry->resetFrom(ids);
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
            {QStringLiteral("allowLocalNetwork"), c.allowLocalNetwork},
            {QStringLiteral("allowInsecureHttpAuth"), c.allowInsecureHttpAuth},
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

bool OpdsClient::activeCatalogAllowsLocalNetwork() const {
    return catalogAllowsLocalNetwork(m_activeCatalogId);
}

bool OpdsClient::catalogAllowsLocalNetwork(const QString &catalogId) const {
    if (const Catalog *c = findCatalog(catalogId))
        return c->allowLocalNetwork;
    return false;
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
                               const QString &username, const QString &password,
                               bool allowLocalNetwork) {
    const QString trimmedUrl = url.trimmed();
    const QUrl parsed(trimmedUrl);
    if (!OpdsParser::isAllowedHttpUrl(parsed) || !UrlGuard::isAllowedUrl(parsed, true)) {
        m_error = tr("Catalog URL must start with http:// or https://.");
        emit feedChanged();
        return {};
    }

    const bool effectiveAllowLocal = allowLocalNetwork || UrlGuard::isLocalNetworkUrl(parsed);
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(parsed, effectiveAllowLocal, &guardReason)) {
        m_error = guardReason;
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
    cat.allowLocalNetwork = effectiveAllowLocal;
    cat.allowInsecureHttpAuth = false;
    m_customCatalogs.append(cat);

    if (m_localRegistry)
        m_localRegistry->setCatalog(id, cat.allowLocalNetwork);
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

bool OpdsClient::setCatalogAllowInsecureHttpAuth(const QString &catalogId, bool allow) {
    Catalog *cat = findCatalog(catalogId);
    if (!cat)
        return false;
    if (cat->allowInsecureHttpAuth == allow)
        return true;
    cat->allowInsecureHttpAuth = allow;
    saveSettings();
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

    if (m_localRegistry)
        m_localRegistry->setCatalog(catalogId, false);
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
    if (m_activeFeedRequest) {
        QPointer<PinnedHttpRequest> r = m_activeFeedRequest;
        m_activeFeedRequest = nullptr;
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

    auto &pending = m_pendingPasswordCallbacks[catalogId];
    pending.append(std::move(ready));
    if (pending.size() > 1)
        return;  // Keyring lookup for this catalogId is already in flight.

    auto deliver = [this, catalogId](const QString &secret) {
        m_passwordLoaded.insert(catalogId);
        if (!secret.isEmpty())
            m_passwordCache.insert(catalogId, secret);
        else
            m_passwordCache.remove(catalogId);

        const auto callbacks = m_pendingPasswordCallbacks.take(catalogId);
        for (const auto &cb : callbacks) {
            if (cb)
                cb();
        }
    };

    QFuture<QString> future = m_keyStore->loadLater(secretKeyFor(catalogId));
    if (future.isFinished()) {
        deliver(future.result());
        return;
    }

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [watcher, deliver = std::move(deliver)]() mutable {
                const QString secret = watcher->result();
                watcher->deleteLater();
                deliver(secret);
            });
    watcher->setFuture(future);
}

QList<QPair<QByteArray, QByteArray>> OpdsClient::authHeadersIfAllowed(const QUrl &targetUrl,
                                                                      const QUrl &catalogOrigin,
                                                                      const QUrl &initialOrigin,
                                                                      const QString &catalogId) const {
    const Catalog *cat = findCatalog(catalogId);
    if (!cat)
        return {};
    const QString user = cat->username;
    const QString pass = m_passwordCache.value(catalogId);
    if (user.isEmpty() && pass.isEmpty())
        return {};

    // Security invariant 1: never transmit credentials to any origin other than the catalog's own.
    if (!sameOrigin(targetUrl, catalogOrigin))
        return {};
    if (initialOrigin.isValid() && !sameOrigin(targetUrl, initialOrigin))
        return {};

    // Security invariant 2: block sending Basic/Bearer credentials over non-loopback plaintext http://
    // unless explicitly opted-in for this catalog.
    const bool isHttps = targetUrl.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    if (!isHttps && !UrlGuard::isLoopbackUrl(targetUrl) && !cat->allowInsecureHttpAuth)
        return {};

    const QByteArray token = (user + QLatin1Char(':') + pass).toUtf8().toBase64();
    return {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Basic ") + token}};
}

void OpdsClient::openCatalog(const QString &catalogId) {
    const Catalog *cat = findCatalog(catalogId);
    if (!cat) {
        m_error = tr("Selected catalog was not found.");
        emit feedChanged();
        return;
    }

    const QUrl url(cat->url);
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, cat->allowLocalNetwork, &guardReason)) {
        m_error = guardReason;
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
    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(cid, [this, url, gen]() {
        if (gen == m_feedGeneration)
            fetchFeed(url, false, url);
    });
}

void OpdsClient::navigate(const QString &url, const QString &title) {
    const QUrl target(url.trimmed());
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(target, activeCatalogAllowsLocalNetwork(), &guardReason)) {
        m_error = guardReason;
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

    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(m_activeCatalogId, [this, target, gen]() {
        if (gen == m_feedGeneration)
            fetchFeed(target, false, target);
    });
}

void OpdsClient::goBack() {
    if (m_history.size() <= 1)
        return;
    m_history.removeLast();
    const QUrl target(m_history.last().url);
    if (!UrlGuard::isAllowedUrl(target, activeCatalogAllowsLocalNetwork()))
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

    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(m_activeCatalogId, [this, target, gen]() {
        if (gen == m_feedGeneration)
            fetchFeed(target, false, target);
    });
}

void OpdsClient::navigateBreadcrumb(int index) {
    if (index < 0 || index >= m_history.size() - 1)
        return;
    m_history.resize(index + 1);
    const QUrl target(m_history.last().url);
    if (!UrlGuard::isAllowedUrl(target, activeCatalogAllowsLocalNetwork()))
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

    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(m_activeCatalogId, [this, target, gen]() {
        if (gen == m_feedGeneration)
            fetchFeed(target, false, target);
    });
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
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(osUrl, activeCatalogAllowsLocalNetwork(), &guardReason)) {
        m_error = guardReason;
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

    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(m_activeCatalogId, [this, osUrl, trimmed, gen]() {
        if (gen == m_feedGeneration)
            fetchOpenSearchAndExecute(osUrl, trimmed, osUrl);
    });
}

void OpdsClient::loadMore() {
    if (m_nextUrl.isEmpty() || m_loading || m_loadingMore)
        return;
    const QUrl next(m_nextUrl);
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(next, activeCatalogAllowsLocalNetwork(), &guardReason)) {
        m_error = guardReason;
        emit feedChanged();
        return;
    }

    abortActiveFeedRequest();
    m_error.clear();
    m_loading = false;
    m_loadingMore = true;
    emit loadingChanged();

    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(m_activeCatalogId, [this, next, gen]() {
        if (gen == m_feedGeneration)
            fetchFeed(next, true, next);
    });
}

void OpdsClient::retry() {
    if (!m_pendingSearchQuery.isEmpty() && m_pendingOpenSearchUrl.isValid()) {
        const QUrl osUrl = m_pendingOpenSearchUrl;
        const QString query = m_pendingSearchQuery;
        QString guardReason;
        if (!UrlGuard::isAllowedUrl(osUrl, activeCatalogAllowsLocalNetwork(), &guardReason)) {
            m_error = guardReason;
            emit feedChanged();
            return;
        }
        abortActiveFeedRequest();
        m_authRequired = false;
        m_error.clear();
        m_loading = true;
        m_loadingMore = false;
        emit loadingChanged();
        emit feedChanged();
        const quint64 gen = m_feedGeneration;
        ensurePasswordLoaded(m_activeCatalogId, [this, osUrl, query, gen]() {
            if (gen == m_feedGeneration)
                fetchOpenSearchAndExecute(osUrl, query, osUrl);
        });
        return;
    }

    if (m_currentUrl.isEmpty()) {
        if (!m_activeCatalogId.isEmpty())
            openCatalog(m_activeCatalogId);
        return;
    }

    const QUrl target(m_currentUrl);
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(target, activeCatalogAllowsLocalNetwork(), &guardReason)) {
        m_error = guardReason;
        emit feedChanged();
        return;
    }

    abortActiveFeedRequest();
    m_authRequired = false;
    m_error.clear();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    emit feedChanged();

    const quint64 gen = m_feedGeneration;
    ensurePasswordLoaded(m_activeCatalogId, [this, target, gen]() {
        if (gen == m_feedGeneration)
            fetchFeed(target, false, target);
    });
}

void OpdsClient::fetchFeed(const QUrl &url, bool appendMode, const QUrl &initialOrigin) {
    const Catalog *cat = findCatalog(m_activeCatalogId);
    const bool allowLocal = cat ? cat->allowLocalNetwork : false;
    const QUrl catalogOrigin = cat ? QUrl(cat->url) : url;
    const QString catalogId = m_activeCatalogId;

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, allowLocal, &guardReason)) {
        m_loading = false;
        m_loadingMore = false;
        m_error = guardReason;
        emit loadingChanged();
        emit feedChanged();
        return;
    }

    const quint64 gen = m_feedGeneration;
    PinnedHttpOptions opts;
    opts.url = url;
    opts.allowLocalNetwork = allowLocal;
    opts.maxBytes = kMaxFeedBytes;
    opts.maxRedirects = kMaxRedirects;
    opts.timeoutMs = kTransferTimeoutMs;
    opts.userAgent = opdsUserAgent();
    opts.acceptHeader = QByteArrayLiteral(
        "application/opds+json, application/opds-publication+json, "
        "application/atom+xml;profile=opds-catalog, application/atom+xml, application/xml;q=0.9, */*;q=0.5");
    opts.headersForHop = [this, catalogOrigin, initialOrigin, catalogId](const QUrl &hopUrl) {
        return authHeadersIfAllowed(hopUrl, catalogOrigin, initialOrigin, catalogId);
    };

    auto reqPtr = std::make_shared<QPointer<PinnedHttpRequest>>();
    PinnedHttpRequest *req = UrlGuard::fetchPinned(
        std::move(opts),
        [this, reqPtr, url, appendMode, gen](PinnedHttpResponse resp) {
            if (*reqPtr) {
                if (m_activeFeedRequest == *reqPtr)
                    m_activeFeedRequest = nullptr;
                (*reqPtr)->deleteLater();
            }
            if (gen != m_feedGeneration)
                return;

            if (resp.sizeExceeded) {
                m_loading = false;
                m_loadingMore = false;
                m_error = tr("Catalog feed exceeds the 10 MB size limit.");
                emit loadingChanged();
                emit feedChanged();
                return;
            }
            if (resp.tooManyRedirects) {
                m_loading = false;
                m_loadingMore = false;
                m_error = tr("Too many redirects while loading this catalog.");
                emit loadingChanged();
                emit feedChanged();
                return;
            }

            if (resp.ok && resp.httpStatus == 401) {
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

            if (!resp.ok || (resp.httpStatus > 0 && (resp.httpStatus < 200 || resp.httpStatus >= 300))) {
                m_loading = false;
                m_loadingMore = false;
                m_error = friendlyHttpError(resp);
                emit loadingChanged();
                emit feedChanged();
                return;
            }

            const QUrl effectiveUrl = resp.finalUrl.isValid() ? resp.finalUrl : url;
            const OpdsFeed parsed = OpdsParser::parse(resp.body, effectiveUrl, resp.contentType);
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

            const QString reqEncoded = effectiveUrl.toString(QUrl::FullyEncoded);
            m_nextUrl = (!parsed.nextUrl.isEmpty() && parsed.nextUrl != reqEncoded) ? parsed.nextUrl : QString();

            emit loadingChanged();
            emit feedChanged();
        },
        this);
    *reqPtr = req;
    if (!req->isFinished())
        m_activeFeedRequest = req;
}

void OpdsClient::fetchOpenSearchAndExecute(const QUrl &openSearchUrl, const QString &query,
                                           const QUrl &initialOrigin) {
    const Catalog *cat = findCatalog(m_activeCatalogId);
    const bool allowLocal = cat ? cat->allowLocalNetwork : false;
    const QUrl catalogOrigin = cat ? QUrl(cat->url) : openSearchUrl;
    const QString catalogId = m_activeCatalogId;

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(openSearchUrl, allowLocal, &guardReason)) {
        m_loading = false;
        m_error = guardReason;
        emit loadingChanged();
        emit feedChanged();
        return;
    }

    const quint64 gen = m_feedGeneration;
    PinnedHttpOptions opts;
    opts.url = openSearchUrl;
    opts.allowLocalNetwork = allowLocal;
    opts.maxBytes = kMaxFeedBytes;
    opts.maxRedirects = kMaxRedirects;
    opts.timeoutMs = kTransferTimeoutMs;
    opts.userAgent = opdsUserAgent();
    opts.acceptHeader = QByteArrayLiteral("application/opensearchdescription+xml, application/xml, text/xml;q=0.9, */*;q=0.5");
    opts.headersForHop = [this, catalogOrigin, initialOrigin, catalogId](const QUrl &hopUrl) {
        return authHeadersIfAllowed(hopUrl, catalogOrigin, initialOrigin, catalogId);
    };

    auto reqPtr = std::make_shared<QPointer<PinnedHttpRequest>>();
    PinnedHttpRequest *req = UrlGuard::fetchPinned(
        std::move(opts),
        [this, reqPtr, openSearchUrl, query, initialOrigin, gen](PinnedHttpResponse resp) {
            if (*reqPtr) {
                if (m_activeFeedRequest == *reqPtr)
                    m_activeFeedRequest = nullptr;
                (*reqPtr)->deleteLater();
            }
            if (gen != m_feedGeneration)
                return;

            if (resp.sizeExceeded) {
                m_loading = false;
                m_error = tr("OpenSearch description exceeds the 10 MB size limit.");
                emit loadingChanged();
                emit feedChanged();
                return;
            }
            if (resp.tooManyRedirects) {
                m_loading = false;
                m_error = tr("Could not follow catalog search redirect.");
                emit loadingChanged();
                emit feedChanged();
                return;
            }
            if (resp.ok && resp.httpStatus == 401) {
                m_loading = false;
                m_authRequired = true;
                m_error.clear();
                emit loadingChanged();
                emit feedChanged();
                return;
            }
            if (!resp.ok || (resp.httpStatus > 0 && (resp.httpStatus < 200 || resp.httpStatus >= 300))) {
                m_loading = false;
                m_error = friendlyHttpError(resp);
                emit loadingChanged();
                emit feedChanged();
                return;
            }

            const QUrl effectiveUrl = resp.finalUrl.isValid() ? resp.finalUrl : openSearchUrl;
            const QString tmpl = OpdsParser::parseOpenSearchTemplate(resp.body, effectiveUrl);
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
        },
        this);
    *reqPtr = req;
    if (!req->isFinished())
        m_activeFeedRequest = req;
}

void OpdsClient::downloadPublication(const QString &acquisitionUrl, const QString &title,
                                     const QString &author, const QString &format,
                                     int targetCollectionId) {
    const QString key = acquisitionUrl.trimmed();
    const QUrl url(key);
    const Catalog *cat = findCatalog(m_activeCatalogId);
    const bool allowLocal = cat ? cat->allowLocalNetwork : false;

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, allowLocal, &guardReason)) {
        setDownloadState(key, QStringLiteral("error"), 0.0, 0, guardReason);
        emit downloadFailed(key, guardReason);
        return;
    }

    if (m_activeDownloads.contains(key))
        return;

    auto dl = std::make_shared<ActiveDownload>();
    dl->acquisitionUrl = key;
    dl->currentUrl = key;
    dl->initialOrigin = url;
    dl->catalogOrigin = cat ? QUrl(cat->url) : url;
    dl->catalogId = m_activeCatalogId.isEmpty() ? QStringLiteral("catalog") : m_activeCatalogId;
    dl->catalogName = cat ? cat->name : url.host();
    dl->allowLocalNetwork = allowLocal;
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
    if (dl->request) {
        dl->request->abort();
        dl->request->deleteLater();
        dl->request = nullptr;
    }
    if (dl->saveFile) {
        dl->saveFile->cancelWriting();
        dl->saveFile.reset();
    }

    m_downloadStates.remove(key);
    emit downloadStatesChanged();
}

void OpdsClient::startDownloadRequest(const std::shared_ptr<ActiveDownload> &dl) {
    const QUrl reqUrl(dl->currentUrl);
    QString guardReason;
    if (!UrlGuard::isAllowedUrl(reqUrl, dl->allowLocalNetwork, &guardReason)) {
        m_activeDownloads.remove(dl->acquisitionUrl);
        setDownloadState(dl->acquisitionUrl, QStringLiteral("error"), 0.0, 0, guardReason);
        emit downloadFailed(dl->acquisitionUrl, guardReason);
        return;
    }

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

    auto isHttpSuccess = std::make_shared<bool>(false);

    PinnedHttpOptions opts;
    opts.url = reqUrl;
    opts.allowLocalNetwork = dl->allowLocalNetwork;
    opts.maxBytes = kMaxBookBytes;
    opts.maxRedirects = kMaxRedirects;
    opts.timeoutMs = kTransferTimeoutMs;
    opts.userAgent = opdsUserAgent();
    opts.acceptHeader = QByteArrayLiteral("application/epub+zip, application/pdf, application/octet-stream;q=0.8, */*;q=0.5");
    opts.rewriteHopUrl = [](const QUrl &hopUrl) { return rewriteStandardEbooksDownloadUrl(hopUrl); };
    opts.headersForHop = [this, dl](const QUrl &hopUrl) {
        return authHeadersIfAllowed(hopUrl, dl->catalogOrigin, dl->initialOrigin, dl->catalogId);
    };

    opts.onHeaders = [dl, isHttpSuccess](int statusCode, const QUrl &, const QList<QPair<QByteArray, QByteArray>> &,
                                         qint64 contentLength, qint64 *, QString *) {
        if (dl->cancelled)
            return false;
        *isHttpSuccess = (statusCode >= 200 && statusCode < 300);
        if (!*isHttpSuccess)
            return true;
        if (contentLength >= 0) {
            dl->bytesTotal = contentLength;
            if (dl->bytesTotal > kMaxBookBytes) {
                dl->sizeExceeded = true;
                return false;
            }
        }
        return true;
    };

    opts.onBodyChunk = [this, dl, isHttpSuccess](const QByteArray &chunk, qint64 wireBytes, qint64, QString *) {
        if (dl->cancelled)
            return false;
        if (!*isHttpSuccess)
            return true;  // Discard error HTML bodies without touching disk.
        if (chunk.isEmpty())
            return true;

        dl->bytesReceived += chunk.size();
        if (dl->bytesReceived > kMaxBookBytes) {
            dl->sizeExceeded = true;
            return false;
        }

        if (!dl->saveFile) {
            dl->headerBuffer.append(chunk);
            if (dl->headerBuffer.size() < 5)
                return true;

            dl->verifiedFormat = detectBookMagicFormat(dl->headerBuffer);
            if (dl->verifiedFormat.isEmpty()) {
                dl->invalidMagic = true;
                return false;
            }

            dl->destPath = buildDownloadPath(dl->catalogId, dl->title, dl->verifiedFormat, dl->acquisitionUrl);
            QDir().mkpath(QFileInfo(dl->destPath).absolutePath());
            dl->saveFile = std::make_unique<QSaveFile>(dl->destPath);
            if (!dl->saveFile->open(QIODevice::WriteOnly) ||
                dl->saveFile->write(dl->headerBuffer) != dl->headerBuffer.size()) {
                dl->diskError = true;
                dl->saveFile->cancelWriting();
                dl->saveFile.reset();
                return false;
            }
            dl->headerBuffer.clear();
        } else {
            if (dl->saveFile->write(chunk) != chunk.size()) {
                dl->diskError = true;
                dl->saveFile->cancelWriting();
                dl->saveFile.reset();
                return false;
            }
        }

        if (dl->bytesTotal > 0) {
            const double p = std::clamp(double(wireBytes) / double(dl->bytesTotal), 0.0, 0.99);
            setDownloadState(dl->acquisitionUrl, QStringLiteral("downloading"), p);
        }
        return true;
    };

    auto reqPtr = std::make_shared<QPointer<PinnedHttpRequest>>();
    PinnedHttpRequest *req = UrlGuard::fetchPinned(
        std::move(opts),
        [this, reqPtr, dl](PinnedHttpResponse resp) {
            if (*reqPtr) {
                if (dl->request == *reqPtr)
                    dl->request = nullptr;
                (*reqPtr)->deleteLater();
            }
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

            if (resp.ok && resp.httpStatus == 401) {
                if (dl->catalogId == m_activeCatalogId) {
                    m_authRequired = true;
                    emit feedChanged();
                }
                fail(tr("Sign in required to download from this catalog."));
                return;
            }
            if (dl->sizeExceeded || resp.sizeExceeded) {
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
            if (resp.tooManyRedirects) {
                fail(tr("Too many redirects while downloading this book."));
                return;
            }
            if (!resp.ok || (resp.httpStatus > 0 && (resp.httpStatus < 200 || resp.httpStatus >= 300))) {
                fail(friendlyHttpError(resp));
                return;
            }

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
        },
        this);
    *reqPtr = req;
    if (!req->isFinished())
        dl->request = req;
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

QString OpdsClient::coverImageUrl(const QString &coverUrl, const QString &catalogId) const {
    const QString trimmed = coverUrl.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QString cid = catalogId.trimmed().isEmpty() ? m_activeCatalogId : catalogId.trimmed();
    const bool allowLocal = catalogAllowsLocalNetwork(cid);
    if (!UrlGuard::isAllowedUrl(QUrl(trimmed), allowLocal))
        return {};
    return OpdsCoverProvider::imageUrl(trimmed, cid);
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

namespace {

class OpdsCoverResponse : public QQuickImageResponse, public QRunnable {
public:
    OpdsCoverResponse(QString cacheKey, QUrl url, bool allowLocalNetwork, QSize requestedSize,
                      std::function<QImage(const QString &)> cacheGet,
                      std::function<void(const QString &, const QImage &)> cachePut)
        : m_cacheKey(std::move(cacheKey)),
          m_url(std::move(url)),
          m_allowLocalNetwork(allowLocalNetwork),
          m_requestedSize(requestedSize),
          m_cacheGet(std::move(cacheGet)),
          m_cachePut(std::move(cachePut)) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    void cancel() override { m_cancelled.store(true, std::memory_order_relaxed); }

    void run() override {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished();
            return;
        }
        if (m_cacheGet) {
            if (const QImage hit = m_cacheGet(m_cacheKey); !hit.isNull()) {
                m_image = hit;
                emit finished();
                return;
            }
        }

        QString guardReason;
        if (!UrlGuard::isAllowedUrl(m_url, m_allowLocalNetwork, &guardReason)) {
            m_error = guardReason;
            emit finished();
            return;
        }

        PinnedHttpOptions opts;
        opts.url = m_url;
        opts.allowLocalNetwork = m_allowLocalNetwork;
        opts.maxBytes = OpdsCoverProvider::maxCoverBytes;
        opts.maxRedirects = UrlGuard::maxRedirects;
        opts.timeoutMs = OpdsCoverProvider::defaultTimeoutMs;
        opts.userAgent = opdsUserAgent();
        opts.acceptHeader = QByteArrayLiteral("image/webp, image/png, image/jpeg, image/gif, */*;q=0.1");

        const PinnedHttpResponse resp = UrlGuard::fetchPinnedSync(std::move(opts), &m_cancelled);
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished();
            return;
        }
        if (!resp.ok || resp.httpStatus < 200 || resp.httpStatus >= 300 || resp.body.isEmpty()) {
            m_error = resp.error.isEmpty() ? QStringLiteral("Could not load cover image.") : resp.error;
            emit finished();
            return;
        }

        // Reject SVG payloads in OPDS cover thumbnails (raster formats only).
        const QString ctLower = resp.contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        const QByteArray headLower = resp.body.left(256).trimmed().toLower();
        if (ctLower.contains(QLatin1String("svg")) || headLower.startsWith("<svg") ||
            headLower.contains("<svg")) {
            m_error = QStringLiteral("SVG cover thumbnails are not allowed.");
            emit finished();
            return;
        }

        QByteArray data = resp.body;
        QBuffer buf(&data);
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);

        const QByteArray fmt = reader.format().toLower();
        if (fmt == "svg" || fmt == "svgz" || !reader.canRead()) {
            m_error = QStringLiteral("Unsupported cover image format.");
            emit finished();
            return;
        }

        const QSize nativeSize = reader.size();
        if (!nativeSize.isValid() || nativeSize.width() <= 0 || nativeSize.height() <= 0 ||
            nativeSize.width() > OpdsCoverProvider::maxImageDimension ||
            nativeSize.height() > OpdsCoverProvider::maxImageDimension ||
            qint64(nativeSize.width()) * qint64(nativeSize.height()) > OpdsCoverProvider::maxImagePixels) {
            m_error = QStringLiteral("Cover image dimensions exceed safety limits.");
            emit finished();
            return;
        }

        const int maxW = (m_requestedSize.width() > 0) ? qMin(m_requestedSize.width(), 1024) : 512;
        const int maxH = (m_requestedSize.height() > 0) ? qMin(m_requestedSize.height(), 1024) : 768;
        if (nativeSize.width() > maxW || nativeSize.height() > maxH) {
            const QSize scaled = nativeSize.scaled(maxW, maxH, Qt::KeepAspectRatio);
            if (scaled.isValid() && !scaled.isEmpty())
                reader.setScaledSize(scaled);
        }

        const QImage decoded = reader.read();
        if (decoded.isNull() || decoded.width() > OpdsCoverProvider::maxImageDimension ||
            decoded.height() > OpdsCoverProvider::maxImageDimension) {
            m_error = QStringLiteral("Could not decode cover image.");
            emit finished();
            return;
        }

        if (!m_cancelled.load(std::memory_order_relaxed)) {
            if (m_cachePut)
                m_cachePut(m_cacheKey, decoded);
            m_image = decoded;
        }
        emit finished();
    }

private:
    QString m_cacheKey;
    QUrl m_url;
    bool m_allowLocalNetwork = false;
    QSize m_requestedSize;
    std::function<QImage(const QString &)> m_cacheGet;
    std::function<void(const QString &, const QImage &)> m_cachePut;
    std::atomic_bool m_cancelled{false};
    QImage m_image;
    QString m_error;
};

}  // namespace

OpdsCoverProvider::OpdsCoverProvider(const OpdsClient *client)
    : m_localRegistry(client ? client->m_localRegistry : nullptr),
      m_cache(std::make_shared<CoverCache>()) {
    m_pool.setMaxThreadCount(4);
}

OpdsCoverProvider::~OpdsCoverProvider() {
    m_pool.waitForDone(3000);
}

QString OpdsCoverProvider::imageUrl(const QString &coverUrl, const QString &catalogId) {
    const QString trimmed = coverUrl.trimmed();
    if (trimmed.isEmpty())
        return {};
    QString out = QStringLiteral("image://opdscover/") +
                  QString::fromLatin1(QUrl::toPercentEncoding(trimmed));
    const QString cid = catalogId.trimmed();
    if (!cid.isEmpty()) {
        out += QStringLiteral("?catalog=") + QString::fromLatin1(QUrl::toPercentEncoding(cid));
    }
    return out;
}

QQuickImageResponse *OpdsCoverProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // Supported formats:
    //   "<percent-encoded-url>?catalog=<percent-encoded-catalog-id>"
    //   "?url=<percent-encoded-url>&catalog=<percent-encoded-catalog-id>"
    const qsizetype qPos = id.indexOf(QLatin1Char('?'));
    const QString encodedPath = qPos >= 0 ? id.left(qPos) : id;
    const QUrlQuery query(qPos >= 0 ? id.mid(qPos + 1) : QString());

    const QString rawUrl = encodedPath.isEmpty()
                               ? QUrl::fromPercentEncoding(
                                     query.queryItemValue(QStringLiteral("url"), QUrl::FullyEncoded).toUtf8())
                               : QUrl::fromPercentEncoding(encodedPath.toUtf8());
    const QString catalogId = QUrl::fromPercentEncoding(
        query.queryItemValue(QStringLiteral("catalog"), QUrl::FullyEncoded).toUtf8());

    const QUrl url(rawUrl.trimmed());
    const bool allowLocal = m_localRegistry && m_localRegistry->allowsLocal(catalogId);

    QString guardReason;
    if (!UrlGuard::isAllowedUrl(url, allowLocal, &guardReason))
        return new ImmediateOpdsCoverResponse({}, guardReason);

    const QString cacheKey = QStringLiteral("%1|%2|%3x%4")
                                 .arg(allowLocal ? QStringLiteral("1") : QStringLiteral("0"),
                                      url.toString(QUrl::FullyEncoded))
                                 .arg(requestedSize.width())
                                 .arg(requestedSize.height());
    if (const QImage hit = m_cache->get(cacheKey); !hit.isNull())
        return new ImmediateOpdsCoverResponse(hit);

    auto cache = m_cache;
    auto *response = new OpdsCoverResponse(
        cacheKey, url, allowLocal, requestedSize,
        [cache](const QString &k) { return cache->get(k); },
        [cache](const QString &k, const QImage &img) { cache->insert(k, img); });
    m_pool.start(response);
    return response;
}
