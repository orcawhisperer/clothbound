// Clothbound — an ebook reader for Omarchy: your EPUBs and PDFs in one library,
// and a calm page to read them on.

#include "ai/aiservices.h"
#include "audio/audiocache.h"
#include "formats/bookscheme.h"
#include "formats/coverprovider.h"
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
#include "ai/booksearch.h"
#include "library/marks.h"
#include "audio/localspeech.h"
#include "audio/voicepacks.h"
#include "app/pointerdriver.h"
#include "app/migration.h"
#include "app/notifier.h"
#include "app/instance.h"
#include "app/portalfilepicker.h"
#include "reader/reader.h"
#include "reader/readerimageprovider.h"
#include "app/surfacewatcher.h"
#include "app/theme.h"
#include "app/translator.h"
#include "app/trayicon.h"
#include "mcp/mcpserver.h"
#include "online/opdsclient.h"
#include "online/webimporter.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlNetworkAccessManagerFactory>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
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
    for (const QString &book : books) {
        if (!Instance::passToRunning(book))
            return handed;
        handed = true;
    }
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

static bool profileEnabled() {
    return qEnvironmentVariableIntValue("CLOTHBOUND_PROFILE") > 0;
}

struct StartupProfiler {
    bool enabled = false;
    QElapsedTimer timer;
    qint64 lastNs = 0;

    explicit StartupProfiler(bool on) : enabled(on) {
        if (enabled)
            timer.start();
    }

    void mark(const char *phase) {
        if (!enabled)
            return;
        const qint64 nowNs = timer.nsecsElapsed();
        const double elapsedMs = double(nowNs) / 1e6;
        const double deltaMs = double(nowNs - lastNs) / 1e6;
        lastNs = nowNs;
        fprintf(stderr, "PROFILE phase=%s elapsed_ms=%.2f delta_ms=%.2f\n", phase, elapsedMs, deltaMs);
        fflush(stderr);
    }

    static double readProcStatusMiB(const char *field) {
        QFile f(QStringLiteral("/proc/self/status"));
        if (!f.open(QIODevice::ReadOnly))
            return -1.0;
        const QByteArray prefix(field);
        const QList<QByteArray> lines = f.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith(prefix)) {
                const QList<QByteArray> parts = line.simplified().split(' ');
                if (parts.size() >= 2)
                    return parts[1].toDouble() / 1024.0;
            }
        }
        return -1.0;
    }
};

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
    args.setApplicationDescription(QStringLiteral("A calm, local-first EPUB and PDF reader for Linux.\n\n") + cliSummary());
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

    if (!qEnvironmentVariableIsSet("QT_LOGGING_RULES"))
        QLoggingCategory::setFilterRules(QStringLiteral("qt.pdf.bookmarks.warning=false"));

    StartupProfiler prof(profileEnabled());

    // GUI launch (or --screenshot / --run): Qt requires QtWebEngineQuick to be
    // initialized before QGuiApplication is constructed. Keep occluded-workspace
    // surfaces alive in Chromium so returning from another Hyprland workspace
    // doesn't find an evicted Viz surface; explicit tray hides still freeze via
    // WebEngineView.lifecycleState.
    registerBookScheme();
    SurfaceWatcher::applyChromiumFlags();
    QtWebEngineQuick::initialize();
    prof.mark("webengine_init");

    QGuiApplication app(argc, argv);
    // The window can hide while a book is read aloud; Clothbound quits itself
    // when it's closed otherwise, or the reading ends.
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("clothbound"));
    app.setOrganizationDomain(QStringLiteral("clothbound.app"));
    app.setApplicationVersion(CLOTHBOUND_VERSION);
    Migration::runIfNeeded();
    // Inside Flatpak, match the reverse-DNS Flatpak ID (app.clothbound.Reader)
    // so Wayland app_id, portals, and desktop icons resolve cleanly; on native
    // installs, keep clothbound.desktop for existing launchers and window rules.
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID").trimmed();
    const QString desktopId = flatpakId.isEmpty() ? QStringLiteral("clothbound") : flatpakId;
    app.setDesktopFileName(desktopId);
    // The name people see, in the bar's tray among others.
    app.setApplicationDisplayName(QStringLiteral("Clothbound"));
    app.setWindowIcon(flatpakId.isEmpty()
                          ? QIcon::fromTheme(QStringLiteral("clothbound"))
                          : QIcon::fromTheme(flatpakId, QIcon::fromTheme(QStringLiteral("clothbound"))));
    prof.mark("qguiapp");

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
    if (!scripted && !instance.claim()) {
        if (passBooksToRunning(bookFiles))
            return 0;
        // Another instance holds the lock (possibly still starting up before
        // registering on D-Bus). Retry handoff briefly, then exit rather than
        // starting a second GUI against the same SQLite/audio stores.
        for (int attempt = 0; attempt < 10; ++attempt) {
            QThread::msleep(100);
            if (passBooksToRunning(bookFiles))
                return 0;
        }
        fprintf(stderr, "Another Clothbound instance is already running for this library.\n");
        return 1;
    }

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
    prof.mark("fonts_and_style");

    Theme theme;
    theme.setAppearance(appearance.value(QStringLiteral("appearance/mode"), QStringLiteral("match")).toString());
    theme.setCorners(appearance.value(QStringLiteral("appearance/corners"), QStringLiteral("match")).toString());
    theme.setReducedMotion(appearance.value(QStringLiteral("appearance/reducedMotion"), false).toBool());
    Library library;
    if (!library.open()) {
        fprintf(stderr, "The library can't be opened: %s\n", qPrintable(library.errorString()));
        return 1;
    }
    PortalFilePicker picker;
    prof.mark("theme_and_library");

    // The reading surface's own profile: Qt's default QQuickWebEngineProfile is
    // off-the-record with an in-memory HTTP cache (nothing written to disk, no
    // cookies), serves books solely from their ZIPs, and blocks all network I/O.
    // LazyWebProfile defers creating defaultProfile() (and Chromium's child
    // processes) until the first EPUB is opened.
    BookScheme bookScheme;
    OfflineInterceptor offline;
    LazyWebProfile webProfile(&bookScheme, &offline);
    Reader reader(&library, &bookScheme);
    prof.mark("webengine_profile");

    // The composition root for intelligence: Gemini is today's provider,
    // with its key in the keyring. Features only ever see AiServices.
    GeminiProvider gemini;
    SecretKeyStore keys;
    AiServices ai(&gemini, nullptr, library.database().connectionName());
    ai.lookUpKeyIn(&keys);
    // Models the reader runs on this computer, for text work routed there.
    LocalModels localModels;
    ai.setLocalModels(&localModels);
    prof.mark("services_ai");
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
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &narration, &Narration::stop);
    Mpris mpris(&narration, &reader, &library);
    Download download(&ai, &audioCache, &reader, &narration);
    narration.setCast(&cast);
    download.setCast(&cast);
    Audiobook audiobook(&audioCache, &download, &reader, &library);
    prof.mark("services_audio");
    BookSearch bookSearch(&reader);
    bookSearch.setMeaning(&ai, library.database().connectionName());
    Marks marks(&library);
    Companion companion(&ai, &reader, library.database().connectionName());
    Illustrator illustrator(&ai, &reader, library.database().connectionName(),
                            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/illustrations"));
    // Declared after `library` and `keys` so both are destroyed first on shutdown.
    const QString onlineStorageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    WebImporter webImporter(&library, onlineStorageDir);
    OpdsClient opds(&library, &keys, onlineStorageDir);
    QObject::connect(&reader, &Reader::bookChanged, &marks, [&marks, &reader] { marks.setBook(reader.bookId()); });

    LibraryFinder libraryFinder;
    // Every book's words, searchable from the palette, read in gently after
    // start-up and after each scan.
    LibraryIndex libraryIndex;
    prof.mark("services_reader_online");
    auto indexLibrary = [&library, &libraryIndex] {
        QList<LibraryIndex::Source> books;
        for (const BookRecord &r : library.visibleRecords())
            books.append({r.id, r.path, r.format, r.mtime});
        libraryIndex.update(books);
    };
    QObject::connect(&library, &Library::scanFinished, &libraryIndex, [indexLibrary] { indexLibrary(); });
    QTimer::singleShot(8000, &libraryIndex, indexLibrary);

    // Declared BEFORE QQmlApplicationEngine so every context object and the
    // raw QQmlNetworkAccessManagerFactory outlive engine teardown in reverse
    // stack destruction order.
    Notifier notifier;
    TrayIcon trayIcon;
    SurfaceWatcher surfaceWatcher;
    // Use HTTP/1.1 for QML remote cover thumbnails (e.g. covers.openlibrary.org)
    // to prevent Qt 6 HTTP/2 "Data on closed stream" / half-closed stream errors.
    class Http1NetworkAccessManager : public QNetworkAccessManager {
    public:
        using QNetworkAccessManager::QNetworkAccessManager;
    protected:
        QNetworkReply *createRequest(Operation op, const QNetworkRequest &originalReq,
                                     QIODevice *outgoingData = nullptr) override {
            QNetworkRequest req(originalReq);
            if (req.url().scheme().startsWith(QLatin1String("http"), Qt::CaseInsensitive))
                req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
            return QNetworkAccessManager::createRequest(op, req, outgoingData);
        }
    };
    class Http1NamFactory : public QQmlNetworkAccessManagerFactory {
    public:
        QNetworkAccessManager *create(QObject *parent) override { return new Http1NetworkAccessManager(parent); }
    };
    Http1NamFactory qmlNetFactory;

    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&qmlNetFactory);
    struct NamFactoryGuard {
        QQmlApplicationEngine &engine;
        ~NamFactoryGuard() { engine.setNetworkAccessManagerFactory(nullptr); }
    } namFactoryGuard{engine};
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
    engine.rootContext()->setContextProperty("webImporter", &webImporter);
    engine.rootContext()->setContextProperty("opds", &opds);
    engine.rootContext()->setContextProperty("desktop", &notifier);
    engine.rootContext()->setContextProperty("trayIcon", &trayIcon);
    engine.rootContext()->setContextProperty("instance", &instance);
    engine.rootContext()->setContextProperty("surfaceWatcher", &surfaceWatcher);
    engine.rootContext()->setContextProperty("systemCheck", &systemCheck);
    engine.rootContext()->setContextProperty("localSetup", &localSetup);
    engine.rootContext()->setContextProperty("localSpeech", &localSpeech);
    engine.rootContext()->setContextProperty("audioCache", &audioCache);
    engine.rootContext()->setContextProperty("webProfileProvider", &webProfile);
    engine.rootContext()->setContextProperty("debugMode", debugEnabled());
    engine.rootContext()->setContextProperty(
        "settingsUrl", QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                           QStringLiteral("/settings.ini")));
    engine.rootContext()->setContextProperty("executablePath", QCoreApplication::applicationFilePath());
    engine.rootContext()->setContextProperty("hasCoverProvider", true);
    engine.addImageProvider(QStringLiteral("cover"), new CoverProvider);
    engine.addImageProvider(QStringLiteral("pdf"), new PdfPages);
    engine.addImageProvider(QStringLiteral("bookimg"), new ReaderImageProvider);
    engine.addImageProvider(QStringLiteral("opdscover"), new OpdsCoverProvider(&opds));
    engine.load(QUrl("qrc:/ui/Main.qml"));
    prof.mark("qml_engine_load");
    if (engine.rootObjects().isEmpty())
        return 1;
    // Defer CLI book imports until the event loop is running so incoming D-Bus
    // single-instance handoffs are serviced immediately instead of timing out.
    if (!bookFiles.isEmpty()) {
        QObject *root = engine.rootObjects().first();
        QTimer::singleShot(0, root, [&library, &notifier, root, bookFiles] {
            int bookToOpen = 0;
            QStringList startupErrors;
            for (const QString &file : bookFiles) {
                const int id = library.addBookFile(file);
                if (id) {
                    if (!bookToOpen)
                        bookToOpen = id;
                } else {
                    fprintf(stderr, "%s\n", qPrintable(library.errorString()));
                    startupErrors.append(library.errorString());
                }
            }
            if (bookToOpen)
                QMetaObject::invokeMethod(root, "openBook", Q_ARG(QVariant, bookToOpen));
            if (!startupErrors.isEmpty())
                notifier.notify(QObject::tr("Clothbound can't open this book"), startupErrors.first());
        });
    }
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
        if (prof.enabled) {
            auto firstFrameConn = std::make_shared<QMetaObject::Connection>();
            *firstFrameConn = QObject::connect(
                window, &QQuickWindow::frameSwapped, window,
                [window, &prof, &app, firstFrameConn]() {
                    QObject::disconnect(*firstFrameConn);
                    const auto api = window->rendererInterface()
                                         ? window->rendererInterface()->graphicsApi()
                                         : QSGRendererInterface::Unknown;
                    const char *apiName =
                        api == QSGRendererInterface::OpenGL     ? "OpenGL"
                        : api == QSGRendererInterface::Vulkan   ? "Vulkan"
                        : api == QSGRendererInterface::Software ? "Software"
                        : api == QSGRendererInterface::Metal    ? "Metal"
                                                                : "Other";
                    // Emit metrics on the GUI thread right after the first frame swap.
                    QMetaObject::invokeMethod(
                        window,
                        [window, &prof, &app, apiName]() {
                            prof.mark("first_frame");
                            const qsizetype qmlObjects = window->findChildren<QObject *>().size();
                            const qsizetype qmlItems = window->findChildren<QQuickItem *>().size();
                            const double rssMiB = StartupProfiler::readProcStatusMiB("VmRSS:");
                            const double hwmMiB = StartupProfiler::readProcStatusMiB("VmHWM:");
                            fprintf(stderr,
                                    "PROFILE metrics qml_objects=%lld qml_items=%lld rss_mib=%.2f hwm_mib=%.2f "
                                    "gfx_api=%s platform=%s a11y_always=%s\n",
                                    static_cast<long long>(qmlObjects), static_cast<long long>(qmlItems), rssMiB, hwmMiB,
                                    apiName, qPrintable(QGuiApplication::platformName()),
                                    qEnvironmentVariable("QT_LINUX_ACCESSIBILITY_ALWAYS_ON").toUtf8().constData());
                            fflush(stderr);
                            if (qEnvironmentVariableIntValue("CLOTHBOUND_PROFILE_EXIT_AFTER_FRAME") > 0) {
                                const int settleMs = qMax(0, qEnvironmentVariableIntValue("CLOTHBOUND_PROFILE_SETTLE_MS"));
                                if (settleMs > 0) {
                                    QTimer::singleShot(settleMs, &app, [window, &app]() {
                                        const qsizetype settledObjects = window->findChildren<QObject *>().size();
                                        const qsizetype settledItems = window->findChildren<QQuickItem *>().size();
                                        const double settledRss = StartupProfiler::readProcStatusMiB("VmRSS:");
                                        const double settledHwm = StartupProfiler::readProcStatusMiB("VmHWM:");
                                        fprintf(stderr,
                                                "PROFILE settled qml_objects=%lld qml_items=%lld rss_mib=%.2f hwm_mib=%.2f\n",
                                                static_cast<long long>(settledObjects),
                                                static_cast<long long>(settledItems), settledRss, settledHwm);
                                        fflush(stderr);
                                        app.exit(0);
                                    });
                                } else {
                                    app.exit(0);
                                }
                            } else {
                                // Interactive profile mode: log animation frame gaps and GUI-thread stalls.
                                struct FrameProbe {
                                    QElapsedTimer swapTimer;
                                    QElapsedTimer logThrottle;
                                    bool haveSwap = false;
                                };
                                auto probe = std::make_shared<FrameProbe>();
                                probe->logThrottle.start();
                                QObject::connect(
                                    window, &QQuickWindow::frameSwapped, window,
                                    [probe]() {
                                        if (probe->haveSwap) {
                                            const double dtMs = double(probe->swapTimer.nsecsElapsed()) / 1e6;
                                            if (dtMs > 30.0 && dtMs < 1500.0 && probe->logThrottle.elapsed() >= 150) {
                                                probe->logThrottle.restart();
                                                fprintf(stderr, "PROFILE frame_jank dt_ms=%.1f\n", dtMs);
                                                fflush(stderr);
                                            }
                                        }
                                        probe->swapTimer.restart();
                                        probe->haveSwap = true;
                                    },
                                    Qt::DirectConnection);
                                auto guiClock = std::make_shared<QElapsedTimer>();
                                guiClock->start();
                                auto *guiProbe = new QTimer(window);
                                guiProbe->setInterval(20);
                                QObject::connect(guiProbe, &QTimer::timeout, window, [guiClock]() {
                                    const double dtMs = double(guiClock->nsecsElapsed()) / 1e6;
                                    guiClock->restart();
                                    if (dtMs > 45.0) {
                                        fprintf(stderr, "PROFILE gui_stall dt_ms=%.1f\n", dtMs);
                                        fflush(stderr);
                                    }
                                });
                                guiProbe->start();
                            }
                        },
                        Qt::QueuedConnection);
                },
                Qt::DirectConnection);
        }
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
