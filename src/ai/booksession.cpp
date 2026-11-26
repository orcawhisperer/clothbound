#include "ai/booksession.h"

#include "ai/aiservices.h"
#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/illustrator.h"
#include "ai/passageindex.h"
#include "library/library.h"
#include "library/marks.h"
#include "reader/reader.h"

#include <QStandardPaths>
#include <QThread>

BookSession::BookSession(Library *library, BookScheme *scheme, AiServices *ai, PassageIndex *sharedPassageIndex,
                         const QString &illustrationsDir, QObject *parent)
    : QObject(parent), m_library(library) {
    Q_ASSERT(library != nullptr);

    m_reader = new Reader(library, scheme, this);
    m_reader->setSessionOwner(this);

    m_marks = new Marks(library, this);
    m_marks->setSessionOwner(this);

    m_bookSearch = new BookSearch(m_reader, this);
    m_bookSearch->setLibrary(library);

    const QString dbConnection = library ? library->database().connectionName() : QString();
    if (ai) {
        PassageIndex *index = sharedPassageIndex;
        if (!index && !dbConnection.isEmpty())
            index = new PassageIndex(ai, dbConnection, this);
        m_passageIndex = index;
        if (index)
            m_bookSearch->setMeaning(ai, index);
        else if (!dbConnection.isEmpty())
            m_bookSearch->setMeaning(ai, dbConnection);

        m_companion = new Companion(ai, m_reader, dbConnection, this, index);

        const QString resolvedPicturesDir =
            illustrationsDir.isEmpty()
                ? (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/illustrations"))
                : illustrationsDir;
        m_illustrator = new Illustrator(ai, m_reader, dbConnection, resolvedPicturesDir, this);
    }

    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (m_marks && m_reader)
            m_marks->setBook(m_reader->bookId());
        emit bookChanged();
    });
}

BookSession::~BookSession() {
    close();
    // Tear down reader consumers before destroying Reader so QPointer<Reader>
    // observers (including Narration/Cast/Download) see a clean close first.
    delete m_illustrator;
    m_illustrator = nullptr;
    delete m_companion;
    m_companion = nullptr;
    delete m_bookSearch;
    m_bookSearch = nullptr;
    delete m_marks;
    m_marks = nullptr;
    delete m_reader;
    m_reader = nullptr;
}

bool BookSession::openBook(int bookId) {
    Q_ASSERT(QThread::currentThread() == thread());
    return m_reader ? m_reader->openBook(bookId) : false;
}

void BookSession::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_reader && m_reader->isOpen())
        m_reader->close();
}

bool BookSession::isOpen() const { return m_reader && m_reader->isOpen(); }

int BookSession::bookId() const { return m_reader ? m_reader->bookId() : 0; }
