#include "mpris.h"

#include "library.h"
#include "narration.h"
#include "reader.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QUrl>

namespace {
const QString path = QStringLiteral("/org/mpris/MediaPlayer2");
const QString playerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
}

Mpris::Mpris(Narration *narration, Reader *reader, Library *library, QObject *parent)
    : QObject(parent), m_narration(narration), m_reader(reader), m_library(library) {
    new MprisRoot(this);
    new MprisPlayer(this);
    connect(narration, &Narration::stateChanged, this, &Mpris::update);
    connect(narration, &Narration::positionChanged, this, &Mpris::update);
    connect(narration, &Narration::rateChanged, this, &Mpris::update);
    connect(reader, &Reader::bookChanged, this, &Mpris::update);
}

Mpris::~Mpris() {
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(path);
        QDBusConnection::sessionBus().unregisterService(m_service);
    }
}

QString Mpris::playbackStatus() const {
    if (!m_narration->active())
        return QStringLiteral("Stopped");
    return m_narration->playing() ? QStringLiteral("Playing") : QStringLiteral("Paused");
}

QVariantMap Mpris::metadata() const {
    if (!m_narration->active())
        return {};
    const int chapter = m_narration->chapter();
    QVariantMap map;
    map["mpris:trackid"] = QVariant::fromValue(
        QDBusObjectPath(QStringLiteral("/org/dogear/book%1/chapter%2").arg(m_reader->bookId()).arg(chapter)));
    const QString title = m_reader->titleOfChapter(chapter);
    map["xesam:title"] = title.isEmpty() ? m_reader->title() : title;
    map["xesam:album"] = m_reader->title();
    if (!m_reader->author().isEmpty())
        map["xesam:artist"] = QStringList{m_reader->author()};
    // The cover, without the cache-busting query the window uses.
    const QString cover = m_library->coverUrl(m_reader->bookId()).section('?', 0, 0);
    if (!cover.isEmpty())
        map["mpris:artUrl"] = cover;
    return map;
}

// Dogear is a player while it reads aloud, and gone from the desktop's list
// of players otherwise.
void Mpris::update() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    if (m_narration->active() && !m_registered) {
        bus.registerObject(path, this, QDBusConnection::ExportAdaptors);
        m_service = QStringLiteral("org.mpris.MediaPlayer2.dogear");
        // A second Dogear takes a name of its own.
        if (!bus.registerService(m_service)) {
            m_service += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
            bus.registerService(m_service);
        }
        m_registered = true;
    } else if (!m_narration->active() && m_registered) {
        bus.unregisterService(m_service);
        bus.unregisterObject(path);
        m_registered = false;
        m_status.clear();
        m_metadata.clear();
        return;
    }
    if (!m_registered)
        return;
    QVariantMap changed;
    const QString status = playbackStatus();
    if (status != m_status) {
        m_status = status;
        changed["PlaybackStatus"] = status;
    }
    const QVariantMap now = metadata();
    if (now != m_metadata) {
        m_metadata = now;
        changed["Metadata"] = now;
    }
    if (!qFuzzyCompare(m_narration->rate(), m_rate)) {
        m_rate = m_narration->rate();
        changed["Rate"] = m_rate;
    }
    if (!changed.isEmpty())
        announce(changed);
}

void Mpris::announce(const QVariantMap &changed) {
    QDBusMessage signal =
        QDBusMessage::createSignal(path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
    signal << playerInterface << changed << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

MprisRoot::MprisRoot(Mpris *mpris) : QDBusAbstractAdaptor(mpris), m_mpris(mpris) {}

void MprisRoot::Raise() { emit m_mpris->raiseRequested(); }

MprisPlayer::MprisPlayer(Mpris *mpris) : QDBusAbstractAdaptor(mpris), m_mpris(mpris) {}

double MprisPlayer::rate() const { return m_mpris->narration()->rate(); }
void MprisPlayer::setRate(double rate) { m_mpris->narration()->setRate(rate); }
void MprisPlayer::Next() { m_mpris->narration()->nextParagraph(); }
void MprisPlayer::Previous() { m_mpris->narration()->previousParagraph(); }
void MprisPlayer::Pause() { m_mpris->narration()->pause(); }
void MprisPlayer::PlayPause() { m_mpris->narration()->toggle(); }
void MprisPlayer::Stop() { m_mpris->narration()->stop(); }
void MprisPlayer::Play() { m_mpris->narration()->resume(); }
