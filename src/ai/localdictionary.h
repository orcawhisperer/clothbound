#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>

class QProcess;

// Local StarDict dictionary lookup via the `sdcv` CLI: strips surrounding
// punctuation, parses JSON entries, and manages bounded subprocess lifetimes
// both synchronously and asynchronously.
class LocalDictionary : public QObject {
    Q_OBJECT

public:
    explicit LocalDictionary(QObject *parent = nullptr);
    ~LocalDictionary() override;

    static QString cleanWord(const QString &raw);
    static bool isAvailable();
    static QStringList parseSdcvJson(const QByteArray &json, const QString &word);
    static QStringList lookupSync(const QString &word, int timeoutMs = 4000);

    // Starts an asynchronous single-word lookup via `sdcv`. Returns true if a
    // subprocess was launched (in which case `done` will fire unless cancel()
    // is called), or false immediately when `word` is empty/multi-word or sdcv
    // is not installed.
    bool lookupAsync(const QString &word, std::function<void(const QStringList &entries)> done, int timeoutMs = 4000);
    void cancel();

private:
    QPointer<QProcess> m_process;
    int m_generation = 0;
};
