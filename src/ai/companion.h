#pragma once

#include "ai/ai.h"
#include "ai/bookcontextreader.h"
#include "ai/companionstore.h"
#include "ai/localdictionary.h"
#include "ai/mathformatter.h"
#include "ai/passageindex.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <atomic>
#include <memory>

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
    // Adaptive Story ("fiction") vs. Study ("study") mode per book.
    Q_PROPERTY(QString readingMode READ readingMode WRITE setReadingMode NOTIFY readingModeChanged)
    Q_PROPERTY(bool studyMode READ studyMode NOTIFY readingModeChanged)
    // Smart AI scope: "upto" (spoiler-free), "book" (whole document), or "shelf" (multi-document).
    Q_PROPERTY(QString scope READ scope WRITE setScope NOTIFY scopeChanged)
    Q_PROPERTY(QString shelfLabel READ shelfLabel NOTIFY scopeChanged)
    // Pending visual snip attached to the Ask composer (trusted local PNG data URI).
    Q_PROPERTY(QString pendingSnipUrl READ pendingSnipUrl NOTIFY pendingSnipChanged)
    Q_PROPERTY(int pendingSnipPage READ pendingSnipPage NOTIFY pendingSnipChanged)
    Q_PROPERTY(bool hasPendingSnip READ hasPendingSnip NOTIFY pendingSnipChanged)

public:
    using Chapter = BookContextReader::Chapter;
    using Citation = CompanionStore::Citation;
    using Turn = CompanionStore::Turn;

    Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent = nullptr,
              PassageIndex *sharedIndex = nullptr);
    ~Companion() override;

    PassageIndex *passageIndex() const { return m_index.data(); }
    void setPassageIndex(PassageIndex *sharedIndex);

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
    QString readingMode() const { return m_readingMode; }
    void setReadingMode(const QString &mode);
    bool studyMode() const { return m_readingMode == QLatin1String("study"); }
    QString scope() const { return m_scope; }
    void setScope(const QString &scope);
    QString shelfLabel() const { return m_shelfLabel; }
    QString pendingSnipUrl() const { return m_pendingSnipUrl; }
    int pendingSnipPage() const { return m_pendingSnipPage; }
    bool hasPendingSnip() const { return !m_pendingSnipPng.isEmpty(); }

    // A word, as used at (chapter, offset).
    Q_INVOKABLE void define(const QString &word, int chapter, qint64 offset);
    // Words selected, ending at (chapter, end).
    Q_INVOKABLE void explain(const QString &text, int chapter, qint64 end);
    Q_INVOKABLE void translate(const QString &text, int chapter, qint64 end);
    // Re-translates the last selected passage using the current targetLanguage.
    Q_INVOKABLE void retranslate();
    // About the book so far; follow-ups keep the conversation.
    Q_INVOKABLE void ask(const QString &question);
    // Visual snip entry point: action is "explain", "latex", "table", or "ask".
    Q_INVOKABLE void askAboutSnipBase64(const QString &base64Png, const QString &action, const QString &prompt = {},
                                        int page = -1);
    Q_INVOKABLE void clearPendingSnip();
    Q_INVOKABLE void activateCitation(int bookId, int chapter, qint64 offset = 0, const QString &quote = {});
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
    // Strips Markdown image embeds and raw <img> tags, and converts offline LaTeX math to rich HTML.
    Q_INVOKABLE QString sanitizeMarkdown(const QString &text) const { return cleanMarkdown(text); }
    Q_INVOKABLE bool hasMath(const QString &text) const;
    Q_INVOKABLE QString formatDisplayHtml(const QString &text, const QString &mathBlockBg = {}) const;

    // The book's text up to (chapter, offset), from its file (any thread).
    // When `limit > 0`, reads backward from `chapter` and stops once enough text is gathered.
    static QList<Chapter> textUpTo(const QString &path, const QString &format, int chapter, qint64 offset,
                                   qint64 limit = -1, std::shared_ptr<std::atomic_bool> cancel = {});
    // The entire book's text from chapter/page 0 to the end (any thread).
    static QList<Chapter> textOfBook(const QString &path, const QString &format, qint64 limit = -1,
                                     std::shared_ptr<std::atomic_bool> cancel = {});
    // Auto-detects "study" vs "fiction" from format, title, TOC entries, and opening text.
    static QString detectReadingMode(const QString &format, const QString &title, const QStringList &tocTitles,
                                     const QString &sampleText);
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
    void readingModeChanged();
    void scopeChanged();
    void pendingSnipChanged();
    void openAskRequested();
    void openBookPassage(int bookId, int chapter, const QString &quote);

private:
    struct Action {
        QString feature;
        QString text;
        int chapter = 0;
        qint64 offset = 0;
        QByteArray snipPng;
        QString snipAction;
    };

    void resetPeopleState();
    void resetRecapState();
    void resetSelectionState();
    void loadBookState(int bookId);
    QString detectCurrentBookMode() const;
    void refreshShelfLabel();
    bool spoilerFilterActive() const;
    bool isAheadOfReader(int chapter, qint64 offset) const;
    bool isTurnVisibleAtCurrentPlace(const Turn &turn) const;
    void pruneFutureTurnsIfSpoilerFree();
    QList<Ai::Message> buildHistoryMessages(qint64 budget, bool filterSpoiler) const;
    bool loadWhosWhoSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter);
    bool loadRecapSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter);
    void begin(const QString &feature, const QString &subject);
    void setStatus(const QString &status);
    void fail(const QString &problem, bool isConfig = false);
    void rollbackPendingTurn();
    void commitAssistantTurn(int bookId, const QList<Citation> &sources, int turnChapter, qint64 turnOffset);
    void finishCachedSnapshot(const QString &where, bool whereLocal);
    void flushStreamedAnswer();
    void executeAction(const Action &action);
    qint64 soFarCharsEstimate() const;
    qint64 wholeBookCharsEstimate() const;
    void readChapters(bool wholeBook, int chapter, qint64 offset, qint64 limit,
                      std::function<void(const QList<Chapter> &)> then);
    void readSoFar(int chapter, qint64 offset, qint64 limit, std::function<void(const QList<Chapter> &)> then);
    void readWholeBook(qint64 limit, std::function<void(const QList<Chapter> &)> then);
    void askShelf(const QString &asked);
    void sendSnipRequest(const QByteArray &pngBytes, const QString &action, const QString &prompt, int page);
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
    bool wholeBookStudyActive() const { return studyMode() && m_scope != QLatin1String("upto"); }

    AiServices *m_ai;
    QPointer<Reader> m_reader;
    CompanionStore m_store;
    QPointer<PassageIndex> m_index;
    bool m_ownsIndex = false;
    bool m_indexOpActive = false;
    LocalDictionary m_dictionary;
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
    QString m_readingMode = QStringLiteral("fiction");
    QString m_scope = QStringLiteral("upto");
    QString m_shelfLabel;
    QByteArray m_pendingSnipPng;
    QString m_pendingSnipUrl;
    int m_pendingSnipPage = -1;
    QString m_targetLanguage;
    QString m_lastTranslateText;
    int m_lastTranslateChapter = 0;
    qint64 m_lastTranslateEnd = 0;
    Action m_lastAction;
    Action m_pendingSetup;
    int m_requestChapter = -1;
    qint64 m_requestStart = -1;
    qint64 m_requestOffset = -1;
    double m_lastCheckedCost = -1.0;
    QTimer m_streamFlush;
    bool m_streamDirty = false;
    bool m_streamNotifiedFirst = false;
    QPointer<Ai::Reply> m_reply;
    std::shared_ptr<std::atomic_bool> m_readCancel;
    int m_generation = 0;
};
