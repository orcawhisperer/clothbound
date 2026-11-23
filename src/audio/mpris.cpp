#include "audio/mpris.h"

#include "library/library.h"
#include "audio/narration.h"
#include "reader/reader.h"

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
    auto *player = new MprisPlayer(this);
    connect(narration, &Narration::stateChanged, this, &Mpris::update);
    connect(narration, &Narration::positionChanged, this, &Mpris::update);
    connect(narration, &Narration::rateChanged, this, &Mpris::update);
    connect(narration, &Narration::volumeChanged, this, &Mpris::update);
    connect(narration, &Narration::seeked, this, [this, player](qint64 positionUs) {
        if (m_registered)
            emit player->Seeked(qlonglong(positionUs));
    });
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
    const int book = qMax(0, m_reader->bookId());
    const int chapter = qMax(0, m_narration->chapter());
    QVariantMap map;
    map["mpris:trackid"] = QVariant::fromValue(
        QDBusObjectPath(QStringLiteral("/app/clothbound/book%1/chapter%2").arg(book).arg(chapter)));
    const qint64 length = m_narration->durationUs();
    if (length > 0)
        map["mpris:length"] = QVariant::fromValue(qlonglong(length));
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

// Clothbound is a player while it reads aloud, and gone from the desktop's list
// of players otherwise.
void Mpris::update() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    if (m_narration->active() && !m_registered) {
        if (!bus.registerObject(path, this, QDBusConnection::ExportAdaptors))
            return;
        m_service = QStringLiteral("org.mpris.MediaPlayer2.clothbound");
        // A second Clothbound takes a name of its own.
        bool registered = bus.registerService(m_service);
        if (!registered) {
            m_service += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
            registered = bus.registerService(m_service);
        }
        if (!registered) {
            bus.unregisterObject(path);
            m_service.clear();
            return;
        }
        m_registered = true;
    } else if (!m_narration->active() && m_registered) {
        bus.unregisterService(m_service);
        bus.unregisterObject(path);
        m_registered = false;
        m_service.clear();
        m_status.clear();
        m_metadata.clear();
        m_rate = 1.0;
        m_volume = 1.0;
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
    if (!qFuzzyCompare(m_narration->volume(), m_volume)) {
        m_volume = m_narration->volume();
        changed["Volume"] = m_volume;
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
void MprisPlayer::setRate(double rate) {
    // Per MPRIS2 spec, setting Rate to 0 pauses playback.
    if (rate <= 0.0)
        m_mpris->narration()->pause();
    else
        m_mpris->narration()->setRate(rate);
}
double MprisPlayer::volume() const { return m_mpris->narration()->volume(); }
void MprisPlayer::setVolume(double volume) { m_mpris->narration()->setVolume(volume); }
qlonglong MprisPlayer::position() const { return qlonglong(m_mpris->narration()->positionUs()); }
void MprisPlayer::Next() { m_mpris->narration()->nextParagraph(); }
void MprisPlayer::Previous() { m_mpris->narration()->previousParagraph(); }
void MprisPlayer::Pause() { m_mpris->narration()->pause(); }
void MprisPlayer::PlayPause() { m_mpris->narration()->toggle(); }
void MprisPlayer::Stop() { m_mpris->narration()->stop(); }
void MprisPlayer::Play() { m_mpris->narration()->resume(); }
void MprisPlayer::Seek(qlonglong offsetUs) { m_mpris->narration()->seekByUs(qint64(offsetUs)); }
void MprisPlayer::SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs) {
    const QDBusObjectPath current = m_mpris->metadata().value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>();
    if (current.path().isEmpty() || trackId.path() != current.path())
        return;
    m_mpris->narration()->seekToUs(qint64(positionUs));
}
