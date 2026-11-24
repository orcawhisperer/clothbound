#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

enum class OpdsFormat {
    Unknown,
    Opds1Atom,
    Opds2Json,
};

struct OpdsLink {
    QString href;
    QString rel;
    QString type;
    QString title;
    QString format;
    QString price;
    bool openAccess = false;
    bool templated = false;

    QVariantMap toVariantMap() const;
};

struct OpdsEntry {
    QString id;
    QString title;
    QString subtitle;
    QString author;
    QStringList authors;
    QString summary;
    QString published;
    int year = 0;
    QString language;
    QString publisher;
    QStringList categories;
    QString coverUrl;
    QString thumbnailUrl;
    QString navigationUrl;
    QString navigationType;
    QString groupTitle;
    QString groupUrl;
    QList<OpdsLink> acquisitions;
    bool isNavigation = false;
    bool borrowOnly = false;

    QVariantMap toVariantMap() const;
};

struct OpdsGroup {
    QString title;
    QString selfUrl;
    int entryCount = 0;

    QVariantMap toVariantMap() const;
};

struct OpdsFeed {
    OpdsFormat format = OpdsFormat::Unknown;
    QString id;
    QString title;
    QString subtitle;
    QString selfUrl;
    QString startUrl;
    QString upUrl;
    QString nextUrl;
    QString searchUrl;
    QString searchType;
    bool searchTemplated = false;
    QList<OpdsLink> links;
    QList<OpdsEntry> entries;
    QList<OpdsGroup> groups;
    QString error;

    bool isValid() const { return error.isEmpty() && format != OpdsFormat::Unknown; }
    QVariantMap toVariantMap() const;
};

// Pure stateless OPDS 1.2 (Atom/XML) and OPDS 2.0 (JSON) catalog parser,
// plus OpenSearch description template extraction and expansion.
class OpdsParser {
public:
    static OpdsFormat detectFormat(const QByteArray &data, const QString &contentType = {});
    static OpdsFeed parse(const QByteArray &data, const QUrl &baseUrl, const QString &contentType = {});
    static OpdsFeed parseAtom(const QByteArray &xml, const QUrl &baseUrl);
    static OpdsFeed parseOpds2(const QByteArray &json, const QUrl &baseUrl);

    static QString parseOpenSearchTemplate(const QByteArray &xml, const QUrl &baseUrl);
    static QString expandSearchTemplate(const QString &templateUrl, const QString &query);

    static QString resolveHref(const QUrl &baseUrl, const QString &href, bool preserveTemplate = false);
    static QString stripHtmlToPlainText(const QString &html);
    static bool isAllowedHttpUrl(const QUrl &url);
};
