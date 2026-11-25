#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <functional>

class AiServices;
class QNetworkReply;

// Hardware specs needed by LocalSetup to pick Ollama packages and model tiers,
// decoupled from src/app/systemcheck.h so src/ai/ never includes src/app/.
struct LocalHardwareInfo {
    double memoryGb = 0;
    QString graphics;
    double graphicsMemoryGb = 0;
    bool discreteGraphics = false;
};

// Abstract hardware probe interface implemented by SystemCheck (in src/app/)
// or test doubles. Allows dependency inversion without introducing an ai -> app edge.
class LocalHardwareProbe {
public:
    virtual ~LocalHardwareProbe() = default;
    virtual LocalHardwareInfo probeHardware() const = 0;
    // Optional stateless functor safe to run on a worker thread without touching `this`.
    virtual std::function<LocalHardwareInfo()> makeAsyncProbe() const { return {}; }
};

// Setting up a model on this computer, for someone who has never heard of
// Ollama: whether it's installed and running, the model that suits this
// computer (with its download size, asked of Ollama's registry), installing
// Ollama in Omarchy's terminal (where the reader types their password, never
// Clothbound), and downloading the model through Ollama's own API, with
// progress, then using it.
class LocalSetup : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    // {model, title, bytes} for text, and for search by meaning; {} when this
    // computer is too small for a model worth having.
    Q_PROPERTY(QVariantMap textModel READ textModel NOTIFY changed)
    Q_PROPERTY(QVariantMap searchModel READ searchModel NOTIFY changed)
    Q_PROPERTY(bool waiting READ waiting NOTIFY changed)   // for the terminal to finish
    Q_PROPERTY(bool pulling READ pulling NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)

public:
    explicit LocalSetup(AiServices *ai, QObject *parent = nullptr);
    LocalSetup(AiServices *ai, LocalHardwareProbe *probe, QObject *parent = nullptr);
    ~LocalSetup() override { cancel(); }

    void setHardwareProbe(LocalHardwareProbe *probe);
    // Registers the default background hardware probe used when no per-instance
    // probe is injected (wired by SystemCheck in src/app/systemcheck.cpp).
    static void setDefaultHardwareProbe(std::function<LocalHardwareInfo()> probeFn);

    bool installed() const { return m_installed; }
    bool running() const { return m_running; }
    QVariantMap textModel() const { return m_text; }
    QVariantMap searchModel() const { return m_search; }
    bool waiting() const { return m_waiting; }
    bool pulling() const { return m_pulling; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    QString problem() const { return m_problem; }

    // Looks again: installed, running, and what to recommend.
    Q_INVOKABLE void refresh();
    // Opens Omarchy's terminal to install (or just start) Ollama.
    Q_INVOKABLE void install();
    // Downloads the recommended models, then uses them.
    Q_INVOKABLE void download();
    // Downloads a specific Ollama model tag (e.g. "qwen3-embedding:0.6b"), with progress.
    Q_INVOKABLE void pullModel(const QString &model);
    // Uses recommended models already installed in Ollama (works offline).
    Q_INVOKABLE void useExisting();
    Q_INVOKABLE void cancel();

    // Ollama's tags for a computer with this much memory (GB) and graphics
    // memory: {text, search}; empty text when it's too small.
    static QPair<QString, QString> modelsFor(double memoryGb, double graphicsGb);
    static QPair<QString, QString> modelsFor(const LocalHardwareInfo &machine);

signals:
    void changed();

private:
    void applyMachine(const LocalHardwareInfo &machine);
    void probe();
    void size(const QString &model, bool text);
    void pull(const QStringList &models);
    QString ollamaPackage() const;

    AiServices *m_ai;
    LocalHardwareProbe *m_probe = nullptr;
    QNetworkAccessManager m_network;
    LocalHardwareInfo m_machine;
    bool m_inspected = false;
    bool m_inspecting = false;
    bool m_installed = false;
    bool m_running = false;
    bool m_waiting = false;
    bool m_pulling = false;
    bool m_pullCustom = false;
    int m_pullGen = 0;
    QString m_customPulledModel;
    double m_progress = 0;
    QString m_status;
    QString m_problem;
    QVariantMap m_text;
    QVariantMap m_search;
    QHash<QString, qint64> m_sizes;
    QTimer m_poll;
    int m_polls = 0;
    QPointer<QNetworkReply> m_reply;
};
