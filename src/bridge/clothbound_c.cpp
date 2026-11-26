#include "bridge/clothbound_c.h"

#include "formats/bookinfo.h"
#include "formats/bookresourcestore.h"
#include "formats/epubbook.h"
#include "formats/pdfbook.h"
#include "library/library.h"
#include "library/libraryindex.h"
#include "library/librarymodel.h"
#include "library/marks.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QMetaObject>
#include <QMultiMap>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#ifndef CLOTHBOUND_VERSION
#define CLOTHBOUND_VERSION "0.5.2"
#endif

namespace {

// ---------------------------------------------------------------------------
// C-heap allocation helpers (freed via cb_string_free / cb_bytes_free)
// ---------------------------------------------------------------------------

char *allocCString(const QByteArray &utf8) {
    const size_t n = static_cast<size_t>(utf8.size());
    char *buf = static_cast<char *>(std::malloc(n + 1));
    if (!buf)
        return nullptr;
    if (n > 0)
        std::memcpy(buf, utf8.constData(), n);
    buf[n] = '\0';
    return buf;
}

char *allocCString(const QString &str) { return allocCString(str.toUtf8()); }

char *allocJson(const QJsonObject &obj) {
    return allocCString(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

cb_bytes_t allocBytes(const QByteArray &data) {
    cb_bytes_t out{nullptr, 0};
    if (data.isEmpty())
        return out;
    const size_t n = static_cast<size_t>(data.size());
    auto *buf = static_cast<uint8_t *>(std::malloc(n));
    if (!buf)
        return out;
    std::memcpy(buf, data.constData(), n);
    out.data = buf;
    out.len = n;
    return out;
}

void setErrOut(char **err_out, const QString &message) {
    if (!err_out)
        return;
    *err_out = message.isEmpty() ? nullptr : allocCString(message);
}

void setCurrentThreadName() {
#if defined(__APPLE__)
    pthread_setname_np("app.clothbound.core");
#elif defined(__linux__)
    // Linux limits thread names to 15 chars + NUL (16 bytes total).
    pthread_setname_np(pthread_self(), "clothbound.core");
#endif
    if (QThread *t = QThread::currentThread())
        t->setObjectName(QStringLiteral("app.clothbound.core"));
}

// ---------------------------------------------------------------------------
// Headless JSON Catalog Translator (zero QGuiApplication dependency)
// ---------------------------------------------------------------------------

class BridgeTranslator final : public QTranslator {
public:
    explicit BridgeTranslator(const QString &i18nDir, QObject *parent = nullptr)
        : QTranslator(parent), m_i18nDir(i18nDir) {
        setLanguage(QStringLiteral("en"));
    }

    ~BridgeTranslator() override {
        if (m_installed && QCoreApplication::instance())
            QCoreApplication::removeTranslator(this);
    }

    void installOnApp() {
        if (!m_installed && QCoreApplication::instance()) {
            QCoreApplication::installTranslator(this);
            m_installed = true;
        }
    }

    void removeFromApp() {
        if (m_installed && QCoreApplication::instance()) {
            QCoreApplication::removeTranslator(this);
            m_installed = false;
        }
    }

    static QString sanitizeLanguageCode(QString code) {
        code = code.trimmed();
        code.replace(QLatin1Char('_'), QLatin1Char('-'));
        static const QRegularExpression valid(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2,4})*$"));
        return valid.match(code).hasMatch() ? code : QString();
    }

    static QString pluralCategory(const QString &language, long long count) {
        const long long n = count < 0 ? -count : count;
        const long long n10 = n % 10;
        const long long n100 = n % 100;
        const bool millions = n != 0 && n % 1000000 == 0;
        const bool teens = n100 >= 12 && n100 <= 14;
        auto in = [&](std::initializer_list<const char *> codes) {
            for (const char *c : codes)
                if (language == QLatin1String(c))
                    return true;
            return false;
        };
        if (in({"ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms"}))
            return QStringLiteral("other");
        if (in({"hi", "bn", "fa", "si"}))
            return n <= 1 ? QStringLiteral("one") : QStringLiteral("other");
        if (in({"fr", "pt-BR"}))
            return n <= 1 ? QStringLiteral("one") : millions ? QStringLiteral("many") : QStringLiteral("other");
        if (in({"ca", "es", "es-MX", "it", "pt"}))
            return n == 1 ? QStringLiteral("one") : millions ? QStringLiteral("many") : QStringLiteral("other");
        if (language == QLatin1String("fil"))
            return (n10 == 4 || n10 == 6 || n10 == 9) ? QStringLiteral("other") : QStringLiteral("one");
        if (language == QLatin1String("is"))
            return n10 == 1 && n100 != 11 ? QStringLiteral("one") : QStringLiteral("other");
        if (in({"cs", "sk"}))
            return n == 1 ? QStringLiteral("one") : (n >= 2 && n <= 4 ? QStringLiteral("few") : QStringLiteral("other"));
        if (in({"pl", "ru", "uk"}))
            return n == 1 && language == QLatin1String("pl")
                       ? QStringLiteral("one")
                       : (n10 == 1 && n100 != 11 && language != QLatin1String("pl")
                              ? QStringLiteral("one")
                              : (n10 >= 2 && n10 <= 4 && !teens ? QStringLiteral("few") : QStringLiteral("many")));
        if (in({"hr", "sr"}))
            return n10 == 1 && n100 != 11 ? QStringLiteral("one")
                                          : (n10 >= 2 && n10 <= 4 && !teens ? QStringLiteral("few") : QStringLiteral("other"));
        if (language == QLatin1String("sl"))
            return n100 == 1 ? QStringLiteral("one")
                             : (n100 == 2 ? QStringLiteral("two")
                                          : (n100 >= 3 && n100 <= 4 ? QStringLiteral("few") : QStringLiteral("other")));
        if (language == QLatin1String("lt"))
            return n100 >= 11 && n100 <= 19 ? QStringLiteral("other")
                                            : (n10 == 1 ? QStringLiteral("one")
                                                        : (n10 >= 2 ? QStringLiteral("few") : QStringLiteral("other")));
        if (language == QLatin1String("lv"))
            return n10 == 0 || (n100 >= 11 && n100 <= 19) ? QStringLiteral("zero")
                                                          : (n10 == 1 ? QStringLiteral("one") : QStringLiteral("other"));
        if (language == QLatin1String("ro"))
            return n == 1 ? QStringLiteral("one")
                          : (n == 0 || (n100 >= 1 && n100 <= 19) ? QStringLiteral("few") : QStringLiteral("other"));
        if (language == QLatin1String("ar")) {
            if (n <= 2)
                return n == 0 ? QStringLiteral("zero") : (n == 1 ? QStringLiteral("one") : QStringLiteral("two"));
            return n100 >= 3 && n100 <= 10 ? QStringLiteral("few")
                                           : (n100 >= 11 ? QStringLiteral("many") : QStringLiteral("other"));
        }
        if (language == QLatin1String("he"))
            return n == 1 ? QStringLiteral("one") : (n == 2 ? QStringLiteral("two") : QStringLiteral("other"));
        if (language == QLatin1String("ga"))
            return n == 1 ? QStringLiteral("one")
                          : (n == 2 ? QStringLiteral("two")
                                    : (n >= 3 && n <= 6 ? QStringLiteral("few")
                                                        : (n >= 7 && n <= 10 ? QStringLiteral("many") : QStringLiteral("other"))));
        return n == 1 ? QStringLiteral("one") : QStringLiteral("other");
    }

    QString language() const {
        QMutexLocker lock(&m_mutex);
        return m_language;
    }

    bool setLanguage(const QString &rawCode) {
        QString code = rawCode.trimmed();
        if (code.isEmpty()) {
            code = QStringLiteral("en");
        } else {
            code = sanitizeLanguageCode(code);
            if (code.isEmpty())
                return false;
        }

        auto catalog = std::make_shared<Catalog>();
        QStringList codes{QStringLiteral("en"), code.section(QLatin1Char('-'), 0, 0), code};
        codes.removeDuplicates();
        for (const QString &c : std::as_const(codes)) {
            const QString path = resolveCatalogFile(c);
            if (path.isEmpty())
                continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (it.value().isString() && !it.value().toString().isEmpty()) {
                    Entry e;
                    e.isPlural = false;
                    e.text = it.value().toString();
                    catalog->entries.insert(it.key().toUtf8(), std::move(e));
                } else if (it.value().isObject()) {
                    const QJsonObject formsObj = it.value().toObject();
                    Entry e;
                    e.isPlural = true;
                    e.language = c;
                    for (auto fit = formsObj.begin(); fit != formsObj.end(); ++fit)
                        if (fit.value().isString())
                            e.forms.insert(fit.key(), fit.value().toString());
                    catalog->entries.insert(it.key().toUtf8(), std::move(e));
                }
            }
        }
        catalog->empty = catalog->entries.isEmpty();

        {
            QMutexLocker lock(&m_mutex);
            m_language = code;
            m_catalog = std::move(catalog);
        }
        QLocale::setDefault(QLocale(code));
        return true;
    }

    bool isEmpty() const override {
        QMutexLocker lock(&m_mutex);
        return !m_catalog || m_catalog->empty;
    }

    QString translate(const char *, const char *sourceText, const char *, int n = -1) const override {
        if (!sourceText)
            return {};
        std::shared_ptr<const Catalog> catalog;
        {
            QMutexLocker lock(&m_mutex);
            catalog = m_catalog;
        }
        if (!catalog || catalog->empty)
            return {};
        const QByteArray key = QByteArray::fromRawData(sourceText, static_cast<qsizetype>(std::strlen(sourceText)));
        const auto it = catalog->entries.constFind(key);
        if (it == catalog->entries.cend())
            return {};
        const Entry &e = *it;
        if (!e.isPlural)
            return e.text;
        QString form = e.forms.value(n < 0 ? QStringLiteral("other") : pluralCategory(e.language, n));
        if (form.isEmpty())
            form = e.forms.value(QStringLiteral("other"));
        if (form.isEmpty())
            form = e.forms.value(QStringLiteral("many"));
        return form;
    }

private:
    struct Entry {
        bool isPlural = false;
        QString text;
        QString language;
        QHash<QString, QString> forms;
    };
    struct Catalog {
        QHash<QByteArray, Entry> entries;
        bool empty = true;
    };

    QString resolveCatalogFile(const QString &code) const {
        if (code.isEmpty())
            return {};
        const QString suffix = QLatin1Char('/') + code + QStringLiteral(".json");
        if (!m_i18nDir.isEmpty()) {
            const QString candidate = m_i18nDir + suffix;
            if (QFile::exists(candidate))
                return candidate;
        }
        const QString envDir = qEnvironmentVariable("CLOTHBOUND_I18N_DIR");
        if (!envDir.isEmpty()) {
            const QString candidate = envDir + suffix;
            if (QFile::exists(candidate))
                return candidate;
        }
#ifdef CLOTHBOUND_SOURCE_I18N_DIR
        {
            const QString candidate = QStringLiteral(CLOTHBOUND_SOURCE_I18N_DIR) + suffix;
            if (QFile::exists(candidate))
                return candidate;
        }
#endif
        const QString qrcCandidate = QStringLiteral(":/i18n") + suffix;
        if (QFile::exists(qrcCandidate))
            return qrcCandidate;
        return {};
    }

    QString m_i18nDir;
    bool m_installed = false;
    mutable QMutex m_mutex;
    QString m_language = QStringLiteral("en");
    std::shared_ptr<const Catalog> m_catalog;
};

// ---------------------------------------------------------------------------
// Palette search normalization & scoring (zero nested event loops)
// ---------------------------------------------------------------------------

QString normalizeSearchText(const QString &text) {
    if (text.isEmpty())
        return {};
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c);
    }
    return out.toLower();
}

double scoreBookMatch(const QString &q, const QStringList &tokens, const QString &p, const QString &s,
                      const QString &full) {
    if (q.isEmpty())
        return 1.0;
    const qsizetype pIdx = p.indexOf(q);
    if (pIdx == 0)
        return 1000.0 - double(p.size()) * 0.2;
    if (pIdx > 0) {
        const QChar prev = p.at(pIdx - 1);
        const bool boundary = prev == QLatin1Char(' ') || prev == QLatin1Char(':') || prev == QLatin1Char('-');
        return (boundary ? 860.0 : 720.0) - double(pIdx);
    }
    const qsizetype sIdx = s.indexOf(q);
    if (sIdx >= 0)
        return (sIdx == 0 ? 640.0 : 540.0) - double(sIdx) * 0.5;
    if (tokens.size() > 1) {
        double tokenScore = 0.0;
        for (const QString &tok : tokens) {
            const qsizetype tp = p.indexOf(tok);
            const qsizetype ts = s.indexOf(tok);
            if (tp >= 0)
                tokenScore += (tp == 0 ? 180.0 : 120.0);
            else if (ts >= 0)
                tokenScore += 70.0;
            else
                return -1.0;
        }
        return 320.0 + tokenScore;
    }
    qsizetype qi = 0;
    double score = 0.0;
    int consecutive = 0;
    for (qsizetype i = 0; i < full.size() && qi < q.size(); ++i) {
        if (full.at(i) == q.at(qi)) {
            const bool isStart = i == 0 || full.at(i - 1) == QLatin1Char(' ') || full.at(i - 1) == QLatin1Char(':');
            consecutive += 1;
            score += (i < p.size() ? 16.0 : 8.0) + double(consecutive) * 6.0 + (isStart ? 24.0 : 0.0);
            qi += 1;
        } else {
            consecutive = 0;
        }
    }
    return qi == q.size() ? score : -1.0;
}

QJsonObject markToJson(const Mark &m) {
    return QJsonObject{
        {QStringLiteral("id"), m.id},
        {QStringLiteral("kind"), m.kind},
        {QStringLiteral("chapter"), m.chapter},
        {QStringLiteral("start"), static_cast<qint64>(m.start)},
        {QStringLiteral("end"), static_cast<qint64>(m.end)},
        {QStringLiteral("text"), m.text},
        {QStringLiteral("note"), m.note},
        {QStringLiteral("color"), m.color},
        {QStringLiteral("label"), m.label},
        {QStringLiteral("prefix"), m.prefix},
        {QStringLiteral("suffix"), m.suffix},
        {QStringLiteral("createdAt"), m.created.isValid() ? m.created.toSecsSinceEpoch() : 0},
        {QStringLiteral("updatedAt"), m.updated.isValid() ? m.updated.toSecsSinceEpoch() : 0},
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// Core Engine Actor & Session Definitions
// ---------------------------------------------------------------------------

struct EngineActor;

struct cb_engine {
    std::thread stdActorThread;
    QThread *qtActorThread = nullptr;
    bool ownsQCoreApplication = false;
    EngineActor *actor = nullptr;
    std::atomic<QThread *> actorQThread{nullptr};
    BookResourceStore resources;
    std::atomic_bool shuttingDown{false};
};

struct cb_session {
    cb_engine_t *engine = nullptr;
    uint64_t sessionId = 0;
    int bookId = 0;
    QString format;
    QString path;
    QString resourceToken;
    std::shared_ptr<EpubBook> epub;
    std::shared_ptr<PdfBook> pdf;
    std::unique_ptr<Marks> marks;
    bool syncingMarks = false;
};

struct EngineActor final : public QObject {
    cb_engine_t *engine = nullptr;
    cb_event_fn onEvent = nullptr;
    void *userData = nullptr;
    std::unique_ptr<BridgeTranslator> translator;
    std::unique_ptr<Library> library;
    std::unique_ptr<LibraryIndex> index;
    QString cacheDir;
    QList<cb_session_t *> sessions;
    uint64_t nextSessionId = 1;

    EngineActor(cb_engine_t *eng, cb_event_fn cb, void *ud) : engine(eng), onEvent(cb), userData(ud) {}

    void emitEvent(const char *type, const QJsonObject &payload) {
        if (!onEvent || !engine || engine->shuttingDown.load(std::memory_order_acquire))
            return;
        const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        onEvent(userData, type, json.constData());
    }

    bool initialize(const QString &dataDir, const QString &cacheDirectory, const QString &i18nDir,
                    bool installTranslator, QString *errOut) {
        cacheDir = cacheDirectory;
        translator = std::make_unique<BridgeTranslator>(i18nDir);
        if (installTranslator)
            translator->installOnApp();

        library = std::make_unique<Library>(dataDir, cacheDirectory);
        if (!library->open()) {
            if (errOut)
                *errOut = library->errorString().isEmpty() ? QStringLiteral("Failed to open Clothbound library.")
                                                           : library->errorString();
            library.reset();
            translator.reset();
            return false;
        }

        const QString idxPath = cacheDirectory.isEmpty()
                                    ? QString()
                                    : (cacheDirectory + QStringLiteral("/index.sqlite"));
        index = std::make_unique<LibraryIndex>(idxPath);

        // Wire Library signals -> typed C ABI async event callback.
        connect(library.get(), &Library::scanningChanged, this, [this] {
            if (library && library->scanning())
                emitEvent("library.scan_started", QJsonObject{});
        });
        connect(library.get(), &Library::scanProgressChanged, this, [this] {
            if (!library)
                return;
            emitEvent("library.scan_progress",
                      QJsonObject{{QStringLiteral("done"), library->scanDone()},
                                  {QStringLiteral("total"), library->scanTotal()}});
        });
        connect(library.get(), &Library::scanFinished, this, [this](int added, int updated) {
            emitEvent("library.scan_finished",
                      QJsonObject{{QStringLiteral("added"), added}, {QStringLiteral("updated"), updated}});
            refreshIndexSources();
        });
        connect(library.get(), &Library::booksChanged, this, [this] {
            if (!library)
                return;
            emitEvent("library.books_changed", QJsonObject{{QStringLiteral("bookCount"), library->bookCount()}});
        });
        connect(library.get(), &Library::booksAdded, this,
                [this](int firstBookId, int count, const QString &firstTitle, int targetCollectionId) {
                    emitEvent("library.books_added",
                              QJsonObject{{QStringLiteral("firstBookId"), firstBookId},
                                          {QStringLiteral("count"), count},
                                          {QStringLiteral("firstTitle"), firstTitle},
                                          {QStringLiteral("targetCollectionId"), targetCollectionId}});
                });
        connect(library.get(), &Library::bookChanged, this, [this](int id) {
            emitEvent("library.book_changed", QJsonObject{{QStringLiteral("bookId"), id}});
        });
        connect(library.get(), &Library::bookUpdated, this, [this](int id) {
            emitEvent("library.book_updated", QJsonObject{{QStringLiteral("bookId"), id}});
        });
        connect(library.get(), &Library::foldersChanged, this, [this] {
            emitEvent("library.folders_changed", QJsonObject{});
        });
        connect(library.get(), &Library::collectionsChanged, this, [this] {
            emitEvent("library.collections_changed", QJsonObject{});
        });
        connect(library.get(), &Library::readingStatsChanged, this, [this] {
            emitEvent("library.reading_stats_changed", QJsonObject{});
        });
        connect(library.get(), &Library::paletteSearchReady, this, [this](int requestId, const QVariantMap &results) {
            QJsonObject obj = QJsonObject::fromVariantMap(results);
            obj.insert(QStringLiteral("requestId"), requestId);
            emitEvent("search.palette_ready", obj);
        });
        connect(library.get(), &Library::problem, this, [this](const QString &msg) {
            emitEvent("error", QJsonObject{{QStringLiteral("message"), msg}});
        });

        refreshIndexSources();
        return true;
    }

    void refreshIndexSources() {
        if (!library || !index)
            return;
        const QList<BookRecord> visible = library->visibleRecords();
        QList<LibraryIndex::Source> sources;
        sources.reserve(visible.size());
        for (const BookRecord &r : visible)
            sources.append(LibraryIndex::Source{r.id, r.path, r.format, r.mtime});
        index->update(sources);
    }

    void closeSessionOnActor(cb_session_t *session) {
        if (!session)
            return;
        sessions.removeAll(session);
        if (!session->resourceToken.isEmpty() && engine)
            engine->resources.unregisterBook(session->resourceToken);
        session->marks.reset();
        session->epub.reset();
        session->pdf.reset();
        session->engine = nullptr;
        delete session;
    }

    void syncSiblingSessionMarks(cb_session_t *origin) {
        if (!origin)
            return;
        const int bookId = origin->bookId;
        for (cb_session_t *other : std::as_const(sessions)) {
            if (!other || other == origin || other->bookId != bookId || !other->marks)
                continue;
            other->syncingMarks = true;
            other->marks->setBook(0);
            other->marks->setBook(bookId);
            other->syncingMarks = false;
        }
    }

    void shutdown() {
        onEvent = nullptr;
        userData = nullptr;
        // Close any lingering sessions so no Marks/Library references outlive Library.
        const QList<cb_session_t *> lingering = sessions;
        sessions.clear();
        for (cb_session_t *s : lingering) {
            if (!s)
                continue;
            if (!s->resourceToken.isEmpty() && engine)
                engine->resources.unregisterBook(s->resourceToken);
            s->marks.reset();
            s->epub.reset();
            s->pdf.reset();
            s->engine = nullptr;
            delete s;
        }
        if (engine)
            engine->resources.clear();
        if (index) {
            index->cancel();
            index.reset();
        }
        if (library) {
            library->cancelScan();
            library->cancelPaletteSearch();
            library.reset();
        }
        if (translator) {
            translator->removeFromApp();
            translator.reset();
        }
    }
};

namespace {

template <typename Fn>
auto invokeSync(cb_engine_t *engine, Fn &&fn) -> decltype(fn()) {
    using Ret = decltype(fn());
    QThread *actorThread = engine->actorQThread.load(std::memory_order_acquire);
    if (QThread::currentThread() == actorThread)
        return fn();
    if constexpr (std::is_void_v<Ret>) {
        QMetaObject::invokeMethod(engine->actor, std::forward<Fn>(fn), Qt::BlockingQueuedConnection);
    } else {
        Ret out{};
        QMetaObject::invokeMethod(
            engine->actor, [&out, f = std::forward<Fn>(fn)]() mutable { out = f(); },
            Qt::BlockingQueuedConnection);
        return out;
    }
}

template <typename Fn>
void invokeAsync(cb_engine_t *engine, Fn &&fn) {
    QMetaObject::invokeMethod(engine->actor, std::forward<Fn>(fn), Qt::QueuedConnection);
}

bool validEngine(const cb_engine_t *engine) {
    return engine != nullptr && engine->actor != nullptr &&
           !engine->shuttingDown.load(std::memory_order_acquire);
}

bool validSession(const cb_session_t *session) {
    return session != nullptr && validEngine(session->engine);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public C ABI Exports
// ---------------------------------------------------------------------------

extern "C" {

const char *cb_engine_version(void) { return CLOTHBOUND_VERSION; }

void cb_string_free(char *str) { std::free(str); }

void cb_bytes_free(cb_bytes_t bytes) { std::free(bytes.data); }

void cb_resource_free(cb_resource_t *resource) {
    if (!resource)
        return;
    std::free(resource->mime_type);
    std::free(resource->body.data);
    if (resource->headers) {
        for (size_t i = 0; i < resource->header_count; ++i) {
            std::free(resource->headers[i].name);
            std::free(resource->headers[i].value);
        }
        std::free(resource->headers);
    }
    resource->status = 0;
    resource->mime_type = nullptr;
    resource->body.data = nullptr;
    resource->body.len = 0;
    resource->headers = nullptr;
    resource->header_count = 0;
}

cb_engine_t *cb_engine_create(const char *data_dir,
                              const char *cache_dir,
                              const char *i18n_dir,
                              cb_event_fn on_event,
                              void *user_data,
                              char **err_out) {
    if (err_out)
        *err_out = nullptr;

    const QString dataDir = data_dir ? QString::fromUtf8(data_dir) : QString();
    const QString cacheDir = cache_dir ? QString::fromUtf8(cache_dir) : QString();
    const QString i18nDir = i18n_dir ? QString::fromUtf8(i18n_dir) : QString();

    auto *engine = new cb_engine();

    if (QCoreApplication::instance() == nullptr) {
        // Standalone non-Qt host (macOS SwiftUI @main App / pure C host):
        // dedicate ONE background std::thread to QCoreApplication::exec().
        engine->ownsQCoreApplication = true;
        struct StartupResult {
            bool ok = false;
            QString error;
        };
        std::promise<StartupResult> readyPromise;
        std::future<StartupResult> readyFuture = readyPromise.get_future();

        engine->stdActorThread = std::thread(
            [engine, dataDir, cacheDir, i18nDir, on_event, user_data, promise = std::move(readyPromise)]() mutable {
                setCurrentThreadName();
                int argc = 1;
                char arg0[] = "clothbound-core";
                char *argv[] = {arg0, nullptr};

                QCoreApplication app(argc, argv);
                app.setApplicationName(QStringLiteral("clothbound"));
                app.setOrganizationDomain(QStringLiteral("clothbound.app"));
                app.setApplicationVersion(QStringLiteral(CLOTHBOUND_VERSION));

                auto actor = std::make_unique<EngineActor>(engine, on_event, user_data);
                QString initError;
                if (!actor->initialize(dataDir, cacheDir, i18nDir, /*installTranslator=*/true, &initError)) {
                    actor->shutdown();
                    actor.reset();
                    promise.set_value({false, initError});
                    return;
                }

                engine->actor = actor.get();
                engine->actorQThread.store(QThread::currentThread(), std::memory_order_release);

                // Signal readiness AFTER QCoreApplication::exec() has entered the event loop
                // so an immediate cb_engine_destroy() never races QCoreApplication::quit().
                QTimer::singleShot(0, &app, [p = std::move(promise)]() mutable { p.set_value({true, {}}); });

                app.exec();

                // Cleanly destroy all Qt objects before QCoreApplication unwinds.
                if (actor) {
                    actor->shutdown();
                    engine->actor = nullptr;
                    actor.reset();
                }
                engine->actorQThread.store(nullptr, std::memory_order_release);
            });

        const StartupResult res = readyFuture.get();
        if (!res.ok) {
            if (engine->stdActorThread.joinable())
                engine->stdActorThread.join();
            delete engine;
            setErrOut(err_out, res.error.isEmpty() ? QStringLiteral("Failed to initialize ClothboundCore.") : res.error);
            return nullptr;
        }
        return engine;
    }

    // Existing QCoreApplication host (e.g., QTEST_GUILESS_MAIN):
    // host EngineActor on a dedicated background QThread event loop.
    engine->ownsQCoreApplication = false;
    auto *qtThread = new QThread();
    qtThread->setObjectName(QStringLiteral("app.clothbound.core"));
    qtThread->start();

    auto *actor = new EngineActor(engine, on_event, user_data);
    actor->moveToThread(qtThread);

    bool ok = false;
    QString initError;
    QMetaObject::invokeMethod(
        actor,
        [&]() {
            setCurrentThreadName();
            ok = actor->initialize(dataDir, cacheDir, i18nDir, /*installTranslator=*/false, &initError);
            if (!ok) {
                actor->shutdown();
                delete actor;
                actor = nullptr;
            }
        },
        Qt::BlockingQueuedConnection);

    if (!ok) {
        qtThread->quit();
        qtThread->wait();
        delete qtThread;
        delete engine;
        setErrOut(err_out, initError.isEmpty() ? QStringLiteral("Failed to initialize ClothboundCore.") : initError);
        return nullptr;
    }

    engine->qtActorThread = qtThread;
    engine->actor = actor;
    engine->actorQThread.store(qtThread, std::memory_order_release);
    return engine;
}

void cb_engine_destroy(cb_engine_t *engine) {
    if (!engine)
        return;
    engine->shuttingDown.store(true, std::memory_order_release);

    if (engine->ownsQCoreApplication) {
        if (engine->actor) {
            invokeSync(engine, [engine] {
                if (engine->actor)
                    engine->actor->shutdown();
                QCoreApplication::quit();
            });
        }
        if (engine->stdActorThread.joinable())
            engine->stdActorThread.join();
    } else if (engine->qtActorThread) {
        if (engine->actor) {
            invokeSync(engine, [engine] {
                EngineActor *a = engine->actor;
                engine->actor = nullptr;
                if (a) {
                    a->shutdown();
                    delete a;
                }
            });
        }
        engine->actorQThread.store(nullptr, std::memory_order_release);
        engine->qtActorThread->quit();
        engine->qtActorThread->wait();
        delete engine->qtActorThread;
        engine->qtActorThread = nullptr;
    }

    delete engine;
}

bool cb_engine_is_on_actor_thread(const cb_engine_t *engine) {
    if (!engine)
        return false;
    QThread *actorThread = engine->actorQThread.load(std::memory_order_acquire);
    return actorThread != nullptr && QThread::currentThread() == actorThread;
}

bool cb_engine_set_language(cb_engine_t *engine, const char *bcp47_code) {
    if (!validEngine(engine))
        return false;
    const QString code = bcp47_code ? QString::fromUtf8(bcp47_code) : QString();
    return invokeSync(engine, [engine, code]() -> bool {
        if (!engine->actor || !engine->actor->translator)
            return false;
        const bool ok = engine->actor->translator->setLanguage(code);
        if (ok) {
            engine->actor->emitEvent(
                "language.changed",
                QJsonObject{{QStringLiteral("language"), engine->actor->translator->language()}});
        }
        return ok;
    });
}

char *cb_engine_language(cb_engine_t *engine) {
    if (!validEngine(engine))
        return nullptr;
    const QString lang = invokeSync(engine, [engine]() -> QString {
        return (engine->actor && engine->actor->translator) ? engine->actor->translator->language()
                                                            : QStringLiteral("en");
    });
    return allocCString(lang);
}

char *cb_engine_list_books_json(cb_engine_t *engine) {
    if (!validEngine(engine))
        return nullptr;
    const QJsonObject payload = invokeSync(engine, [engine]() -> QJsonObject {
        if (!engine->actor || !engine->actor->library)
            return {};
        Library *lib = engine->actor->library.get();
        return QJsonObject{
            {QStringLiteral("books"), QJsonArray::fromVariantList(lib->allBooks())},
            {QStringLiteral("continueReading"), QJsonArray::fromVariantList(lib->continueReading())},
            {QStringLiteral("shelfCounts"), QJsonObject::fromVariantMap(lib->shelfCounts())},
            {QStringLiteral("collections"), QJsonArray::fromVariantList(lib->collections())},
            {QStringLiteral("folders"), QJsonArray::fromVariantList(lib->folderStats())},
            {QStringLiteral("readingStats"), QJsonObject::fromVariantMap(lib->readingStats())},
            {QStringLiteral("bookCount"), lib->bookCount()},
            {QStringLiteral("scanning"), lib->scanning()},
            {QStringLiteral("scanDone"), lib->scanDone()},
            {QStringLiteral("scanTotal"), lib->scanTotal()},
        };
    });
    return allocJson(payload);
}

bool cb_engine_add_paths(cb_engine_t *engine, const char *const *paths, size_t count) {
    if (!validEngine(engine) || !paths || count == 0)
        return false;
    QStringList list;
    list.reserve(static_cast<qsizetype>(count));
    for (size_t i = 0; i < count; ++i) {
        if (paths[i] && paths[i][0] != '\0')
            list.append(QString::fromUtf8(paths[i]));
    }
    if (list.isEmpty())
        return false;

    return invokeSync(engine, [engine, list]() -> bool {
        if (!engine->actor || !engine->actor->library)
            return false;
        Library *lib = engine->actor->library.get();
        bool anyAdded = false;
        bool addedFiles = false;
        for (const QString &p : list) {
            const QFileInfo fi(p);
            if (!fi.exists())
                continue;
            // Treat .epub directories (Apple Books unzipped bundles) or regular book files as books
            // when BookFiles::isBook recognizes them and they are not plain folders.
            if (fi.isDir() && !QFileInfo::exists(p + QStringLiteral("/META-INF/container.xml"))) {
                lib->addFolder(p);
                anyAdded = true;
            } else if (BookFiles::isBook(p)) {
                if (lib->addBookFile(p) > 0) {
                    anyAdded = true;
                    addedFiles = true;
                }
            }
        }
        if (addedFiles)
            engine->actor->refreshIndexSources();
        return anyAdded;
    });
}

bool cb_engine_remove_folder(cb_engine_t *engine, const char *folder_path) {
    if (!validEngine(engine) || !folder_path || folder_path[0] == '\0')
        return false;
    const QString folder = QString::fromUtf8(folder_path);
    return invokeSync(engine, [engine, folder]() -> bool {
        if (!engine->actor || !engine->actor->library)
            return false;
        Library *lib = engine->actor->library.get();
        const int before = int(lib->folders().size());
        lib->removeFolder(folder);
        return int(lib->folders().size()) < before;
    });
}

void cb_engine_rescan(cb_engine_t *engine) {
    if (!validEngine(engine))
        return;
    invokeAsync(engine, [engine] {
        if (engine->actor && engine->actor->library)
            engine->actor->library->rescan();
    });
}

void cb_engine_cancel_scan(cb_engine_t *engine) {
    if (!validEngine(engine))
        return;
    invokeSync(engine, [engine] {
        if (engine->actor && engine->actor->library)
            engine->actor->library->cancelScan();
    });
}

char *cb_engine_search_palette_json(cb_engine_t *engine, const char *query, int scope_book_id) {
    if (!validEngine(engine))
        return nullptr;
    const QString qStr = query ? QString::fromUtf8(query).trimmed() : QString();

    const QJsonObject payload = invokeSync(engine, [engine, qStr, scope_book_id]() -> QJsonObject {
        if (!engine->actor || !engine->actor->library)
            return {};
        Library *lib = engine->actor->library.get();
        LibraryIndex *idx = engine->actor->index.get();
        const QList<BookRecord> visible = lib->visibleRecords();

        // 1. Books (omitted when scoped to a single book).
        QJsonArray booksOut;
        if (scope_book_id <= 0) {
            const QString normQ = normalizeSearchText(qStr).trimmed();
            static const QRegularExpression wsRe(QStringLiteral("\\s+"));
            const QStringList qTokens = normQ.split(wsRe, Qt::SkipEmptyParts);
            struct Hit {
                double score = 0.0;
                const BookRecord *rec = nullptr;
            };
            QList<Hit> hits;
            hits.reserve(visible.size());
            for (const BookRecord &r : visible) {
                const QString p = normalizeSearchText(r.title);
                QStringList meta;
                if (!r.author.isEmpty())
                    meta.append(r.author);
                if (!r.series.isEmpty())
                    meta.append(r.series);
                if (!r.format.isEmpty())
                    meta.append(r.format);
                const QString s = normalizeSearchText(meta.join(QLatin1Char(' ')));
                const QString full = s.isEmpty() ? p : (p + QLatin1Char(' ') + s);
                const double sc = scoreBookMatch(normQ, qTokens, p, s, full);
                if (sc >= 0.0)
                    hits.append({sc, &r});
            }
            std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
                if (a.score != b.score)
                    return a.score > b.score;
                if (a.rec->openedAt != b.rec->openedAt)
                    return a.rec->openedAt > b.rec->openedAt;
                return a.rec->id < b.rec->id;
            });
            const int take = qMin(12, int(hits.size()));
            for (int i = 0; i < take; ++i) {
                QJsonObject o = QJsonObject::fromVariantMap(lib->toMap(*hits[i].rec));
                o.insert(QStringLiteral("score"), hits[i].score);
                booksOut.append(o);
            }
        }

        // 2. Passages (FTS5 index + lexical fallback for freshly imported unindexed books).
        QJsonArray passagesOut;
        if (qStr.size() >= 2) {
            QVariantList ftsHits = idx ? idx->search(qStr, 8, scope_book_id) : QVariantList{};
            for (const QVariant &v : std::as_const(ftsHits)) {
                QJsonObject o = QJsonObject::fromVariantMap(v.toMap());
                const int bid = o.value(QStringLiteral("bookId")).toInt();
                if (const BookRecord *rec = lib->bookRecord(bid)) {
                    o.insert(QStringLiteral("bookTitle"), rec->title);
                    o.insert(QStringLiteral("author"), rec->author);
                    o.insert(QStringLiteral("format"), rec->format);
                }
                passagesOut.append(o);
            }
            if (passagesOut.isEmpty()) {
                int scanned = 0;
                for (const BookRecord &rec : visible) {
                    if (passagesOut.size() >= 8 || scanned >= 10)
                        break;
                    if (scope_book_id > 0 && rec.id != scope_book_id)
                        continue;
                    ++scanned;
                    if (rec.format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
                        PdfBook pdf(rec.path);
                        if (!pdf.open())
                            continue;
                        const int pages = qMin(pdf.pageCount(), 50);
                        for (int p = 0; p < pages && passagesOut.size() < 8; ++p) {
                            const QString text = pdf.pageText(p);
                            const qsizetype pos = text.indexOf(qStr, 0, Qt::CaseInsensitive);
                            if (pos >= 0) {
                                const qsizetype from = qMax<qsizetype>(0, pos - 40);
                                const qsizetype len = qMin<qsizetype>(text.size() - from, qStr.size() + 80);
                                passagesOut.append(QJsonObject{
                                    {QStringLiteral("bookId"), rec.id},
                                    {QStringLiteral("chapter"), p},
                                    {QStringLiteral("offset"), static_cast<qint64>(pos)},
                                    {QStringLiteral("snippet"), text.mid(from, len).simplified()},
                                    {QStringLiteral("bookTitle"), rec.title},
                                    {QStringLiteral("author"), rec.author},
                                    {QStringLiteral("format"), rec.format},
                                });
                            }
                        }
                    } else if (rec.format.compare(QLatin1String("epub"), Qt::CaseInsensitive) == 0) {
                        EpubBook epub(rec.path);
                        if (!epub.open() || epub.isDrmProtected())
                            continue;
                        const int chapters = qMin(int(epub.spine().size()), 50);
                        for (int ch = 0; ch < chapters && passagesOut.size() < 8; ++ch) {
                            const QString text = epub.chapterText(ch);
                            const qsizetype pos = text.indexOf(qStr, 0, Qt::CaseInsensitive);
                            if (pos >= 0) {
                                const qsizetype from = qMax<qsizetype>(0, pos - 40);
                                const qsizetype len = qMin<qsizetype>(text.size() - from, qStr.size() + 80);
                                passagesOut.append(QJsonObject{
                                    {QStringLiteral("bookId"), rec.id},
                                    {QStringLiteral("chapter"), ch},
                                    {QStringLiteral("offset"), static_cast<qint64>(pos)},
                                    {QStringLiteral("snippet"), text.mid(from, len).simplified()},
                                    {QStringLiteral("bookTitle"), rec.title},
                                    {QStringLiteral("author"), rec.author},
                                    {QStringLiteral("format"), rec.format},
                                });
                            }
                        }
                    }
                }
            }
        }

        // 3. Marks (direct SQLite query on Library connection, zero nested event loops).
        QJsonArray marksOut;
        QSqlDatabase db = lib->database();
        if (db.isOpen()) {
            const QStringList tokens = WorksModel::searchable(qStr).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            QSqlQuery sql(db);
            if (scope_book_id > 0) {
                sql.prepare(QStringLiteral(
                    "SELECT a.id, a.book_id, a.kind, a.chapter, a.start, a.finish, a.text, a.note, "
                    "a.color, a.label, a.created_at, a.updated_at, b.title, b.author "
                    "FROM annotations a JOIN books b ON b.id = a.book_id "
                    "WHERE b.hidden = 0 AND b.missing = 0 AND a.book_id = ? "
                    "ORDER BY a.updated_at DESC, a.id DESC"));
                sql.addBindValue(scope_book_id);
            } else {
                sql.prepare(QStringLiteral(
                    "SELECT a.id, a.book_id, a.kind, a.chapter, a.start, a.finish, a.text, a.note, "
                    "a.color, a.label, a.created_at, a.updated_at, b.title, b.author "
                    "FROM annotations a JOIN books b ON b.id = a.book_id "
                    "WHERE b.hidden = 0 AND b.missing = 0 "
                    "ORDER BY a.updated_at DESC, a.id DESC"));
            }
            if (sql.exec()) {
                while (sql.next() && marksOut.size() < 8) {
                    const QString quote = sql.value(6).toString();
                    const QString note = sql.value(7).toString();
                    const QString label = sql.value(9).toString();
                    const QString bookTitle = sql.value(12).toString();
                    const QString bookAuthor = sql.value(13).toString();
                    if (!tokens.isEmpty()) {
                        const QString hay = WorksModel::searchable(
                            quote + QLatin1Char(' ') + note + QLatin1Char(' ') + label + QLatin1Char(' ') +
                            bookTitle + QLatin1Char(' ') + bookAuthor);
                        bool ok = true;
                        for (const QString &tok : tokens) {
                            if (!hay.contains(tok)) {
                                ok = false;
                                break;
                            }
                        }
                        if (!ok)
                            continue;
                    }
                    marksOut.append(QJsonObject{
                        {QStringLiteral("id"), sql.value(0).toInt()},
                        {QStringLiteral("bookId"), sql.value(1).toInt()},
                        {QStringLiteral("kind"), sql.value(2).toString()},
                        {QStringLiteral("chapter"), sql.value(3).toInt()},
                        {QStringLiteral("start"), sql.value(4).toLongLong()},
                        {QStringLiteral("end"), sql.value(5).toLongLong()},
                        {QStringLiteral("text"), quote},
                        {QStringLiteral("note"), note},
                        {QStringLiteral("color"), sql.value(8).toString()},
                        {QStringLiteral("label"), label},
                        {QStringLiteral("createdAt"), sql.value(10).toLongLong()},
                        {QStringLiteral("updatedAt"), sql.value(11).toLongLong()},
                        {QStringLiteral("bookTitle"), bookTitle},
                        {QStringLiteral("bookAuthor"), bookAuthor},
                    });
                }
            }
        }

        return QJsonObject{
            {QStringLiteral("query"), qStr},
            {QStringLiteral("scopeBookId"), scope_book_id},
            {QStringLiteral("books"), booksOut},
            {QStringLiteral("passages"), passagesOut},
            {QStringLiteral("marks"), marksOut},
        };
    });

    return allocJson(payload);
}

int cb_engine_search_palette_async(cb_engine_t *engine, const char *query, int scope_book_id) {
    if (!validEngine(engine))
        return 0;
    const QString qStr = query ? QString::fromUtf8(query) : QString();
    return invokeSync(engine, [engine, qStr, scope_book_id]() -> int {
        if (!engine->actor || !engine->actor->library)
            return 0;
        const QString idxPath = engine->actor->index ? engine->actor->index->path() : QString();
        return engine->actor->library->searchPaletteAsync(qStr, 0, 12, 8, 8, scope_book_id,
                                                          QStringLiteral("all"), idxPath);
    });
}

cb_bytes_t cb_engine_book_cover_jpeg(cb_engine_t *engine, int book_id) {
    if (!validEngine(engine) || book_id <= 0)
        return {nullptr, 0};

    const QByteArray jpeg = invokeSync(engine, [engine, book_id]() -> QByteArray {
        if (!engine->actor || !engine->actor->library)
            return {};
        Library *lib = engine->actor->library.get();
        const BookRecord *rec = lib->bookRecord(book_id);
        if (!rec)
            return {};

        if (!rec->cover.isEmpty()) {
            const QString coverPath = lib->coverDirectory() + QLatin1Char('/') + rec->cover;
            QFile f(coverPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray raw = f.readAll();
                if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xFF &&
                    static_cast<unsigned char>(raw[1]) == 0xD8 &&
                    static_cast<unsigned char>(raw[2]) == 0xFF) {
                    return raw;
                }
                QImage img;
                if (img.loadFromData(raw) && !img.isNull()) {
                    QByteArray out;
                    QBuffer buf(&out);
                    if (buf.open(QIODevice::WriteOnly) && img.convertToFormat(QImage::Format_RGB32).save(&buf, "JPEG", 88))
                        return out;
                }
            }
        }

        const BookInfo info = BookFiles::read(rec->path, 480);
        if (info.cover.isNull())
            return {};
        QByteArray out;
        QBuffer buf(&out);
        if (buf.open(QIODevice::WriteOnly) && info.cover.save(&buf, "JPEG", 88))
            return out;
        return {};
    });

    return allocBytes(jpeg);
}

bool cb_engine_resolve_resource(cb_engine_t *engine,
                                const char *url,
                                const char *referer,
                                const char *fallback_token,
                                cb_resource_t *out) {
    if (!out)
        return false;
    std::memset(out, 0, sizeof(*out));
    if (!validEngine(engine) || !url || url[0] == '\0') {
        out->status = 400;
        return false;
    }

    const QUrl parsedUrl(QString::fromUtf8(url));
    if (!parsedUrl.isValid()) {
        out->status = 400;
        return false;
    }
    const QByteArray refBytes = referer ? QByteArray(referer) : QByteArray();
    const QString fallback = fallback_token ? QString::fromUtf8(fallback_token).trimmed() : QString();

    // BookResourceStore is mutex-protected and EpubBook is thread-safe read-only once opened,
    // so WKURLSchemeHandler callers can resolve directly without blocking on the actor thread.
    const BookResourceStore::ResolvedResource resolved = engine->resources.resolve(parsedUrl, refBytes, fallback);
    out->status = resolved.status;
    if (resolved.status < 200 || resolved.status >= 300)
        return false;

    out->mime_type = allocCString(resolved.contentType);
    out->body = allocBytes(resolved.body);
    if (!resolved.headers.isEmpty()) {
        const size_t n = static_cast<size_t>(resolved.headers.size());
        auto *arr = static_cast<cb_header_t *>(std::calloc(n, sizeof(cb_header_t)));
        if (arr) {
            for (size_t i = 0; i < n; ++i) {
                arr[i].name = allocCString(resolved.headers[static_cast<qsizetype>(i)].first);
                arr[i].value = allocCString(resolved.headers[static_cast<qsizetype>(i)].second);
            }
            out->headers = arr;
            out->header_count = n;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-Window Book Session APIs
// ---------------------------------------------------------------------------

cb_session_t *cb_session_open(cb_engine_t *engine, int book_id, char **err_out) {
    if (err_out)
        *err_out = nullptr;
    if (!validEngine(engine) || book_id <= 0) {
        setErrOut(err_out, QStringLiteral("Invalid engine handle or book_id."));
        return nullptr;
    }

    QString error;
    cb_session_t *session = invokeSync(engine, [engine, book_id, &error]() -> cb_session_t * {
        if (!engine->actor || !engine->actor->library) {
            error = QStringLiteral("Engine library is not open.");
            return nullptr;
        }
        Library *lib = engine->actor->library.get();
        const BookRecord *rec = lib->bookRecord(book_id);
        if (!rec) {
            error = QStringLiteral("Book %1 not found in library.").arg(book_id);
            return nullptr;
        }

        auto s = std::make_unique<cb_session>();
        s->engine = engine;
        s->sessionId = engine->actor->nextSessionId++;
        s->bookId = book_id;
        s->format = rec->format.toLower();
        s->path = rec->path;

        if (s->format == QLatin1String("epub")) {
            auto epub = std::make_shared<EpubBook>(rec->path);
            if (!epub->open()) {
                error = epub->errorString().isEmpty() ? QStringLiteral("Failed to open EPUB.") : epub->errorString();
                return nullptr;
            }
            if (epub->isDrmProtected()) {
                error = QStringLiteral("This book is protected by DRM.");
                return nullptr;
            }
            s->resourceToken = engine->resources.registerBook(epub);
            s->epub = std::move(epub);
        } else if (s->format == QLatin1String("pdf")) {
            auto pdf = std::make_shared<PdfBook>(rec->path);
            if (!pdf->open()) {
                error = pdf->errorString().isEmpty() ? QStringLiteral("Failed to open PDF.") : pdf->errorString();
                return nullptr;
            }
            s->pdf = std::move(pdf);
        } else {
            error = QStringLiteral("Unsupported book format: %1").arg(rec->format);
            return nullptr;
        }

        s->marks = std::make_unique<Marks>(lib);
        s->marks->setBook(book_id);

        cb_session_t *raw = s.get();
        QObject::connect(s->marks.get(), &Marks::changed, engine->actor, [actor = engine->actor, raw] {
            if (!actor || !raw || raw->syncingMarks)
                return;
            actor->syncSiblingSessionMarks(raw);
            actor->emitEvent("marks.changed",
                             QJsonObject{{QStringLiteral("bookId"), raw->bookId},
                                         {QStringLiteral("sessionId"), static_cast<qint64>(raw->sessionId)}});
        });

        lib->markOpened(book_id);
        engine->actor->sessions.append(raw);
        return s.release();
    });

    if (!session)
        setErrOut(err_out, error);
    return session;
}

void cb_session_close(cb_session_t *session) {
    if (!session)
        return;
    cb_engine_t *engine = session->engine;
    if (!validEngine(engine)) {
        delete session;
        return;
    }
    invokeSync(engine, [engine, session] {
        if (engine->actor)
            engine->actor->closeSessionOnActor(session);
    });
}

int cb_session_book_id(const cb_session_t *session) {
    return session ? session->bookId : 0;
}

char *cb_session_metadata_json(cb_session_t *session) {
    if (!validSession(session))
        return nullptr;

    const QJsonObject meta = invokeSync(session->engine, [session]() -> QJsonObject {
        Library *lib = session->engine->actor->library.get();
        const BookRecord rec = lib->record(session->bookId);

        QJsonObject out{
            {QStringLiteral("bookId"), session->bookId},
            {QStringLiteral("sessionId"), static_cast<qint64>(session->sessionId)},
            {QStringLiteral("format"), session->format},
            {QStringLiteral("path"), session->path},
            {QStringLiteral("title"), rec.title},
            {QStringLiteral("author"), rec.author},
            {QStringLiteral("editor"), rec.editor},
            {QStringLiteral("publisher"), rec.publisher},
            {QStringLiteral("year"), rec.year},
            {QStringLiteral("language"), rec.language},
            {QStringLiteral("description"), rec.description},
            {QStringLiteral("isbn"), rec.isbn},
            {QStringLiteral("series"), rec.series},
            {QStringLiteral("seriesIndex"), rec.seriesIndex},
            {QStringLiteral("progress"), rec.progress},
            {QStringLiteral("resourceToken"), session->resourceToken},
            {QStringLiteral("baseUrl"),
             session->resourceToken.isEmpty() ? QString() : BookResourceStore::baseUrl(session->resourceToken)},
        };

        if (!rec.position.isEmpty()) {
            const QJsonDocument posDoc = QJsonDocument::fromJson(rec.position.toUtf8());
            if (posDoc.isObject())
                out.insert(QStringLiteral("savedPosition"), posDoc.object());
            else
                out.insert(QStringLiteral("savedPosition"), rec.position);
        } else {
            out.insert(QStringLiteral("savedPosition"), QJsonValue::Null);
        }

        if (session->epub) {
            const EpubBook &epub = *session->epub;
            out.insert(QStringLiteral("chapterCount"), int(epub.spine().size()));
            out.insert(QStringLiteral("pageCount"), int(epub.pageList().size()));
            out.insert(QStringLiteral("isFixedLayout"), epub.isFixedLayout());
            out.insert(QStringLiteral("isRightToLeft"), epub.isRightToLeft());
            out.insert(QStringLiteral("isDrmProtected"), epub.isDrmProtected());
            out.insert(QStringLiteral("epubVersion"), epub.version());

            QJsonArray spineArr;
            const auto &spine = epub.spine();
            for (int i = 0; i < spine.size(); ++i) {
                spineArr.append(QJsonObject{
                    {QStringLiteral("index"), i},
                    {QStringLiteral("id"), spine[i].id},
                    {QStringLiteral("path"), spine[i].path},
                    {QStringLiteral("mediaType"), spine[i].mediaType},
                    {QStringLiteral("linear"), spine[i].linear},
                    {QStringLiteral("estimatedLength"), epub.estimatedChapterLength(i)},
                });
            }
            out.insert(QStringLiteral("spine"), spineArr);

            QJsonArray tocArr;
            for (const EpubTocEntry &e : epub.toc()) {
                tocArr.append(QJsonObject{
                    {QStringLiteral("title"), e.title},
                    {QStringLiteral("path"), e.path},
                    {QStringLiteral("fragment"), e.fragment},
                    {QStringLiteral("depth"), e.depth},
                    {QStringLiteral("spine"), e.spine},
                });
            }
            out.insert(QStringLiteral("toc"), tocArr);

            QJsonArray pageListArr;
            for (const EpubPageTarget &p : epub.pageList()) {
                pageListArr.append(QJsonObject{
                    {QStringLiteral("label"), p.label},
                    {QStringLiteral("path"), p.path},
                    {QStringLiteral("fragment"), p.fragment},
                    {QStringLiteral("spine"), p.spine},
                });
            }
            out.insert(QStringLiteral("pageList"), pageListArr);
        } else if (session->pdf) {
            const PdfBook &pdf = *session->pdf;
            const int pages = pdf.pageCount();
            out.insert(QStringLiteral("chapterCount"), pages);
            out.insert(QStringLiteral("pageCount"), pages);
            out.insert(QStringLiteral("isFixedLayout"), true);
            out.insert(QStringLiteral("isRightToLeft"), false);
            out.insert(QStringLiteral("isDrmProtected"), false);
            out.insert(QStringLiteral("doi"), pdf.detectedDoi());
            out.insert(QStringLiteral("arxiv"), pdf.detectedArxiv());

            QJsonArray tocArr;
            for (const PdfOutlineEntry &e : pdf.outline()) {
                tocArr.append(QJsonObject{
                    {QStringLiteral("title"), e.title},
                    {QStringLiteral("page"), e.page},
                    {QStringLiteral("depth"), e.depth},
                });
            }
            out.insert(QStringLiteral("toc"), tocArr);
        }

        return out;
    });

    return allocJson(meta);
}

bool cb_session_save_position(cb_session_t *session, int chapter, double progress, const char *locator_json) {
    if (!validSession(session) || !std::isfinite(progress))
        return false;
    const QString rawLocator = locator_json ? QString::fromUtf8(locator_json).trimmed() : QString();

    return invokeSync(session->engine, [session, chapter, progress, rawLocator]() -> bool {
        Library *lib = session->engine->actor->library.get();
        if (!lib || !lib->bookRecord(session->bookId))
            return false;

        QJsonObject posObj;
        if (!rawLocator.isEmpty()) {
            const QJsonDocument parsed = QJsonDocument::fromJson(rawLocator.toUtf8());
            if (parsed.isObject())
                posObj = parsed.object();
            else
                posObj.insert(QStringLiteral("locator"), rawLocator);
        }
        if (session->format == QLatin1String("pdf")) {
            if (!posObj.contains(QStringLiteral("page")))
                posObj.insert(QStringLiteral("page"), chapter);
        }
        posObj.insert(QStringLiteral("chapter"), chapter);

        const QString serialized = QString::fromUtf8(QJsonDocument(posObj).toJson(QJsonDocument::Compact));
        lib->savePosition(session->bookId, serialized, progress);
        return true;
    });
}

char *cb_session_marks_json(cb_session_t *session) {
    if (!validSession(session))
        return nullptr;

    const QJsonObject payload = invokeSync(session->engine, [session]() -> QJsonObject {
        if (!session->marks)
            return {};
        Marks *marks = session->marks.get();
        QJsonArray arr;
        const int rows = marks->rowCount();
        for (int r = 0; r < rows; ++r) {
            const int id = marks->data(marks->index(r), Marks::IdRole).toInt();
            arr.append(markToJson(marks->find(id)));
        }
        return QJsonObject{
            {QStringLiteral("bookId"), session->bookId},
            {QStringLiteral("count"), marks->count()},
            {QStringLiteral("canUndo"), marks->canUndo()},
            {QStringLiteral("canRedo"), marks->canRedo()},
            {QStringLiteral("marks"), arr},
        };
    });

    return allocJson(payload);
}

char *cb_session_add_mark_json(cb_session_t *session, const char *mark_json) {
    if (!validSession(session) || !mark_json || mark_json[0] == '\0')
        return nullptr;

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(mark_json));
    if (!doc.isObject())
        return nullptr;
    const QJsonObject input = doc.object();

    const QJsonObject created = invokeSync(session->engine, [session, input]() -> QJsonObject {
        if (!session->marks)
            return {};
        Marks *marks = session->marks.get();
        const QString kind = input.value(QStringLiteral("kind")).toString(QStringLiteral("highlight")).trimmed().toLower();
        const int chapter = input.value(QStringLiteral("chapter")).toInt(0);
        const qint64 start = static_cast<qint64>(input.value(QStringLiteral("start")).toDouble(0));
        const qint64 end = static_cast<qint64>(input.value(QStringLiteral("end")).toDouble(0));
        const QString text = input.value(QStringLiteral("text")).toString();
        const QString color = input.value(QStringLiteral("color")).toString(QStringLiteral("yellow"));
        const QString note = input.value(QStringLiteral("note")).toString();
        const QString label = input.value(QStringLiteral("label")).toString();
        const QString prefix = input.value(QStringLiteral("prefix")).toString();
        const QString suffix = input.value(QStringLiteral("suffix")).toString();

        int id = 0;
        if (kind == QLatin1String("bookmark")) {
            id = marks->addBookmark(chapter, start, text, label, prefix, suffix);
        } else if (kind == QLatin1String("highlight")) {
            id = marks->addHighlight(chapter, start, end, text, color, label, prefix, suffix);
        } else {
            return {};
        }
        if (id <= 0)
            return {};
        if (!note.trimmed().isEmpty())
            marks->setNote(id, note);
        return markToJson(marks->find(id));
    });

    return created.isEmpty() ? nullptr : allocJson(created);
}

bool cb_session_delete_mark(cb_session_t *session, int mark_id) {
    if (!validSession(session) || mark_id <= 0)
        return false;

    return invokeSync(session->engine, [session, mark_id]() -> bool {
        if (!session->marks)
            return false;
        if (session->marks->find(mark_id).id == 0)
            return false;
        session->marks->remove(mark_id);
        return session->marks->find(mark_id).id == 0;
    });
}

char *cb_session_locate_quote_json(cb_session_t *session,
                                   int chapter,
                                   const char *quote,
                                   const char *prefix,
                                   const char *suffix,
                                   int64_t hint_start) {
    if (!validSession(session))
        return nullptr;

    const QString quoteStr = quote ? QString::fromUtf8(quote) : QString();
    const QString prefixStr = prefix ? QString::fromUtf8(prefix) : QString();
    const QString suffixStr = suffix ? QString::fromUtf8(suffix) : QString();

    const QJsonObject out = invokeSync(session->engine, [session, chapter, quoteStr, prefixStr, suffixStr, hint_start]() -> QJsonObject {
        QString chapterText;
        if (session->epub) {
            if (chapter >= 0 && chapter < session->epub->spine().size())
                chapterText = session->epub->chapterText(chapter);
        } else if (session->pdf) {
            if (chapter >= 0 && chapter < session->pdf->pageCount())
                chapterText = session->pdf->pageText(chapter);
        }

        const QPair<qint64, qint64> span = Marks::locateQuote(chapterText, quoteStr, prefixStr, suffixStr, hint_start);
        const bool found = span.first >= 0 && span.second >= span.first;
        const bool drifted = found && hint_start >= 0 && span.first != hint_start;
        const QString matched =
            found ? chapterText.mid(static_cast<qsizetype>(span.first), static_cast<qsizetype>(span.second - span.first))
                  : QString();

        return QJsonObject{
            {QStringLiteral("found"), found},
            {QStringLiteral("chapter"), chapter},
            {QStringLiteral("start"), found ? span.first : static_cast<qint64>(-1)},
            {QStringLiteral("end"), found ? span.second : static_cast<qint64>(-1)},
            {QStringLiteral("drifted"), drifted},
            {QStringLiteral("chapterLengthUtf16"), static_cast<qint64>(chapterText.size())},
            {QStringLiteral("matchedText"), matched},
        };
    });

    return allocJson(out);
}

cb_bytes_t cb_session_pdf_page_png(cb_session_t *session, int page, int width_px, int height_px) {
    if (!validSession(session) || page < 0 || (width_px <= 0 && height_px <= 0))
        return {nullptr, 0};

    const QByteArray png = invokeSync(session->engine, [session, page, width_px, height_px]() -> QByteArray {
        if (!session->pdf || page >= session->pdf->pageCount())
            return {};
        int w = width_px;
        int h = height_px;
        if (w <= 0 || h <= 0) {
            const QSizeF pts = session->pdf->pageSize(page);
            if (pts.width() <= 0.0 || pts.height() <= 0.0)
                return {};
            if (w <= 0)
                w = qMax(1, int(std::lround(double(h) * (pts.width() / pts.height()))));
            else
                h = qMax(1, int(std::lround(double(w) * (pts.height() / pts.width()))));
        }
        const QImage img = session->pdf->renderPage(page, QSize(w, h));
        if (img.isNull())
            return {};
        QByteArray out;
        QBuffer buf(&out);
        if (!buf.open(QIODevice::WriteOnly) || !img.save(&buf, "PNG"))
            return {};
        return out;
    });

    return allocBytes(png);
}

char *cb_session_pdf_text_json(cb_session_t *session, int page) {
    if (!validSession(session) || page < 0)
        return nullptr;

    const QJsonObject payload = invokeSync(session->engine, [session, page]() -> QJsonObject {
        if (!session->pdf || page >= session->pdf->pageCount())
            return {};
        const PdfBook &pdf = *session->pdf;
        const QString text = pdf.pageText(page);
        const QSizeF pts = pdf.pageSize(page);
        const QList<PdfLinkEntry> links = pdf.pageLinks(page);

        QJsonArray linksArr;
        for (const PdfLinkEntry &link : links) {
            QJsonArray rectsArr;
            for (const QRectF &r : link.rects) {
                rectsArr.append(QJsonObject{
                    {QStringLiteral("x"), r.x()},
                    {QStringLiteral("y"), r.y()},
                    {QStringLiteral("w"), r.width()},
                    {QStringLiteral("h"), r.height()},
                });
            }
            linksArr.append(QJsonObject{
                {QStringLiteral("page"), link.page},
                {QStringLiteral("destY"), link.destY},
                {QStringLiteral("url"), link.url.isValid() ? link.url.toString() : QString()},
                {QStringLiteral("rects"), rectsArr},
            });
        }

        return QJsonObject{
            {QStringLiteral("page"), page},
            {QStringLiteral("label"), pdf.pageLabel(page)},
            {QStringLiteral("widthPt"), pts.width()},
            {QStringLiteral("heightPt"), pts.height()},
            {QStringLiteral("text"), text},
            {QStringLiteral("lengthUtf16"), static_cast<qint64>(text.size())},
            {QStringLiteral("speakable"), PdfBook::speakable(text, pdf.title())},
            {QStringLiteral("links"), linksArr},
        };
    });

    return payload.isEmpty() ? nullptr : allocJson(payload);
}

}  // extern "C"
