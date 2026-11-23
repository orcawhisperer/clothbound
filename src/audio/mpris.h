#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class Library;
class Narration;
class Reader;

// The book being read aloud, as a media player the desktop knows: media
// keys, Omarchy's bar, and anything else that speaks MPRIS see the chapter,
// the book, its author and cover, and can play, pause, and move a paragraph
// either way. Dogear is a player only while it's reading aloud.
class Mpris : public QObject {
    Q_OBJECT

public:
    Mpris(Narration *narration, Reader *reader, Library *library, QObject *parent = nullptr);
    ~Mpris() override;

    QString playbackStatus() const;
    QVariantMap metadata() const;
    Narration *narration() const { return m_narration; }

signals:
    // The window is wanted in front.
    void raiseRequested();

private:
    void update();
    void announce(const QVariantMap &changed);

    Narration *m_narration;
    Reader *m_reader;
    Library *m_library;
    QString m_service;
    bool m_registered = false;
    QString m_status;
    QVariantMap m_metadata;
    double m_rate = 1.0;
    double m_volume = 1.0;
};

class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ no)
    Q_PROPERTY(bool CanRaise READ yes)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none)

public:
    explicit MprisRoot(Mpris *mpris);
    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return QStringLiteral("Dogear"); }
    QString desktopEntry() const { return QStringLiteral("dogear"); }
    QStringList none() const { return {}; }

public slots:
    void Raise();
    void Quit() {}

private:
    Mpris *m_mpris;
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ yes)
    Q_PROPERTY(bool CanGoPrevious READ yes)
    Q_PROPERTY(bool CanPlay READ yes)
    Q_PROPERTY(bool CanPause READ yes)
    Q_PROPERTY(bool CanSeek READ yes)
    Q_PROPERTY(bool CanControl READ yes)

public:
    explicit MprisPlayer(Mpris *mpris);
    QString playbackStatus() const { return m_mpris->playbackStatus(); }
    double rate() const;
    void setRate(double rate);
    QVariantMap metadata() const { return m_mpris->metadata(); }
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const;
    double minimumRate() const { return 0.75; }
    double maximumRate() const { return 2.0; }
    bool yes() const { return true; }
    bool no() const { return false; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetUs);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs);
    void OpenUri(const QString &) {}

signals:
    void Seeked(qlonglong position);

private:
    Mpris *m_mpris;
};
