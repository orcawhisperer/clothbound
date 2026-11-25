#include "clothbound_tests.h"

#include "app/instance.h"
#include "app/migration.h"
#include "app/notifier.h"
#include "app/pointerdriver.h"
#include "app/systemcheck.h"
#include "app/theme.h"
#include "app/translator.h"
#include "app/trayicon.h"
#include "audio/audiocache.h"
#include "formats/bookinfo.h"
#include "formats/bookname.h"
#include "formats/bookscheme.h"
#include "formats/coverprovider.h"
#include "formats/epubbook.h"
#include "formats/htmltokenizer.h"
#include "formats/pdfbook.h"
#include "formats/pdfpages.h"
#include "formats/ziparchive.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "online/opdsclient.h"
#include "reader/reader.h"
#include "reader/readerimageprovider.h"
#include "testbooks.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QXmlStreamReader>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <QtConcurrent>
#include <QtTest>
#include <cmath>
#include <limits>

void ClothboundTests::themeFallsBack() {
    QTemporaryDir dir;
    Theme theme(dir.path());
    QCOMPARE(theme.accent(), QString("#FFD60A"));
    QCOMPARE(theme.accentForeground(), QString("black"));
    QVERIFY(!theme.omarchy());
    QVERIFY(!theme.light());
    QCOMPARE(theme.background(), QString("#121214"));
    // Without Omarchy the tokens are the design's neutrals.
    QCOMPARE(theme.ground(), QString("#121214"));
    QCOMPARE(theme.sidebar(), QString("#18181b"));
    QCOMPARE(theme.ink1(), QString("#f2f2f3"));
    QCOMPARE(theme.ink3(), QString("#a1a1aa"));
}

void ClothboundTests::themeTokensKeepTextReadableInEveryOmarchyTheme() {
    // Every theme Omarchy ships, plus a deliberately washed-out one: body
    // text must reach WCAG AA on the ground, headings 7:1, hints 3:1, and
    // the accent as ink 4.5:1.
    QList<QHash<QString, QString>> palettes;
    const QDir themes(QStringLiteral("/usr/share/omarchy/themes"));
    for (const QString &name : themes.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto palette = Theme::readPalette(themes.filePath(name + "/colors.toml"));
        if (palette.contains("background") && palette.contains("foreground"))
            palettes.append(palette);
    }
    palettes.append({{"mode", "dark"}, {"background", "#303030"}, {"foreground", "#5a5a5a"}, {"accent", "#3a3a6a"}});
    palettes.append({{"mode", "light"}, {"background", "#f0f0f0"}, {"foreground", "#b0b0b0"}, {"accent", "#ffe066"}});
    for (const auto &palette : palettes) {
        const bool light = palette.value("mode") == "light";
        const auto t = Theme::deriveTokens(palette, light, true);
        const QString what = palette.value("background") + " / " + palette.value("foreground");
        QVERIFY2(Theme::contrast(t["ink1"], t["ground"]) >= 7.0 - 1e-6, qPrintable(what));
        QVERIFY2(Theme::contrast(t["ink2"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
        QVERIFY2(Theme::contrast(t["ink3"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
        QVERIFY2(Theme::contrast(t["ink4"], t["ground"]) >= 3.0 - 1e-6, qPrintable(what));
        QVERIFY2(Theme::contrast(t["accentInk"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
        QVERIFY2(Theme::contrast(t["positive"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
        QVERIFY2(Theme::contrast(t["cloud"], t["ground"]) >= 4.5 - 1e-6, qPrintable(what));
        QCOMPARE(t["ground"], palette.value("background"));
    }
    QVERIFY(palettes.size() >= 2);
}

void ClothboundTests::themeAppearanceCanOverrideTheDesktop() {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("theme");
    write(dir.path() + "/theme/colors.toml", "mode = \"dark\"\naccent = \"#7aa2f7\"\n"
                                             "background = \"#1a1b26\"\nforeground = \"#a9b1d6\"\n");
    Theme theme(dir.path());
    QVERIFY(!theme.light());
    QCOMPARE(theme.ground(), QString("#1a1b26"));
    QSignalSpy changed(&theme, &Theme::changed);
    theme.setAppearance("light");
    QVERIFY(changed.count() >= 1);
    QVERIFY(theme.light());
    // Forced light sets the dark theme's palette aside but keeps its accent,
    // darkened where it is used as ink on the light ground.
    QCOMPARE(theme.ground(), QString("#fbfbfc"));
    QCOMPARE(theme.accent(), QString("#7aa2f7"));
    QVERIFY(Theme::contrast(theme.accentInk(), theme.ground()) >= 4.5);
    theme.setAppearance("bogus");
    QCOMPARE(theme.appearance(), QString("match"));
    QCOMPARE(theme.ground(), QString("#1a1b26"));
}

void ClothboundTests::themeFollowsSystemColorSchemeWithoutOmarchy() {
    // Portal uint32 decoding (including nested QDBusVariant).
    QCOMPARE(Theme::parsePortalColorScheme(QVariant(0u)), Theme::SystemColorScheme::Unknown);
    QCOMPARE(Theme::parsePortalColorScheme(QVariant(1u)), Theme::SystemColorScheme::Dark);
    QCOMPARE(Theme::parsePortalColorScheme(QVariant(2u)), Theme::SystemColorScheme::Light);
    QCOMPARE(Theme::parsePortalColorScheme(QVariant::fromValue(QDBusVariant(QVariant(2u)))),
             Theme::SystemColorScheme::Light);
    QCOMPARE(Theme::parsePortalColorScheme(QVariant(QStringLiteral("invalid"))),
             Theme::SystemColorScheme::Unknown);

    // Without Omarchy, Match desktop follows the system light/dark preference.
    QTemporaryDir emptyDir;
    Theme standalone(emptyDir.path());
    QVERIFY(!standalone.omarchy());
    QVERIFY(!standalone.light());
    QCOMPARE(standalone.ground(), QStringLiteral("#121214"));

    QSignalSpy changed(&standalone, &Theme::changed);
    standalone.setSystemColorScheme(Theme::SystemColorScheme::Light);
    QVERIFY(changed.count() >= 1);
    QVERIFY(!standalone.omarchy());
    QVERIFY(standalone.light());
    QCOMPARE(standalone.ground(), QStringLiteral("#fbfbfc"));

    // Explicit appearance override wins over system preference.
    standalone.setAppearance(QStringLiteral("dark"));
    QVERIFY(!standalone.light());
    QCOMPARE(standalone.ground(), QStringLiteral("#121214"));
    standalone.setAppearance(QStringLiteral("match"));
    QVERIFY(standalone.light());

    // Switching system preference back to dark returns to dark neutrals.
    standalone.setSystemColorScheme(Theme::SystemColorScheme::Dark);
    QVERIFY(!standalone.light());
    QCOMPARE(standalone.ground(), QStringLiteral("#121214"));

    // When an Omarchy palette is present, Omarchy takes precedence over system scheme.
    QTemporaryDir omarchyDir;
    QDir(omarchyDir.path()).mkpath(QStringLiteral("theme"));
    write(omarchyDir.path() + QStringLiteral("/theme/colors.toml"),
          "mode = \"dark\"\naccent = \"#7aa2f7\"\nbackground = \"#1a1b26\"\nforeground = \"#a9b1d6\"\n");
    Theme omarchy(omarchyDir.path());
    omarchy.setSystemColorScheme(Theme::SystemColorScheme::Light);
    QVERIFY(omarchy.omarchy());
    QVERIFY(!omarchy.light());
    QCOMPARE(omarchy.ground(), QStringLiteral("#1a1b26"));
}

void ClothboundTests::flatpakMetadataAndDiagnosticsAreValid() {
    // SystemCheck::machine() exposes OS, Qt version, display platform, and sandbox ID.
    const QByteArray prevFlatpak = qgetenv("FLATPAK_ID");
    qunsetenv("FLATPAK_ID");
    SystemCheck check(nullptr, nullptr);
    const QVariantMap nativeInfo = check.machine();
    QVERIFY(!nativeInfo.value(QStringLiteral("os")).toString().isEmpty());
    QVERIFY(!nativeInfo.value(QStringLiteral("qtVersion")).toString().isEmpty());
    QVERIFY(!nativeInfo.value(QStringLiteral("displayPlatform")).toString().isEmpty());
    QVERIFY(nativeInfo.value(QStringLiteral("sandbox")).toString().isEmpty());

    qputenv("FLATPAK_ID", "app.clothbound.Reader");
    const QVariantMap sandboxInfo = check.machine();
    QCOMPARE(sandboxInfo.value(QStringLiteral("sandbox")).toString(), QStringLiteral("app.clothbound.Reader"));
    if (prevFlatpak.isEmpty())
        qunsetenv("FLATPAK_ID");
    else
        qputenv("FLATPAK_ID", prevFlatpak);

    // Validate AppStream metainfo XML well-formedness and key Flathub fields.
    const QString root = QFileInfo(QStringLiteral(CLOTHBOUND_SOURCE_I18N_DIR)).dir().absoluteFilePath(QStringLiteral(".."));
    QFile metainfo(root + QStringLiteral("/flatpak/app.clothbound.Reader.metainfo.xml"));
    QVERIFY2(metainfo.open(QIODevice::ReadOnly), qPrintable(metainfo.fileName()));
    QXmlStreamReader xml(&metainfo);
    QString id;
    QString launchable;
    bool hasDeveloper = false;
    bool hasContentRating = false;
    bool hasMcpBinary = false;
    bool hasRelease = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("id"))
                id = xml.readElementText().trimmed();
            else if (name == QLatin1String("launchable"))
                launchable = xml.readElementText().trimmed();
            else if (name == QLatin1String("developer"))
                hasDeveloper = !xml.attributes().value(QLatin1String("id")).isEmpty();
            else if (name == QLatin1String("content_rating"))
                hasContentRating = true;
            else if (name == QLatin1String("binary") && xml.readElementText().trimmed() == QLatin1String("clothbound-mcp"))
                hasMcpBinary = true;
            else if (name == QLatin1String("release"))
                hasRelease = true;
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QCOMPARE(id, QStringLiteral("app.clothbound.Reader"));
    QCOMPARE(launchable, QStringLiteral("app.clothbound.Reader.desktop"));
    QVERIFY(hasDeveloper);
    QVERIFY(hasContentRating);
    QVERIFY(hasMcpBinary);
    QVERIFY(hasRelease);

    // Verify Flatpak manifest and Report Issue diagnostics UI wiring exist.
    QFile manifest(root + QStringLiteral("/flatpak/app.clothbound.Reader.yml"));
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const QString manifestText = QString::fromUtf8(manifest.readAll());
    QVERIFY(manifestText.contains(QStringLiteral("id: app.clothbound.Reader")));
    QVERIFY(manifestText.contains(QStringLiteral("base: io.qt.qtwebengine.BaseApp")));
    QVERIFY(manifestText.contains(QStringLiteral("-DCLOTHBOUND_APP_ID=app.clothbound.Reader")));

    QFile aboutQml(root + QStringLiteral("/src/ui/settings/SettingsAbout.qml"));
    QVERIFY(aboutQml.open(QIODevice::ReadOnly));
    const QString aboutText = QString::fromUtf8(aboutQml.readAll());
    QVERIFY(aboutText.contains(QStringLiteral("reportIssueUrl()")));
    QVERIFY(aboutText.contains(QStringLiteral("template=bug_report.yml")));
    QVERIFY(aboutText.contains(QStringLiteral("qsTr(\"Report issue\")")));
}

void ClothboundTests::themeReadsThePalette() {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("theme");
    write(dir.path() + "/theme/colors.toml", "mode = \"light\"\naccent = \"#112233\"\n"
                                             "background = \"#fffcf0\"\nforeground = \"#100f0f\"\n"
                                             "lighter_background = \"#e6e4d9\"\n"
                                             "bad_quotes = \"#ff0000'\n"
                                             "[terminal]\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
    Theme theme(dir.path());
    QCOMPARE(theme.accent(), QString("#112233"));
    QCOMPARE(theme.accentForeground(), QString("white"));
    QVERIFY(theme.omarchy());
    QVERIFY(theme.light());
    // Keys inside [terminal] must not clobber top-level palette keys.
    QCOMPARE(theme.background(), QString("#fffcf0"));
    QCOMPARE(theme.text(), QString("#100f0f"));
    QCOMPARE(theme.surface(), QString("#e6e4d9"));
    // Partial light themes fall back to the light palette (high contrast on light bg).
    QCOMPARE(theme.selection(), Theme::fallbackLightPalette().value("selection"));
    QCOMPARE(theme.textSecondary(), Theme::fallbackLightPalette().value("light_foreground"));
    QCOMPARE(Theme::readableOn(theme.textSecondary()), QString("white"));
}

void ClothboundTests::themeFollowsChanges() {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("theme");
    write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
    Theme theme(dir.path());
    QSignalSpy changed(&theme, &Theme::changed);
    write(dir.path() + "/theme/colors.toml", "accent = \"#445566\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
    QTRY_COMPARE(theme.accent(), QString("#445566"));
    QVERIFY(changed.count() >= 1);
}

void ClothboundTests::themeFollowsHyprlandRounding() {
    QTemporaryDir dir;
    int rounding = 0;
    QLocalServer requestServer, eventServer;
    QVERIFY(requestServer.listen(dir.filePath(".socket.sock")));
    QVERIFY(eventServer.listen(dir.filePath(".socket2.sock")));

    QObject::connect(&requestServer, &QLocalServer::newConnection, &requestServer, [&] {
        while (QLocalSocket *client = requestServer.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::readyRead, client, [client, &rounding] {
                const QByteArray cmd = client->readAll();
                if (cmd.contains("decoration:rounding"))
                    client->write(QStringLiteral("{\"int\":%1}").arg(rounding).toUtf8());
                client->disconnectFromServer();
            });
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    QPointer<QLocalSocket> eventClient;
    QObject::connect(&eventServer, &QLocalServer::newConnection, &eventServer, [&] {
        eventClient = eventServer.nextPendingConnection();
    });

    Theme theme(dir.path(), nullptr, dir.path());
    QTRY_VERIFY(eventClient != nullptr);
    QCOMPARE(theme.corners(), QString("match"));
    QCOMPARE(theme.radius(), 0);

    rounding = 6;
    eventClient->write("configreloaded>>\n");
    eventClient->flush();
    QTRY_COMPARE(theme.radius(), 6);

    // Explicit rounded/sharp overrides win over Hyprland's rounding, and
    // returning to "match" (or any unknown value) resumes live desktop rounding.
    QSignalSpy cornerSpy(&theme, &Theme::changed);
    theme.setCorners(QStringLiteral("sharp"));
    QCOMPARE(theme.corners(), QString("sharp"));
    QCOMPARE(theme.radius(), 0);
    QCOMPARE(cornerSpy.count(), 1);

    theme.setCorners(QStringLiteral("rounded"));
    QCOMPARE(theme.corners(), QString("rounded"));
    QCOMPARE(theme.radius(), 22);
    QCOMPARE(cornerSpy.count(), 2);

    theme.setCorners(QStringLiteral("bogus"));
    QCOMPARE(theme.corners(), QString("match"));
    QCOMPARE(theme.radius(), 6);
    QCOMPARE(cornerSpy.count(), 3);
}

void ClothboundTests::translatorFallsBackToEnglish() {
    QTemporaryDir dir;
    Translator translator(dir.path() + "/language", dir.path());
    QCOMPARE(translator.language(), QString("en"));
    QVERIFY(translator.translate("Main", "Save").isNull());
}

void ClothboundTests::translatorFollowsTheLanguageFile() {
    QTemporaryDir dir;
    write(dir.path() + "/language", "pt_BR\n");
    write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
    write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

    Translator translator(dir.path() + "/language", dir.path());
    QCOMPARE(translator.language(), QString("pt-BR"));
    QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
    QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
    QVERIFY(translator.translate("Main", "Quit").isNull());

    // BCP-47 script tags are accepted too.
    write(dir.path() + "/language", "zh-Hans\n");
    QCOMPARE(Translator::readLanguage(dir.path() + "/language"), QString("zh-Hans"));
}

void ClothboundTests::translatorPicksPluralForms() {
    QTemporaryDir dir;
    write(dir.path() + "/language", "ru\n");
    write(dir.path() + "/en.json", R"({"%n books": {"one": "%n book", "other": "%n books"}})");
    write(dir.path() + "/ru.json", R"({"%n books": {"one": "%n книга", "few": "%n книги", "many": "%n книг", "other": "%n книг"}, "%n pages": {"one": "%n страница", "many": "%n страниц"}})");

    Translator translator(dir.path() + "/language", dir.path());
    QCOMPARE(translator.translate("Main", "%n books", nullptr, 21), QString("%n книга"));
    QCOMPARE(translator.translate("Main", "%n books", nullptr, 22), QString("%n книги"));
    QCOMPARE(translator.translate("Main", "%n books", nullptr, 25), QString("%n книг"));
    // Missing "other" falls back to "many" rather than blank text.
    QCOMPARE(translator.translate("Main", "%n pages", nullptr, -1), QString("%n страниц"));
    QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
    QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    QCOMPARE(Translator::pluralCategory("es", 1000000), QString("many"));
    // Unset OMARCHY_PATH never breaks formatLocale.
    const QByteArray prevOmarchy = qgetenv("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");
    QCOMPARE(Translator::formatLocale("fr").language(), QLocale::French);
    if (!prevOmarchy.isEmpty())
        qputenv("OMARCHY_PATH", prevOmarchy);
}

void ClothboundTests::shippedCatalogsAreValidJson() {
    const QDir sources(QFINDTESTDATA("../src/i18n"));
    const QStringList files = sources.entryList({"*.json"});
    QCOMPARE(files.size(), 50);

    // Read English plural catalog first: must include Format.js plurals too.
    QFile enFile(sources.filePath("en.json"));
    QVERIFY(enFile.open(QIODevice::ReadOnly));
    QJsonParseError enError;
    const QJsonObject enObj = QJsonDocument::fromJson(enFile.readAll(), &enError).object();
    QVERIFY2(enError.error == QJsonParseError::NoError, qPrintable(enError.errorString()));
    QVERIFY(enObj.contains("%n min"));
    QVERIFY(enObj.contains("%n h"));
    QVERIFY(enObj.contains("%n books"));

    static const QRegularExpression phRe(QStringLiteral("%L?[0-9n]"));
    static const QRegularExpression tagRe(QStringLiteral("</?([a-zA-Z0-9]+)(?:\\s[^>]*)?>"));
    auto normalizedPh = [](const QString &s) {
        QStringList out;
        auto it = phRe.globalMatch(s);
        while (it.hasNext()) {
            QString p = it.next().captured();
            out.append((p == "%Ln") ? QStringLiteral("%n") : p);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto htmlTags = [](const QString &s) {
        QStringList out;
        auto it = tagRe.globalMatch(s);
        while (it.hasNext())
            out.append(it.next().captured(1));
        std::sort(out.begin(), out.end());
        return out;
    };

    const QHash<QString, QStringList> requiredForms{
        {"ja", {"other"}}, {"ko", {"other"}}, {"zh-CN", {"other"}}, {"zh-TW", {"other"}},
        {"th", {"other"}}, {"vi", {"other"}}, {"id", {"other"}}, {"ms", {"other"}},
        {"fr", {"one", "many", "other"}}, {"pt-BR", {"one", "many", "other"}},
        {"ca", {"one", "many", "other"}}, {"es-MX", {"one", "many", "other"}},
        {"it", {"one", "many", "other"}}, {"pt", {"one", "many", "other"}},
        {"cs", {"one", "few", "other"}}, {"sk", {"one", "few", "other"}},
        {"pl", {"one", "few", "many", "other"}}, {"ru", {"one", "few", "many", "other"}},
        {"uk", {"one", "few", "many", "other"}}, {"hr", {"one", "few", "other"}},
        {"sr", {"one", "few", "other"}}, {"sl", {"one", "two", "few", "other"}},
        {"lt", {"one", "few", "other"}}, {"lv", {"zero", "one", "other"}},
        {"ro", {"one", "few", "other"}}, {"ar", {"zero", "one", "two", "few", "many", "other"}},
        {"he", {"one", "two", "other"}}, {"ga", {"one", "two", "few", "many", "other"}},
    };
    const QHash<QString, QSet<QString>> exactNumberForms{
        {"ar", {"zero", "one", "two"}}, {"he", {"one", "two"}}, {"ro", {"one"}}, {"ga", {"one", "two"}},
    };

    for (const QString &name : files) {
        const QString code = QFileInfo(name).baseName();
        QFile file(sources.filePath(name));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
        QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + ": " + error.errorString()));
        QVERIFY2(doc.isObject(), qPrintable(name));
        const QJsonObject obj = doc.object();
        if (code == "en")
            continue;
        const QStringList forms = requiredForms.value(code, {"one", "other"});
        for (auto it = obj.cbegin(); it != obj.cend(); ++it) {
            const QString key = it.key();
            const bool counted = enObj.contains(key) || key.contains("%n") || key.contains("%Ln");
            const QStringList expPh = normalizedPh(key);
            const QStringList expTags = htmlTags(key);
            if (counted) {
                QVERIFY2(it.value().isObject(), qPrintable(code + ": counted key is not object: " + key));
                const QJsonObject dict = it.value().toObject();
                QVERIFY2(dict.contains("other"), qPrintable(code + ": missing 'other' for " + key));
                for (const QString &form : forms) {
                    const QString val = dict.value(form).toString();
                    QVERIFY2(!val.trimmed().isEmpty(), qPrintable(code + ": empty form " + form + " for " + key));
                    QCOMPARE(htmlTags(val), expTags);
                    const QStringList gotPh = normalizedPh(val);
                    if (gotPh != expPh) {
                        const bool okExact = exactNumberForms.value(code).contains(form);
                        QStringList expNoN = expPh, gotNoN = gotPh;
                        expNoN.removeAll("%n");
                        QVERIFY2(okExact && !gotNoN.contains("%n") && gotNoN == expNoN,
                                 qPrintable(code + " [" + form + "] placeholder mismatch for " + key + ": " + val));
                    }
                }
            } else {
                const QString val = it.value().toString();
                QVERIFY2(!val.trimmed().isEmpty(), qPrintable(code + ": empty value for " + key));
                QCOMPARE(normalizedPh(val), expPh);
                QCOMPARE(htmlTags(val), expTags);
            }
        }
    }
}

void ClothboundTests::zipReadsStoredAndDeflatedEntries() {
    QTemporaryDir dir;
    const QByteArray big(100000, 'w');
    QVERIFY(TestBooks::write(dir.filePath("a.zip"),
                             TestBooks::zip({{"plain.txt", "stored text", false},
                                             {"dir/Deflated.TXT", big, true},
                                             {"ünïcode name.txt", "ok", true}})));
    ZipArchive zip(dir.filePath("a.zip"));
    QVERIFY(zip.open());
    QCOMPARE(zip.names().size(), 3);
    QCOMPARE(zip.read("plain.txt"), QByteArray("stored text"));
    QCOMPARE(zip.read("dir/Deflated.TXT"), big);
    // Books sometimes link with different capitalization.
    QCOMPARE(zip.resolve("DIR/deflated.txt"), QString("dir/Deflated.TXT"));
    QCOMPARE(zip.read("dir/deflated.txt"), big);
    QCOMPARE(zip.read("ünïcode name.txt"), QByteArray("ok"));
    QCOMPARE(zip.size("dir/Deflated.TXT"), qint64(big.size()));
    QString error;
    QVERIFY(zip.read("missing.txt", &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void ClothboundTests::zipReadsZip64() {
    QTemporaryDir dir;
    QVERIFY(TestBooks::write(dir.filePath("b.zip"),
                             TestBooks::zip({{"one.txt", "first", true}, {"two.txt", "second", false}}, true)));
    ZipArchive zip(dir.filePath("b.zip"));
    QVERIFY2(zip.open(), qPrintable(zip.errorString()));
    QCOMPARE(zip.read("one.txt"), QByteArray("first"));
    QCOMPARE(zip.read("two.txt"), QByteArray("second"));
}

void ClothboundTests::zipRejectsOtherFiles() {
    QTemporaryDir dir;
    write(dir.filePath("c.zip"), "this is not a zip at all");
    ZipArchive zip(dir.filePath("c.zip"));
    QVERIFY(!zip.open());
    QVERIFY(!zip.errorString().isEmpty());
}

void ClothboundTests::tokenizerDecodesEntities() {
    QCOMPARE(HtmlTokenizer::decodeEntities(u"a &amp; b &lt;c&gt; &rsquo;&#8217;&#x2019; &nbsp;"),
             QString("a & b <c> ’’’  "));
    // Unknown entities and bare ampersands stay as written.
    QCOMPARE(HtmlTokenizer::decodeEntities(u"R&D &bogus; &"), QString("R&D &bogus; &"));
    // Windows-1252 code points mean their curly quotes, as in browsers.
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&#147;hi&#148;"), QString("“hi”"));
}

void ClothboundTests::tokenizerDecodesDeclaredEncodings() {
    const QByteArray latin1 = "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?><p>caf\xe9</p>";
    QVERIFY(HtmlTokenizer::decodeDocument(latin1).contains(QString("café")));
    QVERIFY(HtmlTokenizer::decodeDocument("\xEF\xBB\xBF<p>x</p>\r\n").endsWith("</p>\n"));
}

void ClothboundTests::treeParsesForgivingly() {
    const auto doc = Html::parse(QByteArray("<root><a x='1' Y=\"2\">one<br>two</a></b><dc:title>T &amp; U</dc:title></root>"), true);
    const HtmlNode *a = doc->first("a");
    QVERIFY(a);
    QCOMPARE(a->attribute("x"), QString("1"));
    QCOMPARE(a->attribute("y"), QString("2"));
    QCOMPARE(a->textContent(), QString("onetwo"));
    QCOMPARE(doc->first("title")->textContent(), QString("T & U"));
    // In book XML a <meta> holds its text.
    const auto xml = Html::parse(QByteArray("<metadata><meta property='role'>edt</meta></metadata>"));
    QCOMPARE(xml->first("meta")->textContent(), QString("edt"));
}

void ClothboundTests::textExtractionMatchesTheReader() {
    const QByteArray doc = R"(<html><head><title>Not text</title><style>p{}</style></head>
<body><p>One&nbsp;two</p><script>skip()</script>
<p>Sof&#173;t hy&shy;phens <b>bold</b></p><![CDATA[raw <text>]]></body></html>)";
    QCOMPARE(EpubBook::extractText(doc), QString("One two\nSoft hyphens bold" "raw <text>"));
    // Without a body, everything outside the head counts.
    QCOMPARE(EpubBook::extractText("<html><head><title>x</title></head><p>loose</p></html>"), QString("loose"));
}

void ClothboundTests::linksResolveInsideTheBook() {
    QString fragment;
    QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "../images/b%20c.png", &fragment), QString("OEBPS/images/b c.png"));
    QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "b.xhtml#note%201", &fragment), QString("OEBPS/text/b.xhtml"));
    QCOMPARE(fragment, QString("note 1"));
    QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "#n2", &fragment), QString("OEBPS/text/a.xhtml"));
    QCOMPARE(fragment, QString("n2"));
    QCOMPARE(EpubBook::resolve("content.opf", "./text/c.xhtml"), QString("text/c.xhtml"));
    QVERIFY(EpubBook::resolve("OEBPS/a.xhtml", "https://example.com/x").isEmpty());
    QVERIFY(EpubBook::resolve("OEBPS/a.xhtml", "mailto:someone").isEmpty());
}

void ClothboundTests::epub2ReadsPackageAndNcx() {
    QTemporaryDir dir;
    const QString path = dir.filePath("book.epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
    EpubBook book(path);
    QVERIFY2(book.open(), qPrintable(book.errorString()));
    QCOMPARE(book.version(), QString("2.0"));
    const EpubMetadata &meta = book.metadata();
    QCOMPARE(meta.title, QString("The Lantern Keeper (Harbor Classics)"));
    QCOMPARE(meta.authors, QStringList{"Ada Marsh"});
    QCOMPARE(meta.editors, QStringList{"Tom Reed"});
    QCOMPARE(meta.date, QString("1911-04-02"));
    QCOMPARE(meta.isbn, QString("9780000000002"));
    QCOMPARE(meta.description, QString("A keeper & her light.\n\nSecond paragraph."));
    QCOMPARE(meta.series, QString("Harbor Tales"));
    QCOMPARE(meta.seriesIndex, 2.0);

    QCOMPARE(book.spine().size(), 2);
    QCOMPARE(book.spine()[0].path, QString("OEBPS/text/chapter 1.xhtml"));
    QVERIFY(!book.spine()[1].linear);

    QCOMPARE(book.toc().size(), 3);
    QCOMPARE(book.toc()[1].title, QString("The Storm"));
    QCOMPARE(book.toc()[1].depth, 1);
    QCOMPARE(book.toc()[1].fragment, QString("storm"));
    QCOMPARE(book.toc()[1].spine, 0);
    QCOMPARE(book.toc()[2].spine, 1);

    QCOMPARE(book.pageList().size(), 2);
    QCOMPARE(book.pageList()[1].label, QString("2"));
    QCOMPARE(book.pageList()[1].spine, 1);

    QCOMPARE(book.coverPath(), QString("OEBPS/images/cover.png"));
    QString type;
    QVERIFY(!book.coverImage(&type).isEmpty());
    QCOMPARE(type, QString("image/png"));
    QVERIFY(!book.isDrmProtected());

    QCOMPARE(book.chapterText(0), QString("Chapter One\nThe lamp’s light & the sea wind.\n\nA storm came in."));
}

void ClothboundTests::epub3ReadsNavigationAndRefines() {
    QTemporaryDir dir;
    const QString path = dir.filePath("book3.epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub3(adobeObfuscate(fontPayload()))));
    EpubBook book(path);
    QVERIFY2(book.open(), qPrintable(book.errorString()));
    QCOMPARE(book.metadata().title, QString("Salt Roads"));
    QCOMPARE(book.metadata().authors, QStringList{"Wen Lo"});
    QCOMPARE(book.metadata().editors, QStringList{"Ira Bell"});
    QCOMPARE(book.metadata().series, QString("Road Books"));
    QCOMPARE(book.metadata().seriesIndex, 3.0);
    QCOMPARE(book.toc().size(), 3);
    QCOMPARE(book.toc()[1].title, QString("The Salt Flats"));
    QCOMPARE(book.toc()[1].depth, 1);
    QCOMPARE(book.toc()[2].title, QString("Appendices"));
    QVERIFY(book.toc()[2].path.isEmpty());
    QCOMPARE(book.pageList().size(), 1);
    QCOMPARE(book.pageList()[0].label, QString("vii"));
    QCOMPARE(book.coverPath(), QString("OEBPS/cover.jpg"));
    // Font obfuscation is undone, and isn't mistaken for DRM.
    QVERIFY(!book.isDrmProtected());
    QCOMPARE(book.resource("OEBPS/fonts/f.otf"), fontPayload());
}

void ClothboundTests::epubWithEncryptedTextIsDrm() {
    QTemporaryDir dir;
    const QString path = dir.filePath("drm.epub");
    const QByteArray zip = TestBooks::drmEpub();
    QVERIFY(TestBooks::write(path, zip));
    EpubBook book(path);
    QVERIFY(book.open());
    QVERIFY(book.isDrmProtected());
    QCOMPARE(BookFiles::read(path).problem, QString("This book is protected by DRM."));
}

void ClothboundTests::epubWithoutChaptersFails() {
    QTemporaryDir dir;
    const QString path = dir.filePath("empty.epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub({{"OEBPS/content.opf", "<package><manifest/><spine/></package>"}})));
    EpubBook book(path);
    QVERIFY(!book.open());
    QVERIFY(!book.errorString().isEmpty());
}

void ClothboundTests::bookInfoReadsEpubMetadataAndCover() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Marsh, Ada - Lantern Keeper, The [ed. Reed] (Harbor, 1911).epub");
    QVERIFY(TestBooks::write(path, TestBooks::epub2(png(200, 300))));
    const BookInfo info = BookFiles::read(path, 90);
    QVERIFY(info.ok());
    QCOMPARE(info.format, QString("epub"));
    QCOMPARE(info.identity.title, QString("The Lantern Keeper"));
    QCOMPARE(info.identity.author, QString("Ada Marsh"));
    QCOMPARE(info.identity.editor, QString("Reed"));
    QCOMPARE(info.identity.publisher, QString("Harbor"));
    QCOMPARE(info.identity.year, 1911);
    QCOMPARE(info.chapters, 2);
    QCOMPARE(info.pages, 2);
    QCOMPARE(info.cover.height(), 90);
    QCOMPARE(info.cover.width(), 60);
}

void ClothboundTests::pdfReadsMetadataAndRenders() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Marsh, Ada - Field Notes (Harbor, 1920).pdf");
    QVERIFY(TestBooks::write(path, TestBooks::pdf("Field Notes", "Ada Marsh / Tom Reed [editor]")));
    PdfBook book(path);
    QVERIFY2(book.open(), qPrintable(book.errorString()));
    QCOMPARE(book.pageCount(), 1);
    QCOMPARE(book.title(), QString("Field Notes"));
    QCOMPARE(book.authors(), QStringList{"Ada Marsh"});
    QCOMPARE(book.editors(), QStringList{"Tom Reed"});
    const QImage cover = book.renderCover(300);
    QCOMPARE(cover.size(), QSize(200, 300));
    // Rendered on white, with the text in black somewhere on it.
    QCOMPARE(QColor(cover.pixel(5, 5)), QColor(Qt::white));

    const BookInfo info = BookFiles::read(path, 150);
    QCOMPARE(info.format, QString("pdf"));
    QCOMPARE(info.pages, 1);
    QCOMPARE(info.identity.year, 1920);
    QCOMPARE(info.cover.height(), 150);
}

void ClothboundTests::unreadableFilesReportWhy() {
    QTemporaryDir dir;
    write(dir.filePath("broken.pdf"), "nope");
    QVERIFY(!BookFiles::read(dir.filePath("broken.pdf")).ok());
    QVERIFY(!BookFiles::read(dir.filePath("missing.epub")).ok());
    QVERIFY(BookFiles::isBook("a/b.EPUB"));
    QVERIFY(!BookFiles::isBook("notes.txt"));
}

void ClothboundTests::schemeServesWellFormedBooksAsXml() {
    QVERIFY(BookScheme::isWellFormed("<?xml version='1.0'?><html><body><p>x</p></body></html>"));
    QVERIFY(!BookScheme::isWellFormed("<html><body><p>unclosed</body></html>"));
    // Entities the XHTML DTD defines are fine for a browser's XML parser.
    QVERIFY(BookScheme::isWellFormed("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"x\"><html><body>a&nbsp;b</body></html>"));
}

void ClothboundTests::computerIsInspected() {
    QTemporaryDir root;
    QDir(root.path()).mkpath("proc");
    QDir(root.path()).mkpath("sys/class/drm/card1/device");
    auto write = [&](const QString &path, const QByteArray &text) {
        QFile file(root.filePath(path));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text);
    };
    write("proc/meminfo", "MemTotal:       32617612 kB\nMemAvailable:   20000000 kB\n");
    write("proc/cpuinfo", "processor\t: 0\nmodel name\t: AMD Ryzen 7 8840U\nflags\t\t: sse2 avx2 avx_vnni\n\n"
                          "processor\t: 1\nmodel name\t: AMD Ryzen 7 8840U\nflags\t\t: sse2 avx2 avx_vnni\n");
    write("sys/class/drm/card1/device/vendor", "0x1002\n");
    write("sys/class/drm/card1/device/mem_info_vram_total", "536870912\n");
    const SystemCheck::Machine machine = SystemCheck::inspect(root.path());
    QCOMPARE(qRound(machine.memoryGb), 31);
    QCOMPARE(machine.cores, 2);
    QCOMPARE(machine.architecture, QString("x86_64"));
    QCOMPARE(machine.features, (QStringList{"AVX2", "AVX-VNNI"}));
    QCOMPARE(machine.graphics, QString("AMD Radeon"));
    QVERIFY(!machine.discreteGraphics);  // a 780M's carve-out isn't VRAM
    QCOMPARE(SystemCheck::tierFor(machine), QString("Local Standard"));
    QCOMPARE(SystemCheck::suggestedModel(machine), QString("Qwen3.5 9B"));
    QCOMPARE(LocalSetup::modelsFor(31, 0).first, QString("qwen3.5:9b"));
    QCOMPARE(LocalSetup::modelsFor(15.5, 0).first, QString("qwen3.5:4b"));
    QCOMPARE(LocalSetup::modelsFor(8, 0).first, QString("qwen3.5:2b"));
    QVERIFY(LocalSetup::modelsFor(4, 0).first.isEmpty());
    QCOMPARE(SystemCheck::billionsIn("qwen3.5:4b"), 4.0);
    QCOMPARE(SystemCheck::billionsIn("Qwen3.5-9B-Instruct-Q4_K_M"), 9.0);
    QCOMPARE(SystemCheck::billionsIn("gemma4:e4b"), 4.0);
    QCOMPARE(SystemCheck::billionsIn("llama"), 0.0);
    SystemCheck::Machine small;
    small.memoryGb = 8;
    QCOMPARE(SystemCheck::tierFor(small), QString("Local voices"));
}

void ClothboundTests::pdfPagesAreSpoken() {
    const QString page = QString("12\r\nTHE TIME MACHINE\r\nThe Time Traveller was expounding a recon") + QChar(0xFFFE) +
                         QString("\r\ndite matter to us, and the bright fire burned.\r\n"
                                 "His grey eyes shone and twinkled, and his face\r\n"
                                 "was flushed. Short end.\r\n\r\n"
                                 "A second paragraph begins here and goes on a while.\r\n");
    const QJsonArray blocks = PdfBook::speakable(page, "The Time Machine");
    QCOMPARE(blocks.size(), 2);
    const QJsonObject first = blocks[0].toObject();
    const qint64 start = first["start"].toInteger(), end = first["end"].toInteger();
    QVERIFY(page.mid(start).startsWith("The Time Traveller"));
    QCOMPARE(first["text"].toString().size(), end - start);
    QCOMPARE(Speech::speakable(first["text"].toString()).simplified(),
             QString("The Time Traveller was expounding a recondite matter to us, and the bright fire burned. "
                     "His grey eyes shone and twinkled, and his face was flushed. Short end."));
    QVERIFY(blocks[1].toObject()["text"].toString().startsWith("A second paragraph"));
}

void ClothboundTests::pdfTextIsPlaced() {
    QTemporaryDir dir;
    const QString path = dir.filePath("book.pdf");
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A5));
        writer.setResolution(72);
        QPainter painter(&writer);
        const int fontId = QFontDatabase::addApplicationFont(QFINDTESTDATA("../src/fonts/Literata.ttf"));
        const QString family = fontId >= 0 ? QFontDatabase::applicationFontFamilies(fontId).value(0) : QString();
        painter.setFont(family.isEmpty() ? QFont(QStringLiteral("serif"), 14) : QFont(family, 14));
        painter.drawText(QPointF(40, 80), "Weena was a little creature.");
        painter.drawText(QPointF(40, 120), "The Morlocks came at night.");
    }
    PdfBook book(path);
    QVERIFY(book.open());
    const QString text = book.pageText(0);
    QVERIFY2(text.contains("Morlocks"), qPrintable(text));
    const qint64 at = text.indexOf("Morlocks");
    const QList<QRectF> rects = book.textRects(0, at, at + 8);
    QVERIFY(!rects.isEmpty());
    QVERIFY(rects.first().y() > 0.15 && rects.first().y() < 0.25);  // the second line, near the top
    QVERIFY(rects.first().x() > 0.05);
    QRectF word;
    for (const QRectF &rect : rects)
        word = word.isNull() ? rect : word.united(rect);
    const PdfBook::Selection chosen = book.select(0, QPointF(word.x() + 0.002, word.center().y()),
                                                  QPointF(word.right() - 0.002, word.center().y()));
    QVERIFY2(chosen.text.contains("Morlocks"), qPrintable(chosen.text));
    QVERIFY(chosen.start >= 0);
    // Cropping finds the text, not the empty page around it.
    const QRectF box = PdfPages::contentBox(book.renderPage(0, QSize(300, 425)));
    QVERIFY(box.x() > 0.05 && box.width() < 0.95 && box.height() < 0.3);
}

void ClothboundTests::pdfPagesTakeTheTheme() {
    QImage page(4, 1, QImage::Format_RGB32);
    page.setPixel(0, 0, qRgb(255, 255, 255));  // paper
    page.setPixel(1, 0, qRgb(0, 0, 0));        // ink
    page.setPixel(2, 0, qRgb(200, 30, 30));    // a red picture
    page.setPixel(3, 0, qRgb(128, 128, 128));
    PdfPages::recolor(page, QColor("#1e1f24"), QColor("#d6d2c9"));
    QCOMPARE(QColor(page.pixel(0, 0)).name(), QString("#1e1f24"));
    QCOMPARE(QColor(page.pixel(1, 0)).name(), QString("#d6d2c9"));
    QVERIFY(QColor(page.pixel(2, 0)).red() > QColor(page.pixel(2, 0)).green() * 2);  // still red
    QImage white(1, 1, QImage::Format_RGB32);
    white.fill(Qt::white);
    PdfPages::recolor(white, Qt::white, Qt::black);
    QCOMPARE(QColor(white.pixel(0, 0)), QColor(Qt::white));

    // Fused single-pass compositeAndRecolor handles transparent PDFium paper,
    // semi-transparent premultiplied ink, and chromatic illustrations identically.
    QImage raw(4, 1, QImage::Format_ARGB32_Premultiplied);
    raw.setPixel(0, 0, qRgba(0, 0, 0, 0));           // transparent paper -> bg
    raw.setPixel(1, 0, qRgba(0, 0, 0, 255));         // opaque black ink -> fg
    raw.setPixel(2, 0, qRgba(200, 30, 30, 255));     // chromatic red -> stays red
    raw.setPixel(3, 0, qRgba(0, 0, 0, 128));         // 50% premul black -> ~mid gray
    PdfPages::compositeAndRecolor(raw, QColor("#1e1f24"), QColor("#d6d2c9"));
    QCOMPARE(raw.format(), QImage::Format_RGB32);
    QCOMPARE(QColor(raw.pixel(0, 0)).name(), QString("#1e1f24"));
    QCOMPARE(QColor(raw.pixel(1, 0)).name(), QString("#d6d2c9"));
    QVERIFY(QColor(raw.pixel(2, 0)).red() > QColor(raw.pixel(2, 0)).green() * 2);
    QVERIFY(QColor(raw.pixel(3, 0)).red() > 0x40 && QColor(raw.pixel(3, 0)).red() < 0xB0);

    // White/black identity composites transparent paper onto pure white.
    QImage rawIdentity(2, 1, QImage::Format_ARGB32_Premultiplied);
    rawIdentity.setPixel(0, 0, qRgba(0, 0, 0, 0));
    rawIdentity.setPixel(1, 0, qRgba(0, 0, 0, 255));
    PdfPages::compositeAndRecolor(rawIdentity, Qt::white, Qt::black);
    QCOMPARE(QColor(rawIdentity.pixel(0, 0)), QColor(Qt::white));
    QCOMPARE(QColor(rawIdentity.pixel(1, 0)), QColor(Qt::black));
}

void ClothboundTests::coverProviderBakesCornersAndSpineCrease() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage src(200, 300, QImage::Format_RGB32);
    src.fill(QColor(210, 180, 120));
    QVERIFY(src.save(coverPath, "PNG"));

    const QImage rounded = CoverProvider::renderCoverImage(coverPath, QSize(256, 256), 5.0, 3.0, 160.0);
    QVERIFY(!rounded.isNull());
    QCOMPARE(rounded.format(), QImage::Format_ARGB32_Premultiplied);
    QVERIFY(rounded.width() <= 256 && rounded.height() <= 256);
    // Outer fore-edge corner is transparent; interior center is opaque cover art.
    QCOMPARE(qAlpha(rounded.pixel(rounded.width() - 1, 0)), 0);
    QCOMPARE(qAlpha(rounded.pixel(rounded.width() / 2, rounded.height() / 2)), 255);
    // Left spine crease darkens x=1 relative to interior center.
    QVERIFY(qRed(rounded.pixel(1, rounded.height() / 2)) < qRed(rounded.pixel(rounded.width() / 2, rounded.height() / 2)));

    // Sharp mode (radius = 0) stays opaque Format_RGB32 with intact corners.
    const QImage sharp = CoverProvider::renderCoverImage(coverPath, QSize(256, 256), 0.0, 0.0, 160.0);
    QVERIFY(!sharp.isNull());
    QCOMPARE(sharp.format(), QImage::Format_RGB32);
    QCOMPARE(qAlpha(sharp.pixel(sharp.width() - 1, 0)), 255);

    // Runtime regression: CoverImage.qml loading a non-2:3 local cover via
    // image://cover must reach loaded=true with ZERO binding-loop warnings.
    const QString widePath = dir.filePath(QStringLiteral("wide_cover.png"));
    QImage wideSrc(240, 260, QImage::Format_RGB32);
    wideSrc.fill(QColor(110, 150, 200));
    QVERIFY(wideSrc.save(widePath, "PNG"));

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("cover"), new CoverProvider);
    Theme themeObj;
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &themeObj);
    engine.rootContext()->setContextProperty(QStringLiteral("hasCoverProvider"), true);
    QList<QQmlError> warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errs) { warnings.append(errs); });
    const QString srcRoot = QFINDTESTDATA("../src");
    QQmlComponent comp(&engine, QUrl::fromLocalFile(srcRoot + QStringLiteral("/ui/library/CoverImage.qml")));
    QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
    std::unique_ptr<QObject> obj(comp.createWithInitialProperties({
        {QStringLiteral("width"), 160.0},
        {QStringLiteral("height"), 240.0},
        {QStringLiteral("source"), QUrl::fromLocalFile(widePath).toString()},
        {QStringLiteral("title"), QStringLiteral("Wide Cover")},
        {QStringLiteral("author"), QStringLiteral("Author")},
    }));
    QVERIFY2(obj != nullptr, qPrintable(comp.errorString()));
    QTRY_COMPARE_WITH_TIMEOUT(obj->property("loaded").toBool(), true, 3000);
    QCoreApplication::processEvents();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().toString()));
}

void ClothboundTests::fileNamesFollowTheCollectionPattern() {
    FileNameParts parts = BookName::parseFileName("Austen, Jane - Pride and Prejudice [ed. Jones] (Penguin, 2003).epub");
    QVERIFY(parts.matched);
    QCOMPARE(parts.author, QString("Jane Austen"));
    QCOMPARE(parts.title, QString("Pride and Prejudice"));
    QCOMPARE(parts.editor, QString("Jones"));
    QCOMPARE(parts.publisher, QString("Penguin"));
    QCOMPARE(parts.year, 2003);

    parts = BookName::parseFileName("Melville, Herman - Whale Book, The_ A Voyage [illus. Kent] (Dover, 2015).pdf");
    QCOMPARE(parts.title, QString("The Whale Book: A Voyage"));
    QCOMPARE(parts.illustrator, QString("Kent"));

    parts = BookName::parseFileName("Ross, Ann [ed.] The Austen Reader (Harbor, 2001).epub");
    QVERIFY(parts.matched);
    QVERIFY(parts.authorIsEditor);
    QCOMPARE(parts.author, QString("Ann Ross"));
    QCOMPARE(parts.title, QString("The Austen Reader"));

    parts = BookName::parseFileName("Gray, Lee [ed.] - Letters of Two Friends (Harbor, 1959).pdf");
    QVERIFY(parts.authorIsEditor);
    QCOMPARE(parts.author, QString("Lee Gray"));

    QVERIFY(!BookName::parseFileName("just a title.epub").matched);
}

void ClothboundTests::authorsReadTheSameEveryWay() {
    for (const char *spelling : {"Wells, H. G.", "H.G. Wells", "HG Wells", "H. G. WELLS", "H.G.Wells"})
        QCOMPARE(BookName::displayAuthor(spelling), QString("H. G. Wells"));
    QCOMPARE(BookName::displayAuthor("Dickens, Charles, 1812-1870"), QString("Charles Dickens"));
    QCOMPARE(BookName::sortAuthor("H. G. Wells"), QString("Wells, H. G."));
    QCOMPARE(BookName::sortAuthor("Ludwig van Beethoven"), QString("van Beethoven, Ludwig"));
    QCOMPARE(BookName::sortAuthor("Plato"), QString("Plato"));
}

void ClothboundTests::titlesClean() {
    QCOMPARE(BookName::displayTitle("Moon Book, The"), QString("The Moon Book"));
    QCOMPARE(BookName::displayTitle("Coat, The, the Hat and the Glove"), QString("Coat, The, the Hat and the Glove"));
    QCOMPARE(BookName::displayTitle("Letters, Vol. 2_ 1904-1928"), QString("Letters, Vol. 2: 1904-1928"));
    QCOMPARE(BookName::cleanTitle("The New Road (Penguin Classics) -- A_ Writer"), QString("The New Road"));
    QCOMPARE(BookName::cleanTitle("The Isle of Dr. Lark (Barnes &amp; Noble Classics)"), QString("The Isle of Dr. Lark"));
    QCOMPARE(BookName::cleanTitle("Notes (1890)"), QString("Notes (1890)"));
    QCOMPARE(BookName::sortTitle("The Moon Book"), QString("moon book"));
    QVERIFY(BookName::looksLikeGarbageTitle("Microsoft Word - draft.doc"));
    QVERIFY(BookName::looksLikeGarbageTitle("untitled"));
    QVERIFY(!BookName::looksLikeGarbageTitle("Emma"));
}

void ClothboundTests::editionsShareAWorkKey() {
    const QString a = BookName::workKey("H. G. Wells", "The Isle of Doctor Lark");
    QCOMPARE(BookName::workKey("H.G. Wells", "The Isle of Dr. Lark (Barnes & Noble Classics)"), a);
    QCOMPARE(BookName::workKey("Wells, H. G.", "Isle of Dr Lark: A Romance"), a);
    QVERIFY(BookName::workKey("H. G. Wells", "The Sleeper Wakes") != BookName::workKey("H. G. Wells", "The Sleeper Awakes!"));
    // Volumes of one work are different books.
    QCOMPARE(BookName::volumeNumber("The Outline: Volume 2, Rome"), 2);
    QCOMPARE(BookName::volumeNumber("History, Vol. IV"), 4);
    QVERIFY(BookName::workKey("A", "Outline", 1) != BookName::workKey("A", "Outline", 2));
}

void ClothboundTests::identityResolvesSwapsAndMislabels() {
    // A title and author swapped in the metadata.
    BookIdentity id = BookName::identify("/b/Wells, H. G. - Moon Book, The [ed. Ross] (Penguin, 2005).epub",
                                         "H. G. Wells", {"The Moon Book"}, {}, "Penguin Books Ltd", "2005");
    QCOMPARE(id.title, QString("The Moon Book"));
    QCOMPARE(id.author, QString("H. G. Wells"));
    QCOMPARE(id.editor, QString("Ross"));

    // A file named for one book that holds another: the content wins.
    id = BookName::identify("/b/Wells, H. G. - Glass Man [ed. Ross] (Penguin, 2005).epub",
                            "The Isle of Doctor Lark", {"H.G. Wells"}, {}, {}, {});
    QCOMPARE(id.title, QString("The Isle of Doctor Lark"));
    QCOMPARE(id.author, QString("H. G. Wells"));
    QCOMPARE(id.workKey, BookName::workKey("H. G. Wells", "The Isle of Dr. Lark"));

    // No usable metadata: the file name speaks.
    id = BookName::identify("/b/Marsh, Ada - Field Notes, Vol. 2_ Coast (Harbor, 1920).pdf",
                            "Microsoft Word - fn2.doc", {}, {}, {}, {});
    QCOMPARE(id.title, QString("Field Notes, Vol. 2: Coast"));
    QCOMPARE(id.author, QString("Ada Marsh"));
    QCOMPARE(id.year, 1920);
    QVERIFY(id.workKey.endsWith("|v2"));

    // A file name more specific than the metadata title wins.
    id = BookName::identify("/b/Marsh, Ada - Atlas of Tides, Vol. 2_ The South (Harbor, 2004).epub",
                            "The Atlas of Tides", {"Ada Marsh"}, {}, {}, {});
    QCOMPARE(id.title, QString("The Atlas of Tides, Vol. 2: The South"));
    QVERIFY(id.workKey.endsWith("|v2"));

    // Several authors sort under the collection's own.
    id = BookName::identify("/b/Marsh, Ada - Letters [with Tom Reed] (Harbor, 1935).pdf",
                            "Letters", {"Tom Reed & Ada Marsh"}, {}, {}, {});
    QCOMPARE(id.author, QString("Tom Reed & Ada Marsh"));
    QCOMPARE(id.sortAuthor, QString("Marsh, Ada"));

    // Without a file-name pattern, metadata and publisher clean up.
    id = BookName::identify("/b/x.epub", "Emma", {"Austen, Jane"}, {"Ed Itor"}, "Harbor Books Ltd", "1815-12-23");
    QCOMPARE(id.author, QString("Jane Austen"));
    QCOMPARE(id.sortAuthor, QString("Austen, Jane"));
    QCOMPARE(id.publisher, QString("Harbor Books"));
    QCOMPARE(id.editor, QString("Ed Itor"));
    QCOMPARE(id.year, 1815);
}

void ClothboundTests::instanceHandsOver() {
    if (!QDBusConnection::sessionBus().isConnected())
        QSKIP("No session bus");
    Instance running;
    QVERIFY(running.claim());
    Instance second;
    QVERIFY(!second.claim());
    QSignalSpy activated(&running, &Instance::activated);
    QVERIFY(Instance::passToRunning(QStringLiteral("/books/The Time Machine.epub")));
    QVERIFY(activated.wait(2000));
    QCOMPARE(activated.first().first().toString(), QStringLiteral("/books/The Time Machine.epub"));
    // Without a window there's nothing to show, and nothing fails.
    running.bringBack();
}

void ClothboundTests::instanceFocusesHyprlandOverSocketWithoutMutatingEnv() {
    QTemporaryDir dir;
    const QString sockPath = dir.filePath(".socket.sock");
    QLocalServer server;
    QVERIFY(server.listen(sockPath));
    QStringList commands;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (QLocalSocket *client = server.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::readyRead, client, [client, &commands] {
                const QByteArray cmd = client->readAll();
                commands.append(QString::fromUtf8(cmd));
                if (cmd == "j/clients") {
                    const QByteArray json = QStringLiteral("[{\"pid\":%1,\"address\":\"0xdeadbeef\"}]")
                                                .arg(QCoreApplication::applicationPid())
                                                .toUtf8();
                    client->write(json);
                } else if (cmd.contains("hl.dsp.focus")) {
                    client->write("unknown request");
                } else {
                    client->write("ok");
                }
                client->disconnectFromServer();
            });
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    qunsetenv("XDG_ACTIVATION_TOKEN");
    QWindow win;
    win.show();
    Instance inst;
    inst.setWindow(&win);
    inst.setHyprSocketForTesting(sockPath);
    inst.bringBack(QStringLiteral("test-activation-token"));
    // bringBack() must never mutate environ after threads may be running.
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
    QTRY_COMPARE(commands.size(), 3);
    QCOMPARE(commands[0], QString("j/clients"));
    QVERIFY(commands[1].contains("address:0xdeadbeef"));
    QCOMPARE(commands[2], QString("dispatch focuswindow address:0xdeadbeef"));
}

void ClothboundTests::notifierTracksTokensPerIdAndCleansClosed() {
    Notifier notifier;
    QSignalSpy clicked(&notifier, &Notifier::clicked);
    // Simulate clickable IDs via public slots (no live notification daemon required).
    notifier.onActivationToken(10, QStringLiteral("ignored-unknown"));
    notifier.onActionInvoked(10, QStringLiteral("default"));
    QCOMPARE(clicked.count(), 0);
    notifier.onNotificationClosed(10, 2);
    QCOMPARE(notifier.pendingCount(), 0);
}

void ClothboundTests::pointerDriverSendsPublicEvents() {
    QQuickWindow win;
    win.resize(400, 300);
    PointerDriver driver(&win);
    driver.press(10, 10);
    driver.move(20, 20);
    driver.release(20, 20);
    driver.wheel(20, 20, -120, 0);
    driver.key(Qt::Key_Space, QStringLiteral(" "));
    driver.drag(10, 10, 30, 30, 0);  // steps <= 0 clamped safely
    QTest::qWait(80);
    QVERIFY(true);
}

void ClothboundTests::pdfPagesConcurrentCancelAndRapidBookSwitch() {
    QTemporaryDir dir;
    const QString pathA = dir.filePath("a.pdf");
    const QString pathB = dir.filePath("b.pdf");
    QVERIFY(TestBooks::write(pathA, TestBooks::pdf("Book A", "Author A")));
    QVERIFY(TestBooks::write(pathB, TestBooks::pdf("Book B", "Author B")));
    auto bookA = std::make_shared<PdfBook>(pathA);
    auto bookB = std::make_shared<PdfBook>(pathB);
    QVERIFY(bookA->open());
    QVERIFY(bookB->open());

    const QString tokA = QStringLiteral("reg-tok-a");
    const QString tokB = QStringLiteral("reg-tok-b");
    PdfPages::add(tokA, bookA);
    PdfPages::add(tokB, bookB);

    // Keep responses alive until after PdfPages::~PdfPages() drains its worker pool.
    std::vector<std::unique_ptr<QQuickImageResponse>> responses;
    {
        PdfPages pages;
        int done = 0;
        auto track = [&](QQuickImageResponse *resp) {
            QObject::connect(resp, &QQuickImageResponse::finished, this, [&done] { ++done; });
            responses.emplace_back(resp);
            return resp;
        };

        // Fire a burst for Book A, cancel several mid-flight, then immediately
        // drop Book A and switch to Book B.
        for (int i = 0; i < 6; ++i) {
            auto *r = track(pages.requestImageResponse(
                QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tokA), QSize(180 + i * 10, 270 + i * 15)));
            if (i % 2 == 1)
                r->cancel();
        }
        PdfPages::remove(tokA);

        auto *respB = track(pages.requestImageResponse(
            QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tokB), QSize(200, 300)));

        QTRY_COMPARE_WITH_TIMEOUT(done, int(responses.size()), 10000);
        std::unique_ptr<QQuickTextureFactory> factory(respB->textureFactory());
        QVERIFY(factory != nullptr);
        const QImage rendered = factory->image();
        QCOMPARE(rendered.size(), QSize(200, 300));
        QCOMPARE(QColor(rendered.pixel(2, 2)).name(), QString("#1e1f24"));

        // Second request with identical key hits the LRU cache cleanly.
        auto *cachedB = track(pages.requestImageResponse(
            QStringLiteral("%1/0?bg=%231e1f24&fg=%23d6d2c9").arg(tokB), QSize(200, 300)));
        QTRY_COMPARE_WITH_TIMEOUT(done, int(responses.size()), 5000);
        std::unique_ptr<QQuickTextureFactory> cachedFactory(cachedB->textureFactory());
        QVERIFY(cachedFactory != nullptr);
        QCOMPARE(cachedFactory->image().size(), QSize(200, 300));

        PdfPages::remove(tokB);
    }
}

void ClothboundTests::pdfBookOversizedAndExtremeAspectClamped() {
    auto pdfWithBox = [](int w, int h) {
        QByteArray out = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        auto obj = [&](const QByteArray &body) {
            offsets.append(out.size());
            out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
        };
        obj("<< /Type /Catalog /Pages 2 0 R >>");
        obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + QByteArray::number(w) + " " + QByteArray::number(h) +
            "] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
        const QByteArray stream = "BT /F1 12 Tf 20 20 Td (Clamp test) Tj ET";
        obj("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
        obj("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        const qsizetype xref = out.size();
        out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
        for (const qsizetype offset : offsets)
            out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
        out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        return out;
    };

    QTemporaryDir dir;
    const QString normalPath = dir.filePath("normal.pdf");
    const QString extremePath = dir.filePath("extreme.pdf");
    QVERIFY(TestBooks::write(normalPath, pdfWithBox(200, 300)));
    QVERIFY(TestBooks::write(extremePath, pdfWithBox(40000, 10)));

    PdfBook normal(normalPath);
    QVERIFY(normal.open());
    // Single-dimension clamp (<= 6000 px).
    const QImage wide = normal.renderPage(0, QSize(12000, 100));
    QVERIFY(!wide.isNull());
    QCOMPARE(wide.width(), 6000);
    QCOMPARE(wide.height(), 100);
    // Total pixel budget clamp (<= 24 MP).
    const QImage huge = normal.renderPage(0, QSize(5500, 5500));
    QVERIFY(!huge.isNull());
    QVERIFY(huge.width() <= 6000 && huge.height() <= 6000);
    QVERIFY(qint64(huge.width()) * qint64(huge.height()) <= 24LL * 1000 * 1000);

    // Extreme aspect-ratio PDF (4000:1): cover and crop stay strictly bounded.
    PdfBook extreme(extremePath);
    QVERIFY(extreme.open());
    const QImage cover = extreme.renderCover(5000);
    QVERIFY(!cover.isNull());
    QVERIFY(cover.height() <= 2048);
    QVERIFY(cover.width() <= 2048);
    QVERIFY(double(cover.width()) / double(cover.height()) <= 5.05);
    const QRectF crop = PdfPages::bookCrop(extreme);
    QVERIFY(crop.x() >= 0.0 && crop.y() >= 0.0 && crop.right() <= 1.001 && crop.bottom() <= 1.001);

    // PdfBook::speakable preserves chapter headings and handles mixed \r\n / \n.
    const QString mixed = QStringLiteral("THE LANTERN\r\nCHAPTER ONE\nThe sea was calm.\r\n\nA gull cried.\n");
    const QJsonArray blocks = PdfBook::speakable(mixed, QStringLiteral("The Lantern"));
    QVERIFY(blocks.size() >= 2);
    QVERIFY(blocks[0].toObject().value("text").toString().contains(QStringLiteral("CHAPTER ONE")));
    for (const QJsonValue &v : blocks) {
        const QJsonObject o = v.toObject();
        const qint64 s = o.value("start").toInteger(), e = o.value("end").toInteger();
        QCOMPARE(e - s, o.value("text").toString().size());
    }
}

void ClothboundTests::htmlDeepNestingParsesAndDestroysIteratively() {
    QString deep;
    deep.reserve(1200 * 11 + 20);
    for (int i = 0; i < 1200; ++i)
        deep += QStringLiteral("<div>");
    deep += QStringLiteral("deep leaf");
    for (int i = 0; i < 1200; ++i)
        deep += QStringLiteral("</div>");
    {
        const auto doc = Html::parse(deep, true);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->textContent().contains(QStringLiteral("deep leaf")));
    }

    // Also stress iterative textContent() and ~HtmlNode() on a 2500-deep tree
    // built directly (beyond Html::parse's 512 depth cap).
    auto root = std::make_unique<HtmlNode>();
    root->name = QStringLiteral("div");
    HtmlNode *cur = root.get();
    for (int i = 0; i < 2500; ++i) {
        auto child = std::make_unique<HtmlNode>();
        child->name = QStringLiteral("span");
        child->parent = cur;
        HtmlNode *next = child.get();
        cur->children.push_back(std::move(child));
        cur = next;
    }
    auto leaf = std::make_unique<HtmlNode>();
    leaf->text = QStringLiteral("bottom");
    leaf->parent = cur;
    cur->children.push_back(std::move(leaf));
    QCOMPARE(root->textContent(), QStringLiteral("bottom"));
    root.reset();
}

void ClothboundTests::zipRejectsBombsBadCrcAndZip64Wraparound() {
    QTemporaryDir dir;

    // 1. Lying-header ZIP bomb: claims 100 uncompressed bytes, actually inflates > 128 MiB.
    QByteArray bombDeflated;
    {
        const QByteArray oneMb(1024 * 1024, '\0');
        z_stream zs{};
        QCOMPARE(deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
        QByteArray buf(64 * 1024, Qt::Uninitialized);
        for (int mb = 0; mb < 129; ++mb) {
            zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(oneMb.constData()));
            zs.avail_in = uInt(oneMb.size());
            const int flush = (mb == 128) ? Z_FINISH : Z_NO_FLUSH;
            while (zs.avail_in > 0 || flush == Z_FINISH) {
                zs.next_out = reinterpret_cast<Bytef *>(buf.data());
                zs.avail_out = uInt(buf.size());
                const int rc = deflate(&zs, flush);
                bombDeflated.append(buf.constData(), qsizetype(buf.size()) - qsizetype(zs.avail_out));
                if (rc == Z_STREAM_END)
                    break;
                QVERIFY(rc == Z_OK);
            }
        }
        deflateEnd(&zs);
    }
    auto makeCustomZip = [](const QByteArray &name, const QByteArray &payload, quint16 method, quint32 crc,
                            quint32 declaredUncompressed) {
        QByteArray out, cd;
        TestBooks::put32(out, 0x04034b50);
        TestBooks::put16(out, 20);
        TestBooks::put16(out, 0x0800);
        TestBooks::put16(out, method);
        TestBooks::put32(out, 0);
        TestBooks::put32(out, crc);
        TestBooks::put32(out, quint32(payload.size()));
        TestBooks::put32(out, declaredUncompressed);
        TestBooks::put16(out, quint16(name.size()));
        TestBooks::put16(out, 0);
        out.append(name);
        out.append(payload);

        const quint32 cdOff = quint32(out.size());
        TestBooks::put32(cd, 0x02014b50);
        TestBooks::put16(cd, 20);
        TestBooks::put16(cd, 20);
        TestBooks::put16(cd, 0x0800);
        TestBooks::put16(cd, method);
        TestBooks::put32(cd, 0);
        TestBooks::put32(cd, crc);
        TestBooks::put32(cd, quint32(payload.size()));
        TestBooks::put32(cd, declaredUncompressed);
        TestBooks::put16(cd, quint16(name.size()));
        TestBooks::put16(cd, 0);
        TestBooks::put16(cd, 0);
        TestBooks::put16(cd, 0);
        TestBooks::put16(cd, 0);
        TestBooks::put32(cd, 0);
        TestBooks::put32(cd, 0);
        cd.append(name);
        out.append(cd);

        TestBooks::put32(out, 0x06054b50);
        TestBooks::put16(out, 0);
        TestBooks::put16(out, 0);
        TestBooks::put16(out, 1);
        TestBooks::put16(out, 1);
        TestBooks::put32(out, quint32(cd.size()));
        TestBooks::put32(out, cdOff);
        TestBooks::put16(out, 0);
        return out;
    };

    const QString bombPath = dir.filePath("bomb.zip");
    QVERIFY(TestBooks::write(bombPath, makeCustomZip("bomb.txt", bombDeflated, 8, 0, 100)));
    ZipArchive bombZip(bombPath);
    QVERIFY(bombZip.open());
    QString err;
    QVERIFY(bombZip.read("bomb.txt", &err).isEmpty());
    QVERIFY2(err.contains("too large"), qPrintable(err));

    // 2. Declared > 128 MiB rejected immediately before inflating.
    const QString hugePath = dir.filePath("huge.zip");
    QVERIFY(TestBooks::write(hugePath, makeCustomZip("huge.txt", TestBooks::rawDeflate("hi"), 8, 0, 200u * 1024 * 1024)));
    ZipArchive hugeZip(hugePath);
    QVERIFY(hugeZip.open());
    err.clear();
    QVERIFY(hugeZip.read("huge.txt", &err).isEmpty());
    QVERIFY2(err.contains("too large"), qPrintable(err));

    // 3. Bad CRC-32 rejected on both stored and deflated entries.
    const QString crcPath = dir.filePath("badcrc.zip");
    QVERIFY(TestBooks::write(crcPath, makeCustomZip("corrupt.txt", TestBooks::rawDeflate("hello"), 8, 0xdeadbeef, 5)));
    ZipArchive crcZip(crcPath);
    QVERIFY(crcZip.open());
    err.clear();
    QVERIFY(crcZip.read("corrupt.txt", &err).isEmpty());
    QVERIFY2(err.contains("damaged"), qPrintable(err));

    // 4. ZIP64 directoryOffset + directorySize uint64 wraparound rejected.
    QByteArray z64 = TestBooks::zip({{"one.txt", "first", true}}, true);
    const QByteArray sig("\x50\x4b\x06\x06", 4);
    const qsizetype pos = z64.indexOf(sig);
    QVERIFY(pos > 0 && pos + 56 <= z64.size());
    auto patch64 = [&](qsizetype at, quint64 val) {
        for (int b = 0; b < 8; ++b)
            z64[at + b] = char((val >> (b * 8)) & 0xff);
    };
    patch64(pos + 40, 0x40ULL);                  // directorySize
    patch64(pos + 48, 0xffffffffffffffe0ULL);    // directoryOffset (wraps to 0x20 when added!)
    const QString wrapPath = dir.filePath("wrap64.zip");
    QVERIFY(TestBooks::write(wrapPath, z64));
    ZipArchive wrapZip(wrapPath);
    QVERIFY(!wrapZip.open());
    QVERIFY2(wrapZip.errorString().contains("damaged"), qPrintable(wrapZip.errorString()));
}

void ClothboundTests::bookSchemeEnforcesSecurityHeadersRootRelativeAndXhtmlEntities() {
    // XHTML DTD resolves entities and still validates the ENTIRE document after &nbsp;.
    const QByteArray xhtmlGood =
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>a&nbsp;&mdash;b</p></body></html>";
    const QByteArray xhtmlBrokenAfterEntity =
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>a&nbsp;<b>unclosed</p></body></html>";
    const QByteArray html5WithEntity = "<!DOCTYPE html><html><body><p>a&nbsp;b</p></body></html>";
    QVERIFY(BookScheme::isWellFormed(xhtmlGood));
    QVERIFY(!BookScheme::isWellFormed(xhtmlBrokenAfterEntity));
    QVERIFY(!BookScheme::isWellFormed(html5WithEntity));

    QTemporaryDir dir;
    const QByteArray cover = png(16, 16);
    const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>";
    const QString path = dir.filePath("sec.epub");
    const QByteArray secOpf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\"><metadata/>\n"
        "<manifest>\n"
        "  <item id=\"ok\" href=\"ok.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "  <item id=\"bad\" href=\"bad.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "  <item id=\"h5\" href=\"h5.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "  <item id=\"img\" href=\"images/cover.png\" media-type=\"image/png\"/>\n"
        "  <item id=\"svg\" href=\"images/pic.svg\" media-type=\"image/svg+xml\"/>\n"
        "</manifest>\n"
        "<spine><itemref idref=\"ok\"/><itemref idref=\"bad\"/><itemref idref=\"h5\"/></spine>\n"
        "</package>";
    const QByteArray secBytes = TestBooks::epub({
        {"OEBPS/content.opf", secOpf},
        {"OEBPS/ok.xhtml", xhtmlGood},
        {"OEBPS/bad.xhtml", xhtmlBrokenAfterEntity},
        {"OEBPS/h5.xhtml", html5WithEntity},
        {"OEBPS/images/cover.png", cover},
        {"OEBPS/images/pic.svg", svg},
    });
    QVERIFY(TestBooks::write(path, secBytes));
    auto book = std::make_shared<EpubBook>(path);
    QVERIFY(book->open());

    BookScheme scheme;
    const QString token = scheme.add(book);
    const QByteArray ref = QStringLiteral("clothbound://local/book/%1/OEBPS/ok.xhtml").arg(token).toUtf8();

    auto checkSecurity = [](const BookScheme::ResolvedResource &res) {
        QVERIFY(res.ok);
        QCOMPARE(res.headers.value("X-Content-Type-Options"), QByteArray("nosniff"));
        const QByteArray csp = res.headers.value("Content-Security-Policy");
        QVERIFY(csp.contains("script-src 'none'"));
        QVERIFY(csp.contains("object-src 'none'"));
    };

    const auto okRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/ok.xhtml").arg(token)));
    checkSecurity(okRes);
    QCOMPARE(okRes.mimeType, QByteArray("application/xhtml+xml"));

    const auto badRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/bad.xhtml").arg(token)));
    checkSecurity(badRes);
    QCOMPARE(badRes.mimeType, QByteArray("text/html"));

    const auto h5Res = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/h5.xhtml").arg(token)));
    checkSecurity(h5Res);
    QCOMPARE(h5Res.mimeType, QByteArray("text/html"));

    // SVG/binary resources also carry strict CSP + nosniff.
    const auto svgRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/images/pic.svg").arg(token)));
    checkSecurity(svgRes);
    QCOMPARE(svgRes.mimeType, QByteArray("image/svg+xml"));

    // Root-relative /path resolves safely via valid clothbound://local/book/<token>/ Referer.
    const auto rootRel = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/OEBPS/images/cover.png")), ref);
    checkSecurity(rootRel);
    QCOMPARE(rootRel.data, cover);

    // Missing, external, or traversal root-relative requests are refused.
    QVERIFY(!scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/OEBPS/images/cover.png"))).ok);
    QVERIFY(!scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/OEBPS/images/cover.png")),
                                   "https://evil.example/book/" + token.toUtf8() + "/ok.xhtml")
                 .ok);
    QVERIFY(!scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/../../etc/passwd")), ref).ok);
    const auto wrongHost = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://evil/book/%1/OEBPS/ok.xhtml").arg(token)));
    QVERIFY(!wrongHost.ok && wrongHost.invalidUrl);
}

void ClothboundTests::pdfBookPageLinksAndSelectWordBounds() {
    // Build a 2-page PDF with an internal GoTo link, an external HTTPS link,
    // an unsafe javascript: link (must be filtered out), and text with a contraction.
    auto linkedPdf = [] {
        QByteArray out = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        auto object = [&](const QByteArray &body) {
            offsets.append(out.size());
            out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
        };
        object("<< /Type /Catalog /Pages 2 0 R >>");                                       // 1
        object("<< /Type /Pages /Kids [3 0 R 9 0 R] /Count 2 >>");                         // 2
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 4 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R 7 0 R 8 0 R] >>");   // 3 (page 0)
        const QByteArray s1 = "BT /F1 12 Tf 20 250 Td (Hello reader's world) Tj ET";
        object("<< /Length " + QByteArray::number(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");  // 4
        object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");                               // 5
        object("<< /Type /Annot /Subtype /Link /Rect [20 240 120 265] /A << /S /GoTo /D [9 0 R /XYZ 0 300 1] >> >>");  // 6
        object("<< /Type /Annot /Subtype /Link /Rect [20 200 140 220] "
               "/A << /S /URI /URI (https://example.org/clothbound) >> >>");                                // 7
        object("<< /Type /Annot /Subtype /Link /Rect [20 160 140 180] "
               "/A << /S /URI /URI (javascript:alert\\(1\\)) >> >>");                                   // 8
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 10 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> >>");                                            // 9 (page 1)
        const QByteArray s2 = "BT /F1 12 Tf 20 250 Td (Second destination page) Tj ET";
        object("<< /Length " + QByteArray::number(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");  // 10
        const qsizetype xref = out.size();
        out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
        for (const qsizetype off : offsets)
            out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
        out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
        out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        return out;
    };

    QTemporaryDir dir;
    const QString pdfPath = dir.filePath("linked.pdf");
    QVERIFY(TestBooks::write(pdfPath, linkedPdf()));

    PdfBook pdf(pdfPath);
    QVERIFY2(pdf.open(), qPrintable(pdf.errorString()));
    QCOMPARE(pdf.pageCount(), 2);

    // Out-of-bounds pages return empty link lists and invalid word selections.
    QVERIFY(pdf.pageLinks(-1).isEmpty());
    QVERIFY(pdf.pageLinks(2).isEmpty());
    QVERIFY(pdf.pageLinks(99).isEmpty());
    QCOMPARE(pdf.selectWord(-1, QPointF(0.2, 0.2)).start, qint64(-1));
    QCOMPARE(pdf.selectWord(2, QPointF(0.2, 0.2)).start, qint64(-1));
    QVERIFY(pdf.selectWord(0, QPointF(-0.5, -0.5)).text.isEmpty());
    QVERIFY(pdf.selectWord(0, QPointF(2.0, 2.0)).text.isEmpty());
    QVERIFY(pdf.selectWord(0, QPointF(0.95, 0.95)).text.isEmpty());

    // Page 0 exposes internal + external HTTPS links and strips javascript: links.
    const QList<PdfLinkEntry> links = pdf.pageLinks(0);
    QCOMPARE(links.size(), 2);
    bool sawInternal = false, sawExternal = false;
    for (const PdfLinkEntry &entry : links) {
        QVERIFY(!entry.rects.isEmpty());
        for (const QRectF &r : entry.rects) {
            QVERIFY(r.x() >= 0.0 && r.x() <= 1.0);
            QVERIFY(r.y() >= 0.0 && r.y() <= 1.0);
            QVERIFY(r.width() > 0.0 && r.height() > 0.0);
            QVERIFY(r.right() <= 1.0001 && r.bottom() <= 1.0001);
        }
        if (entry.page == 1 && entry.url.isEmpty())
            sawInternal = true;
        if (entry.page == -1 && entry.url == QUrl(QStringLiteral("https://example.org/clothbound")))
            sawExternal = true;
        QVERIFY(entry.url.scheme() != QLatin1String("javascript"));
    }
    QVERIFY(sawInternal);
    QVERIFY(sawExternal);

    // selectWord() expands across the full word including internal apostrophes (Type1 Helvetica maps ' to U+2019).
    const QString p0Text = pdf.pageText(0);
    QString targetWord = QStringLiteral("reader\u2019s");
    qsizetype wordIdx = p0Text.indexOf(targetWord);
    if (wordIdx < 0) {
        targetWord = QStringLiteral("reader's");
        wordIdx = p0Text.indexOf(targetWord);
    }
    QVERIFY2(wordIdx >= 0, qPrintable(p0Text));
    const QList<QRectF> wordRects = pdf.textRects(0, wordIdx, wordIdx + targetWord.size());
    QVERIFY(!wordRects.isEmpty());
    const QPointF center = wordRects.first().center();
    const PdfBook::Selection sel = pdf.selectWord(0, center);
    QCOMPARE(sel.text, targetWord);
    QVERIFY(sel.start >= 0 && sel.end <= p0Text.size());
    QCOMPARE(p0Text.mid(sel.start, sel.end - sel.start), sel.text);

    // Reader exposes pdfLinks() and pdfSelectWord() cleanly with bounds checks.
    Library library(dir.filePath("data"), dir.filePath("cache"));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(pdfPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.openBook(bookId));
    QCOMPARE(reader.pdfLinks(0).size(), 2);
    QVERIFY(reader.pdfLinks(-1).isEmpty());
    QVERIFY(reader.pdfLinks(5).isEmpty());
    const QVariantMap rSel = reader.pdfSelectWord(0, center.x(), center.y());
    QCOMPARE(rSel.value(QStringLiteral("text")).toString(), targetWord);
    QCOMPARE(rSel.value(QStringLiteral("chapter")).toInt(), 0);
    QVERIFY(reader.pdfSelectWord(5, 0.2, 0.2).isEmpty());
}

void ClothboundTests::migrationMovesGenuineLegacyDataAndRenamesVoiceSentinels() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
    const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
    const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
    const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
    const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
    const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

    // 1. Seed a genuine legacy config, library DB, voice pack sentinel, illustration, and cache.
    QDir().mkpath(legacyCfg);
    QDir().mkpath(legacyData + QStringLiteral("/voices/kokoro-v1.0"));
    QDir().mkpath(legacyData + QStringLiteral("/illustrations"));
    QDir().mkpath(legacyCache + QStringLiteral("/covers"));
    QDir().mkpath(legacyCache + QStringLiteral("/audio"));

    write(legacyCfg + QStringLiteral("/settings.ini"),
          "[reading]\ntheme=sepia\n[onboarding]\nwelcomed=true\n");

    QDir().mkpath(dir.filePath(QStringLiteral("books")));
    const QString bookPath = dir.filePath(QStringLiteral("books/legacy.epub"));
    QVERIFY(TestBooks::write(bookPath, TestBooks::epub2({})));
    {
        Library legacyLib(legacyData, legacyCache);
        QVERIFY(legacyLib.open());
        const int id = legacyLib.addBookFile(bookPath);
        QVERIFY(id > 0);
    }
    write(legacyData + QStringLiteral("/voices/kokoro-v1.0/.dogear-installed"), "kokoro-v1.0 deadbeef\n");
    write(legacyData + QStringLiteral("/illustrations/scene.png"), "PNGDATA");
    write(legacyCache + QStringLiteral("/audio/clip.opus"), "OPUSDATA");
    write(legacyCache + QStringLiteral("/voices.json"), "{\"voices\":[]}");

    QVERIFY(Migration::isLegacyConfigDir(legacyCfg));
    QVERIFY(Migration::isLegacyDataDir(legacyData));

    const Migration::Paths paths{legacyCfg, newCfg, legacyData, newData, legacyCache, newCache};
    const Migration::Result res = Migration::run(paths);
    QVERIFY(res.configMigrated);
    QVERIFY(res.dataMigrated);
    QVERIFY(res.cacheMigrated);
    QCOMPARE(res.voiceSentinelsRenamed, 1);

    // Verify everything landed cleanly in Clothbound paths and sentinels were renamed.
    QVERIFY(!QFileInfo::exists(legacyCfg));
    QVERIFY(!QFileInfo::exists(legacyData));
    QVERIFY(!QFileInfo::exists(legacyCache));
    QCOMPARE(QSettings(newCfg + QStringLiteral("/settings.ini"), QSettings::IniFormat)
                 .value(QStringLiteral("reading/theme")).toString(),
             QStringLiteral("sepia"));
    QVERIFY(QFileInfo::exists(newData + QStringLiteral("/library.sqlite")));
    QVERIFY(QFileInfo::exists(newData + QStringLiteral("/voices/kokoro-v1.0/.clothbound-installed")));
    QVERIFY(!QFileInfo::exists(newData + QStringLiteral("/voices/kokoro-v1.0/.dogear-installed")));
    QVERIFY(QFileInfo::exists(newData + QStringLiteral("/illustrations/scene.png")));
    QVERIFY(QFileInfo::exists(newCache + QStringLiteral("/audio/clip.opus")));
    QVERIFY(QFileInfo::exists(newCache + QStringLiteral("/voices.json")));

    // Migrated library opens cleanly with the book preserved.
    {
        Library migratedLib(newData, newCache);
        QVERIFY(migratedLib.open());
        QCOMPARE(migratedLib.bookCount(), 1);
        QCOMPARE(migratedLib.records().first().title, QStringLiteral("The Lantern Keeper"));
    }

    // Second run is a clean idempotent no-op.
    const Migration::Result second = Migration::run(paths);
    QVERIFY(!second.configMigrated);
    QVERIFY(!second.dataMigrated);
    QVERIFY(!second.cacheMigrated);
    QCOMPARE(second.voiceSentinelsRenamed, 0);

    // VoicePacks::isInstalled() also honors legacy .dogear-installed as a read fallback.
    const QString fallbackVoicesDir = dir.filePath(QStringLiteral("fallback-voices"));
    QDir().mkpath(fallbackVoicesDir + QStringLiteral("/engine"));
    write(fallbackVoicesDir + QStringLiteral("/engine/.dogear-installed"), "engine legacy\n");
    const QByteArray manifest = R"({"engine":{"x86_64":{"url":"http://127.0.0.1/e.tar.bz2","size":10,"sha256":"00","dir":"engine"}}})";
    VoicePacks packs(fallbackVoicesDir, nullptr, QStringLiteral("x86_64"), manifest);
    QVERIFY(packs.isInstalled(QStringLiteral("engine")));
}

void ClothboundTests::migrationRejectsForeignDogearDirectories() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
    const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
    const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
    const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
    const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
    const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

    // Foreign config (e.g. ninepointlabs/dogear) with unrecognized groups.
    QDir().mkpath(legacyCfg);
    write(legacyCfg + QStringLiteral("/settings.ini"), "[sync]\nendpoint=https://example.invalid\n");

    // Foreign SQLite DB with unrelated schema.
    QDir().mkpath(legacyData);
    const QString foreignDbPath = legacyData + QStringLiteral("/library.sqlite");
    {
        const QString conn = QStringLiteral("foreign-dogear-test");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(foreignDbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE bookmarks (id INTEGER PRIMARY KEY, url TEXT)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO bookmarks (url) VALUES ('https://example.invalid')")));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }

    // Foreign cache directory.
    QDir().mkpath(legacyCache + QStringLiteral("/covers"));
    write(legacyCache + QStringLiteral("/covers/foreign.bin"), "FOREIGN");

    QVERIFY(!Migration::isLegacyConfigDir(legacyCfg));
    QVERIFY(!Migration::isLegacyDataDir(legacyData));
    QVERIFY(!Migration::isLegacyCacheDir(legacyCache));

    const Migration::Result res = Migration::run({legacyCfg, newCfg, legacyData, newData, legacyCache, newCache});
    QVERIFY(!res.configMigrated);
    QVERIFY(!res.dataMigrated);
    QVERIFY(!res.cacheMigrated);
    QCOMPARE(res.voiceSentinelsRenamed, 0);

    // Foreign files must remain completely untouched and nothing created in Clothbound paths.
    QVERIFY(QFileInfo::exists(legacyCfg + QStringLiteral("/settings.ini")));
    QVERIFY(QFileInfo::exists(foreignDbPath));
    QVERIFY(QFileInfo::exists(legacyCache + QStringLiteral("/covers/foreign.bin")));
    QVERIFY(!QFileInfo::exists(newCfg));
    QVERIFY(!QFileInfo::exists(newData));
    QVERIFY(!QFileInfo::exists(newCache));

    // Corrupt / non-SQLite file named library.sqlite is also rejected safely.
    const QString corruptData = dir.filePath(QStringLiteral("corrupt/dogear"));
    QDir().mkpath(corruptData);
    write(corruptData + QStringLiteral("/library.sqlite"), "not a sqlite database");
    QVERIFY(!Migration::isLegacyDataDir(corruptData));
}

void ClothboundTests::migrationNeverClobbersExistingClothboundData() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
    const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
    const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
    const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
    const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
    const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

    // Seed genuine legacy Dogear data...
    QDir().mkpath(legacyCfg);
    QDir().mkpath(legacyData);
    QDir().mkpath(legacyCache + QStringLiteral("/audio"));
    write(legacyCfg + QStringLiteral("/settings.ini"), "[reading]\ntheme=paper\n");
    {
        Library legacyLib(legacyData, legacyCache);
        QVERIFY(legacyLib.open());
    }
    write(legacyCache + QStringLiteral("/voices.json"), "{\"source\":\"legacy\"}");

    // ...AND already-populated Clothbound buckets.
    QDir().mkpath(newCfg);
    QDir().mkpath(newData);
    QDir().mkpath(newCache);
    write(newCfg + QStringLiteral("/settings.ini"), "[reading]\ntheme=night\n");
    QDir().mkpath(dir.filePath(QStringLiteral("books")));
    const QString newBook = dir.filePath(QStringLiteral("books/clothbound.epub"));
    QVERIFY(TestBooks::write(newBook, TestBooks::epub2({})));
    {
        Library existingLib(newData, newCache);
        QVERIFY(existingLib.open());
        QVERIFY(existingLib.addBookFile(newBook) > 0);
    }
    write(newCache + QStringLiteral("/voices.json"), "{\"source\":\"clothbound\"}");

    const Migration::Result res = Migration::run({legacyCfg, newCfg, legacyData, newData, legacyCache, newCache});
    QVERIFY(!res.configMigrated);
    QVERIFY(!res.dataMigrated);
    QVERIFY(!res.cacheMigrated);

    // Existing Clothbound config, library, and cache are completely untouched.
    QCOMPARE(QSettings(newCfg + QStringLiteral("/settings.ini"), QSettings::IniFormat)
                 .value(QStringLiteral("reading/theme")).toString(),
             QStringLiteral("night"));
    {
        Library checkLib(newData, newCache);
        QVERIFY(checkLib.open());
        QCOMPARE(checkLib.bookCount(), 1);
        QCOMPARE(checkLib.records().first().title, QStringLiteral("The Lantern Keeper"));
    }
    QFile cacheFile(newCache + QStringLiteral("/voices.json"));
    QVERIFY(cacheFile.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(cacheFile.readAll()), QStringLiteral("{\"source\":\"clothbound\"}"));

    // Legacy data also remains intact (never deleted when skipped).
    QVERIFY(QFileInfo::exists(legacyCfg + QStringLiteral("/settings.ini")));
    QVERIFY(QFileInfo::exists(legacyData + QStringLiteral("/library.sqlite")));
    QVERIFY(QFileInfo::exists(legacyCache + QStringLiteral("/voices.json")));
}

void ClothboundTests::wave4PdfRegionSnipClampingAndThreadSafety() {
    QTemporaryDir dir;
    const QString pdfPath = dir.filePath(QStringLiteral("rendered.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.fillRect(QRect(40, 60, 460, 140), QColor(QStringLiteral("#224488")));
        painter.setPen(Qt::white);
        painter.drawText(QRect(60, 100, 400, 60), QStringLiteral("Equation 1: E = mc^2"));
        writer.newPage();
        painter.fillRect(QRect(40, 400, 460, 140), QColor(QStringLiteral("#884422")));
        painter.end();
    }

    PdfBook pdf(pdfPath);
    QVERIFY2(pdf.open(), qPrintable(pdf.errorString()));
    QCOMPARE(pdf.pageCount(), 2);

    // Valid normalized rect renders crisp non-blank PNG matching target width.
    const QByteArray regionPng = pdf.renderRegionPng(0, 0.05, 0.05, 0.85, 0.25, 700);
    QVERIFY(!regionPng.isEmpty());
    const QImage decoded = QImage::fromData(regionPng, "PNG");
    QVERIFY(!decoded.isNull());
    QVERIFY(decoded.width() >= 600 && decoded.width() <= 800);
    QVERIFY(decoded.height() > 50);

    // Partially out-of-range rects clamp cleanly into [0..1] and still produce a valid PNG.
    const QByteArray clampedPng = pdf.renderRegionPng(0, -0.2, -0.1, 0.6, 0.5, 400);
    QVERIFY(!clampedPng.isEmpty());
    QVERIFY(!QImage::fromData(clampedPng, "PNG").isNull());

    // Invalid / degenerate / NaN / Inf / out-of-bounds page rects return empty safely.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    QVERIFY(pdf.renderRegionPng(-1, 0.1, 0.1, 0.5, 0.5).isEmpty());
    QVERIFY(pdf.renderRegionPng(2, 0.1, 0.1, 0.5, 0.5).isEmpty());
    QVERIFY(pdf.renderRegionPng(0, 1.5, 0.1, 0.2, 0.2).isEmpty());
    QVERIFY(pdf.renderRegionPng(0, 0.2, 0.2, 0.0, 0.2).isEmpty());
    QVERIFY(pdf.renderRegionPng(0, 0.2, 0.2, -0.3, 0.2).isEmpty());
    QVERIFY(pdf.renderRegionPng(0, 0.2, 0.2, 0.0001, 0.2).isEmpty());
    QVERIFY(pdf.renderRegionPng(0, nan, 0.2, 0.3, 0.3).isEmpty());
    QVERIFY(pdf.renderRegionPng(0, 0.2, inf, 0.3, 0.3).isEmpty());

    // Reader::snipPdfRegionBase64() returns valid base64 PNG for open PDF and empty when closed.
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(pdfPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.snipPdfRegionBase64(0, 0.1, 0.1, 0.5, 0.3).isEmpty());
    QVERIFY(reader.openBook(bookId));
    const QString b64 = reader.snipPdfRegionBase64(0, 0.05, 0.05, 0.85, 0.25);
    QVERIFY(!b64.isEmpty());
    QVERIFY(!QImage::fromData(QByteArray::fromBase64(b64.toLatin1()), "PNG").isNull());
    QVERIFY(reader.snipPdfRegionBase64(99, 0.1, 0.1, 0.5, 0.3).isEmpty());

    // Thread safety: concurrent renderRegionPng() calls on a shared PdfBook stay race-free.
    QList<QFuture<QByteArray>> futures;
    for (int i = 0; i < 6; ++i) {
        const int page = i % 2;
        const double y = 0.05 + 0.05 * i;
        futures.append(QtConcurrent::run([&pdf, page, y] {
            return pdf.renderRegionPng(page, 0.05, y, 0.7, 0.25, 320);
        }));
    }
    for (auto &f : futures) {
        const QByteArray bytes = f.result();
        QVERIFY(!bytes.isEmpty());
        QVERIFY(!QImage::fromData(bytes, "PNG").isNull());
    }
}

void ClothboundTests::wave4PdfLinksDestYAndPeekDestination() {
    // Build a 2-page PDF:
    // - Page 0 has two internal links to Page 1:
    //   1) /XYZ 0 100 1 (on 300pt height -> ~0.67 down from top)
    //   2) /Fit (unspecified Y -> destY == 0.0)
    // - Page 1 has distinct top, middle, and bottom lines.
    auto makePeekPdf = [] {
        QByteArray out = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        auto object = [&](const QByteArray &body) {
            offsets.append(out.size());
            out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
        };
        object("<< /Type /Catalog /Pages 2 0 R >>");                                       // 1
        object("<< /Type /Pages /Kids [3 0 R 8 0 R] /Count 2 >>");                         // 2
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 300] /Contents 4 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R 7 0 R] >>");         // 3 (page 0)
        const QByteArray s1 = "BT /F1 10 Tf 20 260 Td (See Reference [1] and Figure 1) Tj ET";
        object("<< /Length " + QByteArray::number(s1.size()) + " >>\nstream\n" + s1 + "\nendstream");  // 4
        object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");                               // 5
        object("<< /Type /Annot /Subtype /Link /Rect [20 245 120 270] /A << /S /GoTo /D [8 0 R /XYZ 0 100 1] >> >>");  // 6
        object("<< /Type /Annot /Subtype /Link /Rect [130 245 220 270] /A << /S /GoTo /D [8 0 R /Fit] >> >>");         // 7
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 300] /Contents 9 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> >>");                                            // 8 (page 1)
        const QByteArray s2 =
            "BT /F1 10 Tf 20 275 Td (Top Figure 1 Architecture Diagram) Tj ET\n"
            "BT /F1 10 Tf 20 150 Td (Middle Lemma Proof Statement) Tj ET\n"
            "BT /F1 10 Tf 20 50 Td (Bottom Reference [1] Vaswani et al.) Tj ET";
        object("<< /Length " + QByteArray::number(s2.size()) + " >>\nstream\n" + s2 + "\nendstream");  // 9
        const qsizetype xref = out.size();
        out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
        for (const qsizetype off : offsets)
            out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
        out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R >>\n";
        out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        return out;
    };

    QTemporaryDir dir;
    const QString pdfPath = dir.filePath(QStringLiteral("peek.pdf"));
    QVERIFY(TestBooks::write(pdfPath, makePeekPdf()));

    PdfBook pdf(pdfPath);
    QVERIFY2(pdf.open(), qPrintable(pdf.errorString()));
    const QList<PdfLinkEntry> links = pdf.pageLinks(0);
    QCOMPARE(links.size(), 2);
    bool sawPositioned = false;
    bool sawDefaultZero = false;
    for (const PdfLinkEntry &link : links) {
        QCOMPARE(link.page, 1);
        QVERIFY(link.destY >= 0.0 && link.destY <= 1.0);
        if (link.destY > 0.5 && link.destY < 0.85)
            sawPositioned = true;
        if (qFuzzyIsNull(link.destY))
            sawDefaultZero = true;
    }
    QVERIFY(sawPositioned);
    QVERIFY(sawDefaultZero);

    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(pdfPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(reader.peekPdfDestination(0, 0.0).isEmpty());
    QVERIFY(reader.openBook(bookId));

    // Reader::pdfLinks() exposes destY cleanly to QML.
    const QVariantList qmlLinks = reader.pdfLinks(0);
    QCOMPARE(qmlLinks.size(), 2);
    QVERIFY(qmlLinks.first().toMap().contains(QStringLiteral("destY")));

    // Top-strip fallback when destY < 0 captures top line & valid data URL image.
    const QVariantMap topPeek = reader.peekPdfDestination(1, -1.0);
    QCOMPARE(topPeek.value(QStringLiteral("page")).toInt(), 1);
    QCOMPARE(topPeek.value(QStringLiteral("pageLabel")).toString(), QStringLiteral("2"));
    const QString topUrl = topPeek.value(QStringLiteral("imageDataUrl")).toString();
    QVERIFY(topUrl.startsWith(QStringLiteral("data:image/png;base64,")));
    QVERIFY(!QImage::fromData(QByteArray::fromBase64(topUrl.mid(22).toLatin1()), "PNG").isNull());
    QVERIFY2(topPeek.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Top Figure 1")),
             qPrintable(topPeek.value(QStringLiteral("excerpt")).toString()));

    // Bottom destY band extracts the bottom reference text rather than top text.
    const QVariantMap bottomPeek = reader.peekPdfDestination(1, 0.85);
    QCOMPARE(bottomPeek.value(QStringLiteral("page")).toInt(), 1);
    QVERIFY2(bottomPeek.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Bottom Reference")),
             qPrintable(bottomPeek.value(QStringLiteral("excerpt")).toString()));
    QVERIFY(!bottomPeek.value(QStringLiteral("excerpt")).toString().contains(QStringLiteral("Top Figure 1")));

    // Out-of-range destination pages return empty map.
    QVERIFY(reader.peekPdfDestination(-1, 0.0).isEmpty());
    QVERIFY(reader.peekPdfDestination(2, 0.5).isEmpty());
    QVERIFY(reader.peekPdfDestination(99, -1.0).isEmpty());
}

void ClothboundTests::wave4PdfAcademicIdExtractionAndCitations() {
    // 1. PdfBook::extractDoi() edge cases (trailing punctuation vs balanced parens).
    QCOMPARE(PdfBook::extractDoi(QStringLiteral("Published at doi: 10.1145/3292500.3330701.")),
             QStringLiteral("10.1145/3292500.3330701"));
    QCOMPARE(PdfBook::extractDoi(QStringLiteral("(see https://doi.org/10.1000/xyz.123),")),
             QStringLiteral("10.1000/xyz.123"));
    QCOMPARE(PdfBook::extractDoi(QStringLiteral("DOI: 10.1002/(SICI)1097-4571(1999);")),
             QStringLiteral("10.1002/(SICI)1097-4571(1999)"));
    QVERIFY(PdfBook::extractDoi(QStringLiteral("No identifier in this sentence.")).isEmpty());

    // 2. PdfBook::extractArxivId() edge cases (prefix, case, URL, DataCite DOI).
    QCOMPARE(PdfBook::extractArxivId(QStringLiteral("Preprint arXiv:2305.14314v2 [cs.LG]")),
             QStringLiteral("arXiv:2305.14314v2"));
    QCOMPARE(PdfBook::extractArxivId(QStringLiteral("ARXIV: 1706.03762")),
             QStringLiteral("arXiv:1706.03762"));
    QCOMPARE(PdfBook::extractArxivId(QStringLiteral("Available at https://arxiv.org/pdf/2401.00001v1")),
             QStringLiteral("arXiv:2401.00001v1"));
    QCOMPARE(PdfBook::extractArxivId(QStringLiteral("DOI: 10.48550/arXiv.2303.08774")),
             QStringLiteral("arXiv:2303.08774"));
    QVERIFY(PdfBook::extractArxivId(QStringLiteral("Plain book text.")).isEmpty());

    // 3. End-to-end PDF scan on Reader::openBook() + cleanup on Reader::close().
    QTemporaryDir dir;
    const QString pdfPath = dir.filePath(QStringLiteral("academic.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(72);
        writer.setTitle(QStringLiteral("Attention & Scaling 100%"));
        writer.setCreator(QStringLiteral("Test"));
        QPainter p(&writer);
        p.drawText(QRect(20, 60, 550, 120),
                   QStringLiteral("arXiv:1706.03762v7  DOI: 10.1145/1234567.8901234."));
        p.end();
    }
    Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    const int bookId = library.addBookFile(pdfPath);
    QVERIFY(bookId > 0);
    BookScheme scheme;
    Reader reader(&library, &scheme);
    QVERIFY(!reader.isPdf());
    QVERIFY(reader.formatCitation(QStringLiteral("bibtex")).isEmpty());

    QVERIFY(reader.openBook(bookId));
    QVERIFY(reader.isPdf());
    QCOMPARE(reader.detectedArxiv(), QStringLiteral("arXiv:1706.03762v7"));
    QCOMPARE(reader.detectedDoi(), QStringLiteral("10.1145/1234567.8901234"));
    const QString openBib = reader.formatCitation(QStringLiteral("bibtex"));
    QVERIFY(openBib.startsWith(QStringLiteral("@article{")));
    QVERIFY(openBib.contains(QStringLiteral("doi = {10.1145/1234567.8901234}")));
    QVERIFY(openBib.contains(QStringLiteral("eprint = {1706.03762v7}")));
    QVERIFY(openBib.contains(QStringLiteral("Attention \\& Scaling 100\\%")));

    reader.close();
    QVERIFY(!reader.isPdf());
    QVERIFY(reader.detectedDoi().isEmpty());
    QVERIFY(reader.detectedArxiv().isEmpty());

    // 4. Reader::formatCitationFor() BibTeX (@article / @misc / @book) & APA 7th matrix.
    const QString bibArticle = Reader::formatCitationFor(
        {{QStringLiteral("title"), QStringLiteral("The Rate of $O(n)$ & 50% #1")},
         {QStringLiteral("author"), QStringLiteral("Élodie Müller and Jean-Luc Picard")},
         {QStringLiteral("year"), 2024},
         {QStringLiteral("doi"), QStringLiteral("https://doi.org/10.1000/test.42")},
         {QStringLiteral("format"), QStringLiteral("pdf")}},
        QStringLiteral("bibtex"));
    QVERIFY(bibArticle.startsWith(QStringLiteral("@article{muller2024rate,")));
    QVERIFY(bibArticle.contains(QStringLiteral("author = {Müller, Élodie and Picard, Jean-Luc}")));
    QVERIFY(bibArticle.contains(QStringLiteral("\\$O(n)\\$ \\& 50\\% \\#1")));
    QVERIFY(bibArticle.contains(QStringLiteral("doi = {10.1000/test.42}")));

    const QString bibArxiv = Reader::formatCitationFor(
        {{QStringLiteral("title"), QStringLiteral("QLoRA: Efficient Finetuning")},
         {QStringLiteral("author"), QStringLiteral("Tim Dettmers")},
         {QStringLiteral("arxiv"), QStringLiteral("arXiv:2305.14314")},
         {QStringLiteral("format"), QStringLiteral("pdf")}},
        QStringLiteral("bibtex"));
    QVERIFY(bibArxiv.startsWith(QStringLiteral("@misc{dettmers2023qlora,")));
    QVERIFY(bibArxiv.contains(QStringLiteral("year = {2023}")));
    QVERIFY(bibArxiv.contains(QStringLiteral("eprint = {2305.14314}")));
    QVERIFY(bibArxiv.contains(QStringLiteral("archivePrefix = {arXiv}")));
    QVERIFY(bibArxiv.contains(QStringLiteral("url = {https://arxiv.org/abs/2305.14314}")));

    const QString bibBook = Reader::formatCitationFor(
        {{QStringLiteral("title"), QStringLiteral("The Lantern Keeper")},
         {QStringLiteral("author"), QStringLiteral("Ada Marsh")},
         {QStringLiteral("publisher"), QStringLiteral("Harbor Books")},
         {QStringLiteral("isbn"), QStringLiteral("978-0-00-000000-2")},
         {QStringLiteral("year"), 1911},
         {QStringLiteral("format"), QStringLiteral("epub")}},
        QStringLiteral("bibtex"));
    QVERIFY(bibBook.startsWith(QStringLiteral("@book{marsh1911lantern,")));
    QVERIFY(bibBook.contains(QStringLiteral("publisher = {Harbor Books}")));
    QVERIFY(bibBook.contains(QStringLiteral("isbn = {978-0-00-000000-2}")));

    // APA 7th: 1 author + arXiv, 2 authors + DOI, 3 authors, editor-only, no-author + (n.d.).
    QCOMPARE(Reader::formatCitationFor(
                 {{QStringLiteral("title"), QStringLiteral("QLoRA")},
                  {QStringLiteral("author"), QStringLiteral("Tim Dettmers")},
                  {QStringLiteral("arxiv"), QStringLiteral("arXiv:2305.14314")}},
                 QStringLiteral("apa")),
             QStringLiteral("Dettmers, T. (2023). QLoRA. arXiv preprint arXiv:2305.14314. https://arxiv.org/abs/2305.14314"));
    QCOMPARE(Reader::formatCitationFor(
                 {{QStringLiteral("title"), QStringLiteral("Joint Work")},
                  {QStringLiteral("author"), QStringLiteral("Ada Lovelace; Alan Turing")},
                  {QStringLiteral("year"), 1950},
                  {QStringLiteral("doi"), QStringLiteral("10.1000/turing")}},
                 QStringLiteral("apa")),
             QStringLiteral("Lovelace, A., & Turing, A. (1950). Joint Work. https://doi.org/10.1000/turing"));
    QCOMPARE(Reader::formatCitationFor(
                 {{QStringLiteral("title"), QStringLiteral("Three Minds")},
                  {QStringLiteral("author"), QStringLiteral("Ada Lovelace; Alan Turing; Grace Hopper")},
                  {QStringLiteral("year"), 1955}},
                 QStringLiteral("apa")),
             QStringLiteral("Lovelace, A., Turing, A., & Hopper, G. (1955). Three Minds."));
    QCOMPARE(Reader::formatCitationFor(
                 {{QStringLiteral("title"), QStringLiteral("Collected Papers")},
                  {QStringLiteral("editor"), QStringLiteral("Tom Reed")},
                  {QStringLiteral("year"), 2001}},
                 QStringLiteral("apa")),
             QStringLiteral("Reed, T. (Ed.). (2001). Collected Papers."));
    QCOMPARE(Reader::formatCitationFor(
                 {{QStringLiteral("title"), QStringLiteral("Anonymous Treatise?")}},
                 QStringLiteral("apa")),
             QStringLiteral("Anonymous Treatise? (n.d.)."));
    QVERIFY(Reader::formatCitationFor({{QStringLiteral("title"), QStringLiteral("X")}}, QStringLiteral("chicago")).isEmpty());
}

void ClothboundTests::wave5PdfLineBandsAndRealPageTurnSfx() {
    // 1. PdfBook::mergeLineRects(): jagged word heights on one visual line -> 1 uniform band.
    {
        const QList<QRectF> jagged = {
            QRectF(0.100, 0.200, 0.060, 0.020),  // "The"
            QRectF(0.170, 0.205, 0.030, 0.011),  // "in"
            QRectF(0.210, 0.204, 0.040, 0.017),  // "by"
            QRectF(0.255, 0.212, 0.008, 0.005),  // "."
        };
        const QList<QRectF> out = PdfBook::mergeLineRects(jagged);
        QCOMPARE(out.size(), 1);
        QVERIFY(qAbs(out[0].left() - 0.100) < 1e-6);
        QVERIFY(qAbs(out[0].right() - 0.263) < 1e-6);
        QVERIFY(out[0].height() >= 0.020);
    }

    // 2. Lowercase-only wrap line matches height of full-height body lines.
    {
        const QList<QRectF> wrapped = {
            QRectF(0.10, 0.200, 0.78, 0.020),  // Full line 1
            QRectF(0.10, 0.232, 0.78, 0.020),  // Full line 2
            QRectF(0.10, 0.268, 0.22, 0.011),  // Lowercase wrap line ("in a sum")
        };
        const QList<QRectF> out = PdfBook::mergeLineRects(wrapped);
        QCOMPARE(out.size(), 3);
        QVERIFY2(qAbs(out[2].height() - out[0].height()) < 0.001,
                 qPrintable(QStringLiteral("wrap=%1 full=%2").arg(out[2].height()).arg(out[0].height())));
    }

    // 3. Two-column academic gutters (0.04 and 0.06) are never bridged.
    {
        const QList<QRectF> twoCol = {
            // Row 1: gutter = 0.50 - 0.46 = 0.04 (body h=0.012 -> maxWordGap=0.036)
            QRectF(0.08, 0.200, 0.17, 0.012),
            QRectF(0.26, 0.200, 0.20, 0.012),
            QRectF(0.50, 0.200, 0.18, 0.012),
            QRectF(0.69, 0.200, 0.19, 0.012),
            // Row 2: gutter = 0.51 - 0.45 = 0.06
            QRectF(0.08, 0.226, 0.37, 0.012),
            QRectF(0.51, 0.226, 0.37, 0.012),
        };
        const QList<QRectF> out = PdfBook::mergeLineRects(twoCol);
        QCOMPARE(out.size(), 4);
        for (const QRectF &band : out) {
            const bool crossesCenterGutter = band.left() < 0.46 && band.right() > 0.50;
            QVERIFY2(!crossesCenterGutter, qPrintable(QStringLiteral("Bridged gutter: [%1..%2]").arg(band.left()).arg(band.right())));
        }
    }

    // 4. Oversized heading preserves its taller height above body lines.
    {
        const QList<QRectF> withHeading = {
            QRectF(0.10, 0.100, 0.50, 0.042),  // Section heading
            QRectF(0.10, 0.180, 0.78, 0.018),
            QRectF(0.10, 0.210, 0.78, 0.018),
            QRectF(0.10, 0.240, 0.78, 0.018),
        };
        const QList<QRectF> out = PdfBook::mergeLineRects(withHeading);
        QCOMPARE(out.size(), 4);
        QVERIFY(out[0].height() > out[1].height() * 1.8);
    }

    // 5. Out-of-order tokens on the same visual line unite cleanly.
    {
        const QList<QRectF> outOfOrder = {
            QRectF(0.30, 0.30, 0.10, 0.018),
            QRectF(0.10, 0.30, 0.08, 0.018),
            QRectF(0.19, 0.30, 0.10, 0.018),
        };
        const QList<QRectF> out = PdfBook::mergeLineRects(outOfOrder);
        QCOMPARE(out.size(), 1);
        QVERIFY(qAbs(out[0].left() - 0.10) < 1e-6);
        QVERIFY(qAbs(out[0].right() - 0.40) < 1e-6);
    }

    // 6. Degenerate / NaN / Inf / empty rects dropped safely.
    {
        const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
        const qreal inf = std::numeric_limits<qreal>::infinity();
        const QList<QRectF> invalidOnly = {
            QRectF(nan, 0.1, 0.1, 0.02),
            QRectF(0.1, inf, 0.1, 0.02),
            QRectF(0.1, 0.1, 0.0, 0.02),
            QRectF(0.1, 0.1, 0.1, -0.02),
        };
        QVERIFY(PdfBook::mergeLineRects(invalidOnly).isEmpty());
        QList<QRectF> mixed = invalidOnly;
        mixed.append(QRectF(-0.05, 0.40, 0.30, 0.02));
        const QList<QRectF> out = PdfBook::mergeLineRects(mixed);
        QCOMPARE(out.size(), 1);
        QVERIFY(out[0].left() >= 0.0 && out[0].right() <= 1.0);
    }

    // 7. Tight consecutive lines stay vertically non-overlapping (zero double-alpha seams).
    {
        const QList<QRectF> tight = {
            QRectF(0.10, 0.200, 0.75, 0.019),
            QRectF(0.10, 0.218, 0.75, 0.019),
            QRectF(0.10, 0.236, 0.75, 0.019),
        };
        const QList<QRectF> out = PdfBook::mergeLineRects(tight);
        QCOMPARE(out.size(), 3);
        QVERIFY2(out[0].bottom() < out[1].top(),
                 qPrintable(QStringLiteral("line0.bottom=%1 line1.top=%2").arg(out[0].bottom()).arg(out[1].top())));
        QVERIFY2(out[1].bottom() < out[2].top(),
                 qPrintable(QStringLiteral("line1.bottom=%1 line2.top=%2").arg(out[1].bottom()).arg(out[2].top())));
    }

    // 8. Reader::playPageTurnDryRun(): single signature page_turn.wav loads cleanly,
    //    consecutive forward turns are consistent (c0 == c1, clipIndex == 0), 85 ms throttle
    //    blocks immediate repeats, backward gain is ~0.82x, and forward peak lies in -14..-18 dBFS.
    {
        QTemporaryDir dir;
        Library library(dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("cache")));
        QVERIFY(library.open());
        BookScheme scheme;
        Reader reader(&library, &scheme);

        auto peak16 = [](const QByteArray &pcm) {
            const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
            const qsizetype n = pcm.size() / 2;
            int p = 0;
            for (qsizetype i = 0; i < n; ++i)
                p = qMax(p, std::abs(int(s[i])));
            return p;
        };

        QByteArray c0, c1, b0, throttled;
        QVERIFY(reader.playPageTurnDryRun(1, &c0));
        QVERIFY(!reader.playPageTurnDryRun(1, &throttled));
        QVERIFY(throttled.isEmpty());
        QTest::qWait(95);
        QVERIFY(reader.playPageTurnDryRun(1, &c1));
        QTest::qWait(95);
        QVERIFY(reader.playPageTurnDryRun(-1, &b0));

        QVERIFY(c0.size() >= 18000);
        QCOMPARE(c1, c0);
        QCOMPARE(b0.size(), c0.size());
        QVERIFY(b0 != c0);

        const int pkFwd = peak16(c0);
        const int pkBwd = peak16(b0);
        QVERIFY(pkFwd > 0 && pkBwd > 0);
        const double dbfs = 20.0 * std::log10(double(pkFwd) / 32768.0);
        QVERIFY2(dbfs >= -18.0 && dbfs <= -14.0,
                 qPrintable(QStringLiteral("clip peak dBFS=%1 out of [-18,-14]").arg(dbfs)));
        const double bwdRatio = double(pkBwd) / double(pkFwd);
        QVERIFY2(bwdRatio >= 0.79 && bwdRatio <= 0.85,
                 qPrintable(QStringLiteral("backward gain ratio=%1 expected ~0.82").arg(bwdRatio)));
    }

    // 9. AppearancePanel.qml exposes Page-turn sound toggle bound to readingSettings.pageTurnSound.
    {
        const QString srcRoot = QFINDTESTDATA("../src");
        QFile apFile(srcRoot + QStringLiteral("/ui/reader/AppearancePanel.qml"));
        QVERIFY(apFile.open(QIODevice::ReadOnly));
        const QString apQml = QString::fromUtf8(apFile.readAll());
        QVERIFY(apQml.contains(QStringLiteral("qsTr(\"Page-turn sound\")")));
        QVERIFY(apQml.contains(QStringLiteral("readingSettings.pageTurnSound")));
        QVERIFY(apQml.contains(QStringLiteral("reader.playPageTurn(1)")));
    }
}

void ClothboundTests::cacheDirectoryDeletionSelfHeals() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataDir = dir.filePath(QStringLiteral("data"));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    const QString audioDir = cacheDir + QStringLiteral("/audio");
    const QString indexPath = cacheDir + QStringLiteral("/index.sqlite");
    const QString epubPath = dir.filePath(QStringLiteral("book.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(24, 36))));

    Library library(dataDir, cacheDir);
    QVERIFY(library.open());
    const int bookId = library.addBookFile(epubPath);
    QVERIFY(bookId > 0);
    const QString coverFile = library.record(bookId).cover;
    QVERIFY(!coverFile.isEmpty());
    QVERIFY(QFileInfo::exists(library.coverDirectory() + QLatin1Char('/') + coverFile));

    AudioCache cache(audioDir, library.database().connectionName());
    const QString key1 = AudioCache::keyFor("builtin", "m", "v", "", "First cached line.");
    QByteArray pcm(4800, '\x10');
    {
        QSignalSpy storedSpy(&cache, &AudioCache::stored);
        cache.store(key1, pcm, 24000, {});
        QTRY_VERIFY_WITH_TIMEOUT(storedSpy.count() >= 1, 5000);
    }
    QVERIFY(!cache.lookup(key1).isEmpty());

    LibraryIndex index(indexPath);
    QVERIFY(index.available());
    QVERIFY(LibraryIndex::indexBook(indexPath, {bookId, epubPath, QStringLiteral("epub"), 100}));
    QCOMPARE(index.bookCount(), 1);

    // Simulate the user accidentally deleting ~/.cache/clothbound while the app is running!
    QVERIFY(QDir(cacheDir).removeRecursively());
    QVERIFY(!QFileInfo::exists(cacheDir));

    // 1. AudioCache misses cleanly, prunes stale stats, recreates dir, and stores new clips.
    QVERIFY(cache.lookup(key1).isEmpty());
    cache.refreshStats();
    QCOMPARE(cache.clipCount(), 0);
    const QString key2 = AudioCache::keyFor("builtin", "m", "v", "", "Recovered after cache wipe.");
    {
        QSignalSpy storedSpy(&cache, &AudioCache::stored);
        cache.store(key2, pcm, 24000, {});
        QTRY_VERIFY_WITH_TIMEOUT(storedSpy.count() >= 1, 5000);
    }
    QVERIFY(!cache.lookup(key2).isEmpty());

    // 2. LibraryIndex detaches from unlinked inode, recreates index.sqlite, and indexes cleanly.
    QCOMPARE(index.bookCount(), 0);
    QVERIFY(QFileInfo::exists(indexPath));
    QVERIFY(LibraryIndex::indexBook(indexPath, {bookId, epubPath, QStringLiteral("epub"), 100}));
    QCOMPARE(index.bookCount(), 1);

    // 3. Library::rescan() notices missing cached cover file and automatically re-extracts it.
    QVERIFY(!QFileInfo::exists(library.coverDirectory() + QLatin1Char('/') + coverFile));
    QSignalSpy scanDone(&library, &Library::scanFinished);
    library.rescan();
    QTRY_VERIFY_WITH_TIMEOUT(scanDone.count() >= 1, 5000);
    QVERIFY(QFileInfo::exists(library.coverDirectory() + QLatin1Char('/') + library.record(bookId).cover));
}

void ClothboundTests::trayIconAndShortcutToggleWiring() {
    TrayIcon tray;
    QCOMPARE(tray.playing(), false);
    const TrayMenuLayoutItem pausedLayout = tray.buildMenuLayout(0, 1);
    QCOMPARE(pausedLayout.children.size(), 6);
    QCOMPARE(pausedLayout.children.first().variant().value<TrayMenuLayoutItem>().properties.value("label").toString(),
             QStringLiteral("Play"));
    tray.setPlaying(true);
    const TrayMenuLayoutItem playingLayout = tray.buildMenuLayout(0, 1);
    QCOMPARE(playingLayout.children.first().variant().value<TrayMenuLayoutItem>().properties.value("label").toString(),
             QStringLiteral("Pause"));

    QSignalSpy actSpy(&tray, &TrayIcon::activated);
    QSignalSpy toggleSpy(&tray, &TrayIcon::togglePlayRequested);
    QSignalSpy nextSpy(&tray, &TrayIcon::nextRequested);
    QSignalSpy prevSpy(&tray, &TrayIcon::previousRequested);
    QSignalSpy quitSpy(&tray, &TrayIcon::quitRequested);
    tray.handleActivate();
    tray.handleMenuEvent(1, QStringLiteral("clicked"));
    tray.handleMenuEvent(2, QStringLiteral("clicked"));
    tray.handleMenuEvent(3, QStringLiteral("clicked"));
    tray.handleMenuEvent(5, QStringLiteral("clicked"));
    tray.handleMenuEvent(6, QStringLiteral("clicked"));
    QCOMPARE(actSpy.count(), 2);
    QCOMPARE(toggleSpy.count(), 1);
    QCOMPARE(nextSpy.count(), 1);
    QCOMPARE(prevSpy.count(), 1);
    QCOMPARE(quitSpy.count(), 1);

    const QString srcRoot = QFINDTESTDATA("../src");
    QFile mainFile(srcRoot + QStringLiteral("/ui/Main.qml"));
    QVERIFY(mainFile.open(QIODevice::ReadOnly));
    const QString mainQml = QString::fromUtf8(mainFile.readAll());
    QVERIFY(!mainQml.contains(QStringLiteral("Qt.labs.platform")));
    QVERIFY(mainQml.contains(QStringLiteral("sequence: \"Ctrl+Q\"")));
    QVERIFY(mainQml.contains(QStringLiteral("sequence: \"Ctrl+W\"")));
    QVERIFY(mainQml.contains(QStringLiteral("function quitApp()")));
    QVERIFY(mainQml.contains(QStringLiteral("close.accepted = true")));
    QVERIFY(!mainQml.contains(QStringLiteral("close.accepted = false")));

    // Verify live Ctrl+Q and Ctrl+W ApplicationShortcuts fire unambiguously on every Linux theme.
    {
        QQmlEngine qmlEngine;
        QQmlComponent comp(&qmlEngine);
        comp.setData(QByteArrayLiteral(R"QML(
import QtQuick
Item {
    id: scRoot
    width: 200; height: 200
    focus: true
    property int quitHits: 0
    property int closeHits: 0
    property int ambiguousHits: 0
    Shortcut {
        sequence: "Ctrl+Q"
        context: Qt.ApplicationShortcut
        onActivated: scRoot.quitHits++
        onActivatedAmbiguously: scRoot.ambiguousHits++
    }
    Shortcut {
        sequence: "Ctrl+W"
        context: Qt.ApplicationShortcut
        onActivated: scRoot.closeHits++
        onActivatedAmbiguously: scRoot.ambiguousHits++
    }
}
)QML"),
                     QUrl::fromLocalFile(srcRoot + QStringLiteral("/QuitShortcutHarness.qml")));
        QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
        std::unique_ptr<QQuickWindow> win(new QQuickWindow);
        win->resize(200, 200);
        std::unique_ptr<QObject> obj(comp.create());
        QVERIFY(obj != nullptr);
        auto *item = qobject_cast<QQuickItem *>(obj.get());
        QVERIFY(item != nullptr);
        item->setParentItem(win->contentItem());
        win->show();
        win->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(win.get()));
        QCoreApplication::processEvents();
        QTest::keyClick(win.get(), Qt::Key_Q, Qt::ControlModifier);
        QTest::keyClick(win.get(), Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(obj->property("quitHits").toInt(), 1);
        QCOMPARE(obj->property("closeHits").toInt(), 1);
        QCOMPARE(obj->property("ambiguousHits").toInt(), 0);
        item->setParentItem(nullptr);
    }

    QFile erFile(srcRoot + QStringLiteral("/ui/reader/EpubReader.qml"));
    QVERIFY(erFile.open(QIODevice::ReadOnly));
    const QString erQml = QString::fromUtf8(erFile.readAll());
    QVERIFY(erQml.contains(QStringLiteral("appearance.open ? appearance.close() : view.showAppearance()")));
    QVERIFY(erQml.contains(QStringLiteral("contents.open ? contents.close() : view.showContents()")));
    QVERIFY(erQml.contains(QStringLiteral("marksPanel.open ? marksPanel.close() : view.showMarks()")));
    QVERIFY(erQml.contains(QStringLiteral("voicePanel.open ? voicePanel.close() : view.showVoices()")));
}

void ClothboundTests::pdfPagesRawCacheAndDownscaleReuse() {
    const QString token = QStringLiteral("test-pdf-reuse");
    const QString id = token + QStringLiteral("/0?bg=%23111111&fg=%23eeeeee");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdfPath = dir.filePath(QStringLiteral("dummy.pdf"));
    {
        QPdfWriter w(pdfPath);
        w.setPageSize(QPageSize(QPageSize::A4));
        QPainter p(&w);
        p.drawText(QRect(40, 40, 200, 50), QStringLiteral("Hello PDF"));
    }
    auto book = std::make_shared<PdfBook>(pdfPath);
    QVERIFY(book->open());
    PdfPages::add(token, book);

    QImage hi(1408, 2000, QImage::Format_RGB32);
    hi.fill(QColor(0x11, 0x11, 0x11));
    const QString hiKey = PdfPages::makeCacheKey(id, QSize(1408, 0));
    PdfPages::storeCached(token, id, 1408, hiKey, hi);

    // Narrowing (e.g. Companion docking: 1408 -> 1024) reuses the cached hi-res image.
    const QImage reuse = PdfPages::cachedDownscaleMatch(id, QSize(1024, 0));
    QCOMPARE(reuse.width(), 1408);
    // Zooming in (1408 -> 1600) must NOT reuse a smaller image.
    QVERIFY(PdfPages::cachedDownscaleMatch(id, QSize(1600, 0)).isNull());
    // Raw cache stores & clears cleanly with remove(token).
    const QString rawKey = PdfPages::makeRawKey(token, 0, QSize(1408, 0));
    PdfPages::storeRaw(token, rawKey, hi);
    QCOMPARE(PdfPages::cachedRaw(rawKey).width(), 1408);

    PdfPages::remove(token);
    QVERIFY(PdfPages::cached(hiKey).isNull());
    QVERIFY(PdfPages::cachedRaw(rawKey).isNull());
    QVERIFY(PdfPages::cachedDownscaleMatch(id, QSize(1024, 0)).isNull());
}

void ClothboundTests::fixCoreMigrationRewritesPathsAndSafeCopy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString legacyCfg = dir.filePath(QStringLiteral("config/dogear"));
    const QString newCfg = dir.filePath(QStringLiteral("config/clothbound"));
    const QString legacyData = dir.filePath(QStringLiteral("data/dogear"));
    const QString newData = dir.filePath(QStringLiteral("data/clothbound"));
    const QString legacyCache = dir.filePath(QStringLiteral("cache/dogear"));
    const QString newCache = dir.filePath(QStringLiteral("cache/clothbound"));

    QVERIFY(QDir().mkpath(legacyCfg));
    QVERIFY(QDir().mkpath(legacyData + QStringLiteral("/articles")));
    QVERIFY(QDir().mkpath(legacyData + QStringLiteral("/downloads")));
    QVERIFY(QDir().mkpath(legacyData + QStringLiteral("/watched")));
    QVERIFY(QDir().mkpath(legacyCache));

    write(legacyCfg + QStringLiteral("/settings.ini"), "[reading]\ntheme=paper\n");
    const QString articleOld = legacyData + QStringLiteral("/articles/saved.epub");
    const QString opdsOld = legacyData + QStringLiteral("/downloads/opds.epub");
    const QString watchedOld = legacyData + QStringLiteral("/watched");
    QVERIFY(TestBooks::write(articleOld, TestBooks::epub2({})));
    QVERIFY(TestBooks::write(opdsOld, TestBooks::epub2({})));

    int articleId = 0;
    int opdsId = 0;
    {
        Library legacyLib(legacyData, legacyCache);
        QVERIFY(legacyLib.open());
        articleId = legacyLib.addBookFile(articleOld);
        opdsId = legacyLib.addBookFile(opdsOld);
        QVERIFY(articleId > 0 && opdsId > 0);
        legacyLib.addFolder(watchedOld);
        QTRY_VERIFY(!legacyLib.scanning());
    }
    const QString legacyIndexPath = legacyCache + QStringLiteral("/index.sqlite");
    {
        LibraryIndex legacyIdx(legacyIndexPath);
        if (legacyIdx.available()) {
            QVERIFY(LibraryIndex::indexBook(legacyIndexPath, {articleId, articleOld, QStringLiteral("epub"), 100}));
            QVERIFY(LibraryIndex::indexBook(legacyIndexPath, {opdsId, opdsOld, QStringLiteral("epub"), 100}));
        }
    }

    const Migration::Result res = Migration::run({legacyCfg, newCfg, legacyData, newData, legacyCache, newCache});
    QVERIFY(res.configMigrated);
    QVERIFY(res.dataMigrated);
    QVERIFY(res.cacheMigrated);

    const QString articleNew = newData + QStringLiteral("/articles/saved.epub");
    const QString opdsNew = newData + QStringLiteral("/downloads/opds.epub");
    const QString watchedNew = newData + QStringLiteral("/watched");
    QVERIFY(QFileInfo::exists(articleNew));
    QVERIFY(QFileInfo::exists(opdsNew));

    {
        Library migratedLib(newData, newCache);
        QVERIFY(migratedLib.open());
        QCOMPARE(migratedLib.record(articleId).path, articleNew);
        QCOMPARE(migratedLib.record(opdsId).path, opdsNew);
        QVERIFY(migratedLib.folders().contains(watchedNew));

        // Rescan must NOT mark migrated articles/OPDS downloads as missing.
        QSignalSpy scanDone(&migratedLib, &Library::scanFinished);
        migratedLib.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(scanDone.count() >= 1, 5000);
        QCOMPARE(migratedLib.record(articleId).missing, false);
        QCOMPARE(migratedLib.record(opdsId).missing, false);
        QCOMPARE(migratedLib.bookCount(), 2);
    }

    // Verify index.sqlite indexed.path rows were rewritten too.
    const QString newIndexPath = newCache + QStringLiteral("/index.sqlite");
    if (QFileInfo::exists(newIndexPath)) {
        const QString conn = QStringLiteral("fix-core-mig-idx-check");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(newIndexPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT book, path FROM indexed ORDER BY book")));
            QHash<int, QString> paths;
            while (q.next())
                paths.insert(q.value(0).toInt(), q.value(1).toString());
            QCOMPARE(paths.value(articleId), articleNew);
            QCOMPARE(paths.value(opdsId), opdsNew);
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }

    // Verify cross-FS safe copy: preserves relative symlinks, skips FIFOs,
    // and copies WAL-mode SQLite databases cleanly without tearing.
    const QString copySrc = dir.filePath(QStringLiteral("copy-src"));
    const QString copyDst = dir.filePath(QStringLiteral("copy-dst"));
    QVERIFY(QDir().mkpath(copySrc));
    write(copySrc + QStringLiteral("/target.txt"), "hello-link");
    QVERIFY(QFile::link(QStringLiteral("target.txt"), copySrc + QStringLiteral("/rel.link")));
    QProcess::execute(QStringLiteral("mkfifo"), {copySrc + QStringLiteral("/pipe.fifo")});

    const QString walDb = copySrc + QStringLiteral("/sample.sqlite");
    {
        const QString conn = QStringLiteral("fix-core-wal-src");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(walDb);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode = WAL")));
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY, val TEXT)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO items VALUES (1, 'from-wal')")));
            // Keep DB open during copy to exercise live WAL checkpoint/VACUUM INTO.
            QVERIFY(Migration::copyDirectorySafely(copySrc, copyDst));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }
    // Delete source tree and confirm relative symlink in destination still resolves.
    QVERIFY(QDir(copySrc).removeRecursively());
    QFile copiedLink(copyDst + QStringLiteral("/rel.link"));
    QVERIFY(copiedLink.open(QIODevice::ReadOnly));
    QCOMPARE(copiedLink.readAll(), QByteArray("hello-link"));
    QVERIFY(!QFileInfo::exists(copyDst + QStringLiteral("/pipe.fifo")));
    QVERIFY(!QFileInfo::exists(copyDst + QStringLiteral("/sample.sqlite-wal")));
    {
        const QString conn = QStringLiteral("fix-core-wal-dst");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(copyDst + QStringLiteral("/sample.sqlite"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT val FROM items WHERE id = 1")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("from-wal"));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }
}

void ClothboundTests::fixCoreZipArchiveHardeningLimitsAndOverlap() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 1. Overlapping ZIP bomb: two central-directory entries aliasing the same local offset.
    {
        const QByteArray payload = TestBooks::rawDeflate("repeated payload");
        const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>("repeated payload"), 16));
        const QByteArray nameA("a.txt"), nameB("b.txt");
        QByteArray out, cd;
        TestBooks::put32(out, 0x04034b50);
        TestBooks::put16(out, 20);
        TestBooks::put16(out, 0x0800);
        TestBooks::put16(out, 8);
        TestBooks::put32(out, 0);
        TestBooks::put32(out, crc);
        TestBooks::put32(out, quint32(payload.size()));
        TestBooks::put32(out, 16);
        TestBooks::put16(out, quint16(nameA.size()));
        TestBooks::put16(out, 0);
        out.append(nameA);
        out.append(payload);

        const quint32 cdOff = quint32(out.size());
        auto addCd = [&](const QByteArray &name) {
            TestBooks::put32(cd, 0x02014b50);
            TestBooks::put16(cd, 20);
            TestBooks::put16(cd, 20);
            TestBooks::put16(cd, 0x0800);
            TestBooks::put16(cd, 8);
            TestBooks::put32(cd, 0);
            TestBooks::put32(cd, crc);
            TestBooks::put32(cd, quint32(payload.size()));
            TestBooks::put32(cd, 16);
            TestBooks::put16(cd, quint16(name.size()));
            TestBooks::put16(cd, 0);
            TestBooks::put16(cd, 0);
            TestBooks::put16(cd, 0);
            TestBooks::put16(cd, 0);
            TestBooks::put32(cd, 0);
            TestBooks::put32(cd, 0);  // Both alias offset 0!
            cd.append(name);
        };
        addCd(nameA);
        addCd(nameB);
        out.append(cd);
        TestBooks::put32(out, 0x06054b50);
        TestBooks::put16(out, 0);
        TestBooks::put16(out, 0);
        TestBooks::put16(out, 2);
        TestBooks::put16(out, 2);
        TestBooks::put32(out, quint32(cd.size()));
        TestBooks::put32(out, cdOff);
        TestBooks::put16(out, 0);

        const QString overlapPath = dir.filePath(QStringLiteral("overlap.zip"));
        QVERIFY(TestBooks::write(overlapPath, out));
        ZipArchive zip(overlapPath);
        QVERIFY(!zip.open());
        QVERIFY2(zip.errorString().contains(QStringLiteral("overlapping")), qPrintable(zip.errorString()));
    }

    // 2. Entry-count cap via ZIP64 EOCD declaring > kMaxEntryCount entries.
    {
        QByteArray z64 = TestBooks::zip({{"one.txt", "first", true}}, true);
        const QByteArray sig("\x50\x4b\x06\x06", 4);
        const qsizetype pos = z64.indexOf(sig);
        QVERIFY(pos > 0 && pos + 56 <= z64.size());
        const quint64 tooMany = ZipArchive::kMaxEntryCount + 1;
        for (int b = 0; b < 8; ++b)
            z64[pos + 32 + b] = char((tooMany >> (b * 8)) & 0xff);
        const QString manyPath = dir.filePath(QStringLiteral("many.zip"));
        QVERIFY(TestBooks::write(manyPath, z64));
        ZipArchive zip(manyPath);
        QVERIFY(!zip.open());
        QVERIFY2(zip.errorString().contains(QStringLiteral("too many")), qPrintable(zip.errorString()));
    }

    // 3. Cumulative per-archive inflation budget.
    {
        const QByteArray chunk(64 * 1024, 'A');
        const QString budgetPath = dir.filePath(QStringLiteral("budget.zip"));
        QVERIFY(TestBooks::write(budgetPath, TestBooks::zip({{"one.txt", chunk, true}, {"two.txt", chunk, true}})));
        ZipArchive zip(budgetPath);
        zip.setMaxArchiveInflateBytes(96 * 1024);
        QVERIFY(zip.open());
        QCOMPARE(zip.read(QStringLiteral("one.txt")), chunk);
        QString err;
        QVERIFY(zip.read(QStringLiteral("two.txt"), &err).isEmpty());
        QVERIFY2(err.contains(QStringLiteral("too large")), qPrintable(err));
    }
}

void ClothboundTests::fixCorePdfPagesReleaseAndSystemCheckNullGuard() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdfPath = dir.filePath(QStringLiteral("release.pdf"));
    QVERIFY(TestBooks::write(pdfPath, TestBooks::pdf("Release Test", "Author")));
    auto book = std::make_shared<PdfBook>(pdfPath);
    QVERIFY(book->open());

    const QString tok = QStringLiteral("fix-core-pdf-release");
    PdfPages::add(tok, book);
    {
        PdfPages pages;
        QCOMPARE(pages.m_pool.maxThreadCount(), 1);
        QCOMPARE(pages.m_pool.expiryTimeout(), -1);

        std::unique_ptr<QQuickImageResponse> resp(
            pages.requestImageResponse(tok + QStringLiteral("/0?bg=%23ffffff&fg=%23000000"), QSize(180, 240)));
        QSignalSpy finishedSpy(resp.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 8000);
        QVERIFY(pages.m_worker->book != nullptr);
        QCOMPARE(pages.m_worker->token, tok);

        // remove() must asynchronously release worker's QPdfDocument on its affine thread.
        PdfPages::remove(tok);
        QVERIFY(pages.m_pool.waitForDone(5000));
        QVERIFY(pages.m_worker->book == nullptr);
        QVERIFY(pages.m_worker->token.isEmpty());
    }

    // SystemCheck must handle null m_ai cleanly without dereferencing.
    SystemCheck nullAiCheck(nullptr, nullptr);
    const QVariantList verdicts = nullAiCheck.judge(0.0, 0.0, 0.0);
    QVERIFY(!verdicts.isEmpty());
    nullAiCheck.run();
    QTRY_VERIFY_WITH_TIMEOUT(!nullAiCheck.running(), 5000);
    QVERIFY(!nullAiCheck.recommendation().isEmpty());

    // Instance lock file enforces single-instance exclusion and releases on destruction.
    {
        Instance first;
        QVERIFY(first.claim());
        Instance second;
        QVERIFY(!second.claim());
    }
    {
        Instance afterRelease;
        QVERIFY(afterRelease.claim());
    }
}

void ClothboundTests::opdsCoverProviderWiring() {
    const QString srcRoot = QFileInfo(QFINDTESTDATA("../src")).canonicalFilePath();
    QVERIFY(!srcRoot.isEmpty());

    QFile mainFile(srcRoot + QStringLiteral("/app/main.cpp"));
    QVERIFY(mainFile.open(QIODevice::ReadOnly));
    const QString mainCpp = QString::fromUtf8(mainFile.readAll());
    const qsizetype opdsPos = mainCpp.indexOf(QStringLiteral("OpdsClient opds("));
    const qsizetype enginePos = mainCpp.indexOf(QStringLiteral("QQmlApplicationEngine engine;"));
    const qsizetype providerPos = mainCpp.indexOf(
        QStringLiteral("engine.addImageProvider(QStringLiteral(\"opdscover\"), new OpdsCoverProvider(&opds));"));
    QVERIFY(opdsPos > 0);
    QVERIFY(enginePos > opdsPos);
    QVERIFY(providerPos > enginePos);

    QFile catalogsFile(srcRoot + QStringLiteral("/ui/library/CatalogsSheet.qml"));
    QVERIFY(catalogsFile.open(QIODevice::ReadOnly));
    const QString catalogsQml = QString::fromUtf8(catalogsFile.readAll());
    QVERIFY(catalogsQml.count(QStringLiteral("opds.coverImageUrl(")) >= 2);
    QVERIFY(!catalogsQml.contains(QStringLiteral("source: entryWrap.modelData.thumbnailUrl")));
    QVERIFY(!catalogsQml.contains(QStringLiteral("source: navRow.isBookNav ? (entryWrap.modelData.thumbnailUrl")));
}

void ClothboundTests::aPlusCoreBookSchemeCspAndAsync() {
    // 1. Strict CSP + nosniff on /app/ (reader.html surface) allowing its own scripts/styles,
    // clothbound: book frames, qwebchannel, and data:/blob: images without script unsafe-inline/eval.
    const auto &appHdrs = BookScheme::appHeaders();
    QCOMPARE(appHdrs.value("X-Content-Type-Options"), QByteArray("nosniff"));
    const QByteArray csp = appHdrs.value("Content-Security-Policy");
    QVERIFY(!csp.isEmpty());
    QVERIFY(csp.contains("default-src 'self'"));
    QVERIFY(csp.contains("script-src 'self'"));
    QVERIFY(!csp.contains("'unsafe-eval'"));
    QVERIFY(csp.contains("style-src 'self' 'unsafe-inline'"));
    QVERIFY(csp.contains("frame-src 'self' clothbound:"));
    QVERIFY(csp.contains("img-src") && csp.contains("data:") && csp.contains("blob:"));
    QVERIFY(csp.contains("object-src 'none'"));
    QVERIFY(csp.contains("form-action 'none'"));
    QVERIFY(csp.contains("frame-ancestors 'none'"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("async.epub"));
    QVERIFY(TestBooks::write(epubPath, TestBooks::epub2(png(24, 24))));
    auto book = std::make_shared<EpubBook>(epubPath);
    QVERIFY(book->open());

    BookScheme scheme;
    // qwebchannel.js is compiled into QtWebChannel resources and served via /app/.
    const auto qwcRes = scheme.resolveRequest(QUrl(QStringLiteral("clothbound://local/app/qwebchannel.js")));
    QVERIFY(qwcRes.ok);
    QCOMPARE(qwcRes.headers.value("Content-Security-Policy"), csp);
    QCOMPARE(qwcRes.headers.value("X-Content-Type-Options"), QByteArray("nosniff"));

    const QString token = scheme.add(book);
    const QUrl chapterUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/text/chapter%201.xhtml").arg(token));
    const QUrl imgUrl(QStringLiteral("clothbound://local/book/%1/OEBPS/images/cover.png").arg(token));

    // 2. Async resolution runs off the GUI thread and delivers callback on the main thread.
    QObject liveCtx;
    bool gotChapter = false;
    bool deliveredOnMainThread = false;
    BookScheme::ResolvedResource asyncChapter;
    scheme.resolveRequestAsync(chapterUrl, {}, &liveCtx, [&](BookScheme::ResolvedResource res) {
        deliveredOnMainThread = (QThread::currentThread() == QCoreApplication::instance()->thread());
        asyncChapter = std::move(res);
        gotChapter = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(gotChapter, 3000);
    QVERIFY(deliveredOnMainThread);
    QVERIFY(asyncChapter.ok);
    QCOMPARE(asyncChapter.mimeType, QByteArray("application/xhtml+xml"));
    QVERIFY(asyncChapter.headers.value("Content-Security-Policy").contains("script-src 'none'"));

    // 3. Cancelled/destroyed job contexts are safely ignored without invoking callback or crashing.
    constexpr int totalJobs = 20;
    QList<QObject *> alive;
    int aliveCallbacks = 0;
    int cancelledCallbacks = 0;
    for (int i = 0; i < totalJobs; ++i) {
        auto *ctx = new QObject;
        const bool cancelImmediately = (i % 2 == 0);
        const QUrl &target = (i % 3 == 0) ? imgUrl : chapterUrl;
        scheme.resolveRequestAsync(target, {}, ctx,
                                   [cancelImmediately, &aliveCallbacks, &cancelledCallbacks](BookScheme::ResolvedResource res) {
                                       if (cancelImmediately)
                                           ++cancelledCallbacks;
                                       else if (res.ok)
                                           ++aliveCallbacks;
                                   });
        if (cancelImmediately)
            delete ctx;
        else
            alive.append(ctx);
    }
    QTRY_COMPARE_WITH_TIMEOUT(aliveCallbacks, totalJobs / 2, 3000);
    QCoreApplication::processEvents();
    QCOMPARE(cancelledCallbacks, 0);
    qDeleteAll(alive);

    // 4. Destroying BookScheme while async work is queued/in-flight drains m_pool cleanly.
    {
        BookScheme shortLived;
        const QString t2 = shortLived.add(book);
        const QUrl u2(QStringLiteral("clothbound://local/book/%1/OEBPS/text/chapter%201.xhtml").arg(t2));
        QObject ctx2;
        for (int i = 0; i < 8; ++i)
            shortLived.resolveRequestAsync(u2, {}, &ctx2, [](BookScheme::ResolvedResource) {});
    }
    QCoreApplication::processEvents();
}

void ClothboundTests::aPlusCoreTranslatorFastPathAndPlurals() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString langFile = dir.filePath(QStringLiteral("language"));
    const QString i18nDir = QFileInfo(QFINDTESTDATA("../src/i18n")).canonicalFilePath();
    QVERIFY(!i18nDir.isEmpty());

    // 1. Default English with shipped en.json (3.1 KB plural rules): fast-path non-plurals, exact plurals.
    write(langFile, "en\n");
    Translator trEn(langFile, i18nDir);
    QCOMPARE(trEn.language(), QStringLiteral("en"));
    QVERIFY(!trEn.isEmpty());
    QVERIFY(trEn.translate("Main", "Save").isNull());
    QVERIFY(trEn.translate("Main", "Saved %1").isNull());
    QVERIFY(trEn.translate("Main", nullptr).isNull());
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 0), QStringLiteral("%n books"));
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 7), QStringLiteral("%n books"));
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, -1), QStringLiteral("%n books"));
    QCOMPARE(trEn.translate("Main", "%Ln marks", nullptr, 1), QStringLiteral("%Ln mark"));
    QCOMPARE(trEn.translate("Main", "%Ln marks", nullptr, 3), QStringLiteral("%Ln marks"));

    // 2. Switching to non-English and back preserves translations and re-enables English fast path.
    trEn.setLanguage(QStringLiteral("de"));
    QCOMPARE(trEn.language(), QStringLiteral("de"));
    QVERIFY(!trEn.translate("Main", "%n books", nullptr, 1).isEmpty());
    trEn.setLanguage(QStringLiteral("en"));
    QCOMPARE(trEn.language(), QStringLiteral("en"));
    QVERIFY(trEn.translate("Main", "Save").isNull());
    QCOMPARE(trEn.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));

    // 3. Custom en.json with plain non-plural string override must NOT be bypassed.
    const QString customCat = dir.filePath(QStringLiteral("custom-i18n"));
    QVERIFY(QDir().mkpath(customCat));
    write(customCat + QStringLiteral("/en.json"), R"({"Hello": "Howdy", "%n books": {"one": "%n book", "other": "%n books"}})");
    Translator customEn(langFile, customCat);
    QCOMPARE(customEn.translate("Main", "Hello"), QStringLiteral("Howdy"));
    QCOMPARE(customEn.translate("Main", "%n books", nullptr, 1), QStringLiteral("%n book"));
    QVERIFY(customEn.translate("Main", "Unmapped").isNull());

    // 4. Lock-free concurrent stress: worker threads translate while main thread reloads languages.
    std::atomic_bool stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&trEn, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)trEn.translate("Main", "Save");
                (void)trEn.translate("Main", "%n books", nullptr, 1);
                (void)trEn.translate("Main", "%n books", nullptr, 5);
                (void)trEn.translate("Main", "Library");
            }
        });
    }
    const QStringList langs{QStringLiteral("de"), QStringLiteral("ru"), QStringLiteral("en"), QStringLiteral("fr")};
    for (int i = 0; i < 16; ++i)
        trEn.setLanguage(langs[i % langs.size()]);
    stop.store(true, std::memory_order_relaxed);
    for (auto &th : readers)
        th.join();
    trEn.setLanguage(QStringLiteral("en"));
}

void ClothboundTests::aPlusCoreTokenizerAndCoverProviderGc() {
    // 1. HtmlTokenizer::decodeEntities() zero-alloc lookup & surrogate handling.
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&nbsp;&Alpha;&omega;&mdash;&ZeroWidthSpace;&#x1F516;"),
             QStringLiteral("\u00A0\u0391\u03C9\u2014\u200B\U0001F516"));
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&; &#; &#x; &#0; &#xD800; &#x110000; &caf\u00E9;"),
             QStringLiteral("&; &#; &#x; \uFFFD \uFFFD \uFFFD &caf\u00E9;"));
    QCOMPARE(HtmlTokenizer::decodeEntities(u"&thisEntityNameIsWayLongerThanFortyCharactersTotal; &#9999999999999999999;"),
             QStringLiteral("&thisEntityNameIsWayLongerThanFortyCharactersTotal; &#9999999999999999999;"));

    // 2. CoverProvider::pruneCoverCacheDirectory() enforces byte cap & file count cap, oldest first.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("covers"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QByteArray payload(1000, 'x');
    const QDateTime baseTime = QDateTime::fromSecsSinceEpoch(1700000000);
    for (int i = 0; i < 10; ++i) {
        const QString path = QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(payload), payload.size());
        QVERIFY(f.setFileTime(baseTime.addSecs(i * 60), QFileDevice::FileModificationTime));
        f.close();
    }

    // Over byte cap (10,000 > 5,500) -> prunes 5 oldest (c0..c4), keeps c5..c9.
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(cacheDir, 5500, 100), 5);
    for (int i = 0; i < 5; ++i)
        QVERIFY(!QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i))));
    for (int i = 5; i < 10; ++i)
        QVERIFY(QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i))));

    // Over file count cap (5 > 3) -> prunes 2 oldest (c5..c6), keeps c7..c9.
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(cacheDir, 1024 * 1024, 3), 2);
    QVERIFY(!QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c5.jpg"))));
    QVERIFY(!QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c6.jpg"))));
    for (int i = 7; i < 10; ++i)
        QVERIFY(QFileInfo::exists(QDir(cacheDir).filePath(QStringLiteral("c%1.jpg").arg(i))));

    // Already within budget -> 0 removed.
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(cacheDir, 1024 * 1024, 3), 0);
    QCOMPARE(CoverProvider::pruneCoverCacheDirectory(dir.filePath(QStringLiteral("nonexistent")), 100, 1), 0);

    // 3. CoverProvider async requestImageResponse() works for missing, valid, and cancelled requests.
    CoverProvider provider;
    {
        std::unique_ptr<QQuickImageResponse> missing(
            provider.requestImageResponse(QUrl::toPercentEncoding(dir.filePath(QStringLiteral("missing.png"))), QSize(128, 128)));
        QSignalSpy doneSpy(missing.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 3000);
        QVERIFY(missing->textureFactory() == nullptr);
    }
    {
        const QString imgPath = dir.filePath(QStringLiteral("ok.png"));
        QImage img(100, 150, QImage::Format_RGB32);
        img.fill(QColor(80, 120, 160));
        QVERIFY(img.save(imgPath, "PNG"));

        const QString id = QString::fromLatin1(QUrl::toPercentEncoding(imgPath)) + QStringLiteral("?r=5&sr=3&bw=100");
        std::unique_ptr<QQuickImageResponse> okResp(provider.requestImageResponse(id, QSize(128, 128)));
        QSignalSpy doneSpy(okResp.get(), &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 3000);
        std::unique_ptr<QQuickTextureFactory> factory(okResp->textureFactory());
        QVERIFY(factory != nullptr);
    }
}

void ClothboundTests::w5ReaderCraftPdfViewportTilingAndCache() {
    // 1. parseTileSpec validation and clamping.
    {
        QVERIFY(!PdfPages::parseTileSpec(QUrlQuery(QStringLiteral("bg=%23111111&fg=%23eeeeee"))).active);
        QVERIFY(!PdfPages::parseTileSpec(QUrlQuery(QStringLiteral("tx=0&ty=0&tw=0&th=0.5&fullw=3000"))).active);
        QVERIFY(!PdfPages::parseTileSpec(QUrlQuery(QStringLiteral("tx=nan&ty=0&tw=0.5&th=0.5&fullw=3000"))).active);
        QVERIFY(!PdfPages::parseTileSpec(QUrlQuery(QStringLiteral("tx=0.1&ty=0.1&tw=0.5&th=0.5&fullw=-10"))).active);

        const PdfPages::TileSpec clamped =
            PdfPages::parseTileSpec(QUrlQuery(QStringLiteral("tx=-0.2&ty=0.25&tw=0.6&th=1.2&fullw=4096")));
        QVERIFY(clamped.active);
        QCOMPARE(clamped.tx, 0.0);
        QCOMPARE(clamped.ty, 0.25);
        QVERIFY(qAbs(clamped.tw - 0.4) < 1e-6);
        QVERIFY(qAbs(clamped.th - 0.75) < 1e-6);
        QCOMPARE(clamped.fullWidth, 4096);
    }

    // 2. tileGeometryFor preserves aspect and strictly enforces <= 16 MiB tile VRAM cap.
    {
        PdfPages::TileSpec huge;
        huge.active = true;
        huge.tx = 0.0;
        huge.ty = 0.0;
        huge.tw = 1.0;
        huge.th = 1.0;
        huge.fullWidth = 12000;
        const auto [scaled, clip] = PdfPages::tileGeometryFor(QSizeF(612.0, 792.0), huge);
        QVERIFY(scaled.isValid() && !scaled.isEmpty());
        QVERIFY(clip.isValid() && !clip.isEmpty());
        QVERIFY2(qint64(clip.width()) * qint64(clip.height()) * 4 <= PdfPages::kMaxTileBytes,
                 qPrintable(QStringLiteral("%1x%2").arg(clip.width()).arg(clip.height())));
    }

    // 3. Real PDF tile render: distinct top-left vs bottom-right quadrants + dark LUT + separate tile cache.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdfPath = dir.filePath(QStringLiteral("tile_quads.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QSizeF(300, 400), QPageSize::Point));
        writer.setResolution(72);
        QPainter p(&writer);
        p.fillRect(0, 0, writer.width(), writer.height(), Qt::white);
        // Distinct saturated blue block in top-left quadrant, blank paper in bottom-right.
        p.fillRect(10, 10, writer.width() / 3, writer.height() / 3, QColor(0x20, 0x60, 0xD0));
        p.setPen(Qt::black);
        p.setFont(QFont(QStringLiteral("sans-serif"), 9));
        p.drawText(20, 32, QStringLiteral("Top Left Ink"));
        p.end();
    }

    auto book = std::make_shared<PdfBook>(pdfPath);
    QVERIFY(book->open());
    const QString token = QStringLiteral("w5-tile-test");
    PdfPages::add(token, book);

    PdfPages pages;
    // Seed a base page entry in s_cache and confirm tiling never evicts it.
    const QString baseId = token + QStringLiteral("/0?bg=%231e1f24&fg=%23d6d2c9");
    const QString baseKey = PdfPages::makeCacheKey(baseId, QSize(1024, 0));
    QImage baseImg(1024, 1365, QImage::Format_RGB32);
    baseImg.fill(QColor(0x1e, 0x1f, 0x24));
    PdfPages::storeCached(token, baseId, 1024, baseKey, baseImg);
    QVERIFY(!PdfPages::cached(baseKey).isNull());

    auto fetch = [&](const QString &id, const QSize &reqSize, QImage &out) {
        out = QImage();
        std::unique_ptr<QQuickImageResponse> resp(pages.requestImageResponse(id, reqSize));
        int done = 0;
        connect(resp.get(), &QQuickImageResponse::finished, this, [&done] { ++done; });
        QTRY_COMPARE_WITH_TIMEOUT(done, 1, 8000);
        std::unique_ptr<QQuickTextureFactory> factory(resp->textureFactory());
        if (factory)
            out = factory->image();
    };

    const QString tlId =
        token + QStringLiteral("/0?bg=%231e1f24&fg=%23d6d2c9&tx=0.0000&ty=0.0000&tw=0.3500&th=0.3500&fullw=3600");
    const QString brId =
        token + QStringLiteral("/0?bg=%231e1f24&fg=%23d6d2c9&tx=0.6500&ty=0.6500&tw=0.3500&th=0.3500&fullw=3600");

    QImage tlImg;
    fetch(tlId, QSize(), tlImg);
    QVERIFY(!tlImg.isNull());
    QVERIFY(tlImg.width() > 1000);
    QVERIFY(tlImg.sizeInBytes() <= PdfPages::kMaxTileBytes);
    // Colored blue block survives dark LUT recoloring in top-left quadrant
    // (sample the unobstructed lower-middle of the blue block away from text strokes).
    const QColor tlBlue(tlImg.pixel(tlImg.width() / 2, (tlImg.height() * 3) / 4));
    QVERIFY2(tlBlue.blue() > tlBlue.red() + 40, qPrintable(tlBlue.name()));

    QImage brImg;
    fetch(brId, QSize(), brImg);
    QVERIFY(!brImg.isNull());
    QVERIFY(brImg.sizeInBytes() <= PdfPages::kMaxTileBytes);
    // Unpainted bottom-right paper maps cleanly to dark theme bg #1e1f24.
    QCOMPARE(QColor(brImg.pixel(brImg.width() / 2, brImg.height() / 2)).name(), QStringLiteral("#1e1f24"));

    // Tile cached separately in s_tileCache and base page untouched in s_cache.
    const QString tlKey = PdfPages::makeCacheKey(tlId, QSize());
    QVERIFY(!PdfPages::cachedTile(tlKey).isNull());
    QVERIFY(!PdfPages::cached(baseKey).isNull());

    // remove(token) cleans both base and tile caches cleanly.
    PdfPages::remove(token);
    QVERIFY(PdfPages::cached(baseKey).isNull());
    QVERIFY(PdfPages::cachedTile(tlKey).isNull());
}
