#pragma once

#include "ai.h"
#include "passageindex.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <atomic>
#include <memory>

class AiServices;
class QProcess;
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
    // Who's who: {name, also, who, chapter, chapterIndex, offset}.
    Q_PROPERTY(QVariantList people READ people NOTIFY changed)
    Q_PROPERTY(bool peopleLoaded READ peopleLoaded NOTIFY changed)
    Q_PROPERTY(int peopleChapter READ peopleChapter NOTIFY changed)
    Q_PROPERTY(QString peopleWhere READ peopleWhere NOTIFY changed)
    Q_PROPERTY(bool peopleWhereLocal READ peopleWhereLocal NOTIFY changed)
    // Persisted spoiler-safe "Previously…" recap so tab switches never re-bill or lose it.
    Q_PROPERTY(QString recapText READ recapText NOTIFY recapChanged)
    Q_PROPERTY(bool recapLoaded READ recapLoaded NOTIFY recapChanged)
    Q_PROPERTY(int recapChapter READ recapChapter NOTIFY recapChanged)
    Q_PROPERTY(QString recapWhere READ recapWhere NOTIFY recapChanged)
    Q_PROPERTY(bool recapWhereLocal READ recapWhereLocal NOTIFY recapChanged)
    // Preserved selection result (Define / Explain / Translate) across tab switches.
    Q_PROPERTY(QString selectionFeature READ selectionFeature NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionSubject READ selectionSubject NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionAnswer READ selectionAnswer NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionWhere READ selectionWhere NOTIFY selectionChanged)
    Q_PROPERTY(bool selectionWhereLocal READ selectionWhereLocal NOTIFY selectionChanged)
    // Ask's completed conversation turns: {fromUser, text, sources: [{chapter, offset, label, excerpt}]}.
    // Live streaming text arrives via `answer` so existing message delegates aren't torn down per token.
    Q_PROPERTY(QVariantList conversation READ conversation NOTIFY conversationChanged)
    // Source passages matched for the active streaming answer.
    Q_PROPERTY(QVariantList currentSources READ currentSources NOTIFY answerChanged)
    // Target language for Translate (persisted; defaults to the reader's system language).
    Q_PROPERTY(QString targetLanguage READ targetLanguage WRITE setTargetLanguage NOTIFY targetLanguageChanged)
    Q_PROPERTY(QStringList targetLanguages READ targetLanguages CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // What's happening ("Reading the book so far…"), while busy.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(bool problemIsConfig READ problemIsConfig NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(bool hasPendingSetup READ hasPendingSetup NOTIFY changed)
    Q_PROPERTY(QString pendingSetupLabel READ pendingSetupLabel NOTIFY changed)
    // Where the answer was made, in a few words.
    Q_PROPERTY(QString where READ where NOTIFY changed)
    Q_PROPERTY(bool whereLocal READ whereLocal NOTIFY changed)

public:
    struct Chapter {
        int index = 0;
        QString title;
        QString text;
    };
    struct Citation {
        int chapter = 0;
        qint64 offset = 0;
        QString label;
        QString excerpt;
    };
    struct Turn {
        bool fromUser = true;
        QString text;
        QList<Citation> sources;
        QString provenance;
        bool local = false;
    };

    Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent = nullptr);
    ~Companion() override;

    QString feature() const { return m_feature; }
    QString subject() const { return m_subject; }
    QString answer() const { return m_answer; }
    QVariantList people() const { return m_people; }
    bool peopleLoaded() const { return m_peopleLoaded; }
    int peopleChapter() const { return m_peopleChapter; }
    QString peopleWhere() const { return m_peopleWhere; }
    bool peopleWhereLocal() const { return m_peopleWhereLocal; }
    QString recapText() const { return m_recapText; }
    bool recapLoaded() const { return m_recapLoaded; }
    int recapChapter() const { return m_recapChapter; }
    QString recapWhere() const { return m_recapWhere; }
    bool recapWhereLocal() const { return m_recapWhereLocal; }
    QString selectionFeature() const { return m_selectionFeature; }
    QString selectionSubject() const { return m_selectionSubject; }
    QString selectionAnswer() const { return m_selectionAnswer; }
    QString selectionWhere() const { return m_selectionWhere; }
    bool selectionWhereLocal() const { return m_selectionWhereLocal; }
    QVariantList conversation() const;
    QVariantList currentSources() const;
    QString targetLanguage() const;
    void setTargetLanguage(const QString &language);
    QStringList targetLanguages() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString problem() const { return m_problem; }
    bool problemIsConfig() const { return m_problemIsConfig; }
    bool canRetry() const { return !m_lastAction.feature.isEmpty() && !m_busy; }
    bool hasPendingSetup() const { return !m_pendingSetup.feature.isEmpty(); }
    QString pendingSetupLabel() const;
    QString where() const { return m_where; }
    bool whereLocal() const { return m_whereLocal; }

    // A word, as used at (chapter, offset).
    Q_INVOKABLE void define(const QString &word, int chapter, qint64 offset);
    // Words selected, ending at (chapter, end).
    Q_INVOKABLE void explain(const QString &text, int chapter, qint64 end);
    Q_INVOKABLE void translate(const QString &text, int chapter, qint64 end);
    // Re-translates the last selected passage using the current targetLanguage.
    Q_INVOKABLE void retranslate();
    // About the book so far; follow-ups keep the conversation.
    Q_INVOKABLE void ask(const QString &question);
    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void recap();
    Q_INVOKABLE void refreshRecap();
    Q_INVOKABLE void whosWho();
    Q_INVOKABLE void refreshWhosWho();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void resumePendingSetup();
    Q_INVOKABLE void clearProblem();
    Q_INVOKABLE void stop();
    // Removes persisted Ask threads and Who's Who snapshots for a book.
    Q_INVOKABLE void clearBookHistory(int bookId = 0);
    // What asking would send and cost: {where, words, cost, local}.
    Q_INVOKABLE QVariantMap askEstimate() const;
    // Spoiler-safe, zero-cost starter prompts derived from what the reader has reached.
    Q_INVOKABLE QStringList suggestions() const;
    // Strips Markdown image embeds and raw <img> tags before rendering LLM prose.
    Q_INVOKABLE QString sanitizeMarkdown(const QString &text) const { return cleanMarkdown(text); }

    // The book's text up to (chapter, offset), from its file (any thread).
    // When `limit > 0`, reads backward from `chapter` and stops once enough text is gathered.
    static QList<Chapter> textUpTo(const QString &path, const QString &format, int chapter, qint64 offset,
                                   qint64 limit = -1, std::shared_ptr<std::atomic_bool> cancel = {});
    // At most `limit` characters, the latest kept, with chapter headings.
    static QString joined(const QList<Chapter> &chapters, qint64 limit);
    static QString cleanMarkdown(const QString &text);
    // The language answers are written in: the reader's own.
    static QString readerLanguage();
    // A dictionary on this computer (sdcv), if any: its definitions.
    static QStringList dictionary(const QString &word);

signals:
    void changed();
    void answerChanged();
    void conversationChanged();
    void targetLanguageChanged();
    void recapChanged();
    void selectionChanged();

private:
    struct Action {
        QString feature;
        QString text;
        int chapter = 0;
        qint64 offset = 0;
    };

    void ensureTables();
    void loadBookState(int bookId);
    void saveConversation(int bookId);
    void loadConversation(int bookId);
    bool loadWhosWhoSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter);
    void saveWhosWhoSnapshot(int bookId, int chapter, qint64 offset, qint64 pageStart, const QString &whereLabel,
                             bool isLocal, const QVariantList &people);
    bool loadRecapSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter);
    void saveRecapSnapshot(int bookId, int chapter, qint64 offset, qint64 pageStart, const QString &whereLabel,
                           bool isLocal, const QString &text);
    void begin(const QString &feature, const QString &subject);
    void setStatus(const QString &status);
    void fail(const QString &problem, bool isConfig = false);
    void rollbackPendingTurn();
    void flushStreamedAnswer();
    void executeAction(const Action &action);
    qint64 soFarCharsEstimate() const;
    // Reads the book up to the reader's place on a worker thread, then `then`.
    void readSoFar(int chapter, qint64 offset, qint64 limit, std::function<void(const QList<Chapter> &)> then);
    void run(const QString &feature, const Ai::TextRequest &request, std::function<void(Ai::Reply *)> done = nullptr);
    bool checkBudget(const QString &feature, qint64 charsEstimate, bool fast, qint64 outputTokens = 400);
    qint64 localCharBudget(const QString &feature, bool fast, qint64 fallbackChars, double charsPerToken = 3.0) const;
    Ai::TextModel *modelFor(const QString &feature);
    QString bookLine() const;
    QString placeTitle(int chapter) const;
    bool cloudFor(const QString &feature) const;
    QString formatProvenance(const Ai::Provider *provider, const Ai::Reply *reply) const;
    void runRecap(bool forceRefresh);
    void runWhosWho(bool forceRefresh);

    AiServices *m_ai;
    Reader *m_reader;
    QString m_connection;
    PassageIndex m_index;
    QString m_feature;
    QString m_subject;
    QString m_answer;
    QVariantList m_people;
    bool m_peopleLoaded = false;
    int m_peopleChapter = -1;
    qint64 m_peopleOffset = -1;
    QString m_peopleKey;
    QString m_peopleWhere;
    bool m_peopleWhereLocal = false;
    QString m_recapText;
    bool m_recapLoaded = false;
    int m_recapChapter = -1;
    qint64 m_recapOffset = -1;
    QString m_recapWhere;
    bool m_recapWhereLocal = false;
    QString m_selectionFeature;
    QString m_selectionSubject;
    QString m_selectionAnswer;
    QString m_selectionWhere;
    bool m_selectionWhereLocal = false;
    QList<Turn> m_conversation;
    QList<Citation> m_currentSources;
    bool m_pendingUserTurn = false;
    bool m_busy = false;
    QString m_status;
    QString m_problem;
    bool m_problemIsConfig = false;
    QString m_where;
    bool m_whereLocal = false;
    QString m_targetLanguage;
    QString m_lastTranslateText;
    int m_lastTranslateChapter = 0;
    qint64 m_lastTranslateEnd = 0;
    Action m_lastAction;
    Action m_pendingSetup;
    int m_requestChapter = -1;
    qint64 m_requestStart = -1;
    qint64 m_requestOffset = -1;
    QTimer m_streamFlush;
    bool m_streamDirty = false;
    bool m_streamNotifiedFirst = false;
    QPointer<Ai::Reply> m_reply;
    QPointer<QProcess> m_dictProcess;
    std::shared_ptr<std::atomic_bool> m_readCancel;
    int m_generation = 0;
};
