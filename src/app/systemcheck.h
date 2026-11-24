#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QVariantList>

class AiServices;
class LocalSpeech;

namespace Ai { class Reply; }

// "Test this computer": what this machine has (memory, processor, graphics),
// how fast its built-in voice and local model run, and so, feature by
// feature, what will work well here, what will be slow, and what needs
// Gemini. Expectations set before anything is downloaded.
class SystemCheck : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QVariantMap machine READ machine NOTIFY changed)
    // {feature, verdict, level ("good", "slow", "no"), detail}
    Q_PROPERTY(QVariantList verdicts READ verdicts NOTIFY changed)
    // Compact 3-row summary for Settings › Intelligence: {feature, detail, level}.
    Q_PROPERTY(QVariantList summaryVerdicts READ summaryVerdicts NOTIFY changed)
    Q_PROPERTY(QString tier READ tier NOTIFY changed)
    Q_PROPERTY(QString recommendation READ recommendation NOTIFY changed)

public:
    SystemCheck(AiServices *ai, LocalSpeech *speech, QObject *parent = nullptr);
    ~SystemCheck() override;

    struct Machine {
        double memoryGb = 0;      // installed
        double availableGb = 0;   // free for models now
        int cores = 0;
        QString processor;
        QString architecture;
        // Fast integer instructions local models use: AVX2/AVX-VNNI on x86,
        // dot-product/i8mm on ARM.
        QStringList features;
        QString graphics;     // "AMD Radeon 780M", "NVIDIA RTX 4070", "Apple M1 Pro"
        double graphicsMemoryGb = 0;  // dedicated; 0 when shared with the system
        bool discreteGraphics = false;
    };

    // What this computer is; `root` lets tests supply /proc and /sys.
    static Machine inspect(const QString &root = QStringLiteral("/"));
    // The size of a model from its name ("qwen3.5:4b" → 4), or 0.
    static double billionsIn(const QString &model);
    // The plan's tiers: "Reader", "Local voices", "Local Lite", "Local
    // Standard", "Local Full".
    static QString tierFor(const Machine &machine);
    // A model family that fits, for a machine without one.
    static QString suggestedModel(const Machine &machine);

    bool running() const { return m_running; }
    QVariantMap machine() const;
    QVariantList verdicts() const { return m_verdicts; }
    QVariantList summaryVerdicts() const { return m_summaryVerdicts; }
    QString tier() const { return tierFor(m_machine); }
    QString recommendation() const { return m_recommendation; }

    Q_INVOKABLE void refresh(bool force = false);
    Q_INVOKABLE void run();
    Q_INVOKABLE void cancel();

    // The verdicts, from what was found and measured (public for tests):
    // voiceSpeed is seconds of speech per second (0: none installed),
    // promptRate and answerRate the local model's tokens per second (0:
    // none, or not measured).
    QVariantList judge(double voiceSpeed, double promptRate, double answerRate) const;

signals:
    void changed();

private:
    void rebuildSummary();
    void startBenchmark(int generation);
    void measureModel(int generation);
    void finish(int generation);

    AiServices *m_ai;
    LocalSpeech *m_speech;
    Machine m_machine;
    bool m_inspected = false;
    bool m_inspecting = false;
    bool m_running = false;
    int m_generation = 0;
    double m_voiceSpeed = 0;
    double m_promptRate = 0;
    double m_answerRate = 0;
    QVariantList m_verdicts;
    QVariantList m_summaryVerdicts;
    QString m_recommendation;
    QPointer<Ai::Reply> m_reply;
    QElapsedTimer m_clock;
};
