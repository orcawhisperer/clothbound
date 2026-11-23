// Clothbound — an ebook reader for Omarchy: your EPUBs and PDFs in one library,
// and a calm page to read them on.

#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "formats/bookscheme.h"
#include "ai/gemini.h"
#include "ai/localmodels.h"
#include "ai/companion.h"
#include "audio/cast.h"
#include "audio/audiobook.h"
#include "ai/illustrator.h"
#include "formats/pdfpages.h"
#include "app/systemcheck.h"
#include "ai/localsetup.h"
#include "ai/keystore.h"
#include "app/cli.h"
#include "library/library.h"
#include "library/libraryfinder.h"
#include "library/libraryindex.h"
#include "audio/narration.h"
#include "audio/voicecatalog.h"
#include "audio/mpris.h"
#include "audio/download.h"
#include "library/booksearch.h"
#include "library/marks.h"
#include "audio/localspeech.h"
#include "audio/voicepacks.h"
#include "app/pointerdriver.h"
#include "app/migration.h"
#include "app/notifier.h"
#include "app/instance.h"
#include "app/portalfilepicker.h"
#include "reader/reader.h"
#include "app/surfacewatcher.h"
#include "app/theme.h"
#include "app/translator.h"
#include "mcp/mcpserver.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
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
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWebEngineUrlScheme>
#include <QWindow>
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <cstdio>

// Qt's own words in text fields' right-click menus, which reach the
// Translator because it ignores the context.
[[maybe_unused]] static const char *const qtStrings[] = {QT_TR_NOOP("Cut"), QT_TR_NOOP("Copy"), QT_TR_NOOP("Paste"),
                                                         QT_TR_NOOP("Select All")};

// Normalizes either a local file path or a file:// URI to an absolute path.
static QString normalizeBookArg(const QString &arg) {
    if (arg.isEmpty())
        return {};
    if (arg.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        QUrl url = QUrl::fromEncoded(arg.toUtf8(), QUrl::StrictMode);
        if (!url.isValid())
            url = QUrl(arg);
        if (url.isLocalFile())
            return QFileInfo(url.toLocalFile()).absoluteFilePath();
    }
    return QFileInfo(arg).absoluteFilePath();
}

static bool passBooksToRunning(const QStringList &books) {
    if (books.isEmpty())
        return Instance::passToRunning({});
    bool handed = false;
    for (const QString &book : books)
        handed = Instance::passToRunning(book) || handed;
    return handed;
}

// The desktop's interface font, e.g. "Adwaita Sans 11", from the settings
// portal (as Hype does), queried asynchronously so cold start never blocks.
static void adoptDesktopFont(QObject *context) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.Settings", "ReadOne");
    call.setArguments({"org.gnome.desktop.interface", "font-name"});
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [](QDBusPendingCallWatcher *pending) {
        const QDBusPendingReply<QDBusVariant> reply = *pending;
        pending->deleteLater();
        if (!reply.isValid())
            return;
        const QString name = reply.value().variant().toString();
        const int space = name.lastIndexOf(' ');
        const double size = name.mid(space + 1).toDouble();
        if (space <= 0 || size <= 0)
            return;
        QFont font(name.left(space));
        font.setPointSizeF(size);
        QGuiApplication::setFont(font);
    });
}

static bool debugEnabled() {
    return qEnvironmentVariableIsSet("CLOTHBOUND_DEBUG") || qEnvironmentVariableIsSet("DOGEAR_DEBUG");
}

// Book pages and the reading surface are served from one origin, so the
// surface can lay out and annotate the chapter documents it shows.
static void registerBookScheme() {
    QWebEngineUrlScheme scheme("clothbound");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled |
                    QWebEngineUrlScheme::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
}

static void configureParser(QCommandLineParser &args, QCommandLineOption &runOption, QCommandLineOption &delayOption,
                            QCommandLineOption &stepOption) {
    args.setApplicationDescription(QStringLiteral("An ebook reader for Omarchy.\n\n") + cliSummary());
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument(QStringLiteral("book"), QStringLiteral("One or more EPUB or PDF books to open"),
                               QStringLiteral("[book...]"));
    args.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the window and exit"),
                    QStringLiteral("file")});
    // For tests and the README: QML run in the window, step by step, before
    // the screenshot is taken.
    for (auto *option : {&runOption, &delayOption, &stepOption}) {
        option->setFlags(QCommandLineOption::HiddenFromHelp);
        args.addOption(*option);
    }
}

int main(int argc, char *argv[]) {
    // Clothbound themes itself; Qt's gtk3 platform theme only adds a GTK
    // use-after-free when the desktop theme changes under a running app.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    const bool command = argc > 1 && isCliCommand(QString::fromLocal8Bit(argv[1]));
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("mcp"))
        McpServer::installStderrMessageHandler();
    bool infoFlag = false;
    bool scriptedFlag = false;
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        rawArgs.append(QString::fromLocal8Bit(argument));
        if (i == 0)
            continue;
        if (argument == "--help" || argument == "-h" || argument == "--version" || argument == "-v" ||
            argument == "--help-all") {
            infoFlag = true;
        } else if (argument == "--screenshot" || argument.startsWith("--screenshot=") || argument == "--run" ||
                   argument.startsWith("--run=")) {
            scriptedFlag = true;
        }
    }

    // Fast path 1: pure non-GUI CLI subcommands and --help/--version run on a
    // lightweight QCoreApplication and exit without touching QPA or Chromium.
    if (command || infoFlag) {
        QCoreApplication core(argc, argv);
        core.setApplicationName(QStringLiteral("clothbound"));
        core.setOrganizationDomain(QStringLiteral("clothbound.app"));
        core.setApplicationVersion(CLOTHBOUND_VERSION);
        if (command) {
            Migration::runIfNeeded();
            registerMcpRunner(&McpServer::runStdio);
            return runCli(core.arguments());
        }

        QCommandLineParser infoArgs;
        QCommandLineOption infoRun(QStringLiteral("run"), QStringLiteral("Evaluate a QML expression in the window (repeatable)"),
                                   QStringLiteral("expression"));
        QCommandLineOption infoDelay(QStringLiteral("delay"), QStringLiteral("Milliseconds before the first --run step"),
                                     QStringLiteral("ms"), QStringLiteral("1500"));
        QCommandLineOption infoStep(QStringLiteral("step"), QStringLiteral("Milliseconds between --run steps"),
                                    QStringLiteral("ms"), QStringLiteral("700"));
        configureParser(infoArgs, infoRun, infoDelay, infoStep);
        QStringList argsList = core.arguments();
        if (argsList.value(1) == QLatin1String("open"))
            argsList.removeAt(1);
        infoArgs.process(argsList);
        return 0;
    }

    // Fast path 2: warm single-instance D-Bus handoff BEFORE initializing
    // QtWebEngineQuick or QGuiApplication. Note: we intentionally do NOT create
    // and destroy a temporary QCoreApplication here, because ~QCoreApplication
    // shuts down Qt's QDBusConnectionManager post-routine and would stop a
    // subsequent QGuiApplication from receiving D-Bus calls.
    if (!scriptedFlag) {
        QCoreApplication::setApplicationName(QStringLiteral("clothbound"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("clothbound.app"));
        QCoreApplication::setApplicationVersion(CLOTHBOUND_VERSION);
        QCommandLineParser preArgs;
        QCommandLineOption preRun(QStringLiteral("run"), QStringLiteral("Evaluate a QML expression in the window (repeatable)"),
                                  QStringLiteral("expression"));
        QCommandLineOption preDelay(QStringLiteral("delay"), QStringLiteral("Milliseconds before the first --run step"),
                                    QStringLiteral("ms"), QStringLiteral("1500"));
        QCommandLineOption preStep(QStringLiteral("step"), QStringLiteral("Milliseconds between --run steps"),
                                   QStringLiteral("ms"), QStringLiteral("700"));
        configureParser(preArgs, preRun, preDelay, preStep);

        QStringList preArguments = rawArgs;
        if (preArguments.value(1) == QLatin1String("open"))
            preArguments.removeAt(1);
        if (preArgs.parse(preArguments) && !preArgs.isSet(preRun) && !preArgs.isSet(QStringLiteral("screenshot"))) {
            QStringList handoffBooks;
            for (const QString &arg : preArgs.positionalArguments())
                handoffBooks.append(normalizeBookArg(arg));
            if (passBooksToRunning(handoffBooks))
                return 0;
        } else if (!preArgs.errorText().isEmpty()) {
            // Invalid CLI option: fail fast without launching QtWebEngine.
            QCoreApplication core(argc, argv);
            core.setApplicationName(QStringLiteral("clothbound"));
            core.setOrganizationDomain(QStringLiteral("clothbound.app"));
            core.setApplicationVersion(CLOTHBOUND_VERSION);
            preArgs.process(preArguments);
        }
    }

    // GUI launch (or --screenshot / --run): Qt requires QtWebEngineQuick to be
    // initialized before QGuiApplication is constructed. Keep occluded-workspace
    // surfaces alive in Chromium so returning from another Hyprland workspace
    // doesn't find an evicted Viz surface; explicit tray hides still freeze via
    // WebEngineView.lifecycleState.
    registerBookScheme();
    SurfaceWatcher::applyChromiumFlags();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    // The window can hide while a book is read aloud; Clothbound quits itself
    // when it's closed otherwise, or the reading ends.
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("clothbound"));
    app.setOrganizationDomain(QStringLiteral("clothbound.app"));
    app.setApplicationVersion(CLOTHBOUND_VERSION);
    Migration::runIfNeeded();
    // Matches the window to clothbound.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName(QStringLiteral("clothbound"));
    // The name people see, in the bar's tray among others.
    app.setApplicationDisplayName(QStringLiteral("Clothbound"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("clothbound")));

    QCommandLineParser args;
    QCommandLineOption runOption("run", "Evaluate a QML expression in the window (repeatable)", "expression");
    QCommandLineOption delayOption("delay", "Milliseconds before the first --run step", "ms", "1500");
    QCommandLineOption stepOption("step", "Milliseconds between --run steps", "ms", "700");
    configureParser(args, runOption, delayOption, stepOption);
    QStringList arguments = app.arguments();
    if (arguments.value(1) == "open")
        arguments.removeAt(1);
    args.process(arguments);

    // One Clothbound per library: of two opened at once, the one that doesn't get
    // the name hands over. Scripted runs (--run, --screenshot) never claim the
    // name or hand over.
    const bool scripted = args.isSet(runOption) || args.isSet("screenshot");
    QStringList bookFiles;
    for (const QString &arg : args.positionalArguments())
        bookFiles.append(normalizeBookArg(arg));
    Instance instance;
    if (!scripted && !instance.claim() && passBooksToRunning(bookFiles))
        return 0;

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);
    for (const char *font : {":/fonts/Literata.ttf", ":/fonts/Literata-Italic.ttf"})
        QFontDatabase::addApplicationFont(font);
    // Inter's variable font names its family "Inter Variable".
    const QStringList interFamilies =
        QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")));
    // The interface is set in Inter at 13 px, as designed, unless the reader
    // asked for the desktop's own font in Settings.
    const QSettings appearance(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                   QStringLiteral("/settings.ini"),
                               QSettings::IniFormat);
    if (appearance.value(QStringLiteral("appearance/desktopFont"), false).toBool()) {
        adoptDesktopFont(&app);
    } else {
        QFont inter(interFamilies.value(0, QStringLiteral("Inter")));
        inter.setPixelSize(13);
        inter.setHintingPreference(QFont::PreferNoHinting);
        QGuiApplication::setFont(inter);
    }
    // Clothbound's own Controls style (src/ui/style/Clothbound), on top of Basic.
    QQuickStyle::setStyle(QStringLiteral("Clothbound"));

    Theme theme;
    theme.setAppearance(appearance.value(QStringLiteral("appearance/mode"), QStringLiteral("match")).toString());
    theme.setReducedMotion(appearance.value(QStringLiteral("appearance/reducedMotion"), false).toBool());
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
    profile->installUrlSchemeHandler("clothbound", &bookScheme);
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
    VoiceCatalog voices(&ai, &audioCache, &reader, library.database().connectionName(),
                        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/voices.json"));
    // Constructed before Narration and Download so Cast outlives both during
    // stack unwinding (avoiding dangling m_cast pointers on shutdown).
    Cast cast(&ai, &audioCache, &reader, &library, &voices, &voicePacks);
    Narration narration(&ai, &audioCache, &reader, &library);
    Mpris mpris(&narration, &reader, &library);
    Download download(&ai, &audioCache, &reader, &narration);
    narration.setCast(&cast);
    download.setCast(&cast);
    Audiobook audiobook(&audioCache, &download, &reader, &library);
    BookSearch bookSearch(&reader);
    bookSearch.setMeaning(&ai, library.database().connectionName());
    Marks marks(&library);
    Companion companion(&ai, &reader, library.database().connectionName());
    Illustrator illustrator(&ai, &reader, library.database().connectionName(),
                            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/illustrations"));
    QObject::connect(&reader, &Reader::bookChanged, &marks, [&marks, &reader] { marks.setBook(reader.bookId()); });

    // Books named on the command line join the library (they may live
    // anywhere), and the first valid one opens.
    int bookToOpen = 0;
    QStringList startupErrors;
    for (const QString &file : std::as_const(bookFiles)) {
        const int id = library.addBookFile(file);
        if (id) {
            if (!bookToOpen)
                bookToOpen = id;
        } else {
            fprintf(stderr, "%s\n", qPrintable(library.errorString()));
            startupErrors.append(library.errorString());
        }
    }

    LibraryFinder libraryFinder;
    // Every book's words, searchable from the palette, read in gently after
    // start-up and after each scan.
    LibraryIndex libraryIndex;
    auto indexLibrary = [&library, &libraryIndex] {
        QList<LibraryIndex::Source> books;
        for (const BookRecord &r : library.visibleRecords())
            books.append({r.id, r.path, r.format, r.mtime});
        libraryIndex.update(books);
    };
    QObject::connect(&library, &Library::scanFinished, &libraryIndex, [indexLibrary] { indexLibrary(); });
    QTimer::singleShot(8000, &libraryIndex, indexLibrary);
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/ui/style"));
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("library", &library);
    engine.rootContext()->setContextProperty("libraryFinder", &libraryFinder);
    engine.rootContext()->setContextProperty("libraryIndex", &libraryIndex);
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
    SurfaceWatcher surfaceWatcher;
    engine.rootContext()->setContextProperty("desktop", &notifier);
    engine.rootContext()->setContextProperty("instance", &instance);
    engine.rootContext()->setContextProperty("surfaceWatcher", &surfaceWatcher);
    engine.rootContext()->setContextProperty("systemCheck", &systemCheck);
    engine.rootContext()->setContextProperty("localSetup", &localSetup);
    engine.rootContext()->setContextProperty("localSpeech", &localSpeech);
    engine.rootContext()->setContextProperty("audioCache", &audioCache);
    engine.rootContext()->setContextProperty("readerProfile", profile);
    engine.rootContext()->setContextProperty("debugMode", debugEnabled());
    engine.rootContext()->setContextProperty(
        "settingsUrl", QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                           QStringLiteral("/settings.ini")));
    engine.rootContext()->setContextProperty("executablePath", QCoreApplication::applicationFilePath());
    engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
    engine.load(QUrl("qrc:/ui/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (bookToOpen)
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openBook", Q_ARG(QVariant, bookToOpen));
    if (!startupErrors.isEmpty())
        notifier.notify(QObject::tr("Clothbound can't open this book"), startupErrors.first());
    // Opened again: the window comes back (Instance), and a book named then
    // joins the library and opens.
    if (QObject *root = engine.rootObjects().first()) {
        instance.setWindow(qobject_cast<QWindow *>(root));
        QObject::connect(&instance, &Instance::activated, root, [&library, &notifier, root](const QString &book) {
            const QString file = normalizeBookArg(book);
            if (file.isEmpty())
                return;
            if (const int id = library.addBookFile(file))
                QMetaObject::invokeMethod(root, "openBook", Q_ARG(QVariant, id));
            else
                notifier.notify(QObject::tr("Clothbound can't open this book"), library.errorString());
        });
    }
    // Watch window exposure, scene-graph rebuilds, and Hyprland workspace
    // switches so the reading surface always acquires a fresh compositor frame
    // on return (even when tiled and unfocused).
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        surfaceWatcher.attach(window);
        // While debugging, a pointer that --run can move.
        if (debugEnabled())
            engine.rootContext()->setContextProperty("pointer", new PointerDriver(window, &app));
    }
    // The desktop's media controls can ask for the window.
    QObject::connect(&mpris, &Mpris::raiseRequested, &instance, [&instance] { instance.bringBack(); });
    // New books in watched folders are picked up each time Clothbound starts.
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
