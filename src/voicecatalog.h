#pragma once

#include "ai.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QVariantList>

class AiServices;
class AudioCache;
class Reader;
class SpeechPlayer;

// The voices a book can be read in: the provider's library (fetched, then
// kept for a day), the voices the reader has designed, and a preview of any
// of them reading the book's own words. Casting proposes a narrator for the
// open book and designs it.
class VoiceCatalog : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString previewing READ previewing NOTIFY previewChanged)
    Q_PROPERTY(bool previewBuffering READ previewBuffering NOTIFY previewChanged)
    // "" (not begun), "thinking", "proposed", "designing", "designed", or "failed".
    Q_PROPERTY(QString casting READ casting NOTIFY castingChanged)
    Q_PROPERTY(QVariantMap proposal READ proposal NOTIFY castingChanged)
    Q_PROPERTY(QVariantMap designed READ designed NOTIFY castingChanged)
    Q_PROPERTY(QString castingProblem READ castingProblem NOTIFY castingChanged)
    // The voices of the Studio model server on this computer, as maps.
    Q_PROPERTY(QVariantList studioVoices READ studioVoices NOTIFY studioChanged)

public:
    VoiceCatalog(AiServices *ai, AudioCache *cache, Reader *reader, const QString &databaseConnection,
                 const QString &cacheFile, QObject *parent = nullptr);
    ~VoiceCatalog() override;

    bool loading() const { return m_loading; }
    QString problem() const { return m_problem; }
    int count() const { return int(m_voices.size()); }
    QString previewing() const { return m_previewing; }
    bool previewBuffering() const { return m_previewBuffering; }
    QString casting() const { return m_casting; }
    QVariantMap proposal() const { return m_proposal; }
    QVariantMap designed() const { return m_designed; }
    QString castingProblem() const { return m_castingProblem; }

    // Fetches the provider's voices when the kept list is a day old, or
    // always when forced.
    Q_INVOKABLE void refresh(bool force = false);
    // Voices to offer for a book in `language` ("en", "en-GB"): narrators
    // who speak it, the best suited first, then the classic voices, which
    // speak every language.
    Q_INVOKABLE QVariantList suggestions(const QString &language) const;
    // The voices the reader has designed, newest first.
    Q_INVOKABLE QVariantList designedVoices() const;
    // Every voice matching all the words of `text`; narrators first. With a
    // language, only voices that speak it.
    Q_INVOKABLE QVariantList search(const QString &text, const QString &language, int limit = 80) const;
    Q_INVOKABLE QVariantMap voice(const QString &id) const;

    // A voice reading `text`: from the cache when it's been heard before.
    Q_INVOKABLE void preview(const QString &voiceId, const QString &text);
    Q_INVOKABLE void stopPreview();

    // Proposes a narrator for the open book from its details and a passage.
    Q_INVOKABLE void cast(const QString &passage);
    // Designs the proposed (or edited) narrator; it takes about half a
    // minute, and the new voice's sample plays when it's ready.
    Q_INVOKABLE void design(const QVariantMap &proposal);
    Q_INVOKABLE void playSample();
    // Back to the proposal, to change the description and design again.
    Q_INVOKABLE void revise();
    Q_INVOKABLE void resetCasting();
    // Deletes a designed voice from the provider, for good.
    Q_INVOKABLE void forget(const QString &voiceId);
    QVariantList studioVoices() const;
    // Asks the Studio server which voices it has.
    Q_INVOKABLE void loadStudioVoices();

    void setVoices(const QList<Ai::Voice> &voices);
    static QVariantMap toMap(const Ai::Voice &voice);
    static QString castingPrompt();

signals:
    void changed();
    void previewChanged();
    void castingChanged();
    void studioChanged();

private:
    void load();
    void save() const;
    void remember(const Ai::Voice &voice, const Ai::VoiceDesign &design);
    void setProblem(const QString &problem);
    void setCasting(const QString &state, const QString &problem = {});
    int startPreview(const QString &voiceId);

    AiServices *m_ai;
    AudioCache *m_cache;
    Reader *m_reader;
    QString m_connection;
    QString m_cacheFile;
    QList<Ai::Voice> m_voices;
    QList<Ai::Voice> m_studioVoices;
    QDateTime m_fetched;
    bool m_loading = false;
    QString m_problem;

    SpeechPlayer *m_player;
    QString m_previewing;
    bool m_previewBuffering = false;
    QPointer<Ai::Reply> m_previewReply;
    int m_previewTrack = 0;

    QString m_casting;
    QVariantMap m_proposal;
    QVariantMap m_designed;
    QString m_castingProblem;
    QStringList m_proposed;  // descriptions already offered, so another differs
    QByteArray m_sample;
    int m_sampleRate = 24000;
    QPointer<Ai::Reply> m_castingReply;
};
