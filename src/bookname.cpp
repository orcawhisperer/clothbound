#include "bookname.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace {
constexpr auto ci = QRegularExpression::CaseInsensitiveOption;

int romanValue(const QString &roman) {
    static const QHash<QChar, int> values{{'I', 1}, {'V', 5}, {'X', 10}, {'L', 50}, {'C', 100}};
    int total = 0, previous = 0;
    for (auto it = roman.crbegin(); it != roman.crend(); ++it) {
        const int value = values.value(it->toUpper(), 0);
        total += value < previous ? -value : value;
        previous = qMax(previous, value);
    }
    return total;
}

QString cleanPublisher(QString publisher) {
    publisher = publisher.simplified();
    publisher.remove(QRegularExpression(R"([,\s]+(Ltd|Limited|Inc|LLC|Co|Corp|Corporation|plc)\.?$)", ci));
    return publisher.trimmed();
}

int yearOf(const QString &date) {
    static const QRegularExpression year(R"(\b(1[5-9]\d\d|20\d\d)\b)");
    const auto match = year.match(date);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}
}

namespace BookName {
FileNameParts parseFileName(const QString &fileName) {
    FileNameParts parts;
    QString base = fileName.simplified();
    static const QRegularExpression extension(R"(\.(epub|kepub|pdf|mobi|azw3?|fb2|cbz|txt)$)", ci);
    base.remove(extension);

    static const QRegularExpression publisherYear(R"(\s*\(([^()]*?),\s*(\d{4})\)\s*$)");
    static const QRegularExpression yearOnly(R"(\s*\((\d{4})\)\s*$)");
    if (const auto match = publisherYear.match(base); match.hasMatch()) {
        parts.publisher = match.captured(1).trimmed();
        parts.year = match.captured(2).toInt();
        base.truncate(match.capturedStart());
    } else if (const auto year = yearOnly.match(base); year.hasMatch()) {
        parts.year = year.captured(1).toInt();
        base.truncate(year.capturedStart());
    }

    QString author, rest;
    static const QRegularExpression editorFirst(R"(^(.+?)\s*\[eds?\.?\]\s+(.+)$)", ci);
    const qsizetype dash = base.indexOf(QStringLiteral(" - "));
    if (dash > 0) {
        author = base.left(dash);
        rest = base.mid(dash + 3);
    } else if (const auto match = editorFirst.match(base); match.hasMatch()) {
        author = match.captured(1);
        rest = match.captured(2);
        parts.authorIsEditor = true;
    } else {
        parts.title = displayTitle(base);
        return parts;
    }
    static const QRegularExpression editorMark(R"(\s*[\[(]eds?\.?[\])]\s*$)", ci);
    if (author.contains(editorMark)) {
        author.remove(editorMark);
        parts.authorIsEditor = true;
    }

    static const QRegularExpression bracket(R"(\s*\[([^\]]*)\]\s*)");
    auto matches = bracket.globalMatch(rest);
    while (matches.hasNext()) {
        const QString content = matches.next().captured(1).trimmed();
        static const QRegularExpression editor(R"(^eds?\.?\s+(.+)$)", ci);
        static const QRegularExpression illustrator(R"(^illus(?:trated by|\.)?\s+(.+)$)", ci);
        if (const auto e = editor.match(content); e.hasMatch())
            parts.editor = e.captured(1).trimmed();
        else if (const auto i = illustrator.match(content); i.hasMatch())
            parts.illustrator = i.captured(1).trimmed();
        else if (!content.isEmpty())
            parts.note = content;
    }
    rest.replace(bracket, QStringLiteral(" "));

    parts.author = displayAuthor(author);
    parts.title = displayTitle(rest);
    parts.matched = !parts.author.isEmpty() && !parts.title.isEmpty();
    return parts;
}

QString displayAuthor(const QString &raw) {
    QString name = raw.simplified();
    static const QRegularExpression role(R"(\s*[\[(](?:eds?|editor|edited by|trans|translator)\.?[\])])", ci);
    static const QRegularExpression lifeDates(R"(,?\s*\(?\d{4}\s*-\s*(?:\d{4})?\)?\s*$)");
    name.remove(role);
    name.remove(lifeDates);
    name = name.trimmed();
    while (name.endsWith(',') || name.endsWith(';'))
        name.chop(1);

    // "Wells, H. G." reads "H. G. Wells"; a name with several commas is a
    // list, and suffixes like "King, Jr." or "Media, Inc." stay in place.
    if (name.count(',') == 1) {
        const QString last = name.section(',', 0, 0).trimmed(), first = name.section(',', 1).trimmed();
        static const QRegularExpression suffixAfterComma(
            R"(^(?:jr|sr|ii|iii|iv|v|phd|ph\.d|md|m\.d|dds|esq|inc|ltd|llc|corp|co)\.?$)", ci);
        if (!first.isEmpty() && !last.isEmpty() && !first.contains(QStringLiteral(" and ")) &&
            !suffixAfterComma.match(first).hasMatch())
            name = first + ' ' + last;
    }
    static const QRegularExpression packedInitial(R"(\b([A-Z])\.(?=\S))");
    name.replace(packedInitial, QStringLiteral("\\1. "));

    QStringList words = name.split(' ', Qt::SkipEmptyParts);
    static const QRegularExpression initials(R"(^[A-Z]{2,3}$)");
    static const QSet<QString> acronyms{
        "AI", "ML", "DR", "MR", "MS", "MRS", "ST", "DE", "LA", "LE", "EL", "II", "III", "IV", "VI", "VII", "IX",
        "XI", "XII", "MIT", "BBC", "GNU", "IBM", "USA", "CIA", "FBI", "ACM", "WHO", "UN", "EU", "UK", "US", "NYT",
        "NPR", "CNN", "ABC", "CBS", "NBC", "PBS", "NHK", "OUP", "CUP", "PHP", "SQL", "CSS", "XML", "API", "AWS",
    };
    static const QSet<QString> orgWords{
        "press", "books", "publishing", "publications", "media", "news", "group", "project", "foundation",
        "institute", "university", "lab", "labs", "research", "team", "society", "association", "committee",
        "staff", "editors", "editorial", "magazine", "review", "journal", "library", "museum", "studio",
    };
    for (qsizetype i = 0; i + 1 < words.size(); ++i)
        if (initials.match(words[i]).hasMatch() && !acronyms.contains(words[i]) &&
            !orgWords.contains(words[i + 1].toLower())) {
            QStringList letters;
            for (const QChar c : std::as_const(words[i]))
                letters.append(QString(c) + '.');
            words[i] = letters.join(' ');
        }
    name = words.join(' ');

    // "H. G. WELLS" reads "H. G. Wells".
    if (name == name.toUpper() && name != name.toLower()) {
        QStringList titled;
        for (const QString &word : name.split(' ', Qt::SkipEmptyParts))
            titled.append(word.size() <= 2 && word.endsWith('.') ? word : word.left(1) + word.mid(1).toLower());
        name = titled.join(' ');
    }
    return name.simplified();
}

QString sortAuthor(const QString &displayName) {
    QStringList words = displayName.split(' ', Qt::SkipEmptyParts);
    if (words.size() < 2)
        return displayName;
    static const QStringList suffixes{"Jr.", "Jr", "Sr.", "Sr", "II", "III", "IV"};
    QString suffix;
    if (words.size() > 2 && suffixes.contains(words.last()))
        suffix = words.takeLast();
    QString surname = words.takeLast();
    static const QStringList particles{"de", "van", "von", "der", "den", "du", "la", "le", "da", "di", "del"};
    while (!words.isEmpty() && particles.contains(words.last().toLower()))
        surname = words.takeLast() + ' ' + surname;
    QString sorted = surname;
    if (!words.isEmpty())
        sorted += QStringLiteral(", ") + words.join(' ');
    if (!suffix.isEmpty())
        sorted += QStringLiteral(", ") + suffix;
    return sorted;
}

QString displayTitle(const QString &title) {
    QString t = title.simplified();
    static const QRegularExpression colonStandIn(R"(\s*_\s+)");
    t.replace(colonStandIn, QStringLiteral(": "));
    t.replace('_', ' ');
    static const QRegularExpression article(R"(^(.+?),\s+(The|A|An)(\s*:.*)?$)");
    if (const auto match = article.match(t); match.hasMatch())
        t = match.captured(2) + ' ' + match.captured(1) + match.captured(3);
    return t.simplified();
}

QString cleanTitle(const QString &title) {
    // Metadata text has already been decoded by HtmlTokenizer; do not decode
    // entities a second time (which corrupts literal entities and creates tags).
    QString t = title.simplified();
    static const QRegularExpression byline(R"(\s+--\s+.*$)");
    t.remove(byline);
    static const QRegularExpression trailing(R"(\s*\(([^()]*)\)\s*$)");
    static const QRegularExpression imprint(
        R"(classic|library|edition|books|press|penguin|oxford|signet|bantam|dover|noble|everyman|series|collection)", ci);
    if (const auto match = trailing.match(t); match.hasMatch() && imprint.match(match.captured(1)).hasMatch())
        t.truncate(match.capturedStart());
    static const QRegularExpression colonStandIn(R"(\s*_\s+)");
    t.replace(colonStandIn, QStringLiteral(": "));
    t = t.trimmed();
    while (!t.isEmpty() && QStringLiteral(".,;:/").contains(t.back()))
        t.chop(1);
    return t.trimmed();
}

QString sortTitle(const QString &title) {
    QString t = title.toLower().trimmed();
    static const QRegularExpression leading(R"(^[\s"'“‘(\[]+)");
    t.remove(leading);
    static const QRegularExpression article(R"(^(the|a|an)\s+)");
    t.remove(article);
    return t;
}

QString comparable(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed)
        if (c.isLetterOrNumber())
            out.append(c.toLower());
    return out;
}

int volumeNumber(const QString &title) {
    static const QRegularExpression volume(
        R"(\b(vol(?:ume)?|book|part)\.?\s*(\d+|(?:X{1,3}(?:IX|IV|V?I{0,3})|IX|IV|VI{0,3}|I{1,3}|L|C))(?=$|[\s,:;\-)\]]))",
        ci);
    auto it = volume.globalMatch(title);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString num = match.captured(2);
        if (num.isEmpty())
            continue;
        bool ok = false;
        const int number = num.toInt(&ok);
        if (ok)
            return number;
        // Avoid matching the English pronoun "I" in phrases like "A Book I Read".
        if (num == QLatin1String("I") && match.captured(1).compare(QLatin1String("book"), Qt::CaseInsensitive) == 0) {
            const QString after = title.mid(match.capturedEnd()).trimmed();
            if (!after.isEmpty() && after.front().isLetter())
                continue;
        }
        return romanValue(num);
    }
    return 0;
}

QString workKey(const QString &author, const QString &title, int volume) {
    QStringList names = displayAuthor(author).split(' ', Qt::SkipEmptyParts);
    static const QStringList suffixes{"Jr.", "Jr", "Sr.", "Sr", "II", "III", "IV"};
    while (names.size() > 1 && suffixes.contains(names.last()))
        names.removeLast();
    const QString surname = names.isEmpty() ? QString() : comparable(names.last());

    QString t = cleanTitle(title).toLower();
    const qsizetype colon = t.indexOf(':');
    if (colon > 3) {
        // Strip descriptive/edition subtitles ("A Romance", "Or, ...",
        // "Illustrated Edition"), but preserve series-prefixed book titles
        // ("Harry Potter: The Chamber of Secrets", "Star Wars: Heir...").
        const QString subtitle = t.mid(colon + 1).trimmed();
        static const QRegularExpression descriptiveSubtitle(
            R"(^(?:a|an|or|being|annotated|illustrated|revised|expanded|complete|unabridged|abridged|critical|definitive|centenary|anniversary|selected|collected|with|translated|edited|and\s+other|vol(?:ume)?|book|part)\b)");
        if (descriptiveSubtitle.match(subtitle).hasMatch())
            t.truncate(colon);
    }
    static const QRegularExpression asides(R"(\([^)]*\)|\[[^\]]*\])");
    t.remove(asides);
    static const QRegularExpression volumeWords(R"(\b(?:vol(?:ume)?|book|part)\.?\s*(\d+|[ivxlc]+)\b)");
    t.remove(volumeWords);
    t.replace('&', QStringLiteral(" and "));
    static const QHash<QString, QString> spelledOut{{"dr", "doctor"}, {"mr", "mister"}, {"mrs", "missus"}, {"st", "saint"}};
    static const QRegularExpression wordSplit(R"([^\p{L}\p{N}]+)");
    QStringList words = t.split(wordSplit, Qt::SkipEmptyParts);
    for (QString &word : words)
        word = spelledOut.value(word, word);
    if (!words.isEmpty() && (words.first() == "the" || words.first() == "a" || words.first() == "an"))
        words.removeFirst();
    const QString titleKey = comparable(words.join(QString()));
    if (surname.isEmpty() && titleKey.isEmpty())
        return {};
    QString key = surname + '|' + titleKey;
    if (volume > 0)
        key += QStringLiteral("|v") + QString::number(volume);
    return key;
}

bool looksLikeGarbageTitle(const QString &title) {
    const QString t = title.trimmed();
    if (t.size() < 2 || comparable(t).isEmpty())
        return true;
    static const QRegularExpression garbage(
        R"(^(untitled|unknown|title|document|book|ebook|microsoft word.*|.*\.(docx?|pdf|indd|qxd|rtf|txt|html?))$)", ci);
    return garbage.match(t).hasMatch();
}

BookIdentity identify(const QString &filePath, const QString &metadataTitle, const QStringList &metadataAuthors,
                      const QStringList &metadataEditors, const QString &metadataPublisher,
                      const QString &metadataDate) {
    const QFileInfo info(filePath);
    const FileNameParts file = parseFileName(info.fileName());
    const QString fileAuthor = file.matched ? file.author : QString();

    QString title = cleanTitle(metadataTitle);
    QStringList displayAuthors;
    for (const QString &rawAuthor : metadataAuthors) {
        const QString formatted = displayAuthor(rawAuthor);
        if (!formatted.isEmpty() && !displayAuthors.contains(formatted))
            displayAuthors.append(formatted);
    }
    QString author = displayAuthors.join(QStringLiteral(" & "));

    // A title that is just the author's name is either a swap (the author
    // field then holds the real title) or no title at all.
    if (!title.isEmpty()) {
        const QString t = comparable(title);
        const bool titleIsAuthor = (!fileAuthor.isEmpty() && t == comparable(fileAuthor)) ||
                                   (!author.isEmpty() && t == comparable(author));
        if (titleIsAuthor) {
            if (metadataAuthors.size() == 1 && !author.isEmpty() && comparable(author) != t) {
                const QString swappedTitle = cleanTitle(metadataAuthors.first());
                author = fileAuthor.isEmpty() ? displayAuthor(title) : fileAuthor;
                title = swappedTitle;
            } else {
                title.clear();
            }
        }
    }
    if (title.isEmpty() || looksLikeGarbageTitle(title))
        title = file.matched ? file.title : displayTitle(info.completeBaseName());
    // A file name that says more about the same title ("Outline of History,
    // Vol. 2: …" for "The Outline of History") is the better title; it keeps
    // the metadata's leading article.
    if (file.matched && file.title != title) {
        const QString shortTitle = comparable(sortTitle(title)), longTitle = comparable(sortTitle(file.title));
        if (!shortTitle.isEmpty() && longTitle.size() > shortTitle.size() + 2 && longTitle.startsWith(shortTitle)) {
            static const QRegularExpression article(R"(^(The|A|An)\s)");
            const auto match = article.match(title);
            title = match.hasMatch() && !article.match(file.title).hasMatch() ? match.captured(1) + ' ' + file.title
                                                                              : file.title;
        }
    }
    if (author.isEmpty())
        author = fileAuthor;
    // The file name's spelling, when it names the same person, keeps one
    // author looking the same across the whole collection.
    if (!fileAuthor.isEmpty() && comparable(fileAuthor) == comparable(author))
        author = fileAuthor;

    BookIdentity identity;
    identity.title = title;
    identity.author = author;
    identity.sortTitle = sortTitle(title);
    identity.sortAuthor = sortAuthor(author);
    // Books by several people sort under the one the collection is named
    // for, or else under the first of them.
    if (!fileAuthor.isEmpty() && comparable(author) != comparable(fileAuthor) &&
        comparable(author).contains(comparable(fileAuthor)))
        identity.sortAuthor = sortAuthor(fileAuthor);
    else if (static const QRegularExpression several(R"(\s+(?:&|and)\s+|;\s*)"); author.contains(several))
        identity.sortAuthor = sortAuthor(author.section(several, 0, 0));
    identity.editor = !file.editor.isEmpty() ? file.editor : metadataEditors.join(QStringLiteral(" & "));
    identity.publisher = !file.publisher.isEmpty() ? file.publisher : cleanPublisher(metadataPublisher);
    identity.year = file.year > 0 ? file.year : yearOf(metadataDate);
    int volume = volumeNumber(title);
    if (volume == 0 && file.matched)
        volume = volumeNumber(file.title);
    identity.workKey = workKey(author, title, volume);
    return identity;
}
}
