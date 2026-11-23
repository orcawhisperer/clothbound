#include "localsetup.h"

#include "aiservices.h"
#include "systemcheck.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QStandardPaths>
#include <QThreadPool>

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

void LocalSetup::applyMachine(const SystemCheck::Machine &machine) {
    m_machine = machine;
    m_inspected = true;
    const auto [text, search] = modelsFor(machine.memoryGb, machine.graphicsMemoryGb);
    m_text = text.isEmpty() ? QVariantMap() : QVariantMap{{"model", text}, {"title", titleOf(text)}, {"bytes", m_sizes.value(text, 0)}};
    m_search = text.isEmpty() ? QVariantMap() : QVariantMap{{"model", search}, {"title", titleOf(search)}, {"bytes", m_sizes.value(search, 0)}};
    emit changed();
    // Only query Ollama's registry when setup is actually relevant, not already
    // cached, and not in strict local-only mode.
    if (!text.isEmpty() && !m_ai->hasLocal() && m_ai->preset() != QLatin1String("local")) {
        if (!m_sizes.contains(text))
            size(text, true);
        if (!m_sizes.contains(search))
            size(search, false);
    }
}

void LocalSetup::refresh() {
    m_installed = !QStandardPaths::findExecutable(QStringLiteral("ollama")).isEmpty();
    if (m_inspected) {
        applyMachine(m_machine);
    } else if (!m_inspecting) {
        m_inspecting = true;
        QPointer<LocalSetup> self(this);
        QThreadPool::globalInstance()->start([self] {
            const SystemCheck::Machine machine = SystemCheck::inspect();
            QMetaObject::invokeMethod(
                qApp,
                [self, machine] {
                    if (!self)
                        return;
                    self->m_inspecting = false;
                    self->applyMachine(machine);
                },
                Qt::QueuedConnection);
        });
    }
    probe();
}

// A model's download size, from the layers its manifest lists.
void LocalSetup::size(const QString &model, bool text) {
    QNetworkRequest request(QUrl(QStringLiteral("https://registry.ollama.ai/v2/library/%1/manifests/%2")
                                     .arg(model.section(QLatin1Char(':'), 0, 0), model.section(QLatin1Char(':'), 1))));
    request.setRawHeader("Accept", "application/vnd.docker.distribution.manifest.v2+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(8000);
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, text, model] {
        reply->deleteLater();
        qint64 bytes = 0;
        for (const QJsonValue &layer : QJsonDocument::fromJson(reply->readAll()).object().value("layers").toArray())
            bytes += layer.toObject().value("size").toInteger();
        if (bytes > 0) {
            m_sizes.insert(model, bytes);
            QVariantMap &which = text ? m_text : m_search;
            if (which.value("model") == model) {
                which.insert("bytes", bytes);
                emit changed();
            }
        }
    });
}

void LocalSetup::probe() {
    QNetworkRequest request(QUrl(ollama + QStringLiteral("/api/tags")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(1500);
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        const bool running = reply->error() == QNetworkReply::NoError && status == 200 && json.contains(QStringLiteral("models"));
        m_installed = m_installed || running || !QStandardPaths::findExecutable(QStringLiteral("ollama")).isEmpty();
        const bool shouldFind = (running && m_waiting) || (running != m_running);
        if (running && m_waiting) {
            m_waiting = false;
            m_poll.stop();
        }
        m_running = running;
        if (shouldFind)
            m_ai->findLocal();
        emit changed();
    });
}

QString LocalSetup::ollamaPackage() const {
    if (m_machine.graphics.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive))
        return QStringLiteral("ollama-cuda");
    if (m_machine.graphics.contains(QLatin1String("AMD"), Qt::CaseInsensitive) ||
        m_machine.graphics.contains(QLatin1String("Radeon"), Qt::CaseInsensitive) ||
        (m_machine.graphics.contains(QLatin1String("Intel"), Qt::CaseInsensitive) && m_machine.discreteGraphics))
        return QStringLiteral("ollama-vulkan");
    return QStringLiteral("ollama");
}

void LocalSetup::install() {
    m_problem.clear();
    const bool hasOmarchyPkg = !QStandardPaths::findExecutable(QStringLiteral("omarchy-pkg-add")).isEmpty();
    if (!m_installed && !hasOmarchyPkg) {
        m_problem = tr("Install Ollama from ollama.com (or your package manager), start it, then click Find.");
        emit changed();
        return;
    }
    // Installing needs the reader's password, so it happens where they can
    // see it and type it: Omarchy's terminal.
    const QString pkg = ollamaPackage();
    const QString command = m_installed ? QStringLiteral("sudo systemctl enable --now ollama")
                                        : QStringLiteral("omarchy-pkg-add %1 && sudo systemctl enable --now ollama").arg(pkg);
    const QString omarchy = QStandardPaths::findExecutable(QStringLiteral("omarchy-launch-floating-terminal-with-presentation"));
    const bool started = !omarchy.isEmpty()
                             ? QProcess::startDetached(omarchy, {command})
                             : QProcess::startDetached(QStringLiteral("xdg-terminal-exec"),
                                                       {QStringLiteral("bash"), QStringLiteral("-c"),
                                                        command + QStringLiteral("; read -p 'Press Enter to close'")});
    if (!started) {
        m_problem = tr("No terminal opened. Install Ollama yourself (omarchy-pkg-add %1), then come back.").arg(pkg);
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

void LocalSetup::useExisting() {
    if (m_text.isEmpty() || m_pulling)
        return;
    const QString textTag = m_text.value("model").toString();
    const QString searchTag = m_search.value("model").toString();
    bool hasSearch = false;
    for (const QVariant &entry : m_ai->localServers()) {
        const QVariantMap server = entry.toMap();
        if (server.value("url").toString() == ollama && server.value("embeddingModels").toStringList().contains(searchTag))
            hasSearch = true;
    }
    m_ai->useLocal(QStringLiteral("ollama"), ollama, textTag, hasSearch ? searchTag : QString());
    if (m_ai->preset() == QLatin1String("cloud"))
        m_ai->setPreset(m_ai->hasKey() ? QStringLiteral("hybrid") : QStringLiteral("local"));
    emit changed();
}

void LocalSetup::cancel() {
    m_poll.stop();
    const bool wasWaiting = m_waiting;
    m_waiting = false;
    if (m_reply)
        m_reply->abort();
    if (wasWaiting)
        emit changed();
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
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(120000);
    QNetworkReply *reply = m_network.post(request, QJsonDocument(QJsonObject{{"model", model}, {"stream", true}}).toJson());
    m_reply = reply;
    auto buffer = std::make_shared<QByteArray>();
    auto failed = std::make_shared<QString>();
    const auto parseLines = [this, buffer, failed, model](bool flushAll) {
        qsizetype end;
        while ((end = buffer->indexOf('\n')) >= 0) {
            const QByteArray raw = buffer->left(end).trimmed();
            buffer->remove(0, end + 1);
            if (raw.isEmpty())
                continue;
            const QJsonObject line = QJsonDocument::fromJson(raw).object();
            if (line.contains("error"))
                *failed = line.value("error").toString();
            const double total = line.value("total").toDouble(), done = line.value("completed").toDouble();
            if (total > 0) {
                m_progress = qBound(0.0, done / total, 1.0);
                m_status = tr("Downloading %1… %2%").arg(titleOf(model)).arg(qRound(m_progress * 100));
                emit changed();
            }
        }
        if (flushAll && !buffer->trimmed().isEmpty()) {
            const QJsonObject line = QJsonDocument::fromJson(buffer->trimmed()).object();
            buffer->clear();
            if (line.contains("error"))
                *failed = line.value("error").toString();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, buffer, parseLines] {
        *buffer += reply->readAll();
        parseLines(false);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, buffer, failed, models, parseLines] {
        reply->deleteLater();
        *buffer += reply->readAll();
        parseLines(true);
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
