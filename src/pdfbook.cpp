#include "pdfbook.h"

#include <QPainter>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QRegularExpression>

PdfBook::PdfBook(const QString &path) : m_path(path), m_document(std::make_unique<QPdfDocument>()) {}

PdfBook::~PdfBook() = default;

bool PdfBook::open() {
    switch (m_document->load(m_path)) {
    case QPdfDocument::Error::None:
        return true;
    case QPdfDocument::Error::FileNotFound:
        m_error = QStringLiteral("The file is missing.");
        break;
    case QPdfDocument::Error::IncorrectPassword:
        m_error = QStringLiteral("This PDF is locked with a password.");
        break;
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        m_error = QStringLiteral("This PDF is protected by DRM.");
        break;
    case QPdfDocument::Error::InvalidFileFormat:
        m_error = QStringLiteral("This isn't a readable PDF.");
        break;
    default:
        m_error = QStringLiteral("This PDF can't be opened.");
        break;
    }
    return false;
}

int PdfBook::pageCount() const { return m_document->pageCount(); }

QString PdfBook::title() const {
    return m_document->metaData(QPdfDocument::MetaDataField::Title).toString().simplified();
}

QString PdfBook::subject() const {
    return m_document->metaData(QPdfDocument::MetaDataField::Subject).toString().simplified();
}

QString PdfBook::publisher() const { return {}; }

QDateTime PdfBook::created() const {
    return m_document->metaData(QPdfDocument::MetaDataField::CreationDate).toDateTime();
}

void PdfBook::splitPeople() const {
    if (m_peopleSplit)
        return;
    m_peopleSplit = true;
    const QString field = m_document->metaData(QPdfDocument::MetaDataField::Author).toString().simplified();
    static const QRegularExpression separators(R"(\s*(?:/|;|\|)\s*)");
    static const QRegularExpression editorMark(R"(\s*[\[(](?:eds?\.?|editor|edited by)[\])]\s*$)",
                                               QRegularExpression::CaseInsensitiveOption);
    for (QString person : field.split(separators, Qt::SkipEmptyParts)) {
        if (person.contains(editorMark)) {
            person.remove(editorMark);
            m_editors.append(person.trimmed());
        } else {
            m_authors.append(person.trimmed());
        }
    }
}

QStringList PdfBook::authors() const {
    splitPeople();
    return m_authors;
}

QStringList PdfBook::editors() const {
    splitPeople();
    return m_editors;
}

QList<PdfOutlineEntry> PdfBook::outline() const {
    QList<PdfOutlineEntry> entries;
    QPdfBookmarkModel model;
    model.setDocument(m_document.get());
    std::function<void(const QModelIndex &, int)> walk = [&](const QModelIndex &parent, int depth) {
        for (int row = 0; row < model.rowCount(parent); ++row) {
            const QModelIndex index = model.index(row, 0, parent);
            PdfOutlineEntry entry;
            entry.title = model.data(index, int(QPdfBookmarkModel::Role::Title)).toString().simplified();
            entry.page = model.data(index, int(QPdfBookmarkModel::Role::Page)).toInt();
            entry.depth = depth;
            if (!entry.title.isEmpty())
                entries.append(entry);
            walk(index, depth + 1);
        }
    };
    walk(QModelIndex(), 0);
    return entries;
}

QImage PdfBook::renderPage(int page, const QSize &size) const {
    if (page < 0 || page >= pageCount() || size.isEmpty())
        return {};
    // PDFium leaves unpainted areas transparent; books are printed on white.
    const QImage rendered = m_document->render(page, size);
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.drawImage(0, 0, rendered);
    return image;
}

QImage PdfBook::renderCover(int height) const {
    if (pageCount() < 1)
        return {};
    const QSizeF points = m_document->pagePointSize(0);
    if (points.isEmpty())
        return {};
    const int width = qMax(1, qRound(height * points.width() / points.height()));
    return renderPage(0, QSize(width, height));
}
