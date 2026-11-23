// From Omacut (MIT, David Heinemeier Hansson): the portal request and its
// response handling use async D-Bus calls without blocking Introspect.
#include "app/portalfilepicker.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace {
struct PortalFilterRule {
    uint type;
    QString pattern;
};

using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};

using PortalFileFilters = QList<PortalFileFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule) {
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule) {
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter) {
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

PortalFileFilter bookFilter() {
    return {
        QCoreApplication::translate("PortalFilePicker", "Books"),
        {
            {1, QStringLiteral("application/epub+zip")},
            {1, QStringLiteral("application/pdf")},
            {0, QStringLiteral("*.epub")},
            {0, QStringLiteral("*.kepub")},
            {0, QStringLiteral("*.pdf")},
        },
    };
}

PortalFileFilters bookFilters() {
    return {
        bookFilter(),
        {QCoreApplication::translate("PortalFilePicker", "All files"), {{0, QStringLiteral("*")}}},
    };
}

QString portalToken() {
    return QStringLiteral("dogear_%1").arg(QRandomGenerator::global()->generate());
}

QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

QString openFolder() {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(documents).exists() ? documents : QDir::homePath();
}
}  // namespace

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) {
    registerPortalFilterTypes();
}

PortalFilePicker::~PortalFilePicker() {
    closePendingRequest();
    clearPending();
}

void PortalFilePicker::chooseFolders() {
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Add"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), true);
    options.insert(QStringLiteral("directory"), true);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(openFolder()));
    requestFile(QStringLiteral("OpenFile"), tr("Add Folders of Books"), options, Action::Open);
}

void PortalFilePicker::chooseBooks() {
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Add"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), true);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(openFolder()));
    options.insert(QStringLiteral("filters"), QVariant::fromValue(bookFilters()));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(bookFilter()));
    requestFile(QStringLiteral("OpenFile"), tr("Add Books"), options, Action::Open);
}

bool PortalFilePicker::connectToRequestPath(const QString &path) {
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
        this, SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFilePicker::closePendingRequest() {
    if (m_pendingPath.isEmpty())
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Close"));
    bus.asyncCall(message);
}

void PortalFilePicker::requestFile(const QString &method, const QString &title,
                                   QVariantMap options, Action action) {
    // A request whose Response never arrives (the portal crashed or was
    // killed with the dialog up) must not wedge every later dialog behind
    // it: close the stuck one best-effort and replace it.
    if (m_pendingAction != Action::None) {
        closePendingRequest();
        clearPending();
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(tr("The XDG desktop portal file chooser is not available."));
        return;
    }

    // Subscribe to the Response signal at the request path the portal will
    // derive from our handle_token *before* making the call, so a response
    // can't slip past while our match rule is still being installed.
    const QString token = portalToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pendingAction = action;
    if (!connectToRequestPath(predictedPath)) {
        clearPending();
        emit failed(tr("Could not listen for the portal file picker response."));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"),
        method);
    message << QString() << title << options;

    const int generation = ++m_pendingGeneration;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    m_pendingWatcher = watcher;

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingWatcher == watcher)
            m_pendingWatcher = nullptr;

        // Superseded by a newer request, or a response may already have
        // been handled while the reply was in flight.
        if (generation != m_pendingGeneration || m_pendingAction == Action::None)
            return;

        if (reply.isError()) {
            clearPending();
            emit failed(tr("The portal file picker failed: %1")
                            .arg(reply.error().message()));
            return;
        }

        // Old portal versions can hand back a different request path than the
        // handle_token predicts; move our subscription over if so.
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingPath) {
            QDBusConnection::sessionBus().disconnect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleResponse(uint,QVariantMap)));
            if (!connectToRequestPath(actualPath)) {
                clearPending();
                emit failed(tr("Could not listen for the portal file picker response."));
            }
        }
    });
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    const Action action = m_pendingAction;
    clearPending();

    if (response == 1)
        return;
    if (response != 0) {
        emit failed(tr("The portal file picker could not complete the request."));
        return;
    }

    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty() || action != Action::Open)
        return;
    QList<QUrl> urls;
    urls.reserve(uris.size());
    for (const QString &uri : uris) {
        QUrl url = QUrl::fromEncoded(uri.toUtf8(), QUrl::StrictMode);
        if (!url.isValid())
            url = QUrl(uri);
        urls.append(url);
    }
    emit chosen(urls);
}

void PortalFilePicker::clearPending() {
    // A caller that never gets its reply must not outlive the request it
    // belonged to (repeated stuck requests would otherwise pile them up).
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    if (!m_pendingPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleResponse(uint,QVariantMap)));
    }

    m_pendingPath.clear();
    m_pendingAction = Action::None;
}
