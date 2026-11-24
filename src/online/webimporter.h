#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <functional>

class Library;
class QNetworkReply;

// Online-to-offline URL importer for both QML (`webImporter`) and headless CLI
// (`clothbound add <url>`). Fetches arXiv papers (PDF + Atom metadata), direct
// EPUB/PDF downloads, and web articles (packaged offline into EPUB 3.0 via
// ArticleExtractor + EpubWriter), enforcing UrlGuard on every hop and image.
class WebImporter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString detectedKind READ detectedKind NOTIFY detectedKindChanged)

public:
    explicit WebImporter(Library *library, const QString &storageDir = {}, QObject *parent = nullptr);
    ~WebImporter() override;

    bool busy() const { return m_busy; }
    QString statusText() const { return m_statusText; }
    double progress() const { return m_progress; }
    QString lastError() const { return m_lastError; }
    QString detectedKind() const { return m_detectedKind; }

    // Pure classification and arXiv helpers, usable from both C++ and QML.
    Q_INVOKABLE static QString detectUrlKind(const QString &rawUrl);
    Q_INVOKABLE static QString parseArxivId(const QString &rawUrl);
    Q_INVOKABLE static QVariantMap parseArxivAtom(const QByteArray &xml);
    static QVariantMap parseCitationMetaHtml(const QByteArray &html);

    // Returns a trimmed http(s):// or arxiv: URL from the system clipboard when
    // running in a GUI session, or an empty string otherwise.
    Q_INVOKABLE QString clipboardUrl() const;

    Q_INVOKABLE void importUrl(const QString &rawUrl, int targetCollectionId = 0);
    Q_INVOKABLE void cancel();

    // Blocking convenience wrapper for CLI (`clothbound add <url>`) and tests.
    bool importUrlSync(const QString &rawUrl, int *bookIdOut = nullptr, QString *kindOut = nullptr,
                       QString *errorOut = nullptr, int targetCollectionId = 0);

    // Test-only hooks for local QTcpServer integration suites.
    void setAllowLocalNetworkForTests(bool allow) { m_allowLocalNetwork = allow; }
    void setArxivEndpointsForTests(const QString &apiBaseUrl, const QString &pdfBaseUrl,
                                   const QString &absBaseUrl = {});

signals:
    void busyChanged();
    void statusChanged();
    void progressChanged();
    void lastErrorChanged();
    void detectedKindChanged();
    void finished(int bookId, const QString &title, const QString &kind, const QString &sourceUrl);
    void failed(const QString &error);
    void cancelled();

private:
    struct FetchResult {
        bool ok = false;
        QUrl finalUrl;
        QString contentType;
        QString contentDisposition;
        QByteArray data;
        QString error;
    };

    using FetchCallback = std::function<void(FetchResult)>;

    void setBusy(bool busy);
    void setStatus(const QString &status);
    void setProgress(double progress);
    void setLastError(const QString &error);
    void setDetectedKind(const QString &kind);
    void completeSuccess(int bookId, const QString &title, const QString &kind, const QString &sourceUrl);
    void completeFailure(const QString &error);
    void abortActiveOperations();

    void fetchUrl(const QUrl &url, qint64 maxBytes, bool allowBookCapUpgrade, bool trackProgress, int redirectCount,
                  quint64 generation, const QByteArray &acceptHeader, FetchCallback callback, int timeoutMs = 30000);
    void startHttpRequest(const QUrl &url, qint64 maxBytes, bool allowBookCapUpgrade, bool trackProgress,
                          int redirectCount, quint64 generation, const QByteArray &acceptHeader,
                          FetchCallback callback, int timeoutMs = 30000);

    void runArxivImport(const QString &arxivId, int targetCollectionId, quint64 generation);
    void downloadArxivPdf(const QString &arxivId, const QVariantMap &meta, int targetCollectionId, quint64 generation);
    void runDirectOrArticleImport(const QUrl &initialUrl, const QString &initialKind, int targetCollectionId,
                                  quint64 generation);
    void finishDirectBook(const QString &format, const FetchResult &res, int targetCollectionId,
                          const QVariantMap &citationMeta = {}, const QString &sourceUrlOverride = {});
    void finishWebArticle(const FetchResult &res, int targetCollectionId, quint64 generation);

    QString downloadsDir() const;
    QString articlesDir() const;

    QPointer<Library> m_library;
    QString m_storageDir;
    QNetworkAccessManager m_nam;
    QList<QPointer<QNetworkReply>> m_activeReplies;
    QList<int> m_activeLookups;
    quint64 m_generation = 0;

    bool m_busy = false;
    QString m_statusText;
    double m_progress = 0.0;
    QString m_lastError;
    QString m_detectedKind;
    bool m_allowLocalNetwork = false;
    QString m_arxivApiBase = QStringLiteral("https://export.arxiv.org/api/query?id_list=");
    QString m_arxivAbsBase = QStringLiteral("https://arxiv.org/abs/");
    QString m_arxivPdfBase = QStringLiteral("https://arxiv.org/pdf/");
};
