#pragma once

#include "ai.h"
#include "passageindex.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>

class AiServices;
class Reader;

// The reading companion: a word defined, a passage explained or translated,
// a question about the book answered, the story so far recalled, and who's
// who. It never sees past the reader's place, so it can't spoil anything:
// everything is cut at the end of the page (or of the words selected).
// Gemini reads the whole book so far; a model on this computer reads the
// passages that matter, found by meaning (or by their words).
class Companion : public QObject {
    Q_OBJECT
    // "define", "explain", "translate", "ask", "recap", "whoswho", or "".
    Q_PROPERTY(QString feature READ feature NOTIFY changed)
    // The words selected, or the question asked.
    Q_PROPERTY(QString subject READ subject NOTIFY changed)
    Q_PROPERTY(QString answer READ answer NOTIFY answerChanged)
    // Who's who: {name, also, who, chapter}.
    Q_PROPERTY(QVariantList people READ people NOTIFY changed)
    // Ask's conversation so far: {fromUser, text}.
    Q_PROPERTY(QVariantList conversation READ conversation NOTIFY answerChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // What's happening ("Reading the book so far…"), while busy.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // Where the answer was made, in a few words.
    Q_PROPERTY(QString where READ where NOTIFY changed)

public:
    struct Chapter {
        int index = 0;
        QString title;
        QString text;
    };

    Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent = nullptr);

    QString feature() const { return m_feature; }
    QString subject() const { return m_subject; }
    QString answer() const { return m_answer; }
    QVariantList people() const { return m_people; }
    QVariantList conversation() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString problem() const { return m_problem; }
    QString where() const { return m_where; }

    // A word, as used at (chapter, offset).
    Q_INVOKABLE void define(const QString &word, int chapter, qint64 offset);
    // Words selected, ending at (chapter, end).
    Q_INVOKABLE void explain(const QString &text, int chapter, qint64 end);
    Q_INVOKABLE void translate(const QString &text, int chapter, qint64 end);
    // About the book so far; follow-ups keep the conversation.
    Q_INVOKABLE void ask(const QString &question);
    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void recap();
    Q_INVOKABLE void whosWho();
    Q_INVOKABLE void stop();
    // What asking would send and cost: {where, words, cost, local}.
    Q_INVOKABLE QVariantMap askEstimate() const;

    // The book's text up to (chapter, offset), from its file (any thread).
    static QList<Chapter> textUpTo(const QString &path, const QString &format, int chapter, qint64 offset);
    // At most `limit` characters, the latest kept, with chapter headings.
    static QString joined(const QList<Chapter> &chapters, qint64 limit);
    // The language answers are written in: the reader's own.
    static QString readerLanguage();
    // A dictionary on this computer (sdcv), if any: its definitions.
    static QStringList dictionary(const QString &word);

signals:
    void changed();
    void answerChanged();

private:
    void begin(const QString &feature, const QString &subject);
    void setStatus(const QString &status);
    void fail(const QString &problem);
    // Reads the book up to the reader's place on a worker thread, then `then`.
    void readSoFar(int chapter, qint64 offset, std::function<void(const QList<Chapter> &)> then);
    void run(Ai::TextModel *model, const Ai::TextRequest &request, std::function<void(Ai::Reply *)> done = nullptr);
    Ai::TextModel *modelFor(const QString &feature);
    QString bookLine() const;
    bool cloudFor(const QString &feature) const;

    AiServices *m_ai;
    Reader *m_reader;
    PassageIndex m_index;
    QString m_feature;
    QString m_subject;
    QString m_answer;
    QVariantList m_people;
    QList<Ai::Message> m_conversation;
    bool m_busy = false;
    QString m_status;
    QString m_problem;
    QString m_where;
    QPointer<Ai::Reply> m_reply;
    int m_generation = 0;
    // Who's who, kept for a book and place so it isn't asked again.
    QString m_peopleKey;
};
