#pragma once

#include "ai/companionstore.h"
#include "ai/passageindex.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <atomic>
#include <memory>

// Stateless, worker-thread-safe book/chapter/shelf text extraction,
// spoiler-safe slicing, reading-mode heuristics, citation formatting,
// and first-appearance resolution for Companion & Illustrator.
class BookContextReader {
public:
    struct Chapter {
        int index = 0;
        QString title;
        QString text;
    };

    struct FirstSeen {
        int chapterIndex = -1;
        qint64 offset = 0;
        QString label;
    };

    struct ShelfContextResult {
        QString context;
        QList<CompanionStore::Citation> citations;
    };

    struct LocalAskContext {
        QString context;
        QString how;
        QList<CompanionStore::Citation> sources;
    };

    // The book's text up to (chapter, offset), from its file (any thread).
    // When `limit > 0`, reads backward from `chapter` and stops once enough text is gathered.
    static QList<Chapter> textUpTo(const QString &path, const QString &format, int chapter, qint64 offset,
                                   qint64 limit = -1, std::shared_ptr<std::atomic_bool> cancel = {});

    // The entire book's text from chapter/page 0 to the end (any thread).
    static QList<Chapter> textOfBook(const QString &path, const QString &format, qint64 limit = -1,
                                     std::shared_ptr<std::atomic_bool> cancel = {});

    // Single chapter/page context slice (any thread).
    static QString readPageContext(const QString &path, const QString &format, int pageOrChapter, qint64 maxChars,
                                   const std::shared_ptr<std::atomic_bool> &cancel = {});

    // Opening text sample for reading-mode detection (any thread).
    static QString readOpeningSample(const QString &path, const QString &format, qint64 maxChars = 6000);

    // Auto-detects "study" vs "fiction" from format, title, TOC entries, and opening text.
    static QString detectReadingMode(const QString &format, const QString &title, const QStringList &tocTitles,
                                     const QString &sampleText);

    // At most `limit` characters, the latest kept, with chapter headings.
    static QString joined(const QList<Chapter> &chapters, qint64 limit);

    // Study-mode whole-document join preserving Abstract/Intro, current section,
    // and closing conclusions (deprioritizing trailing References).
    static QString joinedStudyDocument(const QList<Chapter> &chapters, qint64 limit, int currentChapter = 0);

    static QString chapterDisplayLabel(const QList<Chapter> &chapters, int chapterIndex, const QString &format);
    static QString passageText(const PassageIndex::Hit &hit, const QList<Chapter> &chapters, const QString &format);
    static QString makeExcerpt(const QString &text, qsizetype maxChars = 120);
    static QString makeChipQuote(const QString &text);
    static QList<CompanionStore::Citation> citationsFromHits(const QList<PassageIndex::Hit> &hits,
                                                             const QList<Chapter> &chapters, const QString &format,
                                                             int maxCount = 4);

    static FirstSeen resolveFirstSeen(const QList<Chapter> &chapters, const QString &name, const QString &also,
                                      const QString &llmChapter, const QString &format, bool studyGlossary = false);
    static QVariantList parseWhosWhoEntries(const QJsonArray &entries, const QList<Chapter> &chapters,
                                            const QString &format, bool isStudy);

    static QJsonObject extractJsonObject(const QString &raw);
    static QString shelfCitationLabel(const QString &bookTitle, const QString &format, int chapter);

    // Resolves cross-book shelf context from semantic LibraryHits plus BM25 lexical fallback (any thread).
    static ShelfContextResult resolveShelfContext(const QList<CompanionStore::ShelfBook> &shelfBooks,
                                                  const QList<PassageIndex::LibraryHit> &libHits, const QString &asked,
                                                  const std::shared_ptr<std::atomic_bool> &cancel = {});

    // Builds a local-model-budgeted Ask context from ranked passages plus recent/current text.
    static LocalAskContext buildLocalAskContext(const QList<PassageIndex::Hit> &hits, const QList<Chapter> &chapters,
                                                const QString &format, bool wholeBook, int currentChapter,
                                                qint64 localBudget);

    // Builds a cloud Ask context (whole book / up to place) plus lexical source citations (any thread).
    static LocalAskContext buildCloudAskContext(const QList<Chapter> &chapters, const QString &asked,
                                                const QString &fallbackQuery, const QString &format, bool wholeBook,
                                                int currentChapter, qint64 limit);

    // Lexical source citation fallback matched against an answer when no pre-matched sources exist.
    static QList<CompanionStore::Citation> citationsForAnswer(const QList<Chapter> &chapters, const QString &format,
                                                              const QString &answer, int maxCount = 3);

    // Bullet list of non-empty chapter/section titles for recap & study brief prompts.
    static QString formatChapterTitles(const QList<Chapter> &chapters);

    // Shared system instruction wrapper and feature prompt/schema builders.
    static QString systemInstructions(const QString &task, const QString &language, bool spoilerFree = true,
                                      bool allowHeadings = false);
    static QString defineTaskPrompt(bool isStudy);
    static QString explainTaskPrompt(bool isStudy);
    static QString askTaskPrompt(bool isStudy, const QString &how);
    static QString shelfAskTaskPrompt(const QString &whereDesc);
    static QString recapTaskPrompt(bool isStudy);
    static QString whosWhoTaskPrompt(bool isStudy);
    static QString snipTaskPrompt(const QString &normAction, const QString &userLabel);
    static QJsonObject whosWhoSchema(bool isStudy);

    // Snip PNG decoding, downscaling (<=1600px), and thumbnail data URL encoding.
    static QString makeSnipThumbnailDataUrl(const QByteArray &pngBytes);
    static bool decodeAndNormalizeSnipBase64(const QString &base64Png, QByteArray *cleanPngOut, QString *errorOut);
};
