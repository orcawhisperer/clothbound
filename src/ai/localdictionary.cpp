#include "ai/localdictionary.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <memory>

LocalDictionary::LocalDictionary(QObject *parent) : QObject(parent) {}

LocalDictionary::~LocalDictionary() { cancel(); }

QString LocalDictionary::cleanWord(const QString &raw) {
    QString out = raw.trimmed();
    out.replace(QChar(0x2019), QLatin1Char('\''));
    out.replace(QChar(0x2018), QLatin1Char('\''));
    static const QRegularExpression lead(QStringLiteral("^[^\\p{L}\\p{N}]+"));
    static const QRegularExpression trail(QStringLiteral("[^\\p{L}\\p{N}]+$"));
    out.remove(lead);
    out.remove(trail);
    return out;
}

bool LocalDictionary::isAvailable() {
    return !QStandardPaths::findExecutable(QStringLiteral("sdcv")).isEmpty();
}

QStringList LocalDictionary::parseSdcvJson(const QByteArray &json, const QString &word) {
    QStringList out;
    for (const QJsonValue &entry : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = entry.toObject();
        const QString def = object.value(QStringLiteral("definition")).toString().trimmed();
        if (def.isEmpty())
            continue;
        if (object.value(QStringLiteral("word")).toString().compare(word, Qt::CaseInsensitive) == 0 || out.isEmpty()) {
            const QString dict = object.value(QStringLiteral("dict")).toString().trimmed();
            out.append(dict.isEmpty() ? def : def + QStringLiteral("\n— ") + dict);
        }
        if (out.size() >= 2)
            break;
    }
    return out;
}

QStringList LocalDictionary::lookupSync(const QString &word, int timeoutMs) {
    if (word.isEmpty() || !isAvailable())
        return {};
    QProcess sdcv;
    sdcv.start(QStringLiteral("sdcv"),
               {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), word});
    if (!sdcv.waitForFinished(qMax(100, timeoutMs))) {
        sdcv.kill();
        sdcv.waitForFinished(500);
        return {};
    }
    if (sdcv.exitStatus() != QProcess::NormalExit || sdcv.exitCode() != 0)
        return {};
    return parseSdcvJson(sdcv.readAllStandardOutput(), word);
}

bool LocalDictionary::lookupAsync(const QString &word, std::function<void(const QStringList &)> done, int timeoutMs) {
    Q_ASSERT(QThread::currentThread() == thread());
    cancel();
    const QString lookupWord = cleanWord(word);
    if (lookupWord.isEmpty() || lookupWord.contains(QLatin1Char(' ')) || !isAvailable())
        return false;

    const int generation = m_generation;
    auto *sdcv = new QProcess(this);
    m_process = sdcv;
    auto handled = std::make_shared<bool>(false);
    auto *timer = new QTimer(sdcv);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, sdcv, [sdcv] {
        if (sdcv->state() != QProcess::NotRunning)
            sdcv->kill();
    });
    connect(sdcv, &QProcess::errorOccurred, this, [this, sdcv, generation, handled, done](QProcess::ProcessError) {
        if (*handled)
            return;
        *handled = true;
        if (sdcv == m_process)
            m_process.clear();
        sdcv->deleteLater();
        if (generation == m_generation && done)
            done({});
    });
    connect(sdcv, &QProcess::finished, this,
            [this, sdcv, lookupWord, generation, handled, done](int code, QProcess::ExitStatus status) {
                if (*handled)
                    return;
                *handled = true;
                if (sdcv == m_process)
                    m_process.clear();
                const QByteArray output = sdcv->readAllStandardOutput();
                sdcv->deleteLater();
                if (generation != m_generation)
                    return;
                const QStringList entries =
                    (status == QProcess::NormalExit && code == 0) ? parseSdcvJson(output, lookupWord) : QStringList();
                if (done)
                    done(entries);
            });
    timer->start(qMax(100, timeoutMs));
    sdcv->start(QStringLiteral("sdcv"),
                {QStringLiteral("-n"), QStringLiteral("-j"), QStringLiteral("--utf8-output"), QStringLiteral("--"), lookupWord});
    return true;
}

void LocalDictionary::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_generation;
    if (m_process) {
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
        m_process->deleteLater();
        m_process.clear();
    }
}
