#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <atomic>
#include <memory>

class AiServices;
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
    // The open book's pictures, newest first: {id, url, caption, chapter, offset}.
    Q_PROPERTY(QVariantList pictures READ pictures NOTIFY picturesChanged)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // About what a picture costs, in US dollars.
    Q_PROPERTY(double cost READ cost NOTIFY changed)

public:
    Illustrator(AiServices *ai, Reader *reader, const QString &databaseConnection, const QString &folder,
                QObject *parent = nullptr);
    ~Illustrator() override { stop(); }

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString problem() const { return m_problem; }
    QVariantList pictures() const;
    bool available() const;
    double cost() const;

    // The words selected, or "" for the page in view; ending at (chapter, end).
    Q_INVOKABLE void illustrate(const QString &text, int chapter, qint64 end);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void remove(int id);
    // A copy in the Pictures folder; its path.
    Q_INVOKABLE QString saveCopy(int id);

    // The prompt writer's instructions, public for tests.
    static QString promptInstructions();

signals:
    void changed();
    void picturesChanged();
    void made(int id);

private:
    void fail(const QString &problem);
    void draw(const QString &prompt, const QString &caption, int chapter, qint64 offset);

    AiServices *m_ai;
    Reader *m_reader;
    QString m_connection;
    QString m_folder;
    bool m_busy = false;
    QString m_status;
    QString m_problem;
    QPointer<Ai::Reply> m_reply;
    std::shared_ptr<std::atomic_bool> m_readCancel;
    int m_generation = 0;
};
