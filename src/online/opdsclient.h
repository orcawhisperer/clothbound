#pragma once

#include "online/opdsparser.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

class KeyStore;
class Library;
class QNetworkReply;

// OPDS 1.2 & OPDS 2.0 catalog browser and book downloader exposed to QML as `opds`.
// Keeps built-in curated DRM-free catalogs alongside user-added OPDS servers
// (Calibre-Web, Kavita, Komga, Ubooquity, Stump, etc.). Passwords live strictly
// in KeyStore (desktop keyring in production, MemoryKeyStore in tests) and are
// never written to settings.ini or SQLite.
class OpdsClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList catalogs READ catalogs NOTIFY catalogsChanged)
    Q_PROPERTY(QString activeCatalogId READ activeCatalogId NOTIFY feedChanged)
    Q_PROPERTY(QString activeCatalogName READ activeCatalogName NOTIFY feedChanged)
    Q_PROPERTY(QString activeCatalogUsername READ activeCatalogUsername NOTIFY feedChanged)
    Q_PROPERTY(QString feedTitle READ feedTitle NOTIFY feedChanged)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY feedChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY feedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY feedChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY feedChanged)
    Q_PROPERTY(bool canSearch READ canSearch NOTIFY feedChanged)
    Q_PROPERTY(bool authRequired READ authRequired NOTIFY feedChanged)
    Q_PROPERTY(QString authHint READ authHint NOTIFY feedChanged)
    Q_PROPERTY(QString error READ error NOTIFY feedChanged)
    Q_PROPERTY(QVariantMap downloadStates READ downloadStates NOTIFY downloadStatesChanged)
    Q_PROPERTY(bool hasHiddenBuiltIns READ hasHiddenBuiltIns NOTIFY catalogsChanged)

public:
    explicit OpdsClient(Library *library, KeyStore *keyStore, const QString &storageDir,
                        const QString &settingsPath = {}, QObject *parent = nullptr);
    ~OpdsClient() override;

    QVariantList catalogs() const;
    QString activeCatalogId() const { return m_activeCatalogId; }
    QString activeCatalogName() const;
    QString activeCatalogUsername() const;
    QString feedTitle() const { return m_feedTitle; }
    QVariantList breadcrumbs() const;
    QVariantList entries() const { return m_entries; }
    bool loading() const { return m_loading; }
    bool loadingMore() const { return m_loadingMore; }
    bool canGoBack() const { return m_history.size() > 1; }
    bool canLoadMore() const { return !m_nextUrl.isEmpty(); }
    bool canSearch() const { return !effectiveSearchUrl().isEmpty(); }
    bool authRequired() const { return m_authRequired; }
    QString authHint() const { return m_authHint; }
    QString error() const { return m_error; }
    QVariantMap downloadStates() const { return m_downloadStates; }
    bool hasHiddenBuiltIns() const { return !m_hiddenBuiltIns.isEmpty(); }

    Q_INVOKABLE QString addCatalog(const QString &name, const QString &url,
                                   const QString &username = {}, const QString &password = {});
    Q_INVOKABLE bool updateCatalogCredentials(const QString &catalogId,
                                              const QString &username, const QString &password);
    Q_INVOKABLE bool removeCatalog(const QString &catalogId);
    Q_INVOKABLE void restoreBuiltInCatalogs();

    Q_INVOKABLE void openCatalog(const QString &catalogId);
    Q_INVOKABLE void navigate(const QString &url, const QString &title = {});
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void navigateBreadcrumb(int index);
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void retry();

    Q_INVOKABLE void downloadPublication(const QString &acquisitionUrl, const QString &title,
                                         const QString &author, const QString &format,
                                         int targetCollectionId = 0);
    Q_INVOKABLE void cancelDownload(const QString &acquisitionUrl);

    static bool sameOrigin(const QUrl &a, const QUrl &b);

signals:
    void catalogsChanged();
    void feedChanged();
    void loadingChanged();
    void downloadStatesChanged();
    void bookDownloaded(const QString &acquisitionUrl, int bookId, const QString &title);
    void downloadFailed(const QString &acquisitionUrl, const QString &error);

private:
    struct Catalog {
        QString id;
        QString name;
        QString url;
        QString subtitle;
        QString username;
        bool builtIn = false;
        bool authOptional = false;
        QString authHint;
    };

    struct Breadcrumb {
        QString title;
        QString url;
    };

    struct ActiveDownload {
        QString acquisitionUrl;
        QString currentUrl;
        QUrl initialOrigin;
        QUrl catalogOrigin;
        QString catalogId;
        QString catalogName;
        QString title;
        QString author;
        QString requestedFormat;
        int targetCollectionId = 0;
        int hops = 0;
        qint64 bytesReceived = 0;
        qint64 bytesTotal = -1;
        QByteArray headerBuffer;
        QString verifiedFormat;
        QString destPath;
        std::unique_ptr<QSaveFile> saveFile;
        QPointer<QNetworkReply> reply;
        bool cancelled = false;
        bool sizeExceeded = false;
        bool invalidMagic = false;
        bool diskError = false;
    };

    void seedBuiltInCatalogs();
    void loadSettings();
    void saveSettings() const;
    void loadExistingDownloadStates();

    Catalog *findCatalog(const QString &catalogId);
    const Catalog *findCatalog(const QString &catalogId) const;
    QString effectiveSearchUrl() const;
    bool effectiveSearchTemplated() const;

    void abortActiveFeedRequest();
    void ensurePasswordLoaded(const QString &catalogId, std::function<void()> ready);
    void applyAuthIfAllowed(QNetworkRequest &request, const QUrl &targetUrl,
                            const QUrl &catalogOrigin, const QUrl &initialOrigin,
                            const QString &catalogId) const;

    void fetchFeed(const QUrl &url, bool appendMode, int hops = 0, const QUrl &initialOrigin = {});
    void fetchOpenSearchAndExecute(const QUrl &openSearchUrl, const QString &query,
                                   int hops = 0, const QUrl &initialOrigin = {});
    void startDownloadRequest(const std::shared_ptr<ActiveDownload> &dl);
    void finishDownload(const std::shared_ptr<ActiveDownload> &dl, QNetworkReply *reply);
    void setDownloadState(const QString &acquisitionUrl, const QString &status,
                          double progress, int bookId = 0, const QString &error = {});

    QString buildDownloadPath(const QString &catalogId, const QString &title,
                              const QString &ext, const QString &acquisitionUrl) const;
    static QString sanitizeSlug(const QString &raw, const QString &fallback, int maxLen = 48);
    static QString sanitizeFilename(const QString &raw, const QString &fallback, int maxLen = 96);
    static QString secretKeyFor(const QString &catalogId);

    Library *m_library = nullptr;
    KeyStore *m_keyStore = nullptr;
    QString m_storageDir;
    QString m_settingsPath;
    QNetworkAccessManager m_nam;

    QList<Catalog> m_builtInCatalogs;
    QList<Catalog> m_customCatalogs;
    QSet<QString> m_hiddenBuiltIns;
    QHash<QString, QString> m_passwordCache;
    QSet<QString> m_passwordLoaded;

    QString m_activeCatalogId;
    QString m_feedTitle;
    QList<Breadcrumb> m_history;
    QVariantList m_entries;
    QString m_currentUrl;
    QString m_nextUrl;
    QString m_searchUrl;
    bool m_searchTemplated = false;
    QString m_catalogSearchUrl;
    bool m_catalogSearchTemplated = false;
    QHash<QString, QString> m_openSearchTemplates;
    QString m_pendingSearchQuery;
    QUrl m_pendingOpenSearchUrl;

    bool m_loading = false;
    bool m_loadingMore = false;
    bool m_authRequired = false;
    QString m_authHint;
    QString m_error;
    quint64 m_feedGeneration = 0;
    QPointer<QNetworkReply> m_activeFeedReply;

    QVariantMap m_downloadStates;
    QHash<QString, std::shared_ptr<ActiveDownload>> m_activeDownloads;
};
