// Language following comes from Omacut (MIT, David Heinemeier Hansson).
#include "app/translator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <cstring>

namespace {
QSettings translatorSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

QString sanitizeLanguageCode(QString code) {
    code = code.trimmed();
    code.replace(QLatin1Char('_'), QLatin1Char('-'));
    static const QRegularExpression valid(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2,4})*$"));
    return valid.match(code).hasMatch() ? code : QString();
}
}  // namespace

Translator::Translator(const QString &languageFile, const QString &catalogDirectory, QObject *parent)
    : QTranslator(parent),
      m_languageFile(languageFile.isEmpty() ? QDir::homePath() + "/.config/omarchy/language" : languageFile),
      m_catalogDirectory(catalogDirectory),
      m_useSettings(languageFile.isEmpty()) {
    if (m_useSettings)
        m_languageOverride = sanitizeLanguageCode(
            translatorSettings().value(QStringLiteral("appearance/language")).toString());
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Translator::reload);
    reload();
}

QString Translator::language() const {
    QReadLocker locker(&m_lock);
    return m_language;
}

QString Translator::languageOverride() const {
    QReadLocker locker(&m_lock);
    return m_languageOverride;
}

void Translator::setLanguage(const QString &code) {
    const QString cleaned = code.trimmed().isEmpty() ? QString() : sanitizeLanguageCode(code);
    if (!code.trimmed().isEmpty() && cleaned.isEmpty())
        return;
    {
        QWriteLocker locker(&m_lock);
        if (m_languageOverride == cleaned)
            return;
        m_languageOverride = cleaned;
    }
    if (m_useSettings) {
        QSettings s = translatorSettings();
        if (cleaned.isEmpty())
            s.remove(QStringLiteral("appearance/language"));
        else
            s.setValue(QStringLiteral("appearance/language"), cleaned);
    }
    const QString before = language();
    reload();
    if (before == language())
        emit changed();
}

bool Translator::isEmpty() const {
    return m_emptyCatalog.load(std::memory_order_acquire);
}

// Unregistered before the catalog and its lock go, since a worker thread can
// still be translating through QCoreApplication while the app shuts down;
// one outliving the application (a test's) has nothing to unregister from.
Translator::~Translator() {
    if (QCoreApplication::instance())
        QCoreApplication::removeTranslator(this);
}

// One registry code on the first line; anything else, or no file, is English.
QString Translator::readLanguage(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return "en";
    QString code = QString::fromUtf8(file.readLine()).trimmed();
    code.replace(QLatin1Char('_'), QLatin1Char('-'));
    static const QRegularExpression valid("^[a-z]{2,3}(-[A-Za-z]{2,4})*$");
    return valid.match(code).hasMatch() ? code : "en";
}

// CLDR cardinal plural categories for whole counts, for Omarchy's languages
// (the same rules as Omarchy's shell/Commons/Plural.js).
QString Translator::pluralCategory(const QString &language, long long count) {
    const long long n = count < 0 ? -count : count, n10 = n % 10, n100 = n % 100;
    const bool millions = n != 0 && n % 1000000 == 0;
    const bool teens = n100 >= 12 && n100 <= 14;
    const QString code = language;
    auto in = [&](std::initializer_list<const char *> codes) {
        for (const char *c : codes) if (code == QLatin1String(c)) return true;
        return false;
    };
    if (in({"ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms"})) return "other";
    if (in({"hi", "bn", "fa", "si"})) return n <= 1 ? "one" : "other";
    if (in({"fr", "pt-BR"})) return n <= 1 ? "one" : millions ? "many" : "other";
    if (in({"ca", "es", "es-MX", "it", "pt"})) return n == 1 ? "one" : millions ? "many" : "other";
    if (code == "fil") return (n10 == 4 || n10 == 6 || n10 == 9) ? "other" : "one";
    if (code == "is") return n10 == 1 && n100 != 11 ? "one" : "other";
    if (in({"cs", "sk"})) return n == 1 ? "one" : n >= 2 && n <= 4 ? "few" : "other";
    if (code == "pl") return n == 1 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "many";
    if (in({"ru", "uk"})) return n10 == 1 && n100 != 11 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "many";
    if (in({"hr", "sr"})) return n10 == 1 && n100 != 11 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "other";
    if (code == "sl") return n100 == 1 ? "one" : n100 == 2 ? "two" : n100 >= 3 && n100 <= 4 ? "few" : "other";
    if (code == "lt") return n100 >= 11 && n100 <= 19 ? "other" : n10 == 1 ? "one" : n10 >= 2 ? "few" : "other";
    if (code == "lv") return n10 == 0 || (n100 >= 11 && n100 <= 19) ? "zero" : n10 == 1 ? "one" : "other";
    if (code == "ro") return n == 1 ? "one" : n == 0 || (n100 >= 1 && n100 <= 19) ? "few" : "other";
    if (code == "ar") {
        if (n <= 2) return n == 0 ? "zero" : n == 1 ? "one" : "two";
        return n100 >= 3 && n100 <= 10 ? "few" : n100 >= 11 ? "many" : "other";
    }
    if (code == "he") return n == 1 ? "one" : n == 2 ? "two" : "other";
    if (code == "ga") return n == 1 ? "one" : n == 2 ? "two" : n >= 3 && n <= 6 ? "few" : n >= 7 && n <= 10 ? "many" : "other";
    return n == 1 ? "one" : "other";
}

// The locale Omarchy formats this language with, from its registry, so the
// app's numbers and dates match the desktop's (Arabic there uses Western
// digits); without Omarchy, Qt's own locale for the code.
QLocale Translator::formatLocale(const QString &language) {
    const QString omarchyPath = qEnvironmentVariable("OMARCHY_PATH");
    if (!omarchyPath.isEmpty()) {
        QFile registry(omarchyPath + "/default/i18n/locales.json");
        if (registry.open(QIODevice::ReadOnly)) {
            const QString name = QJsonDocument::fromJson(registry.readAll()).object()
                                     .value(language).toObject().value("formatLocale").toString();
            if (!name.isEmpty())
                return QLocale(name);
        }
    }
    return QLocale(language);
}

QStringList Translator::catalogSearchPaths() {
    QStringList dirs;
    auto addDir = [&dirs](const QString &raw) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty())
            return;
        const QString normalized =
            trimmed.startsWith(QLatin1String(":/")) ? trimmed : QDir::cleanPath(trimmed);
        if (!normalized.isEmpty() && !dirs.contains(normalized))
            dirs.append(normalized);
    };

    // 1. Explicit runtime override via CLOTHBOUND_I18N_DIR.
    addDir(qEnvironmentVariable("CLOTHBOUND_I18N_DIR"));

    // 2. Installed & portable locations relative to the executable.
    if (QCoreApplication::instance()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            addDir(appDir + QStringLiteral("/i18n"));
            addDir(appDir + QStringLiteral("/../share/clothbound/i18n"));
        }
    }

    // 3. Compile-time source tree directory (for dev / build-tree / test runs).
#ifdef CLOTHBOUND_SOURCE_I18N_DIR
    addDir(QStringLiteral(CLOTHBOUND_SOURCE_I18N_DIR));
#endif

    // 4. Embedded QRC fallback (carries en.json even if disk catalogs are missing).
    addDir(QStringLiteral(":/i18n"));
    return dirs;
}

QString Translator::resolveCatalogFile(const QString &code, const QString &explicitDirectory) {
    if (code.isEmpty())
        return {};
    const QString suffix = QLatin1Char('/') + code + QStringLiteral(".json");
    if (!explicitDirectory.isEmpty()) {
        const QString candidate = explicitDirectory + suffix;
        return QFile::exists(candidate) ? candidate : QString();
    }
    const QStringList dirs = catalogSearchPaths();
    for (const QString &dir : dirs) {
        const QString candidate = dir + suffix;
        if (QFile::exists(candidate))
            return candidate;
    }
    return {};
}

QString Translator::translate(const char *, const char *sourceText, const char *, int n) const {
    // A null string tells Qt there is no translation, so it shows the English.
    if (!sourceText)
        return {};
    if (m_emptyCatalog.load(std::memory_order_acquire))
        return {};
    if (n < 0 && m_englishPluralsOnly.load(std::memory_order_acquire)) {
        // When default English only contains counted plural rules (%n / %Ln),
        // non-counted strings without a plural placeholder bypass snapshot lookup.
        const char *p = std::strchr(sourceText, '%');
        bool hasPluralPlaceholder = false;
        while (p) {
            if (p[1] == 'n' || (p[1] == 'L' && p[2] == 'n')) {
                hasPluralPlaceholder = true;
                break;
            }
            p = std::strchr(p + 1, '%');
        }
        if (!hasPluralPlaceholder)
            return {};
    }

    const std::shared_ptr<const Catalog> catalog = m_catalog.load(std::memory_order_acquire);
    if (!catalog || catalog->empty)
        return {};
    const QByteArray key = QByteArray::fromRawData(sourceText, qsizetype(std::strlen(sourceText)));
    const auto it = catalog->entries.constFind(key);
    if (it == catalog->entries.cend())
        return {};
    const CatalogEntry &entry = *it;
    if (!entry.isPlural)
        return entry.text;
    QString form = entry.forms.value(n < 0 ? QStringLiteral("other") : pluralCategory(entry.language, n));
    if (form.isEmpty())
        form = entry.forms.value(QStringLiteral("other"));
    if (form.isEmpty())
        form = entry.forms.value(QStringLiteral("many"));
    return form.isEmpty() ? QString() : form;
}

void Translator::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // omarchy-language replaces the file, which drops it from the watch, and a
    // first pick creates it, and maybe its directory; watch the nearest
    // existing directory too and re-arm every time.
    QString directory = QFileInfo(m_languageFile).absolutePath();
    while (!QFileInfo::exists(directory) && directory != "/")
        directory = QFileInfo(directory).absolutePath();
    m_watcher.addPath(directory);
    if (QFileInfo::exists(m_languageFile))
        m_watcher.addPath(m_languageFile);

    // Catalogs are loaded lazily on demand, so only a different language needs a reload.
    const QString overrideCode = languageOverride();
    const QString language = overrideCode.isEmpty() ? readLanguage(m_languageFile) : overrideCode;
    if (language == this->language())
        return;

    // English first (it carries the plural forms of counted strings), then
    // the base language and the regional file over it (pt, then pt-BR), so a
    // missing string falls back toward English one step at a time.
    auto catalog = std::make_shared<Catalog>();
    QStringList codes{"en", language.section('-', 0, 0), language};
    codes.removeDuplicates();
    for (const auto &code : codes) {
        const QString catalogPath = resolveCatalogFile(code, m_catalogDirectory);
        if (catalogPath.isEmpty())
            continue;
        QFile catalogFile(catalogPath);
        if (!catalogFile.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(catalogFile.readAll()).object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.value().isString() && !it.value().toString().isEmpty()) {
                CatalogEntry entry;
                entry.isPlural = false;
                entry.text = it.value().toString();
                catalog->entries.insert(it.key().toUtf8(), std::move(entry));
            } else if (it.value().isObject()) {
                const QJsonObject formsObj = it.value().toObject();
                CatalogEntry entry;
                entry.isPlural = true;
                entry.language = code;
                entry.forms.reserve(formsObj.size());
                for (auto fit = formsObj.begin(); fit != formsObj.end(); ++fit)
                    if (fit.value().isString())
                        entry.forms.insert(fit.key(), fit.value().toString());
                catalog->entries.insert(it.key().toUtf8(), std::move(entry));
            }
        }
    }
    catalog->empty = catalog->entries.isEmpty();
    bool englishPluralsOnly = (language == QLatin1String("en") && !catalog->empty);
    if (englishPluralsOnly) {
        for (auto it = catalog->entries.cbegin(); it != catalog->entries.cend(); ++it) {
            if (!it.value().isPlural || (!it.key().contains("%n") && !it.key().contains("%Ln"))) {
                englishPluralsOnly = false;
                break;
            }
        }
    }
    catalog->englishPluralsOnly = englishPluralsOnly;

    {
        QWriteLocker locker(&m_lock);
        m_language = language;
        m_catalog.store(catalog, std::memory_order_release);
        m_emptyCatalog.store(catalog->empty, std::memory_order_release);
        m_englishPluralsOnly.store(catalog->englishPluralsOnly, std::memory_order_release);
    }
    // Dates, numbers, and Qt's own day and month names follow the language
    // too, and right-to-left languages mirror the window.
    const QLocale locale = formatLocale(language);
    QLocale::setDefault(locale);
    QGuiApplication::setLayoutDirection(locale.textDirection());
    emit changed();
}
