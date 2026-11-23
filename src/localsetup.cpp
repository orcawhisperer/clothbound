#include "localsetup.h"

#include "aiservices.h"
#include "systemcheck.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QStandardPaths>

namespace {
const QString ollama = QStringLiteral("http://127.0.0.1:11434");

QString titleOf(const QString &model) {
    static const QHash<QString, QString> titles{{QStringLiteral("qwen3.5:2b"), QStringLiteral("Qwen3.5 2B")},
                                                {QStringLiteral("qwen3.5:4b"), QStringLiteral("Qwen3.5 4B")},
                                                {QStringLiteral("qwen3.5:9b"), QStringLiteral("Qwen3.5 9B")},
                                                {QStringLiteral("qwen3-embedding:0.6b"), QStringLiteral("Qwen3-Embedding 0.6B")}};
    return titles.value(model, model);
}
}  // namespace

LocalSetup::LocalSetup(AiServices *ai, QObject *parent) : QObject(parent), m_ai(ai) {
    // After the terminal opens: look for Ollama every few seconds, for a while.
    m_poll.setInterval(3000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (++m_polls > 200) {
            m_poll.stop();
            m_waiting = false;
            emit changed();
            return;
        }
        probe();
    });
}

QPair<QString, QString> LocalSetup::modelsFor(double memoryGb, double graphicsGb) {
    const QString search = QStringLiteral("qwen3-embedding:0.6b");
    if (memoryGb >= 30 || graphicsGb >= 10)
        return {QStringLiteral("qwen3.5:9b"), search};
    if (memoryGb >= 15 || graphicsGb >= 6)
        return {QStringLiteral("qwen3.5:4b"), search};
    if (memoryGb >= 7.5)
        return {QStringLiteral("qwen3.5:2b"), search};
    return {};
}

void LocalSetup::refresh() {
    m_installed = !QStandardPaths::findExecutable(QStringLiteral("ollama")).isEmpty();
    const SystemCheck::Machine machine = SystemCheck::inspect();
    const auto [text, search] = modelsFor(machine.memoryGb, machine.graphicsMemoryGb);
    m_text = text.isEmpty() ? QVariantMap() : QVariantMap{{"model", text}, {"title", titleOf(text)}, {"bytes", 0}};
    m_search = text.isEmpty() ? QVariantMap() : QVariantMap{{"model", search}, {"title", titleOf(search)}, {"bytes", 0}};
    emit changed();
    if (!text.isEmpty()) {
        size(text, true);
        size(search, false);
    }
    probe();
}

// A model's download size, from the layers its manifest lists.
void LocalSetup::size(const QString &model, bool text) {
    QNetworkRequest request(QUrl(QStringLiteral("https://registry.ollama.ai/v2/library/%1/manifests/%2")
                                     .arg(model.section(QLatin1Char(':'), 0, 0), model.section(QLatin1Char(':'), 1))));
    request.setRawHeader("Accept", "application/vnd.docker.distribution.manifest.v2+json");
    request.setTransferTimeout(8000);
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, text, model] {
        reply->deleteLater();
        qint64 bytes = 0;
        for (const QJsonValue &layer : QJsonDocument::fromJson(reply->readAll()).object().value("layers").toArray())
            bytes += layer.toObject().value("size").toInteger();
        QVariantMap &which = text ? m_text : m_search;
        if (bytes > 0 && which.value("model") == model) {
            which.insert("bytes", bytes);
            emit changed();
        }
    });
}

void LocalSetup::probe() {
    QNetworkRequest request(QUrl(ollama + QStringLiteral("/api/tags")));
    request.setTransferTimeout(1500);
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const bool running = reply->error() == QNetworkReply::NoError;
        m_installed = m_installed || running || !QStandardPaths::findExecutable(QStringLiteral("ollama")).isEmpty();
        if (running && m_waiting) {
            m_waiting = false;
            m_poll.stop();
            m_ai->findLocal();
        }
        if (running != m_running)
            m_ai->findLocal();
        m_running = running;
        emit changed();
    });
}

void LocalSetup::install() {
    m_problem.clear();
    // Installing needs the reader's password, so it happens where they can
    // see it and type it: Omarchy's terminal.
    const QString command = m_installed ? QStringLiteral("sudo systemctl enable --now ollama")
                                        : QStringLiteral("omarchy-pkg-add ollama && sudo systemctl enable --now ollama");
    const QString omarchy = QStandardPaths::findExecutable(QStringLiteral("omarchy-launch-floating-terminal-with-presentation"));
    const bool started = !omarchy.isEmpty()
                             ? QProcess::startDetached(omarchy, {command})
                             : QProcess::startDetached(QStringLiteral("xdg-terminal-exec"),
                                                       {QStringLiteral("bash"), QStringLiteral("-c"),
                                                        command + QStringLiteral("; read -p 'Press Enter to close'")});
    if (!started) {
        m_problem = tr("No terminal opened. Install Ollama yourself (omarchy-pkg-add ollama), then come back.");
        emit changed();
        return;
    }
    m_waiting = true;
    m_polls = 0;
    m_poll.start();
    emit changed();
}

void LocalSetup::download() {
    if (m_text.isEmpty() || m_pulling)
        return;
    pull({m_text.value("model").toString(), m_search.value("model").toString()});
}

void LocalSetup::cancel() {
    if (m_reply)
        m_reply->abort();
}

void LocalSetup::pull(const QStringList &models) {
    if (models.isEmpty()) {
        // Everything's here: use it, and let the work run here.
        m_pulling = false;
        m_status.clear();
        m_ai->useLocal(QStringLiteral("ollama"), ollama, m_text.value("model").toString(), m_search.value("model").toString());
        if (m_ai->preset() == QLatin1String("cloud"))
            m_ai->setPreset(m_ai->hasKey() ? QStringLiteral("hybrid") : QStringLiteral("local"));
        m_ai->findLocal();
        emit changed();
        return;
    }
    const QString model = models.first();
    m_pulling = true;
    m_progress = 0;
    m_problem.clear();
    m_status = tr("Downloading %1…").arg(titleOf(model));
    emit changed();
    QNetworkRequest request(QUrl(ollama + QStringLiteral("/api/pull")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setTransferTimeout(120000);
    QNetworkReply *reply = m_network.post(request, QJsonDocument(QJsonObject{{"model", model}, {"stream", true}}).toJson());
    m_reply = reply;
    auto buffer = std::make_shared<QByteArray>();
    auto failed = std::make_shared<QString>();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, buffer, failed, model] {
        *buffer += reply->readAll();
        qsizetype end;
        while ((end = buffer->indexOf('\n')) >= 0) {
            const QJsonObject line = QJsonDocument::fromJson(buffer->left(end)).object();
            buffer->remove(0, end + 1);
            if (line.contains("error"))
                *failed = line.value("error").toString();
            const double total = line.value("total").toDouble(), done = line.value("completed").toDouble();
            if (total > 0) {
                m_progress = done / total;
                m_status = tr("Downloading %1… %2%").arg(titleOf(model)).arg(qRound(m_progress * 100));
                emit changed();
            }
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, failed, models] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || !failed->isEmpty()) {
            m_pulling = false;
            m_status.clear();
            m_problem = reply->error() == QNetworkReply::OperationCanceledError
                            ? QString()
                            : tr("The download stopped: %1").arg(failed->isEmpty() ? reply->errorString() : *failed);
            emit changed();
            return;
        }
        pull(models.mid(1));
    });
}
