#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <optional>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

// Built-in voices as packages: the speech engine for this computer, Kokoro
// (49 voices in 8 languages) and Piper (173 voices in 42), from a pinned
// list compiled into Dogear. Nothing is downloaded until asked. A download
// picks up where it stopped, is checked against its published checksum, and
// is unpacked aside, then moved into place.
class VoicePacks : public QObject {
    Q_OBJECT
    // This computer's processor has an engine to download.
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool engineInstalled READ engineInstalled NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY progressChanged)
    // What's downloading or unpacking, as the reader would call it.
    Q_PROPERTY(QString installing READ installing NOTIFY progressChanged)
    // Of everything asked for, from 0 to 1.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(double megabytes READ megabytes NOTIFY changed)

public:
    struct Pack {
        QString id;    // "engine", "kokoro-v1.0", or "piper:de_DE-thorsten-high"
        QString kind;  // "engine", "kokoro", or "piper"
        QString title;
        QUrl url;
        qint64 size = 0;
        QByteArray sha256;  // hex
        QString dir;        // the folder the archive unpacks to
        QString license;
    };
    // What the engine needs to speak in a voice.
    struct Model {
        QString kind;  // "kokoro" or "piper"
        // Voices with the same key share one loaded model.
        QString key;
        QString model;
        QString voices;
        QString tokens;
        QString dataDir;
        QString lexicon;
        QString lang;
        QString dictDir;
        int speaker = 0;
        // Pocket TTS: its folder, and the recording a voice speaks like.
        QString dir;
        QString reference;
    };

    explicit VoicePacks(const QString &directory, QObject *parent = nullptr, const QString &architecture = {},
                        const QByteArray &manifest = {});
    ~VoicePacks() override;

    static QString currentArchitecture();
    bool supported() const { return !m_engine.url.isEmpty(); }
    bool engineInstalled() const { return isInstalled(m_engine.id); }
    bool busy() const { return m_download || m_unpacking || !m_queue.isEmpty(); }
    QString installing() const { return m_currentTitle; }
    double progress() const;
    QString problem() const { return m_problem; }
    double megabytes() const { return m_megabytes; }
    QString directory() const { return m_directory; }

    // Built-in voices that speak a language ("en-GB", "de"; "" for all),
    // installed or not: the ones installed, then the recommended, first.
    Q_INVOKABLE QVariantList voices(const QString &language) const;
    // Fast search & filter across built-in voices (for virtualized VoicePanel).
    Q_INVOKABLE QVariantList searchVoices(const QString &query, const QString &language = {},
                                          bool installedOnly = false) const;
    // Unique languages offered by built-in packs: {code, name, count, installed}.
    Q_INVOKABLE QVariantList languages() const;
    Q_INVOKABLE QVariantMap voice(const QString &id) const;
    // The best voice to download for a book in `language`, with what it
    // takes: {voice, bytes, titles}; empty when no built-in voice speaks it.
    Q_INVOKABLE QVariantMap offer(const QString &language) const;
    // Downloads what a voice needs, its engine too when it's missing.
    Q_INVOKABLE void installVoice(const QString &voiceId);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void remove(const QString &packId);
    // {id, title, megabytes, license} for each pack on this computer.
    Q_INVOKABLE QVariantList installedPacks() const;

    bool isInstalled(const QString &packId) const;
    // The engine's C API library, when it's installed.
    QString engineLibrary() const;
    // How to speak in a voice, when it and the engine are installed.
    std::optional<Model> model(const QString &voiceId) const;
    std::optional<Pack> pack(const QString &packId) const;

signals:
    void changed();
    void progressChanged();
    // A voice can speak now.
    void ready(const QString &voiceId);
    void failed(const QString &message);
    // Emitted right before a pack directory is removed so LocalSpeech can unload it.
    void aboutToRemove(const QString &packId);

private:
    struct Voice {
        QString id;  // "builtin:kokoro:af_heart"
        QString pack;
        QString name;
        QString language;  // "en-US"
        QString gender;
        QString grade;     // Kokoro's own grade
        QString quality;   // Piper's
        bool recommended = false;
        int speaker = 0;
        QString model;  // Piper's model file
        QString reference;  // Pocket's: the recording it speaks like
    };
    struct LanguageSetup {
        QString lexicon;
        QString lang;
        QString dict;
    };

    void load(const QByteArray &manifest);
    const Voice *findVoice(const QString &id) const;
    QVariantMap toMap(const Voice &voice) const;
    void next();
    void download(const Pack &pack);
    void verifyAndUnpack(const Pack &pack, const QString &archive);
    void fail(const QString &message);
    QString archivePath(const Pack &pack) const;

    QString m_directory;
    QString m_architecture;
    Pack m_engine;
    QHash<QString, Pack> m_packs;
    QList<Voice> m_voices;
    QHash<QString, LanguageSetup> m_kokoroLanguages;
    QString m_kokoroModel;

    QNetworkAccessManager *m_network;
    QList<Pack> m_queue;
    QString m_waitingVoice;
    QPointer<QNetworkReply> m_download;
    QPointer<QProcess> m_tar;
    QFile *m_part = nullptr;
    bool m_unpacking = false;
    quint64 m_generation = 0;
    QString m_currentTitle;
    qint64 m_batchBytes = 0;
    qint64 m_doneBytes = 0;
    qint64 m_currentBytes = 0;
    QString m_problem;
    double m_megabytes = 0;
};
