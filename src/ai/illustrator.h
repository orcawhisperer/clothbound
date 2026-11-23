#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <atomic>
#include <memory>

class AiServices;
class QDBusPendingCallWatcher;
class Reader;
namespace Ai { class Reply; }

// Pictures of a moment in the book: a text model reads the passage (and
// only what came before it) and describes an illustration in the manner of
// the book's own time; an image model draws it. The pictures are kept with
// the book, captioned, to look back at or save.
class Illustrator : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(bool problemIsConfig READ problemIsConfig NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    // The open book's pictures, newest first: {id, url, caption, chapter, offset, style, styleLabel, exists}.
    Q_PROPERTY(QVariantList pictures READ pictures NOTIFY picturesChanged)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool budgetOk READ budgetOk NOTIFY changed)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY changed)
    // About what a picture costs, in US dollars.
    Q_PROPERTY(double cost READ cost NOTIFY changed)
    // Provenance line for pictures.
    Q_PROPERTY(QString where READ where CONSTANT)
    // Preferred art style ("auto", "engraving", "watercolor", "ink", "editorial"), persisted.
    Q_PROPERTY(QString style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(QVariantList styles READ styles CONSTANT)

public:
    Illustrator(AiServices *ai, Reader *reader, const QString &databaseConnection, const QString &folder,
                QObject *parent = nullptr);
    ~Illustrator() override;

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString problem() const { return m_problem; }
    bool problemIsConfig() const { return m_problemIsConfig; }
    bool canRetry() const { return m_hasLastRequest && !m_busy; }
    QVariantList pictures() const;
    bool available() const;
    bool budgetOk() const;
    QString unavailableReason() const;
    double cost() const;
    QString where() const;
    QString style() const { return m_style; }
    void setStyle(const QString &style);
    QVariantList styles() const;

    // The words selected, or "" for the page in view; ending at (chapter, end).
    Q_INVOKABLE void illustrate(const QString &text, int chapter, qint64 end);
    // Regenerates an existing illustration at its saved place using the current style.
    Q_INVOKABLE void regenerate(int id);
    Q_INVOKABLE void retryLast();
    Q_INVOKABLE void clearProblem();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void remove(int id);
    // A copy in the Pictures folder; its path.
    Q_INVOKABLE QString saveCopy(int id);
    // Copies to an explicit destination path; returns the saved path or "".
    Q_INVOKABLE QString saveCopyTo(int id, const QString &destinationPath);
    // Opens the desktop Save File portal (falling back to saveCopy() if unavailable).
    Q_INVOKABLE void chooseSaveCopy(int id);

    // The prompt writer's instructions, public for tests.
    static QString promptInstructions();
    static QString promptInstructions(const QString &styleId);
    static QString styleGuidance(const QString &styleId);

signals:
    void changed();
    void picturesChanged();
    void styleChanged();
    void made(int id);
    void savedCopy(const QString &path);
    void saveFailed(const QString &message);

private slots:
    void handleSaveResponse(uint response, const QVariantMap &results);

private:
    void fail(const QString &problem, bool isConfig = false);
    void draw(const QString &prompt, const QString &caption, int chapter, qint64 offset, const QString &styleId,
              const QString &passage);
    QString suggestedFileName(int id, QString *sourceOut = nullptr) const;
    void clearPendingSave();

    AiServices *m_ai;
    Reader *m_reader;
    QString m_connection;
    QString m_folder;
    QString m_style = QStringLiteral("auto");
    bool m_busy = false;
    QString m_status;
    QString m_problem;
    bool m_problemIsConfig = false;
    bool m_hasLastRequest = false;
    QString m_lastText;
    int m_lastChapter = 0;
    qint64 m_lastEnd = 0;
    QPointer<Ai::Reply> m_reply;
    std::shared_ptr<std::atomic_bool> m_readCancel;
    int m_generation = 0;
    int m_pendingSaveId = 0;
    int m_saveGeneration = 0;
    QString m_pendingSavePath;
    QPointer<QDBusPendingCallWatcher> m_saveWatcher;
};
