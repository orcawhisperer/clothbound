#include "reader.h"

#include "bookscheme.h"
#include "epubbook.h"
#include "library.h"
#include "pdfbook.h"
#include "pdfpages.h"

#include <QClipboard>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtConcurrentRun>
#include <cmath>
#include <cstdio>

Reader::Reader(Library *library, BookScheme *scheme, QObject *parent)
    : QObject(parent), m_library(library), m_scheme(scheme) {
    // Positions are saved shortly after the reader settles on a page, so a
    // burst of page turns writes once.
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Reader::savePosition);
    m_timeFlush.setInterval(60000);
    connect(&m_timeFlush, &QTimer::timeout, this, &Reader::flushReadingTime);
}

Reader::~Reader() { close(); }

bool Reader::openBook(int bookId) {
    const BookRecord record = m_library->record(bookId);
    if (!record.id) {
        m_error = tr("That book isn't in the library.");
        return false;
    }
    if (record.format == QStringLiteral("pdf"))
        return openPdf(bookId);
    if (record.format != QStringLiteral("epub")) {
        m_error = tr("Dogear reads EPUB and PDF books.");
        return false;
    }
    auto book = std::make_shared<EpubBook>(record.path);
    if (!book->open()) {
        m_error = book->errorString();
        return false;
    }
    if (book->isDrmProtected()) {
        m_error = tr("This book is protected by DRM, so it can't be opened.");
        return false;
    }
    close();

    m_book = book;
    m_token = m_scheme->add(book);
    m_bookId = bookId;
    ++m_session;
    m_format = record.format;
    m_title = record.title;
    m_author = record.author;
    m_language = book->metadata().language.isEmpty() ? record.language : book->metadata().language;

    const auto &spine = book->spine();
    QVariantList spineList;
    m_linear.clear();
    m_firstLinear = -1;
    m_lastLinear = -1;
    for (int i = 0; i < spine.size(); ++i) {
        spineList.append(QVariantMap{
            {"href", QString::fromUtf8(QUrl::toPercentEncoding(spine[i].path, "/"))},
            {"linear", spine[i].linear},
        });
        m_linear.append(spine[i].linear);
        if (spine[i].linear) {
            if (m_firstLinear < 0)
                m_firstLinear = i;
            m_lastLinear = i;
        }
    }
    if (m_firstLinear < 0)
        m_firstLinear = 0;
    if (m_lastLinear < 0)
        m_lastLinear = qMax(0, int(spine.size()) - 1);

    // Each chapter is titled by the first contents entry that points into
    // it; chapters the contents skip carry the title before them.
    m_chapterTitles = QStringList(spine.size(), QString());
    m_contents.clear();
    m_contentsChapter.clear();
    for (const auto &entry : book->toc()) {
        m_contents.append(QVariantMap{
            {"title", entry.title}, {"depth", entry.depth}, {"chapter", entry.spine}, {"fragment", entry.fragment}});
        m_contentsChapter.append(entry.spine);
        if (entry.spine >= 0 && entry.spine < m_chapterTitles.size() && m_chapterTitles[entry.spine].isEmpty())
            m_chapterTitles[entry.spine] = entry.title;
    }
    for (int i = 1; i < m_chapterTitles.size(); ++i)
        if (m_chapterTitles[i].isEmpty())
            m_chapterTitles[i] = m_chapterTitles[i - 1];

    QVariantList pageTargets;
    m_pageTargets.clear();
    for (const auto &target : book->pageList()) {
        if (target.spine < 0)
            continue;
        pageTargets.append(QVariantMap{{"chapter", target.spine}, {"label", target.label}, {"fragment", target.fragment}});
        m_pageTargets.append({target.spine, target.label});
    }
    m_hasPageList = !m_pageTargets.isEmpty();

    m_manifest = QVariantMap{
        {"base", BookScheme::baseUrl(m_token)},
        {"spine", spineList},
        {"pageTargets", pageTargets},
        {"language", m_language},
        {"rtl", book->isRightToLeft()},
        {"title", m_title},
        {"session", double(m_session)},
    };

    m_lengths = m_library->chapterLengths(bookId);
    if (m_lengths.size() != spine.size()) {
        m_lengths.clear();
        measureChapters();
    }

    // Where the reader left off, or the first page.
    m_startPosition = QVariantMap{{"chapter", 0}, {"offset", 0}};
    const QJsonObject saved = QJsonDocument::fromJson(record.position.toUtf8()).object();
    if (saved.contains("chapter")) {
        const int chapter = saved.value("chapter").toInt();
        if (chapter >= 0 && chapter < spine.size())
            m_startPosition = QVariantMap{{"chapter", chapter},
                                          {"offset", saved.value("offset").toDouble()},
                                          {"snippet", saved.value("snippet").toString()}};
    }
    m_chapter = m_startPosition.value("chapter").toInt();
    m_offset = m_startPosition.value("offset").toLongLong();
    m_pageEnd = 0;
    m_snippet.clear();
    m_relocationSeq = 0;
    m_page = 0;
    m_pages = 1;
    m_progress = record.progress;
    m_pageLabel.clear();
    m_atEnd = false;

    m_library->markOpened(bookId);
    m_lastGlobal = -1;
    m_sinceTurn.start();
    m_timeFlush.start();
    emit bookChanged();
    emit locationChanged();
    return true;
}

void Reader::close() {
    if (!m_bookId)
        return;
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        savePosition();
    }
    flushReadingTime();
    m_timeFlush.stop();
    ++m_session;
    m_scheme->remove(m_token);
    PdfPages::remove(m_token);
    m_book.reset();
    m_pdf.reset();
    m_pdfCrop = QRectF(0, 0, 1, 1);
    m_token.clear();
    m_bookId = 0;
    m_format.clear();
    m_manifest.clear();
    m_contents.clear();
    m_chapterTitles.clear();
    m_lengths.clear();
    m_pageTargets.clear();
    m_hasPageList = false;
    m_firstLinear = 0;
    m_lastLinear = 0;
    m_chapter = 0;
    m_offset = 0;
    m_pageEnd = 0;
    m_snippet.clear();
    m_relocationSeq = 0;
    m_page = 0;
    m_pages = 1;
    m_progress = 0;
    m_pageLabel.clear();
    m_atEnd = false;
    emit bookChanged();
}

void Reader::measureChapters() {
    // Books added outside a scan haven't had their chapters measured yet;
    // the reader can start meanwhile, with progress by chapters.
    auto book = m_book;
    const int bookId = m_bookId;
    auto *watcher = new QFutureWatcher<QList<int>>(this);
    connect(watcher, &QFutureWatcher<QList<int>>::finished, this, [this, watcher, bookId] {
        watcher->deleteLater();
        const QList<int> lengths = watcher->result();
        m_library->setChapterLengths(bookId, lengths);
        if (bookId != m_bookId)
            return;
        m_lengths = lengths;
        m_progress = fractionAt(m_chapter, m_offset);
        emit lengthsChanged();
        emit locationChanged();
    });
    watcher->setFuture(QtConcurrent::run([book] {
        QList<int> lengths;
        for (int i = 0; i < book->spine().size(); ++i)
            lengths.append(int(book->chapterText(i).size()));
        return lengths;
    }));
}

qint64 Reader::globalOffset(int chapter, qint64 offset) const {
    qint64 before = 0;
    for (int i = 0; i < chapter && i < m_lengths.size(); ++i)
        before += m_lengths[i];
    const qint64 length = m_lengths.value(chapter, 0);
    return before + qBound<qint64>(0, offset, length > 0 ? length : offset);
}

double Reader::fractionAt(int chapter, qint64 offset) const {
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0) {
        const int count = qMax(1, chapterCount());
        return qBound(0.0, (chapter + double(m_page) / qMax(1, m_pages)) / count, 1.0);
    }
    return qBound(0.0, double(globalOffset(chapter, offset)) / double(total), 1.0);
}

QVariantMap Reader::targetForFraction(double fraction) const {
    fraction = qBound(0.0, fraction, 1.0);
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0) {
        const int chapter = qMin(chapterCount() - 1, int(fraction * chapterCount()));
        return {{"chapter", qMax(0, chapter)}, {"offset", 0}};
    }
    qint64 wanted = qint64(fraction * total), before = 0;
    for (int i = 0; i < m_lengths.size(); ++i) {
        if (wanted < before + m_lengths[i] || i == m_lengths.size() - 1)
            return {{"chapter", i}, {"offset", double(qMax<qint64>(0, wanted - before))}};
        before += m_lengths[i];
    }
    return {{"chapter", 0}, {"offset", 0}};
}

QVariantMap Reader::targetForPageLabel(const QString &label) const {
    const QString wanted = label.trimmed();
    for (const auto &[chapter, pageLabel] : m_pageTargets)
        if (pageLabel.compare(wanted, Qt::CaseInsensitive) == 0)
            return {{"chapter", chapter}, {"pageLabel", pageLabel}};
    // A number the page list skips lands on the nearest page before it.
    bool ok = false;
    const int number = wanted.toInt(&ok);
    if (!ok)
        return {};
    int bestChapter = -1, bestNumber = -1;
    QString bestLabel;
    for (const auto &[chapter, pageLabel] : m_pageTargets) {
        bool isNumber = false;
        const int n = pageLabel.toInt(&isNumber);
        if (isNumber && n <= number && n > bestNumber) {
            bestNumber = n;
            bestChapter = chapter;
            bestLabel = pageLabel;
        }
    }
    if (bestChapter < 0)
        return {};
    return {{"chapter", bestChapter}, {"pageLabel", bestLabel}};
}

QVariantList Reader::chapterMarks() const {
    QVariantList marks;
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0)
        return marks;
    qint64 before = 0;
    for (int i = 0; i < m_lengths.size(); ++i) {
        // Only chapters the contents name, and not too close together.
        const double at = double(before) / double(total);
        if (i > 0 && m_contentsChapter.contains(i) && (marks.isEmpty() || at - marks.last().toDouble() > 0.004))
            marks.append(at);
        before += m_lengths[i];
    }
    return marks;
}

int Reader::contentsIndex() const {
    int found = -1;
    for (int i = 0; i < m_contentsChapter.size(); ++i)
        if (m_contentsChapter[i] >= 0 && m_contentsChapter[i] <= m_chapter)
            found = i;
    return found;
}

int Reader::minutesLeftInChapter() const {
    if (m_atEnd)
        return 0;
    const double pace = m_library->charsPerMinute();
    const qint64 length = m_lengths.value(m_chapter, 0);
    if (length <= 0 || pace <= 0)
        return 0;
    return qMax(1, int(std::ceil((length - qMin(m_offset, length)) / pace)));
}

int Reader::minutesLeftInBook() const {
    if (m_atEnd)
        return 0;
    const double pace = m_library->charsPerMinute();
    qint64 total = 0;
    for (const int length : m_lengths)
        total += length;
    if (total <= 0 || pace <= 0)
        return 0;
    return qMax(1, int(std::ceil((total - globalOffset(m_chapter, m_offset)) / pace)));
}

void Reader::relocated(const QVariantMap &location) {
    if (!m_bookId)
        return;
    if (location.contains(QStringLiteral("session")) && location.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    m_chapter = location.value("chapter").toInt();
    m_offset = qMax<qint64>(0, location.value("offset").toLongLong());
    m_snippet = location.value("snippet").toString();
    m_pageEnd = qMax(m_offset, location.value("end", m_offset).toLongLong());
    m_page = location.value("page").toInt();
    m_pages = qMax(1, location.value("pages").toInt());
    m_pageLabel = location.value("pageLabel").toString();
    // Keep startPosition current so a WebEngine crash reload resumes here.
    m_startPosition = QVariantMap{{"chapter", m_chapter}, {"offset", double(m_offset)}, {"snippet", m_snippet}};
    const bool nearEnd = m_pdf ? (m_chapter >= qMax(0, chapterCount() - 2)) : (m_chapter >= m_lastLinear);
    m_atEnd = location.value("atEnd").toBool() && nearEnd;
    m_progress = m_atEnd ? 1.0 : fractionAt(m_chapter, m_offset);
    ++m_relocationSeq;

    // Reading pace comes from steady forward reading: the time spent on a
    // page and how much text it held. Jumps and quick flips don't count.
    const qint64 global = globalOffset(m_chapter, m_offset);
    const qint64 seconds = m_sinceTurn.restart() / 1000;
    if (m_lastGlobal >= 0 && seconds >= 2 && seconds <= 600) {
        const qint64 delta = global - m_lastGlobal;
        m_pendingSeconds += int(seconds);
        if (delta > 0 && delta < 20000)
            m_pendingChars += delta;
    }
    m_lastGlobal = global;
    m_saveTimer.start();
    emit locationChanged();
}

void Reader::savePosition() {
    if (!m_bookId)
        return;
    const QJsonObject position{{"chapter", m_chapter}, {"offset", double(m_offset)}, {"snippet", m_snippet}};
    m_library->savePosition(m_bookId, QString::fromUtf8(QJsonDocument(position).toJson(QJsonDocument::Compact)),
                            m_progress);
    if (m_atEnd && m_library->record(m_bookId).finishedAt == 0)
        m_library->setFinished(m_bookId, true);
}

void Reader::flushReadingTime() {
    if (!m_bookId || m_pendingSeconds <= 0)
        return;
    m_library->addReadingTime(m_bookId, m_pendingSeconds, m_pendingChars);
    m_pendingSeconds = 0;
    m_pendingChars = 0;
}

void Reader::surfaceLoaded() { emit surfaceReady(); }
void Reader::tapped(const QString &zone) { emit tap(zone); }

void Reader::openExternal(const QString &url) {
    const QUrl link(url);
    if (link.scheme() == QStringLiteral("http") || link.scheme() == QStringLiteral("https") ||
        link.scheme() == QStringLiteral("mailto"))
        emit externalLink(url);
}

void Reader::keyPressed(const QString &key, int modifiers) { emit this->key(key, modifiers); }

void Reader::speakableReadyFor(qint64 session, int chapter, const QString &blocks) {
    if (session != m_session || !m_bookId)
        return;
    emit speakable(chapter, blocks);
}

void Reader::selectionChanged(const QVariantMap &selection) {
    if (selection.contains(QStringLiteral("session")) && selection.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    emit selected(selection);
}

void Reader::markClicked(int id, const QVariantMap &rect) {
    if (rect.contains(QStringLiteral("session")) && rect.value(QStringLiteral("session")).toLongLong() != m_session)
        return;
    emit markChosen(id, rect);
}

void Reader::listenFromSession(qint64 session, int chapter, qint64 offset) {
    if (session != m_session || !m_bookId)
        return;
    emit listenRequested(chapter, offset);
}

void Reader::log(const QString &message) {
    if (qEnvironmentVariableIsSet("DOGEAR_DEBUG"))
        fprintf(stderr, "reader: %s\n", qPrintable(message));
}

QString Reader::passageFrom(int chapter, int maxChars) const {
    if (m_pdf) {
        QString text;
        for (int page = qMax(0, chapter); page < m_pdf->pageCount() && text.size() < maxChars; ++page) {
            const QString words = m_pdf->pageText(page).simplified();
            if (words.size() > 600)
                text += words.left(maxChars - text.size()) + QStringLiteral("\n\n");
        }
        return text.trimmed();
    }
    if (!m_book)
        return {};
    QString text;
    const QVariantList spine = m_manifest.value(QStringLiteral("spine")).toList();
    for (int i = qMax(0, chapter); i < spine.size() && text.size() < maxChars; ++i) {
        const QString words = m_book->chapterText(i).simplified();
        if (words.size() < 1200)
            continue;
        text += words.left(maxChars - text.size()) + QStringLiteral("\n\n");
    }
    return text.trimmed();
}

QString Reader::bookPath() const { return m_book ? m_book->filePath() : m_pdf ? m_pdf->path() : QString(); }

void Reader::copyText(const QString &text) const {
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

// --- PDFs ----------------------------------------------------------------------

bool Reader::openPdf(int bookId) {
    const BookRecord record = m_library->record(bookId);
    auto book = std::make_shared<PdfBook>(record.path);
    if (!book->open()) {
        m_error = book->errorString();
        return false;
    }
    const int pages = book->pageCount();
    if (pages <= 0) {
        m_error = tr("This PDF has no pages.");
        return false;
    }
    close();
    m_pdf = book;
    m_bookId = bookId;
    ++m_session;
    m_token = QStringLiteral("p%1-%2").arg(bookId).arg(QDateTime::currentMSecsSinceEpoch());
    PdfPages::add(m_token, book);
    m_format = record.format;
    m_title = record.title;
    m_author = record.author;
    m_language = record.language;

    // Each page is titled by the outline entry it falls under.
    m_chapterTitles = QStringList(pages, QString());
    m_contents.clear();
    m_contentsChapter.clear();
    for (const PdfOutlineEntry &entry : book->outline()) {
        m_contents.append(QVariantMap{{"title", entry.title}, {"depth", entry.depth}, {"chapter", entry.page}, {"fragment", QString()}});
        m_contentsChapter.append(entry.page);
        if (entry.page >= 0 && entry.page < pages && m_chapterTitles[entry.page].isEmpty())
            m_chapterTitles[entry.page] = entry.title;
    }
    for (int i = 1; i < pages; ++i)
        if (m_chapterTitles[i].isEmpty())
            m_chapterTitles[i] = m_chapterTitles[i - 1];
    QVariantList spine;
    m_pageTargets.clear();
    m_pageTargets.reserve(pages);
    for (int i = 0; i < pages; ++i) {
        spine.append(QVariantMap{{"linear", true}});
        const QString label = book->pageLabel(i).trimmed();
        m_pageTargets.append({i, label.isEmpty() ? QString::number(i + 1) : label});
    }
    m_linear = QList<bool>(pages, true);
    m_firstLinear = 0;
    m_lastLinear = pages - 1;
    m_hasPageList = !m_pageTargets.isEmpty();
    m_manifest = QVariantMap{{"format", "pdf"}, {"spine", spine}, {"pages", pages}, {"title", m_title},
                             {"language", m_language}, {"session", double(m_session)}};

    // Progress by text: each page's length, measured once and kept.
    m_lengths = m_library->chapterLengths(bookId);
    const QString path = record.path;
    if (m_lengths.size() != pages) {
        m_lengths = QList<int>(pages, 1500);
        auto *watcher = new QFutureWatcher<QList<int>>(this);
        connect(watcher, &QFutureWatcher<QList<int>>::finished, this, [this, watcher, bookId] {
            watcher->deleteLater();
            const QList<int> lengths = watcher->result();
            m_library->setChapterLengths(bookId, lengths);
            if (bookId != m_bookId)
                return;
            m_lengths = lengths;
            m_progress = fractionAt(m_chapter, m_offset);
            emit lengthsChanged();
            emit locationChanged();
        });
        watcher->setFuture(QtConcurrent::run([path] {
            PdfBook own(path);
            QList<int> lengths;
            if (own.open())
                for (int page = 0; page < own.pageCount(); ++page)
                    lengths.append(qMax(1, int(own.pageText(page).size())));
            return lengths;
        }));
    }
    // The margins to cut: restored from book_settings when the file hasn't
    // changed, or measured once in the background and saved.
    m_pdfCrop = QRectF(0, 0, 1, 1);
    bool cropCached = false;
    const qint64 mtime = record.mtime;
    const QString savedCrop = m_library->bookSetting(bookId, QStringLiteral("pdf_crop"));
    if (!savedCrop.isEmpty()) {
        const QStringList parts = savedCrop.split(QLatin1Char(':'));
        if (parts.size() == 2 && parts[0].toLongLong() == mtime) {
            const QStringList nums = parts[1].split(QLatin1Char(','));
            if (nums.size() == 4) {
                bool ok0 = false, ok1 = false, ok2 = false, ok3 = false;
                const double cx = nums[0].toDouble(&ok0), cy = nums[1].toDouble(&ok1);
                const double cw = nums[2].toDouble(&ok2), ch = nums[3].toDouble(&ok3);
                if (ok0 && ok1 && ok2 && ok3 && cx >= 0.0 && cy >= 0.0 && cw >= 0.4 && ch >= 0.4 && cx + cw <= 1.001 &&
                    cy + ch <= 1.001) {
                    m_pdfCrop = QRectF(cx, cy, qMin(cw, 1.0 - cx), qMin(ch, 1.0 - cy));
                    cropCached = true;
                }
            }
        }
    }
    if (!cropCached) {
        auto *crop = new QFutureWatcher<QRectF>(this);
        connect(crop, &QFutureWatcher<QRectF>::finished, this, [this, crop, bookId, mtime] {
            crop->deleteLater();
            const QRectF result = crop->result();
            m_library->setBookSetting(
                bookId, QStringLiteral("pdf_crop"),
                QStringLiteral("%1:%2,%3,%4,%5")
                    .arg(mtime)
                    .arg(result.x(), 0, 'f', 4)
                    .arg(result.y(), 0, 'f', 4)
                    .arg(result.width(), 0, 'f', 4)
                    .arg(result.height(), 0, 'f', 4));
            if (bookId != m_bookId)
                return;
            m_pdfCrop = result;
            emit pdfCropChanged();
        });
        crop->setFuture(QtConcurrent::run([path] {
            PdfBook own(path);
            return own.open() ? PdfPages::bookCrop(own) : QRectF(0, 0, 1, 1);
        }));
    }

    m_startPosition = QVariantMap{{"chapter", 0}, {"offset", 0}};
    const QJsonObject saved = QJsonDocument::fromJson(record.position.toUtf8()).object();
    if (saved.contains("chapter")) {
        const int page = saved.value("chapter").toInt();
        if (page >= 0 && page < pages)
            m_startPosition = QVariantMap{{"chapter", page}, {"offset", saved.value("offset").toDouble()}};
    }
    m_chapter = m_startPosition.value("chapter").toInt();
    m_offset = m_startPosition.value("offset").toLongLong();
    m_pageEnd = 0;
    m_snippet.clear();
    m_relocationSeq = 0;
    m_page = 0;
    m_pages = 1;
    m_progress = record.progress;
    m_pageLabel.clear();
    m_atEnd = false;
    m_library->markOpened(bookId);
    m_lastGlobal = -1;
    m_sinceTurn.start();
    m_timeFlush.start();
    emit bookChanged();
    emit locationChanged();
    emit pdfCropChanged();
    return true;
}

QVariantMap Reader::pdfPageSize(int page) const {
    const QSizeF size = m_pdf ? m_pdf->pageSize(page) : QSizeF();
    return {{"width", size.width()}, {"height", size.height()}};
}

QVariantMap Reader::pdfSelect(int page, qreal x1, qreal y1, qreal x2, qreal y2) const {
    if (!m_pdf)
        return {};
    const PdfBook::Selection selection = m_pdf->select(page, QPointF(x1, y1), QPointF(x2, y2));
    if (selection.start < 0)
        return {};
    // Line breaks are the page's, not the words': copied and noted, they're spaces.
    QString text = selection.text;
    text.replace(QString(QChar(0xFFFE)) + QLatin1String("\r\n"), QString())
        .replace(QLatin1String("\r\n"), QLatin1String(" "));
    return {{"chapter", page}, {"start", selection.start}, {"end", selection.end}, {"text", text.simplified()}};
}

QVariantList Reader::pdfRects(int page, qint64 start, qint64 end) const {
    QVariantList out;
    if (!m_pdf)
        return out;
    for (const QRectF &rect : m_pdf->textRects(page, start, end))
        out.append(QVariantMap{{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}});
    return out;
}

QString Reader::pdfPageLabel(int page) const { return m_pdf ? m_pdf->pageLabel(page) : QString(); }

QString Reader::pdfPageText(int page) const { return m_pdf ? m_pdf->pageText(page) : QString(); }

void Reader::pdfSpeakable(int page) {
    if (!m_pdf)
        return;
    const QJsonArray blocks = PdfBook::speakable(m_pdf->pageText(page), m_title);
    const QString json = QString::fromUtf8(QJsonDocument(blocks).toJson(QJsonDocument::Compact));
    const qint64 session = m_session;
    // Deliver asynchronously so consecutive blank/scanned PDF pages never
    // recurse synchronously through Narration::addBlocks() -> blocksWanted().
    QMetaObject::invokeMethod(
        this,
        [this, session, page, json] {
            if (session == m_session && m_pdf)
                emit speakable(page, json);
        },
        Qt::QueuedConnection);
}
