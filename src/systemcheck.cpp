#include "systemcheck.h"

#include "aiservices.h"
#include "localmodels.h"
#include "localspeech.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QThreadPool>

namespace {

QString readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

double meminfoGb(const QString &meminfo, const QString &field) {
    const QRegularExpression line(QStringLiteral("^%1:\\s+(\\d+) kB").arg(field), QRegularExpression::MultilineOption);
    const auto match = line.match(meminfo);
    return match.hasMatch() ? match.captured(1).toDouble() / 1024.0 / 1024.0 : 0;
}

QVariantMap verdict(const QString &feature, const QString &text, const QString &level, const QString &detail = {}) {
    return {{QStringLiteral("feature"), feature}, {QStringLiteral("verdict"), text},
            {QStringLiteral("level"), level}, {QStringLiteral("detail"), detail}};
}

// About 1,500 words of a public-domain book: what the director reads.
QString samplePassage() {
    const QString paragraph = QStringLiteral(
        "The Time Traveller (for so it will be convenient to speak of him) was expounding a recondite matter to us. "
        "His grey eyes shone and twinkled, and his usually pale face was flushed and animated. The fire burned "
        "brightly, and the soft radiance of the incandescent lights in the lilies of silver caught the bubbles that "
        "flashed and passed in our glasses. ");
    QString text;
    for (int i = 0; i < 18; ++i)
        text += QStringLiteral("[%1] ").arg(i) + paragraph + QLatin1Char('\n');
    return text;
}

}  // namespace

SystemCheck::SystemCheck(AiServices *ai, LocalSpeech *speech, QObject *parent)
    : QObject(parent), m_ai(ai), m_speech(speech) {
    if (m_ai) {
        auto refreshSummary = [this] {
            if (!m_inspected)
                return;
            rebuildSummary();
            emit changed();
        };
        connect(m_ai, &AiServices::routingChanged, this, refreshSummary);
        connect(m_ai, &AiServices::changed, this, refreshSummary);
    }
}

SystemCheck::~SystemCheck() { cancel(); }

void SystemCheck::rebuildSummary() {
    QVariantList rows;
    auto add = [&rows](const QString &feature, const QString &detail, const QString &level) {
        rows.append(QVariantMap{{QStringLiteral("feature"), feature},
                                {QStringLiteral("detail"), detail},
                                {QStringLiteral("level"), level}});
    };

    // 1. Narration
    if (m_voiceSpeed >= 1.2)
        add(tr("Narration"), tr("%1× faster than real time").arg(m_voiceSpeed, 0, 'f', 1), QStringLiteral("good"));
    else if (m_voiceSpeed > 0)
        add(tr("Narration"), tr("%1× real time · prepares ahead").arg(m_voiceSpeed, 0, 'f', 1), QStringLiteral("slow"));
    else if (m_machine.memoryGb >= 7.5)
        add(tr("Narration"), tr("runs on this computer"), QStringLiteral("good"));
    else if (m_machine.memoryGb > 0)
        add(tr("Narration"), tr("may be slow here"), QStringLiteral("slow"));
    else
        add(tr("Narration"), tr("not measured yet"), QStringLiteral("no"));

    // 2. Define, translate
    const double quick = (m_promptRate > 0 && m_answerRate > 0) ? (1700.0 / m_promptRate + 200.0 / m_answerRate) : 0.0;
    const bool hasLocal = m_ai && m_ai->hasLocal();
    if (quick > 0 && quick <= 8.0)
        add(tr("Define, translate"), tr("a few seconds"), QStringLiteral("good"));
    else if (quick > 0)
        add(tr("Define, translate"), tr("about %n s", "", qRound(quick)),
            quick > 30.0 ? QStringLiteral("slow") : QStringLiteral("good"));
    else if (hasLocal)
        add(tr("Define, translate"), tr("a few seconds"), QStringLiteral("good"));
    else if (m_machine.memoryGb >= 15 || m_machine.graphicsMemoryGb >= 6)
        add(tr("Define, translate"), tr("ready with a local model"), QStringLiteral("good"));
    else if (m_machine.memoryGb >= 7.5)
        add(tr("Define, translate"), tr("slower here · fast with Gemini"), QStringLiteral("slow"));
    else
        add(tr("Define, translate"), tr("needs Gemini"), QStringLiteral("slow"));

    // 3. Ask the whole book
    const bool wholeBookLocal = m_machine.memoryGb >= 30 || m_machine.graphicsMemoryGb >= 16 ||
                                (m_machine.graphics == QLatin1String("Apple") && m_machine.memoryGb >= 23);
    if (wholeBookLocal)
        add(tr("Ask the whole book"), hasLocal ? tr("runs on this computer") : tr("ready with a 9B+ model"),
            QStringLiteral("good"));
    else
        add(tr("Ask the whole book"), tr("needs Gemini or 32 GB"), QStringLiteral("slow"));

    m_summaryVerdicts = rows;
}

void SystemCheck::refresh() {
    if (m_inspected || m_inspecting)
        return;
    m_inspecting = true;
    QPointer<SystemCheck> self(this);
    QThreadPool::globalInstance()->start([self] {
        const Machine found = inspect();
        QMetaObject::invokeMethod(
            qApp,
            [self, found] {
                if (!self)
                    return;
                self->m_machine = found;
                self->m_inspected = true;
                self->m_inspecting = false;
                self->rebuildSummary();
                emit self->changed();
            },
            Qt::QueuedConnection);
    });
}

void SystemCheck::cancel() {
    ++m_generation;
    if (m_reply)
        m_reply->abort();
    if (m_running) {
        m_running = false;
        emit changed();
    }
}

SystemCheck::Machine SystemCheck::inspect(const QString &root) {
    Machine machine;
    const QString base = QDir(root).absolutePath();
    const QString meminfo = readFile(base + QStringLiteral("/proc/meminfo"));
    machine.memoryGb = meminfoGb(meminfo, QStringLiteral("MemTotal"));
    machine.availableGb = meminfoGb(meminfo, QStringLiteral("MemAvailable"));
    const QString cpuinfo = readFile(base + QStringLiteral("/proc/cpuinfo"));
    machine.cores = int(cpuinfo.count(QRegularExpression(QStringLiteral("^processor\\s*:"), QRegularExpression::MultilineOption)));
    if (machine.cores == 0)
        machine.cores = QThread::idealThreadCount();
    const auto field = [&cpuinfo](const QString &name) {
        const auto match = QRegularExpression(QStringLiteral("^%1\\s*:\\s*(.+)$").arg(name), QRegularExpression::MultilineOption).match(cpuinfo);
        return match.hasMatch() ? match.captured(1).trimmed() : QString();
    };
    machine.processor = field(QStringLiteral("model name"));
    const QString flags = field(QStringLiteral("flags")) + QLatin1Char(' ') + field(QStringLiteral("Features"));
    machine.architecture = flags.contains(QLatin1String("asimd")) ? QStringLiteral("aarch64")
                         : flags.contains(QLatin1String("sse2")) ? QStringLiteral("x86_64")
                                                                 : QSysInfo::currentCpuArchitecture();
    const QStringList words = flags.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const auto &[flag, name] : {std::pair{"avx2", "AVX2"}, std::pair{"avx_vnni", "AVX-VNNI"},
                                     std::pair{"avx512_vnni", "AVX-512 VNNI"}, std::pair{"asimddp", "dot product"},
                                     std::pair{"i8mm", "i8mm"}})
        if (words.contains(QLatin1String(flag)))
            machine.features.append(QLatin1String(name));

    // Graphics: each DRM card's PCI vendor, or the Apple GPU's driver.
    // Prefer discrete GPUs (NVIDIA, discrete AMD/Intel) over integrated GPUs
    // regardless of DRM card numbering order on hybrid laptops.
    int bestRank = 0;
    bool hasNvidia = false;
    const QDir drm(base + QStringLiteral("/sys/class/drm"));
    for (const QString &card : drm.entryList({QStringLiteral("card*")}, QDir::Dirs | QDir::System)) {
        if (card.contains(QLatin1Char('-')))
            continue;
        const QString device = drm.filePath(card) + QStringLiteral("/device");
        const QString vendor = readFile(device + QStringLiteral("/vendor")).trimmed().toLower();
        const QString driver = QFileInfo(device + QStringLiteral("/driver")).symLinkTarget().section(QLatin1Char('/'), -1);
        double vram = readFile(device + QStringLiteral("/mem_info_vram_total")).trimmed().toDouble() / 1024 / 1024 / 1024;
        if (vram <= 0)
            vram = readFile(device + QStringLiteral("/lmem_total")).trimmed().toDouble() / 1024 / 1024 / 1024;

        QString name;
        bool discrete = false;
        double vramGb = 0;
        int rank = 0;
        if (vendor == QLatin1String("0x10de")) {
            hasNvidia = true;
            name = QStringLiteral("NVIDIA");
            discrete = true;
            rank = 4;
        } else if (vendor == QLatin1String("0x1002")) {
            name = QStringLiteral("AMD Radeon");
            // An integrated Radeon reports a small carve-out as its VRAM.
            discrete = vram >= 4;
            vramGb = discrete ? vram : 0;
            rank = discrete ? 3 : 1;
        } else if (vendor == QLatin1String("0x8086")) {
            name = QStringLiteral("Intel");
            discrete = vram >= 4;
            vramGb = discrete ? vram : 0;
            rank = discrete ? 3 : 1;
        } else if (driver == QLatin1String("asahi") || readFile(device + QStringLiteral("/uevent")).contains(QLatin1String("asahi"))) {
            name = QStringLiteral("Apple");
            rank = 2;
        }
        if (rank > bestRank || (rank == bestRank && vramGb > machine.graphicsMemoryGb)) {
            bestRank = rank;
            machine.graphics = name;
            machine.discreteGraphics = discrete;
            machine.graphicsMemoryGb = vramGb;
        }
    }
    if (root == QLatin1String("/") && hasNvidia && !QStandardPaths::findExecutable(QStringLiteral("nvidia-smi")).isEmpty()) {
        QProcess smi;
        smi.start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-gpu=name,memory.total"), QStringLiteral("--format=csv,noheader,nounits")});
        if (!smi.waitForFinished(3000)) {
            smi.kill();
            smi.waitForFinished(500);
        } else if (smi.exitStatus() == QProcess::NormalExit && smi.exitCode() == 0) {
            QString bestName;
            double bestMem = -1;
            const QStringList lines = QString::fromUtf8(smi.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const QStringList parts = line.split(QLatin1Char(','));
                const QString gpuName = parts.value(0).trimmed();
                bool ok = false;
                const double memGb = parts.value(1).trimmed().toDouble(&ok) / 1024.0;
                const double gb = ok ? memGb : 0.0;
                if (!gpuName.isEmpty() && gb > bestMem) {
                    bestName = gpuName;
                    bestMem = gb;
                }
            }
            if (!bestName.isEmpty()) {
                machine.graphics = bestName;
                machine.graphicsMemoryGb = qMax(0.0, bestMem);
                machine.discreteGraphics = true;
            }
        }
    }
    if (machine.graphics == QLatin1String("Apple") && machine.processor.isEmpty())
        machine.processor = QStringLiteral("Apple Silicon");
    return machine;
}

double SystemCheck::billionsIn(const QString &model) {
    // "qwen3.5:4b", "gemma4:e4b", "Qwen3.5-9B-Instruct-Q4_K_M", "26b-a4b".
    static const QRegularExpression size(QStringLiteral("(?:^|[^a-z0-9.])e?(\\d+(?:\\.\\d+)?)b(?:$|[^a-z])"),
                                         QRegularExpression::CaseInsensitiveOption);
    const auto match = size.match(model);
    return match.hasMatch() ? match.captured(1).toDouble() : 0;
}

QString SystemCheck::tierFor(const Machine &machine) {
    const double memory = machine.memoryGb;
    const bool appleLarge = machine.graphics == QLatin1String("Apple") && memory >= 23;
    if (machine.graphicsMemoryGb >= 16 && memory >= 30)
        return tr("Local Full");
    if (memory >= 30 || appleLarge || machine.graphicsMemoryGb >= 10 || (memory >= 15 && machine.graphicsMemoryGb >= 8))
        return tr("Local Standard");
    if (memory >= 15 || machine.graphicsMemoryGb >= 6)
        return tr("Local Lite");
    if (memory >= 7.5)
        return tr("Local voices");
    return tr("Reader");
}

QString SystemCheck::suggestedModel(const Machine &machine) {
    // Aligned with tierFor() and LocalSetup::modelsFor().
    const bool appleLarge = machine.graphics == QLatin1String("Apple") && machine.memoryGb >= 23;
    if (machine.memoryGb >= 30 || appleLarge || machine.graphicsMemoryGb >= 10 ||
        (machine.memoryGb >= 15 && machine.graphicsMemoryGb >= 8))
        return QStringLiteral("Qwen3.5 9B");
    if (machine.memoryGb >= 15 || machine.graphicsMemoryGb >= 6)
        return QStringLiteral("Qwen3.5 4B");
    return QStringLiteral("Qwen3.5 2B");
}

QVariantMap SystemCheck::machine() const {
    return {{QStringLiteral("memoryGb"), m_machine.memoryGb},
            {QStringLiteral("availableGb"), m_machine.availableGb},
            {QStringLiteral("cores"), m_machine.cores},
            {QStringLiteral("processor"), m_machine.processor},
            {QStringLiteral("architecture"), m_machine.architecture},
            {QStringLiteral("features"), m_machine.features},
            {QStringLiteral("graphics"), m_machine.graphics},
            {QStringLiteral("graphicsMemoryGb"), m_machine.graphicsMemoryGb},
            {QStringLiteral("discreteGraphics"), m_machine.discreteGraphics}};
}

void SystemCheck::run() {
    if (m_running)
        return;
    const int generation = ++m_generation;
    m_running = true;
    m_voiceSpeed = m_promptRate = m_answerRate = 0;
    emit changed();
    QPointer<SystemCheck> self(this);
    QThreadPool::globalInstance()->start([self, generation] {
        const Machine found = inspect();
        QMetaObject::invokeMethod(
            qApp,
            [self, found, generation] {
                if (!self || generation != self->m_generation)
                    return;
                self->m_machine = found;
                self->m_inspected = true;
                emit self->changed();
                self->startBenchmark(generation);
            },
            Qt::QueuedConnection);
    });
}

void SystemCheck::startBenchmark(int generation) {
    if (generation != m_generation)
        return;
    // The built-in voice: reuse its measured inference speed if known (avoiding
    // cold ONNX load latency), or speak one sample sentence now.
    if (m_speech) {
        Ai::Reply *voices = m_speech->listVoices(QString());
        m_reply = voices;
        connect(voices, &Ai::Reply::finished, this, [this, voices, generation] {
            voices->deleteLater();
            if (generation != m_generation)
                return;
            if (voices->voices().isEmpty()) {
                measureModel(generation);
                return;
            }
            const QString voice = voices->voices().first().id;
            const double known = m_speech->speedOf(voice);
            if (known > 0) {
                m_voiceSpeed = known;
                measureModel(generation);
                return;
            }
            Ai::SpeechRequest request;
            request.feature = QStringLiteral("check");
            request.voice = voice;
            request.text = QStringLiteral("The Time Traveller was expounding a recondite matter to us. His grey eyes shone and twinkled, and his usually pale face was flushed and animated.");
            request.sentences = {request.text};
            m_clock.start();
            Ai::Reply *spoken = m_speech->synthesize(request);
            m_reply = spoken;
            connect(spoken, &Ai::Reply::finished, this, [this, spoken, voice, generation] {
                spoken->deleteLater();
                if (generation != m_generation)
                    return;
                const double measured = m_speech->speedOf(voice);
                if (measured > 0) {
                    m_voiceSpeed = measured;
                } else {
                    const double seconds = m_clock.elapsed() / 1000.0;
                    const double audio = spoken->audio().size() / 2.0 / qMax(1, spoken->sampleRate());
                    if (!spoken->error() && seconds > 0)
                        m_voiceSpeed = audio / seconds;
                }
                measureModel(generation);
            });
        });
    } else {
        measureModel(generation);
    }
}

void SystemCheck::measureModel(int generation) {
    if (generation != m_generation)
        return;
    Ai::TextModel *model = m_ai->hasLocal() ? m_ai->localModels()->text() : nullptr;
    if (!model) {
        finish(generation);
        return;
    }
    // What a chapter's direction costs: a long prompt read, a short answer.
    Ai::TextRequest request;
    request.feature = QStringLiteral("check");
    request.fast = true;
    request.system = QStringLiteral("You summarize passages in one short sentence.");
    request.messages = {{true, samplePassage() + QStringLiteral("\nSummarize these paragraphs in one sentence.")}};
    m_clock.start();
    Ai::Reply *reply = model->generate(request);
    m_reply = reply;
    connect(reply, &Ai::Reply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        const double seconds = m_clock.elapsed() / 1000.0;
        const Ai::Usage usage = reply->usage();
        if (!reply->error() && seconds > 0) {
            const double input = usage.inputTokens ? usage.inputTokens : 1200;
            const double output = usage.outputTokens ? usage.outputTokens : 40;
            if (usage.promptEvalNs > 0 && usage.evalNs > 0) {
                // Exact server timings (Ollama), excluding cold model-load time.
                m_promptRate = input / qMax(0.01, usage.promptEvalNs / 1e9);
                m_answerRate = output / qMax(0.01, usage.evalNs / 1e9);
            } else {
                m_promptRate = input / qMax(0.05, seconds * 0.6);
                m_answerRate = output / qMax(0.05, seconds * 0.4);
            }
        }
        finish(generation);
    });
}

QVariantList SystemCheck::judge(double voiceSpeed, double promptRate, double answerRate) const {
    QVariantList out;
    out.append(verdict(tr("Reading, search, highlights"), tr("Works well"), QStringLiteral("good")));

    // Narration.
    if (voiceSpeed >= 1.2)
        out.append(verdict(tr("Built-in voices"), tr("%1× faster than real time: plays as you listen").arg(voiceSpeed, 0, 'f', 1),
                           QStringLiteral("good")));
    else if (voiceSpeed > 0)
        out.append(verdict(tr("Built-in voices"), tr("Slower than real time: chapters are prepared ahead"),
                           QStringLiteral("slow")));
    else
        out.append(verdict(tr("Built-in voices"),
                           m_machine.memoryGb >= 7.5 ? tr("Not installed yet; this computer can run them")
                                                     : tr("Not installed yet; may be slow with this little memory"),
                           m_machine.memoryGb >= 7.5 ? QStringLiteral("good") : QStringLiteral("slow"),
                           tr("Install one in the Narrator panel (V).")));
    out.append(verdict(tr("Gemini voices and styles"), m_ai->hasKey() ? tr("Ready, with your key") : tr("Needs a Gemini key"),
                       m_ai->hasKey() ? QStringLiteral("good") : QStringLiteral("no")));

    // Text work on the local model.
    const QString model = m_ai->localTextModel();
    const double size = billionsIn(model);
    const auto seconds = [&](double input, double output) {
        return promptRate > 0 && answerRate > 0 ? input / promptRate + output / answerRate : 0.0;
    };
    const QString suggestion = tr("Install Ollama, llama.cpp or LM Studio with a model such as %1, then Find again.")
                                   .arg(suggestedModel(m_machine));
    if (model.isEmpty()) {
        out.append(verdict(tr("Styles, Define, Translate, Who's who"),
                           m_ai->hasKey() ? tr("With Gemini") : tr("Needs Gemini or a local model"),
                           m_ai->hasKey() ? QStringLiteral("good") : QStringLiteral("no"), suggestion));
        out.append(verdict(tr("Explain, recaps"), m_ai->hasKey() ? tr("With Gemini") : tr("Needs Gemini or a local model"),
                           m_ai->hasKey() ? QStringLiteral("good") : QStringLiteral("no")));
    } else {
        const double quick = seconds(1700, 200);
        out.append(verdict(tr("Styles, Define, Translate, Who's who"),
                           quick > 0 ? tr("About %n s per answer with %1", "", qMax(1, qRound(quick))).arg(model)
                                     : tr("With %1").arg(model),
                           quick > 0 && quick > 30 ? QStringLiteral("slow") : QStringLiteral("good")));
        const double careful = seconds(6000, 500);
        const bool smallModel = size > 0 && size < 8;
        out.append(verdict(tr("Explain, recaps"),
                           careful > 0 ? tr("About %n s per answer", "", qMax(1, qRound(careful))) : tr("With %1").arg(model),
                           smallModel || careful > 60 ? QStringLiteral("slow") : QStringLiteral("good"),
                           smallModel ? tr("Small models may be less accurate; Gemini is better at this.") : QString()));
    }
    // Ask: the whole book needs a long context.
    out.append(verdict(tr("Ask about the book"),
                       m_ai->hasKey() ? tr("With Gemini, about the whole book so far")
                       : !model.isEmpty() ? tr("With %1, from the passages that match").arg(model)
                                          : tr("Needs Gemini or a local model"),
                       m_ai->hasKey() || !model.isEmpty() ? QStringLiteral("good") : QStringLiteral("no"),
                       m_ai->hasKey() ? QString() : tr("A whole book is too long for a model on this computer: %1 reads the passages that match.").arg(model)));
    const bool embeds = !m_ai->localEmbeddingModel().isEmpty() || m_ai->hasKey();
    out.append(verdict(tr("Search by meaning"), embeds ? tr("Ready") : tr("Needs Gemini or a local embedding model"),
                       embeds ? QStringLiteral("good") : QStringLiteral("no"),
                       embeds ? QString() : tr("Qwen3-Embedding 0.6B or EmbeddingGemma are small and fast.")));
    out.append(verdict(tr("Illustrate"), m_ai->hasKey() ? tr("With Gemini") : tr("Needs a Gemini key"),
                       m_ai->hasKey() ? QStringLiteral("good") : QStringLiteral("no"),
                       tr("Pictures need a large graphics card to make on a computer; Dogear makes them with Gemini.")));
    return out;
}

void SystemCheck::finish(int generation) {
    if (generation != m_generation)
        return;
    m_verdicts = judge(m_voiceSpeed, m_promptRate, m_answerRate);
    rebuildSummary();
    const QString tierName = tierFor(m_machine);
    if (m_ai->hasLocal() && m_ai->hasKey())
        m_recommendation = tr("Hybrid: quick work on this computer, careful work with Gemini.");
    else if (m_ai->hasLocal())
        m_recommendation = tr("On this computer: everything that can run here does, and nothing leaves it.");
    else if (m_machine.memoryGb >= 15 || m_machine.graphicsMemoryGb >= 6)
        m_recommendation = tr("This computer (%1) can run a local model such as %2 for quick work.").arg(tierName, suggestedModel(m_machine));
    else
        m_recommendation = tr("Gemini for text work; built-in voices for free narration.");
    m_running = false;
    emit changed();
}
