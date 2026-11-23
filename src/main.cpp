// Dogear — an ebook reader for Omarchy: your EPUBs and PDFs in one library,
// and a calm page to read them on.

#include "aiservices.h"
#include "audiocache.h"
#include "bookscheme.h"
#include "gemini.h"
#include "localmodels.h"
#include "companion.h"
#include "cast.h"
#include "audiobook.h"
#include "illustrator.h"
#include "pdfpages.h"
#include "systemcheck.h"
#include "localsetup.h"
#include "keystore.h"
#include "cli.h"
#include "library.h"
#include "narration.h"
#include "voicecatalog.h"
#include "mpris.h"
#include "download.h"
#include "booksearch.h"
#include "marks.h"
#include "localspeech.h"
#include "voicepacks.h"
#include "pointerdriver.h"
#include "notifier.h"
#include "instance.h"
#include "portalfilepicker.h"
#include "reader.h"
#include "theme.h"
#include "translator.h"

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QWindow>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFileInfo>
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
        // The page keeps drawing while the window is on another workspace,
        // so coming back shows it at once rather than a blank frame first.
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        for (const QByteArray &flag : {QByteArray("--disable-renderer-backgrounding"),
                                      QByteArray("--disable-backgrounding-occluded-windows"),
                                      QByteArray("--disable-background-timer-throttling")})
            if (!flags.contains(flag))
                flags += ' ' + flag;
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.trimmed());
        QtWebEngineQuick::initialize();
    }

    QGuiApplication app(argc, argv);
    // The window can hide while a book is read aloud; Dogear quits itself
    // when it's closed otherwise, or the reading ends.
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("dogear");
    app.setApplicationVersion(DOGEAR_VERSION);
    // Matches the window to dogear.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("dogear");
    // The name people see, in the bar's tray among others.
    app.setApplicationDisplayName("Dogear");
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

    // One Dogear per library: opened again (the launcher, a terminal, a book
    // file), it hands over to the one running, which comes back as it was,
    // reading aloud or not, and opens the book if one was named. Scripted runs
    // (--run, --screenshot) never hand over, so they stay their own.
    const bool scripted = args.isSet(runOption) || args.isSet("screenshot");
    QString bookFile;
    if (!args.positionalArguments().isEmpty())
        bookFile = QFileInfo(args.positionalArguments().first()).absoluteFilePath();
    Instance instance;
    if (!scripted && Instance::passToRunning(bookFile))
        return 0;
    // Of two opened at once, the one that doesn't get the name hands over.
    if (!instance.claim() && !scripted && Instance::passToRunning(bookFile))
        return 0;

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

    // The composition root for intelligence: Gemini is today's provider,
    // with its key in the keyring. Features only ever see AiServices.
    GeminiProvider gemini;
    SecretKeyStore keys;
    AiServices ai(&gemini, nullptr, library.database().connectionName());
    ai.lookUpKeyIn(&keys);
    // Models the reader runs on this computer, for text work routed there.
    LocalModels localModels;
    ai.setLocalModels(&localModels);
    // Built-in voices: made on this computer, from voices downloaded on request.
    VoicePacks voicePacks(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/voices"));
    LocalSpeech localSpeech(&voicePacks);
    ai.setLocalSpeech(&localSpeech);
    AudioCache audioCache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/audio"),
                          library.database().connectionName());
    SystemCheck systemCheck(&ai, &localSpeech);
    LocalSetup localSetup(&ai);
    Narration narration(&ai, &audioCache, &reader, &library);
    Mpris mpris(&narration, &reader, &library);
    Download download(&ai, &audioCache, &reader, &narration);
    BookSearch bookSearch(&reader);
    bookSearch.setMeaning(&ai, library.database().connectionName());
    Marks marks(&library);
    Companion companion(&ai, &reader, library.database().connectionName());
    Illustrator illustrator(&ai, &reader, library.database().connectionName(),
                            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/illustrations"));
    QObject::connect(&reader, &Reader::bookChanged, &marks, [&marks, &reader] { marks.setBook(reader.bookId()); });
    VoiceCatalog voices(&ai, &audioCache, &reader, library.database().connectionName(),
                        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/voices.json"));
    // A full cast, when a book has it on: characters in voices of their own.
    Cast cast(&ai, &audioCache, &reader, &library, &voices, &voicePacks);
    narration.setCast(&cast);
    download.setCast(&cast);
    Audiobook audiobook(&audioCache, &download, &reader, &library);
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
    engine.rootContext()->setContextProperty("ai", &ai);
    engine.rootContext()->setContextProperty("narration", &narration);
    engine.rootContext()->setContextProperty("voices", &voices);
    engine.rootContext()->setContextProperty("download", &download);
    engine.rootContext()->setContextProperty("bookSearch", &bookSearch);
    engine.rootContext()->setContextProperty("marks", &marks);
    engine.rootContext()->setContextProperty("companion", &companion);
    engine.rootContext()->setContextProperty("cast", &cast);
    engine.rootContext()->setContextProperty("audiobook", &audiobook);
    engine.rootContext()->setContextProperty("illustrator", &illustrator);
    engine.rootContext()->setContextProperty("voicePacks", &voicePacks);
    Notifier notifier;
    engine.rootContext()->setContextProperty("desktop", &notifier);
    engine.rootContext()->setContextProperty("instance", &instance);
    engine.rootContext()->setContextProperty("systemCheck", &systemCheck);
    engine.rootContext()->setContextProperty("localSetup", &localSetup);
    engine.rootContext()->setContextProperty("localSpeech", &localSpeech);
    engine.rootContext()->setContextProperty("audioCache", &audioCache);
    engine.rootContext()->setContextProperty("readerProfile", profile);
    engine.rootContext()->setContextProperty("debugMode", qEnvironmentVariableIsSet("DOGEAR_DEBUG"));
    engine.rootContext()->setContextProperty(
        "settingsUrl", QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                           QStringLiteral("/settings.ini")));
    engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (bookToOpen)
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openBook", Q_ARG(QVariant, bookToOpen));
    // Opened again: the window comes back (Instance), and a book named then
    // joins the library and opens.
    if (QObject *root = engine.rootObjects().first()) {
        instance.setWindow(qobject_cast<QWindow *>(root));
        QObject::connect(&instance, &Instance::activated, root, [&library, &notifier, root](const QString &book) {
            if (book.isEmpty())
                return;
            if (const int id = library.addBookFile(book))
                QMetaObject::invokeMethod(root, "openBook", Q_ARG(QVariant, id));
            else
                notifier.notify(QObject::tr("Dogear can't open this book"), library.errorString());
        });
    }
    // What the window has drawn is kept while it's away (on another
    // workspace), so it comes back as it was instead of black for a moment.
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        window->setPersistentGraphics(true);
        window->setPersistentSceneGraph(true);
    }
    // While debugging, a pointer that --run can move.
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window && qEnvironmentVariableIsSet("DOGEAR_DEBUG"))
        engine.rootContext()->setContextProperty("pointer", new PointerDriver(window, &app));
    // The desktop's media controls can ask for the window.
    QObject::connect(&mpris, &Mpris::raiseRequested, &instance, [&instance] { instance.bringBack(); });
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
