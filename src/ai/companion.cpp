#include "ai/companion.h"

#include "ai/aiservices.h"
#include "ai/aiusage.h"
#include "ai/localmodels.h"
#include "reader/reader.h"

#include <QCoreApplication>
#include <QDate>
#include <QLocale>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <cmath>
#include <utility>

namespace {

// Gemini reads up to this much of the book so far (about 300,000 tokens);
// a model on this computer, a chapter's worth (further bounded by actual num_ctx).
constexpr qint64 cloudLimit = 1200000;
constexpr qint64 localLimit = 24000;
constexpr qint64 localWhosWhoLimit = 16000;

QSettings companionSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}

QString instructions(const QString &task, bool spoilerFree = true, bool allowHeadings = false) {
    return BookContextReader::systemInstructions(task, Companion::readerLanguage(), spoilerFree, allowHeadings);
}

}  // namespace

Companion::Companion(AiServices *ai, Reader *reader, const QString &databaseConnection, QObject *parent)
    : QObject(parent),
      m_ai(ai),
      m_reader(reader),
      m_store(databaseConnection),
      m_index(ai, databaseConnection),
      m_dictionary(this) {
    m_targetLanguage = companionSettings().value(QStringLiteral("companion/translateLanguage")).toString().trimmed();
    m_streamFlush.setSingleShot(true);
    m_streamFlush.setInterval(33);
    connect(&m_streamFlush, &QTimer::timeout, this, &Companion::flushStreamedAnswer);
    m_store.ensureTables();
    if (m_reader && m_reader->bookId() > 0)
        loadBookState(m_reader->bookId());

    connect(reader, &Reader::bookChanged, this, [this] {
        stop();
        m_feature.clear();
        m_subject.clear();
        m_answer.clear();
        resetPeopleState();
        resetRecapState();
        resetSelectionState();
        m_conversation.clear();
        m_currentSources.clear();
        m_pendingUserTurn = false;
        m_pendingSnipPng.clear();
        m_pendingSnipUrl.clear();
        m_pendingSnipPage = -1;
        m_problem.clear();
        m_problemIsConfig = false;
        m_lastAction = {};
        m_pendingSetup = {};
        m_readingMode = QStringLiteral("fiction");
        m_scope = QStringLiteral("upto");
        m_shelfLabel.clear();
        if (m_reader && m_reader->bookId() > 0)
            loadBookState(m_reader->bookId());
        emit changed();
        emit answerChanged();
        emit conversationChanged();
        emit recapChanged();
        emit selectionChanged();
        emit readingModeChanged();
        emit scopeChanged();
        emit pendingSnipChanged();
    });

    connect(reader, &Reader::locationChanged, this, [this] {
        if (!m_reader || m_reader->bookId() <= 0 || wholeBookStudyActive())
            return;
        const int bookId = m_reader->bookId();
        const int ch = m_reader->chapter();
        const qint64 end = m_reader->pageEnd();
        const qint64 start = m_reader->pageStart();
        // Cancel an in-flight chapter-scoped request (including Ask) only if the reader truly
        // navigated backward (not when pageEnd shrinks during a 2-column -> 1-column dock resize).
        if (m_busy &&
            (m_feature == QLatin1String("whoswho") || m_feature == QLatin1String("recap") ||
             m_feature == QLatin1String("ask")) &&
            m_requestChapter >= 0) {
            const bool movedBack =
                ch < m_requestChapter || (ch == m_requestChapter && m_requestStart > 0 && start < m_requestStart);
            if (movedBack) {
                if (m_feature == QLatin1String("ask"))
                    m_answer.clear();  // never commit a spoiler-tainted partial stream
                stop();
            }
        }
        for (const Turn &turn : std::as_const(m_conversation)) {
            if (turn.chapter >= 0) {
                emit conversationChanged();
                break;
            }
        }
        bool changedAny = false;
        bool recapUpdated = false;
        if (m_peopleLoaded) {
            const bool aheadOfReader =
                m_peopleChapter > ch || (m_peopleChapter == ch && end > 0 && m_peopleOffset > end);
            if (aheadOfReader) {
                if (!loadWhosWhoSnapshot(bookId, ch, end, start, false)) {
                    resetPeopleState();
                } else {
                    m_where = m_peopleWhere;
                    m_whereLocal = m_peopleWhereLocal;
                }
                changedAny = true;
            }
        }
        if ((!m_peopleLoaded || m_peopleChapter < ch) && !(m_busy && m_feature == QLatin1String("whoswho"))) {
            if (loadWhosWhoSnapshot(bookId, ch, end, start, true)) {
                if (m_feature == QLatin1String("whoswho")) {
                    m_where = m_peopleWhere;
                    m_whereLocal = m_peopleWhereLocal;
                }
                changedAny = true;
            }
        }
        if (m_recapLoaded) {
            const bool recapAhead =
                m_recapChapter > ch || (m_recapChapter == ch && end > 0 && m_recapOffset > end);
            if (recapAhead) {
                if (!loadRecapSnapshot(bookId, ch, end, start, false))
                    resetRecapState();
                if (m_feature == QLatin1String("recap") && !m_busy) {
                    m_answer = m_recapText;
                    m_where = m_recapWhere;
                    m_whereLocal = m_recapWhereLocal;
                    emit answerChanged();
                }
                recapUpdated = true;
                changedAny = true;
            }
        }
        if ((!m_recapLoaded || m_recapChapter < ch) && !(m_busy && m_feature == QLatin1String("recap"))) {
            if (loadRecapSnapshot(bookId, ch, end, start, true)) {
                if (m_feature == QLatin1String("recap")) {
                    m_answer = m_recapText;
                    m_where = m_recapWhere;
                    m_whereLocal = m_recapWhereLocal;
                    emit answerChanged();
                }
                recapUpdated = true;
                changedAny = true;
            }
        }
        if (recapUpdated)
            emit recapChanged();
        if (changedAny)
            emit changed();
    });
}

Companion::~Companion() { stop(); }

void Companion::resetPeopleState() {
    m_people.clear();
    m_peopleLoaded = false;
    m_peopleChapter = -1;
    m_peopleOffset = -1;
    m_peopleKey.clear();
    m_peopleWhere.clear();
    m_peopleWhereLocal = false;
}

void Companion::resetRecapState() {
    m_recapText.clear();
    m_recapLoaded = false;
    m_recapChapter = -1;
    m_recapOffset = -1;
    m_recapWhere.clear();
    m_recapWhereLocal = false;
}

void Companion::resetSelectionState() {
    m_selectionFeature.clear();
    m_selectionSubject.clear();
    m_selectionAnswer.clear();
    m_selectionWhere.clear();
    m_selectionWhereLocal = false;
}

QString Companion::detectReadingMode(const QString &format, const QString &title, const QStringList &tocTitles,
                                     const QString &sampleText) {
    return BookContextReader::detectReadingMode(format, title, tocTitles, sampleText);
}

QString Companion::detectCurrentBookMode() const {
    if (!m_reader || m_reader->bookId() <= 0)
        return QStringLiteral("fiction");
    const QString format = m_reader->format();
    if (format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("study");

    QStringList tocTitles;
    for (const QVariant &entry : m_reader->contents()) {
        const QString t = entry.toMap().value(QStringLiteral("title")).toString().trimmed();
        if (!t.isEmpty())
            tocTitles.append(t);
    }
    QString sample = m_reader->passageFrom(0, 6000);
    if (sample.isEmpty())
        sample = BookContextReader::readOpeningSample(m_reader->bookPath(), format, 6000);
    return detectReadingMode(format, m_reader->title(), tocTitles, sample);
}

void Companion::refreshShelfLabel() {
    QString label;
    if (m_reader && m_reader->bookId() > 0)
        (void)m_store.shelfBooksFor(m_reader->bookId(), &label);
    m_shelfLabel = label;
}

void Companion::setReadingMode(const QString &mode) {
    Q_ASSERT(QThread::currentThread() == thread());
    QString normalized = mode.trimmed().toLower();
    if (normalized == QLatin1String("story"))
        normalized = QStringLiteral("fiction");
    if (normalized != QLatin1String("fiction") && normalized != QLatin1String("study"))
        return;
    const bool modeChanged = (normalized != m_readingMode);
    if (m_reader && m_reader->bookId() > 0)
        m_store.saveReadingMode(m_reader->bookId(), normalized);
    if (!modeChanged)
        return;

    if (m_busy && (m_feature == QLatin1String("recap") || m_feature == QLatin1String("whoswho")))
        stop();

    m_readingMode = normalized;
    const QString targetScope = studyMode() ? QStringLiteral("book") : QStringLiteral("upto");
    const bool scopeDidChange = (m_scope != targetScope);
    m_scope = targetScope;

    // Reload mode-specific Recap/Brief and Who's Who/Glossary snapshots so modes never collide.
    resetPeopleState();
    resetRecapState();

    if (m_reader && m_reader->bookId() > 0) {
        const int bookId = m_reader->bookId();
        loadWhosWhoSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        loadRecapSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        if (m_feature == QLatin1String("recap") && !m_busy) {
            m_answer = m_recapText;
            m_where = m_recapWhere;
            m_whereLocal = m_recapWhereLocal;
        } else if (m_feature == QLatin1String("whoswho") && !m_busy) {
            m_where = m_peopleWhere;
            m_whereLocal = m_peopleWhereLocal;
        }
    }

    emit readingModeChanged();
    if (scopeDidChange)
        emit scopeChanged();
    emit recapChanged();
    emit answerChanged();
    emit changed();
}

void Companion::setScope(const QString &scope) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString normalized = scope.trimmed().toLower();
    if (normalized != QLatin1String("upto") && normalized != QLatin1String("book") &&
        normalized != QLatin1String("shelf"))
        return;
    if (normalized == m_scope)
        return;
    const bool wasWholeStudy = wholeBookStudyActive();
    m_scope = normalized;
    if (m_scope == QLatin1String("shelf"))
        refreshShelfLabel();
    const bool nowWholeStudy = wholeBookStudyActive();
    if (studyMode() && wasWholeStudy != nowWholeStudy && m_reader && m_reader->bookId() > 0) {
        resetPeopleState();
        resetRecapState();
        loadWhosWhoSnapshot(m_reader->bookId(), m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        loadRecapSnapshot(m_reader->bookId(), m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
        emit recapChanged();
        emit changed();
    }
    emit scopeChanged();
    emit conversationChanged();
}

void Companion::loadBookState(int bookId) {
    if (bookId <= 0)
        return;
    const QString savedMode = m_store.loadReadingMode(bookId);
    m_readingMode = savedMode.isEmpty() ? detectCurrentBookMode() : savedMode;
    m_scope = studyMode() ? QStringLiteral("book") : QStringLiteral("upto");
    refreshShelfLabel();
    m_conversation = m_store.loadConversation(bookId);
    if (loadWhosWhoSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false) &&
        m_where.isEmpty()) {
        m_where = m_peopleWhere;
        m_whereLocal = m_peopleWhereLocal;
    }
    loadRecapSnapshot(bookId, m_reader->chapter(), m_reader->pageEnd(), m_reader->pageStart(), false);
}

bool Companion::spoilerFilterActive() const {
    return !wholeBookStudyActive() && m_scope == QLatin1String("upto") && m_reader != nullptr;
}

bool Companion::isAheadOfReader(int chapter, qint64 offset) const {
    if (!m_reader)
        return false;
    const int ch = m_reader->chapter();
    if (chapter > ch)
        return true;
    const qint64 end = m_reader->pageEnd();
    return chapter == ch && end > 0 && offset > end;
}

bool Companion::isTurnVisibleAtCurrentPlace(const Turn &turn) const {
    if (!spoilerFilterActive())
        return true;
    return CompanionStore::isTurnVisible(turn, true, m_reader->chapter(), m_reader->pageEnd());
}

void Companion::pruneFutureTurnsIfSpoilerFree() {
    if (!spoilerFilterActive())
        return;
    QList<Turn> kept = CompanionStore::filterVisibleTurns(m_conversation, true, m_reader->chapter(), m_reader->pageEnd());
    if (kept.size() != m_conversation.size())
        m_conversation = std::move(kept);
}

QList<Ai::Message> Companion::buildHistoryMessages(qint64 budget, bool filterSpoiler) const {
    const bool filter = filterSpoiler && spoilerFilterActive();
    return CompanionStore::buildHistoryMessages(m_conversation, budget, filter,
                                                m_reader ? m_reader->chapter() : -1,
                                                m_reader ? m_reader->pageEnd() : -1);
}

bool Companion::loadWhosWhoSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter) {
    CompanionStore::WhosWhoSnapshot snap;
    if (!m_store.loadWhosWhoSnapshot(bookId, m_readingMode, wholeBookStudyActive(), chapter, pageEnd, pageStart,
                                     requireSameChapter, &snap)) {
        return false;
    }
    m_people = std::move(snap.people);
    m_peopleLoaded = true;
    m_peopleChapter = snap.chapter;
    m_peopleOffset = snap.offset;
    m_peopleWhere = std::move(snap.whereLabel);
    m_peopleWhereLocal = snap.isLocal;
    m_peopleKey = QStringLiteral("%1:%2:%3:%4:%5").arg(bookId).arg(m_readingMode).arg(m_peopleChapter).arg(m_peopleOffset).arg(m_peopleWhere);
    return true;
}

bool Companion::loadRecapSnapshot(int bookId, int chapter, qint64 pageEnd, qint64 pageStart, bool requireSameChapter) {
    CompanionStore::RecapSnapshot snap;
    if (!m_store.loadRecapSnapshot(bookId, m_readingMode, wholeBookStudyActive(), chapter, pageEnd, pageStart,
                                   requireSameChapter, &snap)) {
        return false;
    }
    m_recapText = std::move(snap.text);
    m_recapLoaded = true;
    m_recapChapter = snap.chapter;
    m_recapOffset = snap.offset;
    m_recapWhere = std::move(snap.whereLabel);
    m_recapWhereLocal = snap.isLocal;
    return true;
}

void Companion::clearBookHistory(int bookId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const int target = bookId > 0 ? bookId : (m_reader ? m_reader->bookId() : 0);
    if (target <= 0)
        return;
    m_store.clearBookHistory(target);
    if (m_reader && target == m_reader->bookId()) {
        m_conversation.clear();
        m_currentSources.clear();
        resetPeopleState();
        resetRecapState();
        resetSelectionState();
        emit changed();
        emit answerChanged();
        emit conversationChanged();
        emit recapChanged();
        emit selectionChanged();
    }
}

QVariantList Companion::conversation() const {
    return CompanionStore::conversationToVariant(m_conversation, spoilerFilterActive(),
                                                 m_reader ? m_reader->chapter() : -1,
                                                 m_reader ? m_reader->pageEnd() : -1);
}

QStringList Companion::suggestions() const {
    QStringList askedLower;
    for (const Turn &turn : m_conversation)
        if (turn.fromUser && isTurnVisibleAtCurrentPlace(turn))
            askedLower.append(turn.text.trimmed().toLower());

    QStringList out;
    auto addIfFresh = [&](const QString &prompt) {
        const QString clean = prompt.trimmed();
        if (clean.isEmpty() || out.size() >= 2)
            return;
        const QString low = clean.toLower();
        for (const QString &prev : askedLower)
            if (prev == low)
                return;
        if (!out.contains(clean, Qt::CaseInsensitive))
            out.append(clean);
    };
    auto addFirstUnaskedPerson = [&](const QString &tmpl) {
        for (const QVariant &entry : m_people) {
            const QString name = entry.toMap().value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty())
                continue;
            const QString lowName = name.toLower();
            bool mentioned = false;
            for (const QString &prev : askedLower) {
                if (prev.contains(lowName)) {
                    mentioned = true;
                    break;
                }
            }
            if (!mentioned) {
                addIfFresh(tmpl.arg(name));
                break;
            }
        }
    };

    if (m_scope == QLatin1String("shelf")) {
        addIfFresh(tr("Compare the key arguments across this shelf"));
        addIfFresh(tr("How does this document connect to others on my shelf?"));
        addIfFresh(tr("What methods or findings overlap across these documents?"));
        return out;
    }

    if (studyMode()) {
        addFirstUnaskedPerson(tr("Explain %1 in context"));
        addIfFresh(tr("Summarize the core claims and methodology"));
        addIfFresh(tr("What are the main findings and limitations?"));
        addIfFresh(tr("Explain the key equations and notation"));
        return out;
    }

    addFirstUnaskedPerson(tr("Who is %1?"));
    if (m_reader) {
        const QString chapterTitle = m_reader->chapterTitle().trimmed();
        if (!chapterTitle.isEmpty() && chapterTitle.size() <= 40)
            addIfFresh(tr("What has happened in %1 so far?").arg(chapterTitle));
    }
    addIfFresh(tr("Who are the key people so far?"));
    addIfFresh(tr("Summarize what has happened so far"));
    addIfFresh(tr("What should I remember from the last few pages?"));
    return out;
}

QVariantList Companion::currentSources() const { return CompanionStore::citationsToVariant(m_currentSources); }

QString Companion::targetLanguage() const {
    if (!m_targetLanguage.isEmpty())
        return m_targetLanguage;
    const QString sys = readerLanguage();
    return sys.isEmpty() || sys == QLatin1String("C") ? QStringLiteral("English") : sys;
}

void Companion::setTargetLanguage(const QString &language) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString cleaned = language.trimmed();
    if (cleaned.isEmpty() || cleaned == targetLanguage())
        return;
    m_targetLanguage = cleaned;
    companionSettings().setValue(QStringLiteral("companion/translateLanguage"), m_targetLanguage);
    emit targetLanguageChanged();
}

QStringList Companion::targetLanguages() const {
    static const QStringList common = {
        QStringLiteral("English"),    QStringLiteral("Spanish"),  QStringLiteral("French"),
        QStringLiteral("German"),     QStringLiteral("Italian"),  QStringLiteral("Portuguese"),
        QStringLiteral("Dutch"),      QStringLiteral("Russian"),  QStringLiteral("Ukrainian"),
        QStringLiteral("Polish"),     QStringLiteral("Swedish"),  QStringLiteral("Turkish"),
        QStringLiteral("Greek"),      QStringLiteral("Arabic"),   QStringLiteral("Hindi"),
        QStringLiteral("Chinese"),    QStringLiteral("Japanese"), QStringLiteral("Korean"),
        QStringLiteral("Latin"),
    };
    QStringList out;
    const QString sys = readerLanguage();
    if (!sys.isEmpty() && sys != QLatin1String("C"))
        out.append(sys);
    if (!m_targetLanguage.isEmpty() && !out.contains(m_targetLanguage))
        out.append(m_targetLanguage);
    for (const QString &lang : common)
        if (!out.contains(lang))
            out.append(lang);
    return out;
}

QString Companion::cleanMarkdown(const QString &text) { return MathFormatter::cleanMarkdown(text); }
bool Companion::hasMath(const QString &text) const { return MathFormatter::hasMath(text); }
QString Companion::formatDisplayHtml(const QString &text, const QString &mathBlockBg) const {
    return MathFormatter::formatDisplayHtml(text, mathBlockBg);
}
QString Companion::readerLanguage() { return QLocale::languageToString(QLocale().language()); }

QList<Companion::Chapter> Companion::textUpTo(const QString &path, const QString &format, int chapter, qint64 offset,
                                              qint64 limit, std::shared_ptr<std::atomic_bool> cancel) {
    return BookContextReader::textUpTo(path, format, chapter, offset, limit, std::move(cancel));
}

QList<Companion::Chapter> Companion::textOfBook(const QString &path, const QString &format, qint64 limit,
                                                std::shared_ptr<std::atomic_bool> cancel) {
    return BookContextReader::textOfBook(path, format, limit, std::move(cancel));
}

QString Companion::joined(const QList<Chapter> &chapters, qint64 limit) {
    return BookContextReader::joined(chapters, limit);
}

QStringList Companion::dictionary(const QString &word) { return LocalDictionary::lookupSync(word); }

QString Companion::bookLine() const {
    return QStringLiteral("Book: %1, by %2.").arg(m_reader->title(), m_reader->author());
}

QString Companion::placeTitle(int chapter) const {
    const QString title = m_reader->titleOfChapter(chapter);
    if (!title.isEmpty())
        return title;
    if (m_reader->format() == QLatin1String("pdf")) {
        const QString lbl = m_reader->pdfPageLabel(chapter);
        return tr("Page %1").arg(lbl.isEmpty() ? QString::number(chapter + 1) : lbl);
    }
    return tr("Chapter %1").arg(chapter + 1);
}

bool Companion::cloudFor(const QString &feature) const {
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    return provider && provider->id() != QLatin1String("local");
}

QString Companion::formatProvenance(const Ai::Provider *provider, const Ai::Reply *reply) const {
    if (!provider)
        return m_where;
    if (provider->isLocal()) {
        const QString rawModel = reply && !reply->model().isEmpty() ? reply->model() : m_ai->localTextModel();
        const QString title = AiServices::friendlyModelTitle(rawModel);
        return title.isEmpty() ? tr("On this computer · %1").arg(provider->name())
                               : tr("On this computer · %1").arg(title);
    }
    if (provider->id() == QLatin1String("local")) {
        const QString rawModel = reply && !reply->model().isEmpty() ? reply->model() : m_ai->localTextModel();
        const QString title = AiServices::friendlyModelTitle(rawModel);
        return title.isEmpty() ? tr("Sent to %1").arg(provider->name())
                               : tr("Sent to %1 · %2").arg(provider->name(), title);
    }
    const double dollars = reply ? AiUsageLedger::cost(reply->model(), reply->usage(), QDate::currentDate(), false) : 0.0;
    const QString costText = dollars < 0.01 ? tr("less than 1¢")
                           : dollars < 1.0  ? tr("about %1¢").arg(qMax(1, qRound(dollars * 100)))
                                            : QStringLiteral("$%1").arg(dollars, 0, 'f', 2);
    return tr("Gemini · sent to Google · %1").arg(costText);
}

qint64 Companion::localCharBudget(const QString &feature, bool fast, qint64 fallbackChars, double charsPerToken) const {
    Q_ASSERT(fallbackChars > 0);
    Q_ASSERT(charsPerToken > 0.0);
    Ai::TextModel *model = m_ai->textFor(feature);
    const int ctxTokens = model ? model->contextWindow(fast) : 0;
    if (ctxTokens <= 0)
        return fallbackChars;
    // Reserve headroom for instructions, JSON schema, and generated output.
    const int reserved = qMax(1024, ctxTokens / 4);
    const int usableTokens = qMax(512, ctxTokens - reserved);
    const qint64 budgeted = qint64(double(usableTokens) * charsPerToken);
    return qMin(fallbackChars, budgeted);
}

QString Companion::pendingSetupLabel() const {
    if (m_pendingSetup.feature.isEmpty())
        return {};
    if (m_pendingSetup.feature == QLatin1String("ask"))
        return tr("“%1”").arg(BookContextReader::makeExcerpt(m_pendingSetup.text, 64));
    if (m_pendingSetup.feature == QLatin1String("snip"))
        return m_pendingSetup.text.isEmpty() ? tr("Snipped region")
                                             : tr("“%1”").arg(BookContextReader::makeExcerpt(m_pendingSetup.text, 64));
    if (m_pendingSetup.feature == QLatin1String("define"))
        return tr("Define “%1”").arg(BookContextReader::makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("explain"))
        return tr("Explain “%1”").arg(BookContextReader::makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("translate"))
        return tr("Translate “%1”").arg(BookContextReader::makeExcerpt(m_pendingSetup.text, 48));
    if (m_pendingSetup.feature == QLatin1String("recap"))
        return studyMode() ? tr("Study brief") : tr("Previously…");
    if (m_pendingSetup.feature == QLatin1String("whoswho"))
        return studyMode() ? tr("Glossary") : tr("Who's who");
    return {};
}

Ai::TextModel *Companion::modelFor(const QString &feature) {
    Ai::TextModel *model = m_ai->textFor(feature);
    if (!model) {
        if (!m_ai->hasKey() && !m_ai->hasLocal() && !m_lastAction.feature.isEmpty())
            m_pendingSetup = m_lastAction;
        fail(m_ai->preset() == QLatin1String("local") || m_ai->overrideFor(feature) == QLatin1String("local")
                 ? tr("This needs a model on this computer: set one up in Intelligence (✦).")
                 : tr("This needs a Gemini key or a model on this computer, in Intelligence (✦)."),
             true);
    }
    return model;
}

bool Companion::checkBudget(const QString &feature, qint64 charsEstimate, bool fast, qint64 outputTokens) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(charsEstimate >= 0);
    Q_ASSERT(outputTokens >= 0);
    m_lastCheckedCost = -1.0;
    if (!cloudFor(feature))
        return true;
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    Ai::TextModel *model = provider ? provider->text() : nullptr;
    if (!model)
        return false;
    Ai::Usage usage;
    usage.inputTokens = qMax<qint64>(1, charsEstimate / 4);
    usage.outputTokens = outputTokens;
    usage.thoughtTokens = fast ? qMin<qint64>(outputTokens, 128) : qMin<qint64>(outputTokens, 400);
    const double cost = AiUsageLedger::costForProvider(provider->id(), model->textModelName(fast), usage,
                                                       QDate::currentDate(), false);
    Q_ASSERT(cost >= 0.0 && std::isfinite(cost));
    if (!m_ai->fitsBudget(cost)) {
        fail(m_ai->describe(int(Ai::Error::Budget)), true);
        return false;
    }
    m_lastCheckedCost = cost;
    return true;
}

qint64 Companion::soFarCharsEstimate() const {
    qint64 soFar = m_reader ? m_reader->pageEnd() : 0;
    if (m_reader) {
        for (int i = 0; i < m_reader->chapter(); ++i)
            soFar += m_reader->charactersIn(i);
        // While chapter lengths are still being measured, fall back to a conservative
        // estimate so the cost readout and budget check never under-report as $0.
        if (soFar <= 0 && m_reader->chapter() > 0)
            soFar = qint64(m_reader->chapter() + 1) * 12000;
    }
    return qMin<qint64>(cloudLimit, qMax<qint64>(0, soFar));
}

qint64 Companion::wholeBookCharsEstimate() const {
    if (!m_reader)
        return 0;
    qint64 total = 0;
    const int count = m_reader->chapterCount();
    for (int i = 0; i < count; ++i)
        total += m_reader->charactersIn(i);
    if (total <= 0)
        total = qMax<qint64>(soFarCharsEstimate(),
                             qint64(qMax(1, count)) * (m_reader->format() == QLatin1String("pdf") ? 2800 : 12000));
    return qMin<qint64>(cloudLimit, qMax<qint64>(0, total));
}

void Companion::flushStreamedAnswer() {
    m_streamFlush.stop();
    if (!m_streamDirty)
        return;
    m_streamDirty = false;
    emit answerChanged();
}

void Companion::begin(const QString &feature, const QString &subject) {
    Q_ASSERT(QThread::currentThread() == thread());
    stop();
    m_feature = feature;
    m_subject = subject;
    m_answer.clear();
    m_currentSources.clear();
    m_problem.clear();
    m_problemIsConfig = false;
    m_busy = true;
    m_streamDirty = false;
    m_streamNotifiedFirst = false;
    m_requestChapter = m_reader ? m_reader->chapter() : -1;
    m_requestStart = m_reader ? m_reader->pageStart() : -1;
    m_requestOffset = m_reader ? m_reader->pageEnd() : -1;
    m_where = m_ai->whereFor(feature);
    m_whereLocal = (m_ai->routeFor(feature) == QLatin1String("local") && m_ai->localIsLoopback());
    m_status = tr("Thinking…");
    emit changed();
    emit answerChanged();
}

void Companion::setStatus(const QString &status) {
    m_status = status;
    emit changed();
}

void Companion::rollbackPendingTurn() {
    if (!m_pendingUserTurn)
        return;
    m_pendingUserTurn = false;
    if (!m_answer.trimmed().isEmpty()) {
        const bool stamp = !wholeBookStudyActive() && m_scope == QLatin1String("upto") && m_requestChapter >= 0;
        m_conversation.append({false, m_answer, m_currentSources, m_where, m_whereLocal, {},
                               stamp ? m_requestChapter : -1, stamp ? m_requestOffset : -1});
        m_answer.clear();
        m_currentSources.clear();
        m_store.saveConversation(m_reader->bookId(), m_conversation);
        emit conversationChanged();
        emit answerChanged();
    } else if (!m_conversation.isEmpty() && m_conversation.last().fromUser) {
        m_conversation.removeLast();
        m_answer.clear();
        m_currentSources.clear();
        emit conversationChanged();
        emit answerChanged();
    }
}

void Companion::fail(const QString &problem, bool isConfig) {
    Q_ASSERT(QThread::currentThread() == thread());
    flushStreamedAnswer();
    rollbackPendingTurn();
    m_problem = problem;
    m_problemIsConfig = isConfig;
    m_busy = false;
    m_requestChapter = -1;
    m_requestStart = -1;
    m_requestOffset = -1;
    emit changed();
}

void Companion::clearProblem() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_problem.isEmpty() && !m_problemIsConfig)
        return;
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
}

void Companion::executeAction(const Action &action) {
    if (action.feature == QLatin1String("ask"))
        ask(action.text);
    else if (action.feature == QLatin1String("snip"))
        sendSnipRequest(action.snipPng, action.snipAction, action.text, action.chapter);
    else if (action.feature == QLatin1String("define"))
        define(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("explain"))
        explain(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("translate"))
        translate(action.text, action.chapter, action.offset);
    else if (action.feature == QLatin1String("recap"))
        refreshRecap();
    else if (action.feature == QLatin1String("whoswho"))
        refreshWhosWho();
}

void Companion::retry() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_busy || m_lastAction.feature.isEmpty())
        return;
    const Action action = m_lastAction;
    executeAction(action);
}

void Companion::resumePendingSetup() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_pendingSetup.feature.isEmpty())
        return;
    const Action action = m_pendingSetup;
    m_pendingSetup = {};
    m_problem.clear();
    m_problemIsConfig = false;
    emit changed();
    executeAction(action);
}

void Companion::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_generation;
    m_streamFlush.stop();
    m_streamDirty = false;
    m_streamNotifiedFirst = false;
    m_requestChapter = -1;
    m_requestStart = -1;
    m_requestOffset = -1;
    m_lastCheckedCost = -1.0;
    if (m_readCancel) {
        *m_readCancel = true;
        m_readCancel.reset();
    }
    m_dictionary.cancel();
    m_index.cancel();
    if (m_reply) {
        QPointer<Ai::Reply> active = m_reply;
        m_reply.clear();
        if (active)
            active->abort();
    }
    rollbackPendingTurn();
    if (m_busy) {
        m_busy = false;
        emit changed();
    }
}

void Companion::commitAssistantTurn(int bookId, const QList<Citation> &sources, int turnChapter, qint64 turnOffset) {
    m_pendingUserTurn = false;
    const QString completedAnswer = m_answer;
    m_answer.clear();
    m_currentSources.clear();
    m_conversation.append({false, completedAnswer, sources, m_where, m_whereLocal, {}, turnChapter, turnOffset});
    m_store.saveConversation(bookId, m_conversation);
    emit conversationChanged();
}

void Companion::finishCachedSnapshot(const QString &where, bool whereLocal) {
    m_where = where;
    m_whereLocal = whereLocal;
    m_busy = false;
    m_requestChapter = -1;
    m_requestOffset = -1;
}

void Companion::readChapters(bool wholeBook, int chapter, qint64 offset, qint64 limit,
                             std::function<void(const QList<Chapter> &)> then) {
    const QString path = m_reader->bookPath();
    const QString format = m_reader->format();
    const int generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, wholeBook, path, format, chapter, offset, limit, cancel, generation, then] {
        const QList<Chapter> chapters = wholeBook ? BookContextReader::textOfBook(path, format, limit, cancel)
                                                  : BookContextReader::textUpTo(path, format, chapter, offset, limit, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [self, chapters, generation, then] {
                if (!self || self->m_generation != generation)
                    return;
                QList<Chapter> titled = chapters;
                for (Chapter &ch : titled)
                    if (ch.title.isEmpty())
                        ch.title = self->m_reader->titleOfChapter(ch.index);
                then(titled);
            },
            Qt::QueuedConnection);
    });
}

void Companion::readSoFar(int chapter, qint64 offset, qint64 limit, std::function<void(const QList<Chapter> &)> then) {
    readChapters(false, chapter, offset, limit, std::move(then));
}

void Companion::readWholeBook(qint64 limit, std::function<void(const QList<Chapter> &)> then) {
    readChapters(true, 0, -1, limit, std::move(then));
}

void Companion::run(const QString &feature, const Ai::TextRequest &request, std::function<void(Ai::Reply *)> done) {
    Q_ASSERT(QThread::currentThread() == thread());
    Ai::Provider *provider = m_ai->textProviderFor(feature);
    Ai::TextModel *model = provider ? provider->text() : nullptr;
    if (!model) {
        modelFor(feature);
        return;
    }
    m_where = m_ai->whereFor(feature);
    m_whereLocal = provider->isLocal();
    const bool hasImage = !request.imagePng.isEmpty();
    const double reserveCost = std::exchange(m_lastCheckedCost, -1.0);
    Ai::Reply *reply = model->generate(request);
    m_ai->track(reply, false, reserveCost);
    m_reply = reply;
    const int generation = m_generation;
    QPointer<Ai::Provider> providerGuard(provider);
    setStatus(tr("Writing…"));
    connect(reply, &Ai::Reply::textReceived, this, [this, reply, generation](const QString &delta) {
        if (reply != m_reply || generation != m_generation || delta.isEmpty())
            return;
        m_answer += delta;
        if (!m_streamNotifiedFirst) {
            m_streamNotifiedFirst = true;
            m_streamDirty = false;
            emit answerChanged();
        } else {
            m_streamDirty = true;
            if (!m_streamFlush.isActive())
                m_streamFlush.start();
        }
    });
    connect(reply, &Ai::Reply::finished, this, [this, reply, done, generation, providerGuard, feature, hasImage] {
        reply->deleteLater();
        if (reply != m_reply || generation != m_generation)
            return;
        m_reply.clear();
        flushStreamedAnswer();
        if (reply->error()) {
            if (reply->error().kind != Ai::Error::Cancelled) {
                const auto kind = reply->error().kind;
                const bool localVisionErr =
                    hasImage && providerGuard && providerGuard->id() == QLatin1String("local") &&
                    (kind == Ai::Error::Invalid || kind == Ai::Error::Unsupported) &&
                    reply->error().message == LocalModels::visionUnsupportedMessage();
                const bool isConfig = (localVisionErr || kind == Ai::Error::NoKey || kind == Ai::Error::BadKey ||
                                       kind == Ai::Error::Budget || kind == Ai::Error::NotFound ||
                                       (providerGuard && providerGuard->id() == QLatin1String("local") &&
                                        kind == Ai::Error::Offline));
                fail(localVisionErr ? reply->error().message
                                    : m_ai->describeFor(providerGuard, int(kind), reply->error().message),
                     isConfig);
            }
            return;
        }
        if (m_answer.isEmpty() && reply->result().isEmpty())
            m_answer = reply->text();
        if (!done && m_answer.trimmed().isEmpty()) {
            fail(tr("The model returned an empty answer. Try again."));
            return;
        }
        m_pendingSetup = {};
        m_where = formatProvenance(providerGuard, reply);
        m_whereLocal = providerGuard && providerGuard->isLocal();
        if (feature == QLatin1String("define") || feature == QLatin1String("explain") ||
            feature == QLatin1String("translate")) {
            m_selectionFeature = feature;
            m_selectionSubject = m_subject;
            m_selectionAnswer = m_answer;
            m_selectionWhere = m_where;
            m_selectionWhereLocal = m_whereLocal;
            emit selectionChanged();
        }
        if (done)
            done(reply);
        if (m_problem.isEmpty()) {
            m_busy = false;
            m_requestChapter = -1;
            m_requestOffset = -1;
            emit answerChanged();
            emit changed();
        }
    });
}

// --- Define, explain, translate ----------------------------------------------

void Companion::define(const QString &word, int chapter, qint64 offset) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString wanted = word.trimmed();
    m_lastAction = {QStringLiteral("define"), wanted, chapter, offset, {}, {}};
    begin(QStringLiteral("define"), wanted);
    const int generation = m_generation;
    const auto askModel = [this, wanted, chapter, offset, generation] {
        if (generation != m_generation)
            return;
        if (!modelFor(m_feature))
            return;
        if (!checkBudget(m_feature, 1600, true, 120))
            return;
        setStatus(tr("Reading the sentence…"));
        const qint64 selectionEnd = qMax<qint64>(0, offset) + wanted.size();
        qint64 end = selectionEnd;
        if (wholeBookStudyActive()) {
            end = selectionEnd + 400;
        } else if (m_reader && chapter == m_reader->chapter() && m_reader->pageEnd() > 0) {
            end = qMin<qint64>(selectionEnd + 400, qMax<qint64>(m_reader->pageEnd(), selectionEnd));
        } else if (m_reader && m_reader->format() == QLatin1String("pdf") && chapter <= m_reader->chapter()) {
            end = -1;
        }
        readSoFar(chapter, end, 2000, [this, wanted, generation](const QList<Chapter> &chapters) {
            if (generation != m_generation)
                return;
            const QString around = joined(chapters, 1200);
            Ai::TextRequest request;
            request.feature = QStringLiteral("define");
            request.fast = true;
            request.stream = true;
            const bool spoilerFree = !wholeBookStudyActive();
            request.system = instructions(BookContextReader::defineTaskPrompt(studyMode()), spoilerFree);
            request.messages = {{true, bookLine() + QStringLiteral("\n\nPassage:\n") + around +
                                           QStringLiteral("\n\nWord: ") + wanted}};
            run(QStringLiteral("define"), request);
        });
    };

    // A dictionary on this computer answers first (asynchronously) and costs nothing.
    const bool launchedDict = m_dictionary.lookupAsync(
        wanted, [this, wanted, generation, askModel](const QStringList &entries) {
            if (generation != m_generation)
                return;
            if (!entries.isEmpty()) {
                m_pendingSetup = {};
                m_answer = entries.join(QStringLiteral("\n\n"));
                m_where = tr("Dictionary on this computer");
                m_whereLocal = true;
                m_selectionFeature = QStringLiteral("define");
                m_selectionSubject = wanted;
                m_selectionAnswer = m_answer;
                m_selectionWhere = m_where;
                m_selectionWhereLocal = true;
                m_busy = false;
                m_requestChapter = -1;
                m_requestOffset = -1;
                emit selectionChanged();
                emit answerChanged();
                emit changed();
                return;
            }
            askModel();
        });
    if (launchedDict) {
        setStatus(tr("Checking your dictionary…"));
        return;
    }
    askModel();
}

void Companion::explain(const QString &text, int chapter, qint64 end) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString cleaned = text.trimmed();
    m_lastAction = {QStringLiteral("explain"), cleaned, chapter, end, {}, {}};
    begin(QStringLiteral("explain"), cleaned);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 charLimit = cloud ? 60000 : localCharBudget(m_feature, false, 12000);
    if (!checkBudget(m_feature, charLimit, false, 400))
        return;
    const int generation = m_generation;
    setStatus(tr("Reading up to here…"));
    readSoFar(chapter, end, charLimit, [this, cleaned, charLimit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const bool isCloud = cloudFor(QStringLiteral("explain"));
        const qint64 limit = isCloud ? 60000 : qMin(charLimit, localCharBudget(QStringLiteral("explain"), false, 12000));
        const QString context = joined(chapters, limit);
        if (!checkBudget(QStringLiteral("explain"), context.size() + cleaned.size(), false, 400))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("explain");
        request.stream = true;
        const bool spoilerFree = !wholeBookStudyActive();
        request.system = instructions(BookContextReader::explainTaskPrompt(studyMode()), spoilerFree);
        request.messages = {{true, bookLine() + QStringLiteral("\n\nThe book up to the selection:\n") + context +
                                       QStringLiteral("\n\nSelected: \"") + cleaned + QStringLiteral("\"")}};
        run(QStringLiteral("explain"), request);
    });
}

void Companion::translate(const QString &text, int chapter, qint64 end) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString cleaned = text.trimmed();
    m_lastTranslateText = cleaned;
    m_lastTranslateChapter = chapter;
    m_lastTranslateEnd = end;
    m_lastAction = {QStringLiteral("translate"), cleaned, chapter, end, {}, {}};
    begin(QStringLiteral("translate"), cleaned);
    if (!modelFor(m_feature))
        return;
    if (!checkBudget(m_feature, 2400 + cleaned.size(), true, 300))
        return;
    // Translate into the explicitly chosen target language (defaulting to the reader's
    // own system language — never forcing another language on untagged PDFs or quotes).
    const QString target = targetLanguage();
    const int generation = m_generation;
    readSoFar(chapter, end, 2500, [this, cleaned, target, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        const QString context = joined(chapters, 2000);
        if (!checkBudget(QStringLiteral("translate"), context.size() + cleaned.size(), true, 300))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("translate");
        request.fast = true;
        request.stream = true;
        request.system = QStringLiteral(
                             "Translate the selected words into %1, faithfully and in a natural literary style, "
                             "using the passage before them for context. If the selected words are already in %1, "
                             "render them clearly in modern %1 instead of switching languages. Give only the translation.")
                             .arg(target);
        request.messages = {{true, QStringLiteral("Context:\n") + context + QStringLiteral("\n\nTranslate:\n") + cleaned}};
        run(QStringLiteral("translate"), request);
    });
}

void Companion::retranslate() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_lastTranslateText.isEmpty())
        return;
    translate(m_lastTranslateText, m_lastTranslateChapter, m_lastTranslateEnd);
}

// --- Ask ---------------------------------------------------------------------

QVariantMap Companion::askEstimate() const {
    const bool cloud = cloudFor(QStringLiteral("ask"));
    qint64 contextChars = 0;
    if (m_scope == QLatin1String("shelf"))
        contextChars = 12000;
    else if (m_scope == QLatin1String("book"))
        contextChars = wholeBookCharsEstimate();
    else
        contextChars = soFarCharsEstimate();
    qint64 historyChars = 0;
    for (const Turn &turn : m_conversation)
        if (isTurnVisibleAtCurrentPlace(turn))
            historyChars += turn.text.size();
    const double characters = double(contextChars) + historyChars + (hasPendingSnip() ? 4800.0 : 0.0);
    double cost = 0;
    if (cloud) {
        if (Ai::TextModel *model = m_ai->textFor(QStringLiteral("ask"))) {
            Ai::Usage usage;
            usage.inputTokens = qMax<qint64>(1, qint64(characters / 4));
            usage.outputTokens = 400;
            cost = AiUsageLedger::cost(model->textModelName(false), usage, QDate::currentDate(), false);
        }
    }
    return {{QStringLiteral("where"), m_ai->whereFor(QStringLiteral("ask"))},
            {QStringLiteral("words"), qRound(double(contextChars) / 6.0)},
            {QStringLiteral("cost"), cost},
            {QStringLiteral("local"), !cloud},
            {QStringLiteral("scope"), m_scope}};
}

void Companion::clearPendingSnip() {
    if (m_pendingSnipPng.isEmpty() && m_pendingSnipUrl.isEmpty() && m_pendingSnipPage < 0)
        return;
    m_pendingSnipPng.clear();
    m_pendingSnipUrl.clear();
    m_pendingSnipPage = -1;
    emit pendingSnipChanged();
}

void Companion::activateCitation(int bookId, int chapter, qint64 offset, const QString &quote) {
    Q_UNUSED(offset);
    if (bookId > 0 && (!m_reader || bookId != m_reader->bookId()))
        emit openBookPassage(bookId, chapter, quote);
}

void Companion::askAboutSnipBase64(const QString &base64Png, const QString &action, const QString &prompt, int page) {
    Q_ASSERT(QThread::currentThread() == thread());
    QByteArray cleanPng;
    QString err;
    if (!BookContextReader::decodeAndNormalizeSnipBase64(base64Png, &cleanPng, &err)) {
        fail(err);
        return;
    }

    const QString normAction = action.trimmed().toLower().isEmpty() ? QStringLiteral("explain") : action.trimmed().toLower();
    const int targetPage = page >= 0 ? page : (m_reader ? m_reader->chapter() : -1);
    const QString cleanedPrompt = prompt.trimmed();

    if (normAction == QLatin1String("ask") && cleanedPrompt.isEmpty()) {
        m_pendingSnipPng = cleanPng;
        m_pendingSnipPage = targetPage;
        m_pendingSnipUrl = BookContextReader::makeSnipThumbnailDataUrl(cleanPng);
        emit pendingSnipChanged();
        emit openAskRequested();
        return;
    }
    clearPendingSnip();
    sendSnipRequest(cleanPng, normAction, cleanedPrompt, targetPage);
}

void Companion::sendSnipRequest(const QByteArray &pngBytes, const QString &action, const QString &prompt, int page) {
    const QString normAction = action.trimmed().toLower();
    const int targetPage = page >= 0 ? page : (m_reader ? m_reader->chapter() : -1);
    const bool isPdf = m_reader && m_reader->format().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
    const QString pageSuffix =
        targetPage >= 0
            ? (isPdf ? tr(" (Page %1)").arg(targetPage + 1) : tr(" (%1)").arg(placeTitle(targetPage)))
            : QString();

    QString userLabel = prompt.trimmed();
    if (userLabel.isEmpty()) {
        if (normAction == QLatin1String("latex"))
            userLabel = tr("Extract LaTeX%1").arg(pageSuffix);
        else if (normAction == QLatin1String("table"))
            userLabel = tr("Extract table%1").arg(pageSuffix);
        else
            userLabel = tr("Explain this region%1").arg(pageSuffix);
    }
    const QString taskSystem = BookContextReader::snipTaskPrompt(normAction, prompt.trimmed());
    const QString userPrompt = userLabel;

    m_lastAction = {QStringLiteral("snip"), prompt.trimmed(), targetPage, 0, pngBytes, normAction};
    begin(QStringLiteral("ask"), userLabel);
    emit openAskRequested();
    if (!modelFor(m_feature))
        return;
    if (cloudFor(m_feature) && !checkBudget(m_feature, 6400 + userPrompt.size(), false, 500))
        return;

    const bool spoilerFree = !wholeBookStudyActive();
    const int readerChapter = m_reader ? m_reader->chapter() : -1;
    const qint64 pageEnd = m_reader ? m_reader->pageEnd() : 0;
    const int safePage = (spoilerFree && m_reader && targetPage > readerChapter) ? readerChapter : targetPage;
    const int turnChapter = spoilerFree ? safePage : -1;
    const qint64 turnOffset = (spoilerFree && safePage == readerChapter) ? pageEnd : -1;

    pruneFutureTurnsIfSpoilerFree();
    Turn userTurn;
    userTurn.fromUser = true;
    userTurn.text = userLabel;
    userTurn.snipDataUrl = BookContextReader::makeSnipThumbnailDataUrl(pngBytes);
    userTurn.chapter = turnChapter;
    userTurn.offset = turnOffset;
    m_conversation.append(std::move(userTurn));
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();
    setStatus(tr("Inspecting snipped region…"));

    const int bookId = m_reader ? m_reader->bookId() : 0;
    const int generation = m_generation;
    const QString path = m_reader ? m_reader->bookPath() : QString();
    const QString format = m_reader ? m_reader->format() : QString();
    const QString bookHeader = m_reader ? bookLine() : QString();

    auto dispatch = [this, bookId, generation, pngBytes, taskSystem, userPrompt, bookHeader, spoilerFree, safePage,
                     turnChapter, turnOffset, isPdf](const QString &pageContext) {
        if (generation != m_generation)
            return;
        if (cloudFor(QStringLiteral("ask")) &&
            !checkBudget(QStringLiteral("ask"), 4800 + pageContext.size() + userPrompt.size(), false, 500))
            return;
        QList<Citation> sources;
        if (safePage >= 0 && !pageContext.trimmed().isEmpty()) {
            sources.append({bookId, {}, safePage, 0,
                            isPdf ? tr("Page %1").arg(safePage + 1) : placeTitle(safePage),
                            BookContextReader::makeExcerpt(pageContext)});
        }
        m_currentSources = sources;
        emit answerChanged();

        Ai::TextRequest request;
        request.feature = QStringLiteral("ask");
        request.stream = true;
        request.imagePng = pngBytes;
        request.imageMimeType = QStringLiteral("image/png");
        request.system = instructions(taskSystem, spoilerFree, true);
        QString msgText = bookHeader;
        if (!pageContext.trimmed().isEmpty())
            msgText += QStringLiteral("\n\nSurrounding page context:\n") + pageContext.left(3000);
        msgText += QStringLiteral("\n\nRequest: ") + userPrompt;
        request.messages = {{true, msgText.trimmed()}};

        run(QStringLiteral("ask"), request, [this, bookId, turnChapter, turnOffset, generation](Ai::Reply *) {
            if (generation != m_generation)
                return;
            if (m_answer.trimmed().isEmpty()) {
                fail(tr("The model returned an empty answer. Try again."));
                return;
            }
            commitAssistantTurn(bookId, m_currentSources, turnChapter, turnOffset);
        });
    };

    if (path.isEmpty() || safePage < 0) {
        dispatch({});
        return;
    }
    const qint64 ctxClamp = (spoilerFree && safePage == readerChapter && pageEnd > 0) ? qMin<qint64>(3000, pageEnd) : 3000;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);
    QThreadPool::globalInstance()->start([self, path, format, safePage, ctxClamp, cancel, generation, dispatch] {
        const QString ctx = BookContextReader::readPageContext(path, format, safePage, ctxClamp, cancel);
        if (cancel->load())
            return;
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                app,
                [self, ctx, generation, dispatch] {
                    if (!self || self->m_generation != generation)
                        return;
                    dispatch(ctx);
                },
                Qt::QueuedConnection);
        }
    });
}

void Companion::newConversation() {
    Q_ASSERT(QThread::currentThread() == thread());
    stop();
    m_conversation.clear();
    m_currentSources.clear();
    m_pendingUserTurn = false;
    clearPendingSnip();
    m_answer.clear();
    m_feature = QStringLiteral("ask");
    m_problem.clear();
    m_problemIsConfig = false;
    m_pendingSetup = {};
    if (m_reader && m_reader->bookId() > 0)
        m_store.clearConversation(m_reader->bookId());
    emit changed();
    emit answerChanged();
    emit conversationChanged();
}

void Companion::ask(const QString &question) {
    Q_ASSERT(QThread::currentThread() == thread());
    const QString asked = question.trimmed();
    if (!m_pendingSnipPng.isEmpty()) {
        const QByteArray png = m_pendingSnipPng;
        const int page = m_pendingSnipPage;
        clearPendingSnip();
        sendSnipRequest(png, asked.isEmpty() ? QStringLiteral("explain") : QStringLiteral("ask"), asked, page);
        return;
    }
    if (asked.isEmpty())
        return;
    if (m_scope == QLatin1String("shelf")) {
        askShelf(asked);
        return;
    }
    const bool wholeBook = (m_scope == QLatin1String("book"));
    const bool spoilerFree = !wholeBook;
    m_lastAction = {QStringLiteral("ask"), asked, m_reader ? m_reader->chapter() : 0,
                    m_reader ? m_reader->pageEnd() : 0, {}, {}};
    begin(QStringLiteral("ask"), asked);
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    if (cloud) {
        const double cost = askEstimate().value(QStringLiteral("cost")).toDouble();
        if (!m_ai->fitsBudget(cost)) {
            fail(m_ai->describe(int(Ai::Error::Budget)), true);
            return;
        }
    }
    const int currentChapter = m_reader->chapter();
    const qint64 currentPageEnd = m_reader->pageEnd();
    const int turnChapter = spoilerFree ? currentChapter : -1;
    const qint64 turnOffset = spoilerFree ? currentPageEnd : -1;
    pruneFutureTurnsIfSpoilerFree();
    m_conversation.append({true, asked, {}, {}, false, {}, turnChapter, turnOffset});
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();
    setStatus(cloud ? (wholeBook ? tr("Reading the whole document…") : tr("Reading the book so far…"))
                    : tr("Finding the passages that matter…"));
    const int bookId = m_reader->bookId();
    const QString format = m_reader->format();
    const int generation = m_generation;
    const qint64 readLimit = cloud ? cloudLimit : -1;

    auto onChapters = [this, asked, bookId, format, currentChapter, currentPageEnd, turnChapter, turnOffset, wholeBook,
                       spoilerFree, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        if (spoilerFree && isAheadOfReader(currentChapter, currentPageEnd)) {
            m_answer.clear();
            stop();
            return;
        }
        if (chapters.isEmpty()) {
            fail(wholeBook ? tr("There isn't any readable text in this document yet.")
                           : tr("There isn't any readable text up to this page yet."));
            return;
        }
        const bool isCloud = cloudFor(QStringLiteral("ask"));
        const auto send = [this, isCloud, bookId, format, chapters, spoilerFree, turnChapter, turnOffset, generation](
                              const QString &context, const QString &how, const QList<Citation> &sources) {
            if (generation != m_generation)
                return;
            m_currentSources = sources;
            emit answerChanged();
            const qint64 localTotal = isCloud ? cloudLimit : localCharBudget(QStringLiteral("ask"), false, localLimit);
            const qint64 historyBudget = isCloud ? 100000 : qMin<qint64>(8000, qMax<qint64>(1500, localTotal / 3));
            const QList<Ai::Message> normalized = buildHistoryMessages(historyBudget, true);
            if (isCloud) {
                qint64 totalChars = context.size();
                for (const Ai::Message &msg : normalized)
                    totalChars += msg.text.size();
                if (!checkBudget(QStringLiteral("ask"), totalChars, false, 400))
                    return;
            }
            Ai::TextRequest request;
            request.feature = QStringLiteral("ask");
            request.stream = true;
            request.system = instructions(BookContextReader::askTaskPrompt(studyMode(), how), spoilerFree, studyMode());
            request.messages = {{true, bookLine() + QStringLiteral("\n\n") + context}};
            request.messages.append({false, QStringLiteral("I've read it. What would you like to know?")});
            request.messages += normalized;
            run(QStringLiteral("ask"), request,
                [this, bookId, format, chapters, turnChapter, turnOffset, generation](Ai::Reply *) {
                    if (generation != m_generation)
                        return;
                    if (m_answer.trimmed().isEmpty()) {
                        fail(tr("The model returned an empty answer. Try again."));
                        return;
                    }
                    QList<Citation> finalSources = m_currentSources;
                    if (finalSources.isEmpty())
                        finalSources = BookContextReader::citationsForAnswer(chapters, format, m_answer, 3);
                    commitAssistantTurn(bookId, finalSources, turnChapter, turnOffset);
                });
        };
        if (isCloud) {
            QString fallbackQuery;
            if (m_conversation.size() >= 3) {
                for (qsizetype i = m_conversation.size() - 2; i >= 0; --i) {
                    if (m_conversation[i].fromUser) {
                        fallbackQuery = m_conversation[i].text + QLatin1Char(' ') + asked;
                        break;
                    }
                }
            }
            QPointer<Companion> self(this);
            (void)QtConcurrent::run([self, send, chapters, asked, fallbackQuery, format, wholeBook, currentChapter,
                                     generation] {
                const auto built = BookContextReader::buildCloudAskContext(chapters, asked, fallbackQuery, format,
                                                                           wholeBook, currentChapter, cloudLimit);
                if (QCoreApplication *app = QCoreApplication::instance()) {
                    QMetaObject::invokeMethod(
                        app,
                        [self, send, built, generation] {
                            if (!self || self->m_generation != generation)
                                return;
                            send(built.context, built.how, built.sources);
                        },
                        Qt::QueuedConnection);
                }
            });
            return;
        }
        // On this computer: the passages closest to the question, and the
        // pages around the reader's place, budgeted to the local model's actual context window.
        QList<PassageIndex::Passage> passages;
        for (const Chapter &chapter : chapters)
            passages += PassageIndex::split(chapter.index, chapter.text);
        QPointer<Companion> self(this);
        const auto answer = [self, send, chapters, format, wholeBook, currentChapter,
                             generation](const QList<PassageIndex::Hit> &hits, bool) {
            if (!self || self->m_generation != generation)
                return;
            const qint64 localBudget = self->localCharBudget(QStringLiteral("ask"), false, localLimit);
            const auto built =
                BookContextReader::buildLocalAskContext(hits, chapters, format, wholeBook, currentChapter, localBudget);
            send(built.context, built.how, built.sources);
        };
        if (m_ai->embedder()) {
            m_index.build(
                bookId, passages,
                [self, generation](double share) {
                    if (self && self->m_generation == generation)
                        self->setStatus(share > 0.0
                                            ? tr("Getting to know the book… %1%").arg(qRound(share * 100))
                                            : tr("Getting to know the book…"));
                },
                [self, bookId, passages, asked, answer, generation](const QString &) {
                    if (!self || self->m_generation != generation)
                        return;
                    self->setStatus(tr("Finding the passages that matter…"));
                    self->m_index.search(bookId, passages, asked, 8, answer);
                });
            return;
        }
        m_index.search(bookId, passages, asked, 8, answer);
    };

    if (wholeBook)
        readWholeBook(readLimit, onChapters);
    else
        readSoFar(m_reader->chapter(), m_reader->pageEnd(), readLimit, onChapters);
}

void Companion::askShelf(const QString &asked) {
    const int bookId = m_reader ? m_reader->bookId() : 0;
    m_lastAction = {QStringLiteral("ask"), asked, m_reader ? m_reader->chapter() : 0,
                    m_reader ? m_reader->pageEnd() : 0, {}, {}};
    begin(QStringLiteral("ask"), asked);
    if (!modelFor(m_feature))
        return;
    if (cloudFor(m_feature) && !checkBudget(m_feature, 12000, false, 500))
        return;

    m_conversation.append({true, asked, {}, {}, false, {}, -1, -1});
    m_pendingUserTurn = true;
    emit conversationChanged();
    emit answerChanged();

    QString shelfName;
    const QList<CompanionStore::ShelfBook> shelfBooks = m_store.shelfBooksFor(bookId, &shelfName);
    if (shelfName != m_shelfLabel) {
        m_shelfLabel = shelfName;
        emit scopeChanged();
    }
    QSet<int> shelfIds;
    for (const CompanionStore::ShelfBook &b : shelfBooks)
        shelfIds.insert(b.id);

    setStatus(shelfName.isEmpty() ? tr("Searching across your library…")
                                  : tr("Searching across %1…").arg(shelfName));

    const int generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_readCancel = cancel;
    QPointer<Companion> self(this);

    auto resolvePassages = [self, asked, bookId, shelfBooks, shelfName, cancel,
                            generation](const QList<PassageIndex::LibraryHit> &libHits) {
        if (!self || self->m_generation != generation || cancel->load())
            return;
        self->setStatus(tr("Reading matching passages…"));

        (void)QtConcurrent::run([self, asked, bookId, shelfBooks, libHits, shelfName, cancel, generation] {
            const auto resolved = BookContextReader::resolveShelfContext(shelfBooks, libHits, asked, cancel);
            if (cancel->load())
                return;
            if (QCoreApplication *app = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(
                    app,
                    [self, bookId, resolved, shelfName, generation] {
                        if (!self || self->m_generation != generation)
                            return;
                        if (resolved.context.trimmed().isEmpty()) {
                            self->fail(tr("No matching passages were found on this shelf. Try another question or "
                                          "switch to Whole book."));
                            return;
                        }
                        const bool isCloud = self->cloudFor(QStringLiteral("ask"));
                        const qint64 budgetChars =
                            isCloud ? cloudLimit : self->localCharBudget(QStringLiteral("ask"), false, localLimit);
                        const QString trimmedContext =
                            resolved.context.left(qsizetype(qMax<qint64>(2000, budgetChars - 2500)));
                        if (isCloud && !self->checkBudget(QStringLiteral("ask"), trimmedContext.size(), false, 500))
                            return;

                        self->m_currentSources = resolved.citations;
                        emit self->answerChanged();

                        const QList<Ai::Message> turns = self->buildHistoryMessages(isCloud ? 32000 : 4000, false);

                        Ai::TextRequest request;
                        request.feature = QStringLiteral("ask");
                        request.stream = true;
                        const QString whereDesc =
                            shelfName.isEmpty() ? QStringLiteral("the reader's library")
                                                : QStringLiteral("the shelf \"%1\"").arg(shelfName);
                        request.system = instructions(BookContextReader::shelfAskTaskPrompt(whereDesc), false, true);
                        request.messages = {
                            {true, QStringLiteral("Currently open: %1\n\nExcerpts from %2:\n\n%3")
                                       .arg(self->bookLine(), whereDesc, trimmedContext)}};
                        request.messages.append(
                            {false, QStringLiteral("I've reviewed those excerpts. What would you like to know?")});
                        request.messages += turns;

                        self->run(QStringLiteral("ask"), request, [self, bookId, generation](Ai::Reply *) {
                            if (!self || self->m_generation != generation)
                                return;
                            if (self->m_answer.trimmed().isEmpty()) {
                                self->fail(tr("The model returned an empty answer. Try again."));
                                return;
                            }
                            self->commitAssistantTurn(bookId, self->m_currentSources, -1, -1);
                        });
                    },
                    Qt::QueuedConnection);
            }
        });
    };

    m_index.searchLibrary(asked, 10, resolvePassages, shelfIds);
}

// --- Previously…, Who's who --------------------------------------------------

void Companion::recap() {
    Q_ASSERT(QThread::currentThread() == thread());
    runRecap(false);
}

void Companion::refreshRecap() {
    Q_ASSERT(QThread::currentThread() == thread());
    runRecap(true);
}

void Companion::runRecap(bool forceRefresh) {
    const int bookId = m_reader->bookId();
    const int chapter = m_reader->chapter();
    const qint64 pageEnd = m_reader->pageEnd();
    const qint64 pageStart = m_reader->pageStart();
    const bool isStudy = studyMode();
    const bool wholeStudy = wholeBookStudyActive();
    const int cacheChapter = wholeStudy ? -1 : chapter;
    const qint64 cacheOffset = wholeStudy ? 0 : pageEnd;
    m_lastAction = {QStringLiteral("recap"), QString(), chapter, pageEnd, {}, {}};
    begin(QStringLiteral("recap"), wholeStudy ? m_reader->title() : placeTitle(chapter));
    if (!forceRefresh) {
        const bool memoryValid =
            m_recapLoaded && !m_recapText.trimmed().isEmpty() &&
            (wholeStudy ? (m_recapChapter == -1) : (m_recapChapter == chapter && (pageEnd <= 0 || m_recapOffset <= pageEnd)));
        if (memoryValid || loadRecapSnapshot(bookId, chapter, pageEnd, pageStart, true)) {
            m_answer = m_recapText;
            finishCachedSnapshot(m_recapWhere, m_recapWhereLocal);
            if (!memoryValid)
                emit recapChanged();
            emit answerChanged();
            emit changed();
            return;
        }
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 estChars = wholeStudy ? wholeBookCharsEstimate() : soFarCharsEstimate();
    if (cloud && !checkBudget(m_feature, estChars, false, 500))
        return;
    const qint64 limit = cloud ? cloudLimit : localCharBudget(m_feature, false, localLimit);
    const int generation = m_generation;
    setStatus(wholeStudy ? tr("Reading the document…") : tr("Reading the book so far…"));

    auto onChapters = [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, isStudy, wholeStudy,
                       limit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        if (!wholeStudy && isAheadOfReader(chapter, pageEnd)) {
            stop();
            return;
        }
        const bool isCloud = cloudFor(QStringLiteral("recap"));
        const qint64 effectiveLimit =
            isCloud ? cloudLimit : qMin(limit, localCharBudget(QStringLiteral("recap"), false, localLimit));
        const QString titles = BookContextReader::formatChapterTitles(chapters);
        const QString text = wholeStudy ? BookContextReader::joinedStudyDocument(chapters, effectiveLimit, chapter)
                                        : BookContextReader::joined(chapters, effectiveLimit);
        if (text.trimmed().isEmpty()) {
            fail(tr("There isn't any text to summarize yet. Turn a page or two first."));
            return;
        }
        if (!checkBudget(QStringLiteral("recap"), text.size() + titles.size(), false, 500))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("recap");
        request.stream = true;
        request.system = instructions(BookContextReader::recapTaskPrompt(isStudy), isStudy ? !wholeStudy : true, isStudy);
        request.messages = {{true, bookLine() +
                                       (wholeStudy ? QStringLiteral("\n\nSections:\n")
                                                   : QStringLiteral("\n\nChapters so far:\n")) +
                                       titles + QStringLiteral("\n") + text}};
        run(QStringLiteral("recap"), request,
            [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, wholeStudy, generation](Ai::Reply *) {
                if (generation != m_generation)
                    return;
                if (m_answer.trimmed().isEmpty()) {
                    fail(tr("The model returned an empty answer. Try again."));
                    return;
                }
                if (!wholeStudy && isAheadOfReader(chapter, pageEnd))
                    return;
                m_recapText = m_answer;
                m_recapLoaded = true;
                m_recapChapter = cacheChapter;
                m_recapOffset = cacheOffset;
                m_recapWhere = m_where;
                m_recapWhereLocal = m_whereLocal;
                m_store.saveRecapSnapshot(bookId, m_readingMode, cacheChapter, cacheOffset, wholeStudy ? 0 : pageStart,
                                          m_where, m_whereLocal, m_recapText);
                emit recapChanged();
            });
    };

    if (wholeStudy)
        readWholeBook(limit, onChapters);
    else
        readSoFar(chapter, pageEnd, limit, onChapters);
}

void Companion::refreshWhosWho() {
    Q_ASSERT(QThread::currentThread() == thread());
    m_peopleKey.clear();
    m_peopleWhere.clear();
    m_peopleWhereLocal = false;
    runWhosWho(true);
}

void Companion::whosWho() {
    Q_ASSERT(QThread::currentThread() == thread());
    runWhosWho(false);
}

void Companion::runWhosWho(bool forceRefresh) {
    const int bookId = m_reader->bookId();
    const int chapter = m_reader->chapter();
    const qint64 pageEnd = m_reader->pageEnd();
    const qint64 pageStart = m_reader->pageStart();
    const QString format = m_reader->format();
    const bool isStudy = studyMode();
    const bool wholeStudy = wholeBookStudyActive();
    const int cacheChapter = wholeStudy ? -1 : chapter;
    const qint64 cacheOffset = wholeStudy ? 0 : pageEnd;
    const QString where = m_ai->whereFor(QStringLiteral("whoswho"));
    const QString key =
        QStringLiteral("%1:%2:%3:%4:%5").arg(bookId).arg(m_readingMode).arg(cacheChapter).arg(cacheOffset).arg(where);
    m_lastAction = {QStringLiteral("whoswho"), QString(), chapter, pageEnd, {}, {}};
    begin(QStringLiteral("whoswho"), wholeStudy ? m_reader->title() : placeTitle(chapter));
    if (!forceRefresh) {
        const bool memoryValid =
            m_peopleLoaded &&
            (wholeStudy ? (m_peopleChapter == -1) : (m_peopleChapter == chapter && (pageEnd <= 0 || m_peopleOffset <= pageEnd))) &&
            (key == m_peopleKey || !m_people.isEmpty());
        if (memoryValid || loadWhosWhoSnapshot(bookId, chapter, pageEnd, pageStart, true)) {
            finishCachedSnapshot(m_peopleWhere.isEmpty() ? where : m_peopleWhere, m_peopleWhereLocal);
            emit changed();
            return;
        }
    }
    // Never leave a later-chapter snapshot visible if the reader jumped backward in spoiler-free mode.
    if (!wholeStudy && isAheadOfReader(m_peopleChapter, m_peopleOffset)) {
        resetPeopleState();
        emit changed();
    }
    if (!modelFor(m_feature))
        return;
    const bool cloud = cloudFor(m_feature);
    const qint64 estChars = wholeStudy ? wholeBookCharsEstimate() : soFarCharsEstimate();
    if (cloud && !checkBudget(m_feature, estChars, false, 800))
        return;
    // Budget strictly against the local model's actual context window (2.5 chars/token for JSON/non-Latin safety).
    const qint64 readLimit = cloud ? cloudLimit : localCharBudget(m_feature, false, localWhosWhoLimit, 2.5);
    const int generation = m_generation;
    setStatus(wholeStudy ? tr("Scanning notation & terms…") : tr("Reading the book so far…"));

    auto onChapters = [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, isStudy, wholeStudy,
                       format, key, readLimit, generation](const QList<Chapter> &chapters) {
        if (generation != m_generation)
            return;
        if (!wholeStudy && isAheadOfReader(chapter, pageEnd)) {
            stop();
            return;
        }
        const bool isCloud = cloudFor(QStringLiteral("whoswho"));
        const qint64 effectiveLimit =
            isCloud ? cloudLimit
                    : qMin(readLimit, localCharBudget(QStringLiteral("whoswho"), false, localWhosWhoLimit, 2.5));
        const QString text = wholeStudy ? BookContextReader::joinedStudyDocument(chapters, effectiveLimit, chapter)
                                        : BookContextReader::joined(chapters, effectiveLimit);
        if (text.trimmed().isEmpty()) {
            m_people.clear();
            m_peopleLoaded = true;
            m_peopleChapter = cacheChapter;
            m_peopleOffset = cacheOffset;
            m_peopleKey = key;
            m_peopleWhere.clear();
            m_peopleWhereLocal = false;
            finishCachedSnapshot(m_where, m_whereLocal);
            emit changed();
            return;
        }
        if (!checkBudget(QStringLiteral("whoswho"), text.size(), false, 800))
            return;
        Ai::TextRequest request;
        request.feature = QStringLiteral("whoswho");
        // Use the full context window on local models (never forcing 24K chars into num_ctx: 8192).
        request.fast = false;
        request.schema = BookContextReader::whosWhoSchema(isStudy);
        request.system = instructions(BookContextReader::whosWhoTaskPrompt(isStudy), isStudy ? !wholeStudy : true, false);
        request.messages = {{true, bookLine() + QStringLiteral("\n\n") + text}};
        run(QStringLiteral("whoswho"), request,
            [this, bookId, chapter, pageEnd, pageStart, cacheChapter, cacheOffset, isStudy, wholeStudy, format,
             chapters, key, generation](Ai::Reply *reply) {
                if (generation != m_generation)
                    return;
                if (!wholeStudy && isAheadOfReader(chapter, pageEnd))
                    return;
                QJsonObject result = reply->result();
                if (result.isEmpty())
                    result = BookContextReader::extractJsonObject(reply->text());
                const QJsonValue entriesVal = result.contains(QStringLiteral("terms"))
                                                  ? result.value(QStringLiteral("terms"))
                                                  : result.value(QStringLiteral("people"));
                if (result.isEmpty() || !entriesVal.isArray()) {
                    fail(isStudy ? tr("Glossary couldn't be read from the model's answer. Try again.")
                                 : tr("Who's who couldn't be read from the model's answer. Try again."));
                    return;
                }
                m_people = BookContextReader::parseWhosWhoEntries(entriesVal.toArray(), chapters, format, isStudy);
                m_peopleLoaded = true;
                m_peopleChapter = cacheChapter;
                m_peopleOffset = cacheOffset;
                m_peopleKey = key;
                m_peopleWhere = m_where;
                m_peopleWhereLocal = m_whereLocal;
                m_answer.clear();
                m_store.saveWhosWhoSnapshot(bookId, m_readingMode, cacheChapter, cacheOffset,
                                            wholeStudy ? 0 : pageStart, m_where, m_whereLocal, m_people);
            });
    };

    if (wholeStudy)
        readWholeBook(readLimit, onChapters);
    else
        readSoFar(chapter, pageEnd, readLimit, onChapters);
}
