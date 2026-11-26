#pragma once

#include "ai/booksearch.h"
#include "ai/companion.h"
#include "ai/illustrator.h"
#include "library/marks.h"
#include "reader/reader.h"

#include <QObject>
#include <QPointer>
#include <QString>

class AiServices;
class BookScheme;
class Library;
class PassageIndex;

// One open reading session (one window or tab): owns per-session Reader,
// Marks, BookSearch, Companion, and Illustrator instances wired cleanly
// against the shared Library, BookScheme, AiServices, and PassageIndex.
// Multiple BookSession instances can coexist simultaneously with distinct
// or identical books without duplicating PassageIndex vector memory in RAM.
class BookSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY bookChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(Reader *reader READ reader CONSTANT)
    Q_PROPERTY(Marks *marks READ marks CONSTANT)
    Q_PROPERTY(BookSearch *bookSearch READ bookSearch CONSTANT)
    Q_PROPERTY(Companion *companion READ companion CONSTANT)
    Q_PROPERTY(Illustrator *illustrator READ illustrator CONSTANT)

public:
    BookSession(Library *library, BookScheme *scheme, AiServices *ai = nullptr,
                PassageIndex *sharedPassageIndex = nullptr, const QString &illustrationsDir = {},
                QObject *parent = nullptr);
    ~BookSession() override;

    Q_INVOKABLE bool openBook(int bookId);
    Q_INVOKABLE void close();

    bool isOpen() const;
    int bookId() const;

    Reader *reader() const { return m_reader; }
    Marks *marks() const { return m_marks; }
    BookSearch *bookSearch() const { return m_bookSearch; }
    Companion *companion() const { return m_companion; }
    Illustrator *illustrator() const { return m_illustrator; }
    PassageIndex *passageIndex() const { return m_passageIndex.data(); }

signals:
    void bookChanged();

private:
    QPointer<Library> m_library;
    QPointer<PassageIndex> m_passageIndex;
    Reader *m_reader = nullptr;
    Marks *m_marks = nullptr;
    BookSearch *m_bookSearch = nullptr;
    Companion *m_companion = nullptr;
    Illustrator *m_illustrator = nullptr;
};
