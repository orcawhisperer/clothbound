#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QRectF>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>

class BookScheme;
class EpubBook;
class PdfBook;
class Library;

// The book being read, between the window and the reading surface
// (reader.js, which reaches it as `bridge` over the web channel). It knows
// the book's structure, turns the surface's positions into progress through
// the whole book, saves where the reader is, and learns their pace.
class Reader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY bookChanged)
    Q_PROPERTY(bool isPdf READ isPdf NOTIFY bookChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(QString format READ format NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString author READ author NOTIFY bookChanged)
    Q_PROPERTY(QString language READ language NOTIFY bookChanged)
    Q_PROPERTY(QString detectedDoi READ detectedDoi NOTIFY bookChanged)
    Q_PROPERTY(QString detectedArxiv READ detectedArxiv NOTIFY bookChanged)
    Q_PROPERTY(QVariantMap manifest READ manifest NOTIFY bookChanged)
    Q_PROPERTY(QVariantList contents READ contents NOTIFY bookChanged)
    Q_PROPERTY(QVariantList chapterMarks READ chapterMarks NOTIFY lengthsChanged)
    Q_PROPERTY(QVariantMap startPosition READ startPosition NOTIFY bookChanged)
    Q_PROPERTY(bool hasPageList READ hasPageList NOTIFY bookChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY locationChanged)
    Q_PROPERTY(int chapterCount READ chapterCount NOTIFY bookChanged)
    Q_PROPERTY(QString chapterTitle READ chapterTitle NOTIFY locationChanged)
    Q_PROPERTY(int contentsIndex READ contentsIndex NOTIFY locationChanged)
    Q_PROPERTY(int page READ page NOTIFY locationChanged)
    Q_PROPERTY(int pages READ pages NOTIFY locationChanged)
    Q_PROPERTY(int bookPageStart READ bookPageStart NOTIFY locationChanged)
    Q_PROPERTY(int bookPageEnd READ bookPageEnd NOTIFY locationChanged)
    Q_PROPERTY(int bookPages READ bookPages NOTIFY locationChanged)
    Q_PROPERTY(int visibleColumns READ visibleColumns NOTIFY locationChanged)
    Q_PROPERTY(qint64 previousOpenedAt READ previousOpenedAt NOTIFY bookChanged)
    Q_PROPERTY(double progress READ progress NOTIFY locationChanged)
    Q_PROPERTY(QString pageLabel READ pageLabel NOTIFY locationChanged)
    // Where the page in view starts and ends in its chapter, and its first words.
    Q_PROPERTY(qint64 pageStart READ pageStart NOTIFY locationChanged)
    Q_PROPERTY(qint64 pageEnd READ pageEnd NOTIFY locationChanged)
    Q_PROPERTY(QString snippet READ snippet NOTIFY locationChanged)
    Q_PROPERTY(int minutesLeftInChapter READ minutesLeftInChapter NOTIFY locationChanged)
    Q_PROPERTY(int minutesLeftInBook READ minutesLeftInBook NOTIFY locationChanged)
    Q_PROPERTY(bool atStart READ atStart NOTIFY locationChanged)
    Q_PROPERTY(bool atEnd READ atEnd NOTIFY locationChanged)
    Q_PROPERTY(bool rtl READ rtl NOTIFY locationChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY locationChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY locationChanged)
    Q_PROPERTY(QString backLabel READ backLabel NOTIFY locationChanged)
    // Incremented only when the active surface reports a real location,
    // distinguishing post-load syncs from openBook()'s initial signal.
    Q_PROPERTY(int relocationSeq READ relocationSeq NOTIFY locationChanged)
    // Typographic controls & per-book appearance overrides (stored in book_settings).
    Q_PROPERTY(int fontWeight READ fontWeight WRITE setFontWeight NOTIFY appearanceChanged)
    Q_PROPERTY(double paragraphSpacing READ paragraphSpacing WRITE setParagraphSpacing NOTIFY appearanceChanged)
    Q_PROPERTY(bool bookAppearanceOverride READ bookAppearanceOverride WRITE setBookAppearanceOverride NOTIFY appearanceChanged)
    Q_PROPERTY(QVariantMap bookAppearance READ bookAppearance NOTIFY appearanceChanged)
    // A PDF: the surface's image source for its pages, and the crop that
    // cuts the margins the same on every page (parts of a page).
    Q_PROPERTY(QString pdfSource READ pdfSource NOTIFY bookChanged)
    Q_PROPERTY(QRectF pdfCrop READ pdfCrop NOTIFY pdfCropChanged)

public:
    Reader(Library *library, BookScheme *scheme, QObject *parent = nullptr);
    ~Reader() override;

    bool isOpen() const { return m_bookId > 0; }
    bool isPdf() const { return m_pdf != nullptr; }
    int bookId() const { return m_bookId; }
    QString format() const { return m_format; }
    QString title() const { return m_title; }
    QString author() const { return m_author; }
    QString language() const { return m_language; }
    QString detectedDoi() const { return m_detectedDoi; }
    QString detectedArxiv() const { return m_detectedArxiv; }
    // Academic citations ("bibtex" or "apa") for the open book or any library record.
    Q_INVOKABLE QString formatCitation(const QString &style) const;
    Q_INVOKABLE QString formatBookCitation(const QVariantMap &bookInfo, const QString &style) const;
    static QString formatCitationFor(const QVariantMap &bookInfo, const QString &style);
    qint64 pageStart() const { return m_offset; }
    qint64 pageEnd() const { return m_pageEnd; }
    QString snippet() const { return m_snippet; }
    // The open book's file.
    QString bookPath() const;
    QVariantMap manifest() const { return m_manifest; }
    QVariantList contents() const { return m_contents; }
    QVariantList chapterMarks() const;
    QVariantMap startPosition() const { return m_startPosition; }
    // Some of the book's text from a chapter on, past short pages (a title
    // page, the contents), up to `maxChars`: what the book sounds like.
    Q_INVOKABLE QString passageFrom(int chapter, int maxChars = 4000) const;
    // How many characters of text a chapter has.
    Q_INVOKABLE qint64 charactersIn(int chapter) const { return m_lengths.value(chapter); }
    Q_INVOKABLE void copyText(const QString &text) const;
    bool hasPageList() const { return m_hasPageList; }
    int chapter() const { return m_chapter; }
    int chapterCount() const { return int(m_chapterTitles.size()); }
    QString chapterTitle() const { return m_chapterTitles.value(m_chapter); }
    int contentsIndex() const;
    int page() const { return m_page; }
    int pages() const { return m_pages; }
    int bookPageStart() const { return m_bookPageStart; }
    int bookPageEnd() const { return m_bookPageEnd; }
    int bookPages() const { return m_bookPages; }
    int visibleColumns() const { return m_visibleColumns; }
    qint64 previousOpenedAt() const { return m_previousOpenedAt; }
    double progress() const { return m_progress; }
    QString pageLabel() const { return m_pageLabel; }
    int minutesLeftInChapter() const;
    int minutesLeftInBook() const;
    bool atStart() const { return m_chapter <= m_firstLinear && m_page <= 0; }
    bool atEnd() const { return m_atEnd; }
    bool rtl() const { return m_rtl; }
    bool canGoBack() const { return m_canGoBack; }
    bool canGoForward() const { return m_canGoForward; }
    QString backLabel() const { return m_backLabel; }
    int relocationSeq() const { return m_relocationSeq; }
    int fontWeight() const { return m_fontWeight; }
    void setFontWeight(int weight);
    double paragraphSpacing() const { return m_paragraphSpacing; }
    void setParagraphSpacing(double spacing);
    bool bookAppearanceOverride() const { return m_bookAppearanceOverride; }
    void setBookAppearanceOverride(bool enabled);
    QVariantMap bookAppearance() const { return m_bookAppearance; }
    Q_INVOKABLE void saveBookAppearance(const QVariantMap &appearance);
    Q_INVOKABLE void clearBookAppearance();

    Q_INVOKABLE bool openBook(int bookId);
    Q_INVOKABLE void close();
    // A place in the book for a fraction of the whole, for the scrubber and
    // "go to 40%": {chapter, offset}.
    Q_INVOKABLE QVariantMap targetForFraction(double fraction) const;
    // The place a printed page number starts, for "go to page 87".
    Q_INVOKABLE QVariantMap targetForPageLabel(const QString &label) const;
    Q_INVOKABLE QString errorString() const { return m_error; }
    Q_INVOKABLE QString titleOfChapter(int chapter) const { return m_chapterTitles.value(chapter); }

    // From the reading surface.
    Q_INVOKABLE void relocated(const QVariantMap &location);
    Q_INVOKABLE void surfaceLoaded();
    Q_INVOKABLE void tapped(const QString &zone);
    Q_INVOKABLE void openExternal(const QString &url);
    Q_INVOKABLE void keyPressed(const QString &key, int modifiers);
    Q_INVOKABLE void log(const QString &message);
    // Tactile paper-flip sound on page turns (throttled, headless-safe).
    Q_INVOKABLE void playPageTurn(int direction = 1);
    Q_INVOKABLE void notifyPageTurn(int direction = 1);
    // --- PDFs: a page is a chapter, its text's offsets the place on it -------
    QString pdfSource() const { return m_pdf ? QStringLiteral("image://pdf/") + m_token : QString(); }
    QRectF pdfCrop() const { return m_pdfCrop; }
    // A page's size in points: {width, height}.
    Q_INVOKABLE QVariantMap pdfPageSize(int page) const;
    // The words between two points (parts of the page): {chapter, start, end, text}.
    Q_INVOKABLE QVariantMap pdfSelect(int page, qreal x1, qreal y1, qreal x2, qreal y2) const;
    // The word under a point (parts of the page): {chapter, start, end, text}.
    Q_INVOKABLE QVariantMap pdfSelectWord(int page, qreal x, qreal y) const;
    // Clickable internal/external links on a page: [{x, y, width, height, page, destY, url}].
    Q_INVOKABLE QVariantList pdfLinks(int page) const;
    // High-DPI PNG crop of a normalized [0..1] page region, encoded as base64.
    Q_INVOKABLE QString snipPdfRegionBase64(int pageIndex, double normX, double normY, double normW, double normH) const;
    // Off-GUI-thread async PDF snip (emits pdfSnipReady when complete; returns request id or 0).
    Q_INVOKABLE int requestPdfSnip(int pageIndex, double normX, double normY, double normW, double normH,
                                   const QString &action = {}, const QString &customPrompt = {});
    // Zero-jump preview strip for an internal PDF link destination: {page, pageLabel, imageDataUrl, excerpt}.
    Q_INVOKABLE QVariantMap peekPdfDestination(int destPage, double destY = -1.0) const;
    // Off-GUI-thread async PDF destination peek (emits pdfPeekReady when complete; returns request id or 0).
    Q_INVOKABLE int requestPdfPeek(int destPage, double destY = -1.0);
    // Where a stretch of a page's text is: [{x, y, width, height}] in parts of the page.
    Q_INVOKABLE QVariantList pdfRects(int page, qint64 start, qint64 end) const;
    Q_INVOKABLE QString pdfPageLabel(int page) const;
    Q_INVOKABLE QString pdfPageText(int page) const;
    Q_INVOKABLE void pdfPrefetch(const QVariantList &pages, int pixelWidth, const QString &colorQuery);
    // Reads a page's paragraphs aloud (the surface asks for them).
    Q_INVOKABLE void pdfSpeakable(int page);

    // A chapter's blocks to read aloud, as JSON from the surface.
    Q_INVOKABLE void speakableReady(int chapter, const QString &blocks) { emit speakable(chapter, blocks); }
    Q_INVOKABLE void speakableReadyFor(qint64 session, int chapter, const QString &blocks);
    // What the reader has selected on the page ({} when nothing is).
    Q_INVOKABLE void selectionChanged(const QVariantMap &selection);
    Q_INVOKABLE void markClicked(int id, const QVariantMap &rect);
    Q_INVOKABLE void imageClicked(const QVariantMap &info);
    // The reader asked to listen from a paragraph.
    Q_INVOKABLE void listenFrom(int chapter, qint64 offset) { emit listenRequested(chapter, offset); }
    Q_INVOKABLE void listenFromSession(qint64 session, int chapter, qint64 offset);

    // Progress through the whole book at a chapter and an offset in it.
    double fractionAt(int chapter, qint64 offset) const;

signals:
    void bookChanged();
    void lengthsChanged();
    void locationChanged();
    void appearanceChanged();
    void jumpRecorded(const QString &label);
    void imageChosen(const QString &source, const QString &caption);
    void surfaceReady();
    void tap(const QString &zone);
    void externalLink(const QString &url);
    void key(const QString &key, int modifiers);
    void failed(const QString &message);
    void speakable(int chapter, const QString &blocks);
    void selected(const QVariantMap &selection);
    void markChosen(int id, const QVariantMap &rect);
    void listenRequested(int chapter, qint64 offset);
    void pageTurnRequested(int direction);
    void pdfCropChanged();
    void pdfSnipReady(int requestId, const QString &action, const QString &base64Png, int pageIndex,
                      const QString &customPrompt);
    void pdfPeekReady(int requestId, const QVariantMap &info);

private:
    friend class ClothboundTests;
    struct PageTurnSfx;
    bool playPageTurnDryRun(int direction, QByteArray *pcmOut = nullptr);
    void savePosition();
    void flushReadingTime();
    void measureChapters();
    void updateBookPages();
    void loadBookAppearance(int bookId);
    QString formatBackLabel(int chapter, const QString &pageLabel) const;
    qint64 globalOffset(int chapter, qint64 offset) const;

    Library *m_library;
    BookScheme *m_scheme;
    std::shared_ptr<EpubBook> m_book;
    std::shared_ptr<PdfBook> m_pdf;
    QRectF m_pdfCrop{0, 0, 1, 1};
    bool openPdf(int bookId);
    qint64 m_pageEnd = 0;
    QString m_token;
    int m_bookId = 0;
    QString m_format;
    QString m_title;
    QString m_author;
    QString m_language;
    QString m_detectedDoi;
    QString m_detectedArxiv;
    QString m_error;
    QVariantMap m_manifest;
    QVariantList m_contents;
    QVariantMap m_startPosition;
    QStringList m_chapterTitles;
    QList<int> m_contentsChapter;
    QList<int> m_lengths;
    QList<bool> m_linear;
    QList<QPair<int, QString>> m_pageTargets;
    bool m_hasPageList = false;
    int m_firstLinear = 0;
    int m_lastLinear = 0;
    qint64 m_session = 0;
    std::shared_ptr<std::atomic<qint64>> m_activeSession = std::make_shared<std::atomic<qint64>>(0);
    int m_relocationSeq = 0;

    int m_chapter = 0;
    qint64 m_offset = 0;
    QString m_snippet;
    int m_page = 0;
    int m_pages = 1;
    int m_bookPageStart = 1;
    int m_bookPageEnd = 1;
    int m_bookPages = 1;
    int m_visibleColumns = 1;
    int m_visiblePageEnd = -1;
    qint64 m_previousOpenedAt = 0;
    double m_progress = 0;
    QString m_pageLabel;
    bool m_atEnd = false;
    bool m_rtl = false;
    bool m_canGoBack = false;
    bool m_canGoForward = false;
    QString m_backLabel;
    int m_fontWeight = 400;
    double m_paragraphSpacing = 0.0;
    bool m_bookAppearanceOverride = false;
    QVariantMap m_bookAppearance;

    QTimer m_saveTimer;
    QElapsedTimer m_sinceTurn;
    qint64 m_lastGlobal = -1;
    int m_pendingSeconds = 0;
    qint64 m_pendingChars = 0;
    QTimer m_timeFlush;
    std::unique_ptr<PageTurnSfx> m_sfx;
    std::unique_ptr<QTemporaryDir> m_lightboxDir;
    qint64 m_lightboxSeq = 0;
    int m_asyncPdfReqId = 0;
    int m_latestPeekReqId = 0;
};
