#pragma once

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLocale>
#include <QReadWriteLock>
#include <QTimer>
#include <QTranslator>
#include <atomic>
#include <memory>

// Follows the language picked under Omarchy's Setup > Language. Catalogs are
// flat JSON objects from English source text to translation, one per Omarchy
// language code (i18n/da.json, i18n/pt-BR.json), so QML keeps plain
// qsTr("Save") and C++ keeps tr("Save"). A counted string,
// qsTr("%n clips", "", count), maps to an object of plural forms keyed by
// CLDR category ({"one": "%n klip", "other": "%n klip"}).
class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)
    Q_PROPERTY(QString languageOverride READ languageOverride WRITE setLanguage NOTIFY changed)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = ":/i18n",
                        QObject *parent = nullptr);
    ~Translator() override;

    QString language() const;
    QString languageOverride() const;
    Q_INVOKABLE void setLanguage(const QString &code);
    bool isEmpty() const override;
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

    static QString readLanguage(const QString &path);
    static QString pluralCategory(const QString &language, long long count);
    static QLocale formatLocale(const QString &language);

signals:
    void changed();

private:
    void reload();

    QString m_languageFile;
    QString m_catalogDirectory;
    bool m_useSettings = false;
    // Pre-parsed translation entry stored inside an immutable Catalog snapshot
    // so translate() never acquires m_lock or parses QJsonObjects on hot paths.
    struct CatalogEntry {
        bool isPlural = false;
        QString text;
        QString language;
        QHash<QString, QString> forms;
    };
    struct Catalog {
        QHash<QByteArray, CatalogEntry> entries;
        bool empty = true;
        bool englishPluralsOnly = false;
    };

    // m_lock protects m_language and m_languageOverride; translate() and
    // isEmpty() read atomic fast-path flags and the immutable m_catalog snapshot
    // completely lock-free across threads.
    mutable QReadWriteLock m_lock;
    QString m_language;
    QString m_languageOverride;
    std::atomic<std::shared_ptr<const Catalog>> m_catalog;
    std::atomic_bool m_emptyCatalog{true};
    std::atomic_bool m_englishPluralsOnly{false};
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
