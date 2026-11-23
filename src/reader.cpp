#include "reader.h"

#include "bookscheme.h"
#include "epubbook.h"
#include "library.h"

#include <QFutureWatcher>
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
    if (record.format != QStringLiteral("epub")) {
        m_error = tr("Reading PDFs arrives in the next version of Dogear.");
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
    m_format = record.format;
    m_title = record.title;
    m_author = record.author;
    m_language = book->metadata().language.isEmpty() ? record.language : book->metadata().language;

    const auto &spine = book->spine();
    QVariantList spineList;
    m_linear.clear();
    m_firstLinear = -1;
    for (int i = 0; i < spine.size(); ++i) {
        spineList.append(QVariantMap{
            {"href", QString::fromUtf8(QUrl::toPercentEncoding(spine[i].path, "/"))},
            {"linear", spine[i].linear},
        });
        m_linear.append(spine[i].linear);
        if (spine[i].linear && m_firstLinear < 0)
            m_firstLinear = i;
    }
    if (m_firstLinear < 0)
        m_firstLinear = 0;

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
    m_scheme->remove(m_token);
    m_book.reset();
    m_token.clear();
    m_bookId = 0;
    m_format.clear();
    m_manifest.clear();
    m_contents.clear();
    m_chapterTitles.clear();
    m_lengths.clear();
    m_pageTargets.clear();
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
    const double pace = m_library->charsPerMinute();
    const qint64 length = m_lengths.value(m_chapter, 0);
    if (length <= 0 || pace <= 0)
        return 0;
    return qMax(1, int(std::ceil((length - qMin(m_offset, length)) / pace)));
}

int Reader::minutesLeftInBook() const {
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
    m_chapter = location.value("chapter").toInt();
    m_offset = qMax<qint64>(0, location.value("offset").toLongLong());
    m_snippet = location.value("snippet").toString();
    m_page = location.value("page").toInt();
    m_pages = qMax(1, location.value("pages").toInt());
    m_pageLabel = location.value("pageLabel").toString();
    const bool lastChapter = m_chapter >= chapterCount() - 1;
    m_atEnd = location.value("atEnd").toBool() && lastChapter;
    m_progress = m_atEnd ? 1.0 : fractionAt(m_chapter, m_offset);

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

void Reader::log(const QString &message) {
    if (qEnvironmentVariableIsSet("DOGEAR_DEBUG"))
        fprintf(stderr, "reader: %s\n", qPrintable(message));
}

QString Reader::passageFrom(int chapter, int maxChars) const {
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
