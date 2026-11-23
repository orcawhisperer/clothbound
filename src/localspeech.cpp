#include "localspeech.h"
#include "gemini.h"
#include <QFile>

#include "sherpaonnx.h"
#include "speech.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <cstring>

namespace {
constexpr int outputRate = 24000;

QSettings settings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

// Speech made here: audio comes a sentence at a time, each sentence's end
// noted, and stopping is immediate.
class LocalReply : public Ai::Reply {
public:
    LocalReply(QObject *parent, std::shared_ptr<std::atomic_bool> stop, const QString &feature)
        : Ai::Reply(parent), m_stop(std::move(stop)) {
        m_model = QStringLiteral("sherpa-onnx-") + QLatin1String(SherpaOnnx::version);
        m_feature = feature;
        m_sampleRate = outputRate;
    }
    ~LocalReply() override { *m_stop = true; }

    void abort() override {
        *m_stop = true;
        finish({Ai::Error::Cancelled, QCoreApplication::translate("LocalSpeech", "Cancelled.")});
    }
    void deliver(const QByteArray &pcm, qint64 boundary) {
        if (isFinished())
            return;
        addAudio(pcm);
        m_boundaries.append(boundary);
    }
    void complete(Ai::Error::Kind kind = Ai::Error::None, const QString &problem = {}) { finish({kind, problem}); }
    void setVoices(const QList<Ai::Voice> &voices) { m_voices = voices; }

private:
    std::shared_ptr<std::atomic_bool> m_stop;
};

struct Job {
    QPointer<LocalReply> reply;  // used on the main thread only
    QPointer<LocalSpeech> owner;
    std::shared_ptr<std::atomic_bool> stop;
    VoicePacks::Model model;
    QString library;
    QStringList sentences;
};

// Anything for the main thread goes there, and only if the reply still wants it.
template <typename Fn>
void post(const Job &job, Fn fn) {
    QMetaObject::invokeMethod(
        qApp,
        [reply = job.reply, fn] {
            if (reply)
                fn(reply.data());
        },
        Qt::QueuedConnection);
}
}

// Lives on the speaking thread, with the engine.
class LocalWorker : public QObject {
public:
    explicit LocalWorker(LocalSpeech::Engines engines) : m_engines(std::move(engines)) {}

    void run(const Job &job) {
        if (*job.stop)
            return;
        if (!m_engine || m_library != job.library) {
            m_engine = m_engines(job.library);
            m_library = job.library;
        }
        const QString problem = m_engine->prepare(job.model);
        if (!problem.isEmpty()) {
            post(job, [problem](LocalReply *reply) { reply->complete(Ai::Error::Unsupported, problem); });
            return;
        }
        qint64 total = 0;
        for (int i = 0; i < job.sentences.size(); ++i) {
            if (*job.stop)
                return;
            QElapsedTimer clock;
            clock.start();
            QVector<float> samples = m_engine->speak(job.sentences[i], job.model.speaker, *job.stop);
            if (*job.stop)
                return;
            const double seconds = samples.size() / double(qMax(1, m_engine->sampleRate()));
            const qint64 ms = clock.elapsed();
            if (seconds > 0.5 && ms > 0) {
                const double speed = seconds / (ms / 1000.0);
                QMetaObject::invokeMethod(
                    qApp,
                    [owner = job.owner, key = job.model.key, speed] {
                        if (owner)
                            owner->noteSpeed(key, speed);
                    },
                    Qt::QueuedConnection);
            }
            samples = Speech::resample(samples, m_engine->sampleRate(), outputRate);
            // A beat after each sentence, and a longer one at the end, where
            // a paragraph ends.
            samples.resize(samples.size() + qsizetype((i + 1 < job.sentences.size() ? 0.14 : 0.32) * outputRate));
            total += samples.size();
            const QByteArray pcm = Speech::pcm16(samples);
            post(job, [pcm, total](LocalReply *reply) { reply->deliver(pcm, total); });
        }
        post(job, [](LocalReply *reply) { reply->complete(); });
    }

private:
    LocalSpeech::Engines m_engines;
    std::unique_ptr<TtsEngine> m_engine;
    QString m_library;
};

SherpaEngine::SherpaEngine(const QString &library) : m_library(library) {}

int SherpaEngine::threads() {
    // DOGEAR_TTS_THREADS overrides, for measuring.
    const int asked = qEnvironmentVariableIntValue("DOGEAR_TTS_THREADS");
    if (asked > 0)
        return asked;
    // Half the cores, at most four: more makes the engine no faster.
    return qBound(1, QThread::idealThreadCount() / 2, 4);
}

SherpaEngine::~SherpaEngine() {
    if (m_tts && m_destroy)
        reinterpret_cast<SherpaOnnx::DestroyOfflineTts>(m_destroy)(m_tts);
}

QString SherpaEngine::load() {
    if (m_loaded)
        return {};
    if (!m_library.load())
        return QCoreApplication::translate("LocalSpeech", "The speech engine couldn't be loaded: %1").arg(m_library.errorString());
    // The declarations Dogear uses are this version's; another one is refused
    // rather than trusted.
    const auto version = reinterpret_cast<SherpaOnnx::VersionString>(m_library.resolve("SherpaOnnxGetVersionStr"));
    if (!version || QLatin1String(version()) != QLatin1String(SherpaOnnx::version))
        return QCoreApplication::translate("LocalSpeech", "The speech engine is a version Dogear doesn't know (%1). Download the built-in voices again.")
            .arg(version ? QString::fromLatin1(version()) : QStringLiteral("?"));
    m_create = reinterpret_cast<void *>(m_library.resolve("SherpaOnnxCreateOfflineTts"));
    m_destroy = reinterpret_cast<void *>(m_library.resolve("SherpaOnnxDestroyOfflineTts"));
    m_generate = reinterpret_cast<void *>(m_library.resolve("SherpaOnnxOfflineTtsGenerateWithConfig"));
    m_free = reinterpret_cast<void *>(m_library.resolve("SherpaOnnxDestroyOfflineTtsGeneratedAudio"));
    m_rateOf = reinterpret_cast<void *>(m_library.resolve("SherpaOnnxOfflineTtsSampleRate"));
    if (!m_create || !m_destroy || !m_generate || !m_free || !m_rateOf)
        return QCoreApplication::translate("LocalSpeech", "The speech engine is incomplete. Download the built-in voices again.");
    m_loaded = true;
    return {};
}

QString SherpaEngine::prepare(const VoicePacks::Model &model) {
    if (const QString problem = load(); !problem.isEmpty())
        return problem;
    // Pocket voices share one model and differ by their recording.
    if (model.kind == QStringLiteral("pocket") && model.reference != m_referencePath) {
        QFile wav(model.reference);
        int rate = 24000;
        const QByteArray pcm = wav.open(QIODevice::ReadOnly) ? GeminiProvider::pcmFromWav(wav.readAll(), &rate) : QByteArray();
        if (pcm.isEmpty())
            return QCoreApplication::translate("LocalSpeech", "The voice couldn't be loaded; it may need downloading again.");
        m_reference.resize(pcm.size() / 2);
        const auto *data = reinterpret_cast<const qint16 *>(pcm.constData());
        for (qsizetype i = 0; i < m_reference.size(); ++i)
            m_reference[i] = data[i] / 32768.0f;
        m_referenceRate = rate;
        m_referencePath = model.reference;
    } else if (model.kind != QStringLiteral("pocket")) {
        m_reference.clear();
        m_referencePath.clear();
    }
    if (m_tts && m_key == model.key)
        return {};
    if (m_tts) {
        reinterpret_cast<SherpaOnnx::DestroyOfflineTts>(m_destroy)(m_tts);
        m_tts = nullptr;
    }
    // The strings live until the engine has read them.
    const QByteArray file = model.model.toUtf8(), voices = model.voices.toUtf8(), tokens = model.tokens.toUtf8(),
                     data = model.dataDir.toUtf8(), lexicon = model.lexicon.toUtf8(), lang = model.lang.toUtf8(),
                     dict = model.dictDir.toUtf8();
    const auto in = [&model](const char *name) { return (model.dir + QLatin1Char('/') + QLatin1String(name)).toUtf8(); };
    const QByteArray lmFlow = in("lm_flow.int8.onnx"), lmMain = in("lm_main.int8.onnx"), encoder = in("encoder.onnx"),
                     decoder = in("decoder.int8.onnx"), conditioner = in("text_conditioner.onnx"),
                     vocab = in("vocab.json"), scores = in("token_scores.json");
    SherpaOnnx::OfflineTtsConfig config;
    std::memset(&config, 0, sizeof config);
    if (model.kind == QStringLiteral("pocket")) {
        config.model.pocket.lm_flow = lmFlow.constData();
        config.model.pocket.lm_main = lmMain.constData();
        config.model.pocket.encoder = encoder.constData();
        config.model.pocket.decoder = decoder.constData();
        config.model.pocket.text_conditioner = conditioner.constData();
        config.model.pocket.vocab_json = vocab.constData();
        config.model.pocket.token_scores_json = scores.constData();
        config.model.pocket.voice_embedding_cache_capacity = 8;
    } else if (model.kind == QStringLiteral("kokoro")) {
        config.model.kokoro.model = file.constData();
        config.model.kokoro.voices = voices.constData();
        config.model.kokoro.tokens = tokens.constData();
        config.model.kokoro.data_dir = data.constData();
        config.model.kokoro.length_scale = 1.0f;
        config.model.kokoro.dict_dir = dict.constData();
        config.model.kokoro.lexicon = lexicon.constData();
        config.model.kokoro.lang = lang.constData();
    } else {
        config.model.vits.model = file.constData();
        config.model.vits.tokens = tokens.constData();
        config.model.vits.data_dir = data.constData();
        config.model.vits.noise_scale = 0.667f;
        config.model.vits.noise_scale_w = 0.8f;
        config.model.vits.length_scale = 1.0f;
    }
    config.model.num_threads = threads();
    config.model.provider = "cpu";
    config.max_num_sentences = 1;
    config.silence_scale = 0.2f;
    m_tts = reinterpret_cast<SherpaOnnx::CreateOfflineTts>(m_create)(&config);
    if (!m_tts)
        return QCoreApplication::translate("LocalSpeech", "The voice couldn't be loaded; it may need downloading again.");
    m_rate = reinterpret_cast<SherpaOnnx::SampleRate>(m_rateOf)(m_tts);
    m_key = model.key;
    return {};
}

QVector<float> SherpaEngine::speak(const QString &text, int speaker, const std::atomic_bool &stop) {
    if (!m_tts)
        return {};
    SherpaOnnx::GenerationConfig config;
    std::memset(&config, 0, sizeof config);
    config.silence_scale = 0.2f;
    config.speed = 1.0f;
    config.sid = speaker;
    if (!m_reference.isEmpty()) {
        config.reference_audio = m_reference.constData();
        config.reference_audio_len = int32_t(m_reference.size());
        config.reference_sample_rate = m_referenceRate;
    }
    const QByteArray utf8 = text.toUtf8();
    // Asked between pieces of work: stop when the listener has moved on.
    SherpaOnnx::ProgressCallback keepGoing = [](const float *, int32_t, float, void *arg) -> int32_t {
        return static_cast<const std::atomic_bool *>(arg)->load() ? 0 : 1;
    };
    const SherpaOnnx::GeneratedAudio *audio = reinterpret_cast<SherpaOnnx::GenerateWithConfig>(m_generate)(
        m_tts, utf8.constData(), &config, keepGoing, const_cast<std::atomic_bool *>(&stop));
    QVector<float> samples;
    if (audio) {
        if (audio->samples && audio->n > 0)
            samples = QVector<float>(audio->samples, audio->samples + audio->n);
        if (audio->sample_rate > 0)
            m_rate = audio->sample_rate;
        reinterpret_cast<SherpaOnnx::DestroyGeneratedAudio>(m_free)(audio);
    }
    return samples;
}

LocalSpeech::LocalSpeech(VoicePacks *packs, QObject *parent, Engines engines) : Ai::Provider(parent), m_packs(packs) {
    if (!engines)
        engines = [](const QString &library) { return std::make_unique<SherpaEngine>(library); };
    m_worker = new LocalWorker(std::move(engines));
    m_worker->moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("Built-in voices"));
    m_thread.start();
    QSettings s = settings();
    s.beginGroup(QStringLiteral("builtinSpeed"));
    for (const QString &key : s.childKeys())
        m_speeds.insert(QString(key).replace('|', ':'), s.value(key).toDouble());
}

LocalSpeech::~LocalSpeech() {
    // Replies (our children) stop their work as they go.
    qDeleteAll(findChildren<Ai::Reply *>());
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
}

QString LocalSpeech::speechModelName(bool) const { return QStringLiteral("sherpa-onnx-") + QLatin1String(SherpaOnnx::version); }

Ai::Reply *LocalSpeech::check() {
    auto *reply = new LocalReply(this, std::make_shared<std::atomic_bool>(false), QStringLiteral("check"));
    const bool ready = m_packs->engineInstalled();
    QTimer::singleShot(0, reply, [reply, ready] {
        if (ready)
            reply->complete();
        else
            reply->complete(Ai::Error::Unsupported, tr("Download the built-in voices first."));
    });
    return reply;
}

Ai::Reply *LocalSpeech::synthesize(const Ai::SpeechRequest &request) {
    auto stop = std::make_shared<std::atomic_bool>(false);
    auto *reply = new LocalReply(this, stop, request.feature);
    const auto model = m_packs->model(request.voice);
    if (!model) {
        QTimer::singleShot(0, reply, [reply] {
            reply->complete(Ai::Error::Unsupported, tr("This built-in voice isn't downloaded yet."));
        });
        return reply;
    }
    QStringList sentences = request.sentences;
    if (sentences.isEmpty()) {
        SpeechBlock block;
        block.text = request.text;
        block.end = request.text.size();
        for (const SpeechSentence &sentence : Speech::sentences(block))
            sentences.append(sentence.text);
    }
    const Job job{reply, this, stop, *model, m_packs->engineLibrary(), sentences};
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, job] { worker->run(job); }, Qt::QueuedConnection);
    return reply;
}

Ai::Reply *LocalSpeech::listVoices(const QString &language) {
    auto *reply = new LocalReply(this, std::make_shared<std::atomic_bool>(false), QStringLiteral("voices"));
    QList<Ai::Voice> voices;
    for (const QVariant &value : m_packs->voices(language)) {
        const QVariantMap map = value.toMap();
        if (!map.value("installed").toBool())
            continue;
        Ai::Voice voice;
        voice.id = map.value("id").toString();
        voice.name = map.value("name").toString();
        voice.language = map.value("language").toString();
        voice.gender = map.value("gender").toString();
        voices.append(voice);
    }
    QTimer::singleShot(0, reply, [reply, voices] {
        reply->setVoices(voices);
        reply->complete();
    });
    return reply;
}

double LocalSpeech::speedOf(const QString &voiceId) const {
    const auto model = m_packs->model(voiceId);
    return model ? m_speeds.value(model->key) : 0.0;
}

void LocalSpeech::noteSpeed(const QString &modelKey, double speed) {
    const double before = m_speeds.value(modelKey);
    const double now = before > 0 ? 0.7 * before + 0.3 * speed : speed;
    m_speeds.insert(modelKey, now);
    QSettings s = settings();
    s.setValue(QStringLiteral("builtinSpeed/") + QString(modelKey).replace(':', '|'), now);
    emit speedChanged();
}
