// Dogear — an ebook reader for Omarchy: your EPUBs and PDFs in one library,
// and a calm page to read them on.

#include "bookscheme.h"
#include "cli.h"
#include "library.h"
#include "portalfilepicker.h"
#include "reader.h"
#include "theme.h"
#include "translator.h"

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <QTimer>
#include <QWebEngineUrlScheme>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <cstdio>

// Qt's own words in text fields' right-click menus, which reach the
// Translator because it ignores the context.
[[maybe_unused]] static const char *const qtStrings[] = {QT_TR_NOOP("Cut"), QT_TR_NOOP("Copy"), QT_TR_NOOP("Paste"),
                                                         QT_TR_NOOP("Select All")};

// The desktop's interface font, e.g. "Adwaita Sans 11", from the settings
// portal (as Hype does). Without a portal Qt's default font stays.
static void adoptDesktopFont() {
    auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.Settings", "ReadOne");
    call.setArguments({"org.gnome.desktop.interface", "font-name"});
    const auto reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return;
    const QString name = reply.arguments().first().value<QDBusVariant>().variant().toString();
    const int space = name.lastIndexOf(' ');
    const double size = name.mid(space + 1).toDouble();
    if (space <= 0 || size <= 0)
        return;
    QFont font(name.left(space));
    font.setPointSizeF(size);
    QGuiApplication::setFont(font);
}

// Book pages and the reading surface are served from one origin, so the
// surface can lay out and annotate the chapter documents it shows.
static void registerBookScheme() {
    QWebEngineUrlScheme scheme("dogear");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled |
                    QWebEngineUrlScheme::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
}

int main(int argc, char *argv[]) {
    // Dogear themes itself; Qt's gtk3 platform theme only adds a GTK
    // use-after-free when the desktop theme changes under a running app.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    // Commands and help draw no window, so they must not need a display.
    const bool command = argc > 1 && isCliCommand(QString::fromLocal8Bit(argv[1]));
    bool windowless = command;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        windowless = windowless || argument == "--help" || argument == "-h" || argument == "--version" ||
                     argument == "-v" || argument == "--help-all";
    }
    if (windowless) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    } else {
        registerBookScheme();
        QtWebEngineQuick::initialize();
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("dogear");
    app.setApplicationVersion(DOGEAR_VERSION);
    // Matches the window to dogear.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("dogear");
    app.setWindowIcon(QIcon::fromTheme("dogear"));

    if (command)
        return runCli(app.arguments());

    QCommandLineParser args;
    args.setApplicationDescription("An ebook reader for Omarchy.\n\n" + cliSummary());
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument("book", "An EPUB or PDF to open", "[book]");
    args.addOption({"screenshot", "Save a screenshot of the window and exit", "file"});
    // For tests and the README: QML run in the window, step by step, before
    // the screenshot is taken.
    QCommandLineOption runOption("run", "Evaluate a QML expression in the window (repeatable)", "expression");
    QCommandLineOption delayOption("delay", "Milliseconds before the first --run step", "ms", "1500");
    QCommandLineOption stepOption("step", "Milliseconds between --run steps", "ms", "700");
    for (auto *option : {&runOption, &delayOption, &stepOption}) {
        option->setFlags(QCommandLineOption::HiddenFromHelp);
        args.addOption(*option);
    }
    QStringList arguments = app.arguments();
    if (arguments.value(1) == "open")
        arguments.removeAt(1);
    args.process(arguments);

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);
    adoptDesktopFont();
    for (const char *font : {":/fonts/Literata.ttf", ":/fonts/Literata-Italic.ttf"})
        QFontDatabase::addApplicationFont(font);
    QQuickStyle::setStyle("Material");

    Theme theme;
    Library library;
    if (!library.open()) {
        fprintf(stderr, "The library can't be opened: %s\n", qPrintable(library.errorString()));
        return 1;
    }
    PortalFilePicker picker;

    // The reading surface's own profile: nothing is written to disk (no
    // cache, no cookies), books are served from their ZIPs, and nothing a
    // book contains can reach the network.
    BookScheme bookScheme;
    OfflineInterceptor offline;
    auto *profile = new QQuickWebEngineProfile(&app);
    profile->setOffTheRecord(true);
    profile->setHttpCacheType(QQuickWebEngineProfile::MemoryHttpCache);
    profile->installUrlSchemeHandler("dogear", &bookScheme);
    profile->setUrlRequestInterceptor(&offline);
    Reader reader(&library, &bookScheme);
    // A book named on the command line joins the library (it may live
    // anywhere) and opens.
    int bookToOpen = 0;
    if (!args.positionalArguments().isEmpty()) {
        bookToOpen = library.addBookFile(args.positionalArguments().first());
        if (!bookToOpen)
            fprintf(stderr, "%s\n", qPrintable(library.errorString()));
    }

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("library", &library);
    engine.rootContext()->setContextProperty("picker", &picker);
    engine.rootContext()->setContextProperty("reader", &reader);
    engine.rootContext()->setContextProperty("readerProfile", profile);
    engine.rootContext()->setContextProperty("debugMode", qEnvironmentVariableIsSet("DOGEAR_DEBUG"));
    engine.rootContext()->setContextProperty(
        "settingsUrl", QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                           QStringLiteral("/settings.ini")));
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (bookToOpen)
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openBook", Q_ARG(QVariant, bookToOpen));
    // New books in watched folders are picked up each time Dogear starts.
    if (!library.folders().isEmpty())
        QTimer::singleShot(400, &library, &Library::rescan);

    const QStringList steps = args.values(runOption);
    if (!steps.isEmpty() || args.isSet("screenshot")) {
        QObject *root = engine.rootObjects().first();
        const int delay = args.value(delayOption).toInt(), step = args.value(stepOption).toInt();
        for (int i = 0; i < steps.size(); ++i)
            QTimer::singleShot(delay + i * step, root, [root, code = steps[i]] {
                QQmlExpression expression(qmlContext(root), root, code);
                expression.evaluate();
                if (expression.hasError())
                    fprintf(stderr, "--run %s: %s\n", qPrintable(code), qPrintable(expression.error().toString()));
            });
        if (args.isSet("screenshot"))
            QTimer::singleShot(delay + int(steps.size()) * step + 400, &app, [&] {
                auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
                const bool ok = window && window->grabWindow().save(args.value("screenshot"));
                app.exit(ok ? 0 : 1);
            });
    }
    return app.exec();
}
