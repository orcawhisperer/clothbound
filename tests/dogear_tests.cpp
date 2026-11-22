#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "bookinfo.h"
#include "bookname.h"
#include "bookscheme.h"
#include "library.h"
#include "reader.h"
#include "epubbook.h"
#include "htmltokenizer.h"
#include "pdfbook.h"
#include "testbooks.h"
#include "theme.h"
#include "translator.h"
#include "ziparchive.h"

class DogearTests : public QObject {
    Q_OBJECT

private:
    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QByteArray png(int width, int height) {
        QImage image(width, height, QImage::Format_RGB32);
        image.fill(QColor("#336699"));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return bytes;
    }

    static QByteArray fontPayload() {
        QByteArray font(2000, Qt::Uninitialized);
        for (int i = 0; i < font.size(); ++i)
            font[i] = char(i * 7 % 251);
        return font;
    }

    static QByteArray adobeObfuscate(QByteArray data) {
        const QByteArray key = QByteArray::fromHex("123456789abcdef0123456789abcdef0");
        for (int i = 0; i < 1024; ++i)
            data[i] = char(data[i] ^ key[i % 16]);
        return data;
    }

private slots:
    // --- Theme ---------------------------------------------------------

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QVERIFY(!theme.omarchy());
        QVERIFY(!theme.light());
        QCOMPARE(theme.background(), QString("#0e0e10"));
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "mode = \"light\"\naccent = \"#112233\"\n"
                                                 "background = \"#fffcf0\"\nforeground = \"#100f0f\"\n"
                                                 "lighter_background = \"#e6e4d9\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QVERIFY(theme.omarchy());
        QVERIFY(theme.light());
        QCOMPARE(theme.background(), QString("#fffcf0"));
        QCOMPARE(theme.surface(), QString("#e6e4d9"));
        // Keys the theme leaves out come from the fallback.
        QCOMPARE(theme.selection(), Theme::fallbackPalette().value("selection"));
    }

    void themeFollowsChanges() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
        Theme theme(dir.path());
        QSignalSpy changed(&theme, &Theme::changed);
        write(dir.path() + "/theme/colors.toml", "accent = \"#445566\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\n");
        QTRY_COMPARE(theme.accent(), QString("#445566"));
        QVERIFY(changed.count() >= 1);
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    // --- Translator ----------------------------------------------------

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n books": {"one": "%n book", "other": "%n books"}})");
        write(dir.path() + "/ru.json", R"({"%n books": {"one": "%n книга", "few": "%n книги", "many": "%n книг", "other": "%n книги"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 21), QString("%n книга"));
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 22), QString("%n книги"));
        QCOMPARE(translator.translate("Main", "%n books", nullptr, 25), QString("%n книг"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    void shippedCatalogsAreValidJson() {
        const QDir catalogs(":/i18n");
        // Catalogs are compiled into the app, not the tests; check the sources.
        const QDir sources(QFINDTESTDATA("../src/i18n"));
        for (const QString &name : sources.entryList({"*.json"})) {
            QFile file(sources.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError error;
            QJsonDocument::fromJson(file.readAll(), &error);
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + ": " + error.errorString()));
        }
        Q_UNUSED(catalogs);
    }

    // --- ZIP -----------------------------------------------------------

    void zipReadsStoredAndDeflatedEntries() {
        QTemporaryDir dir;
        const QByteArray big(100000, 'w');
        QVERIFY(TestBooks::write(dir.filePath("a.zip"),
                                 TestBooks::zip({{"plain.txt", "stored text", false},
                                                 {"dir/Deflated.TXT", big, true},
                                                 {"ünïcode name.txt", "ok", true}})));
        ZipArchive zip(dir.filePath("a.zip"));
        QVERIFY(zip.open());
        QCOMPARE(zip.names().size(), 3);
        QCOMPARE(zip.read("plain.txt"), QByteArray("stored text"));
        QCOMPARE(zip.read("dir/Deflated.TXT"), big);
        // Books sometimes link with different capitalization.
        QCOMPARE(zip.resolve("DIR/deflated.txt"), QString("dir/Deflated.TXT"));
        QCOMPARE(zip.read("dir/deflated.txt"), big);
        QCOMPARE(zip.read("ünïcode name.txt"), QByteArray("ok"));
        QCOMPARE(zip.size("dir/Deflated.TXT"), qint64(big.size()));
        QString error;
        QVERIFY(zip.read("missing.txt", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void zipReadsZip64() {
        QTemporaryDir dir;
        QVERIFY(TestBooks::write(dir.filePath("b.zip"),
                                 TestBooks::zip({{"one.txt", "first", true}, {"two.txt", "second", false}}, true)));
        ZipArchive zip(dir.filePath("b.zip"));
        QVERIFY2(zip.open(), qPrintable(zip.errorString()));
        QCOMPARE(zip.read("one.txt"), QByteArray("first"));
        QCOMPARE(zip.read("two.txt"), QByteArray("second"));
    }

    void zipRejectsOtherFiles() {
        QTemporaryDir dir;
        write(dir.filePath("c.zip"), "this is not a zip at all");
        ZipArchive zip(dir.filePath("c.zip"));
        QVERIFY(!zip.open());
        QVERIFY(!zip.errorString().isEmpty());
    }

    // --- Markup --------------------------------------------------------

    void tokenizerDecodesEntities() {
        QCOMPARE(HtmlTokenizer::decodeEntities(u"a &amp; b &lt;c&gt; &rsquo;&#8217;&#x2019; &nbsp;"),
                 QString("a & b <c> ’’’  "));
        // Unknown entities and bare ampersands stay as written.
        QCOMPARE(HtmlTokenizer::decodeEntities(u"R&D &bogus; &"), QString("R&D &bogus; &"));
        // Windows-1252 code points mean their curly quotes, as in browsers.
        QCOMPARE(HtmlTokenizer::decodeEntities(u"&#147;hi&#148;"), QString("“hi”"));
    }

    void tokenizerDecodesDeclaredEncodings() {
        const QByteArray latin1 = "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?><p>caf\xe9</p>";
        QVERIFY(HtmlTokenizer::decodeDocument(latin1).contains(QString("café")));
        QVERIFY(HtmlTokenizer::decodeDocument("\xEF\xBB\xBF<p>x</p>\r\n").endsWith("</p>\n"));
    }

    void treeParsesForgivingly() {
        const auto doc = Html::parse(QByteArray("<root><a x='1' Y=\"2\">one<br>two</a></b><dc:title>T &amp; U</dc:title></root>"), true);
        const HtmlNode *a = doc->first("a");
        QVERIFY(a);
        QCOMPARE(a->attribute("x"), QString("1"));
        QCOMPARE(a->attribute("y"), QString("2"));
        QCOMPARE(a->textContent(), QString("onetwo"));
        QCOMPARE(doc->first("title")->textContent(), QString("T & U"));
        // In book XML a <meta> holds its text.
        const auto xml = Html::parse(QByteArray("<metadata><meta property='role'>edt</meta></metadata>"));
        QCOMPARE(xml->first("meta")->textContent(), QString("edt"));
    }

    void textExtractionMatchesTheReader() {
        const QByteArray doc = R"(<html><head><title>Not text</title><style>p{}</style></head>
<body><p>One&nbsp;two</p><script>skip()</script>
<p>Sof&#173;t hy&shy;phens <b>bold</b></p><![CDATA[raw <text>]]></body></html>)";
        QCOMPARE(EpubBook::extractText(doc), QString("One two\nSoft hyphens bold" "raw <text>"));
        // Without a body, everything outside the head counts.
        QCOMPARE(EpubBook::extractText("<html><head><title>x</title></head><p>loose</p></html>"), QString("loose"));
    }

    void linksResolveInsideTheBook() {
        QString fragment;
        QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "../images/b%20c.png", &fragment), QString("OEBPS/images/b c.png"));
        QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "b.xhtml#note%201", &fragment), QString("OEBPS/text/b.xhtml"));
        QCOMPARE(fragment, QString("note 1"));
        QCOMPARE(EpubBook::resolve("OEBPS/text/a.xhtml", "#n2", &fragment), QString("OEBPS/text/a.xhtml"));
        QCOMPARE(fragment, QString("n2"));
        QCOMPARE(EpubBook::resolve("content.opf", "./text/c.xhtml"), QString("text/c.xhtml"));
        QVERIFY(EpubBook::resolve("OEBPS/a.xhtml", "https://example.com/x").isEmpty());
        QVERIFY(EpubBook::resolve("OEBPS/a.xhtml", "mailto:someone").isEmpty());
    }

    // --- EPUB ----------------------------------------------------------

    void epub2ReadsPackageAndNcx() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        EpubBook book(path);
        QVERIFY2(book.open(), qPrintable(book.errorString()));
        QCOMPARE(book.version(), QString("2.0"));
        const EpubMetadata &meta = book.metadata();
        QCOMPARE(meta.title, QString("The Lantern Keeper (Harbor Classics)"));
        QCOMPARE(meta.authors, QStringList{"Ada Marsh"});
        QCOMPARE(meta.editors, QStringList{"Tom Reed"});
        QCOMPARE(meta.date, QString("1911-04-02"));
        QCOMPARE(meta.isbn, QString("9780000000002"));
        QCOMPARE(meta.description, QString("A keeper & her light.\n\nSecond paragraph."));
        QCOMPARE(meta.series, QString("Harbor Tales"));
        QCOMPARE(meta.seriesIndex, 2.0);

        QCOMPARE(book.spine().size(), 2);
        QCOMPARE(book.spine()[0].path, QString("OEBPS/text/chapter 1.xhtml"));
        QVERIFY(!book.spine()[1].linear);

        QCOMPARE(book.toc().size(), 3);
        QCOMPARE(book.toc()[1].title, QString("The Storm"));
        QCOMPARE(book.toc()[1].depth, 1);
        QCOMPARE(book.toc()[1].fragment, QString("storm"));
        QCOMPARE(book.toc()[1].spine, 0);
        QCOMPARE(book.toc()[2].spine, 1);

        QCOMPARE(book.pageList().size(), 2);
        QCOMPARE(book.pageList()[1].label, QString("2"));
        QCOMPARE(book.pageList()[1].spine, 1);

        QCOMPARE(book.coverPath(), QString("OEBPS/images/cover.png"));
        QString type;
        QVERIFY(!book.coverImage(&type).isEmpty());
        QCOMPARE(type, QString("image/png"));
        QVERIFY(!book.isDrmProtected());

        QCOMPARE(book.chapterText(0), QString("Chapter One\nThe lamp’s light & the sea wind.\n\nA storm came in."));
    }

    void epub3ReadsNavigationAndRefines() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book3.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub3(adobeObfuscate(fontPayload()))));
        EpubBook book(path);
        QVERIFY2(book.open(), qPrintable(book.errorString()));
        QCOMPARE(book.metadata().title, QString("Salt Roads"));
        QCOMPARE(book.metadata().authors, QStringList{"Wen Lo"});
        QCOMPARE(book.metadata().editors, QStringList{"Ira Bell"});
        QCOMPARE(book.metadata().series, QString("Road Books"));
        QCOMPARE(book.metadata().seriesIndex, 3.0);
        QCOMPARE(book.toc().size(), 3);
        QCOMPARE(book.toc()[1].title, QString("The Salt Flats"));
        QCOMPARE(book.toc()[1].depth, 1);
        QCOMPARE(book.toc()[2].title, QString("Appendices"));
        QVERIFY(book.toc()[2].path.isEmpty());
        QCOMPARE(book.pageList().size(), 1);
        QCOMPARE(book.pageList()[0].label, QString("vii"));
        QCOMPARE(book.coverPath(), QString("OEBPS/cover.jpg"));
        // Font obfuscation is undone, and isn't mistaken for DRM.
        QVERIFY(!book.isDrmProtected());
        QCOMPARE(book.resource("OEBPS/fonts/f.otf"), fontPayload());
    }

    void epubWithEncryptedTextIsDrm() {
        QTemporaryDir dir;
        const QString path = dir.filePath("drm.epub");
        const QByteArray zip = TestBooks::drmEpub();
        QVERIFY(TestBooks::write(path, zip));
        EpubBook book(path);
        QVERIFY(book.open());
        QVERIFY(book.isDrmProtected());
        QCOMPARE(BookFiles::read(path).problem, QString("This book is protected by DRM."));
    }

    void epubWithoutChaptersFails() {
        QTemporaryDir dir;
        const QString path = dir.filePath("empty.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub({{"OEBPS/content.opf", "<package><manifest/><spine/></package>"}})));
        EpubBook book(path);
        QVERIFY(!book.open());
        QVERIFY(!book.errorString().isEmpty());
    }

    void bookInfoReadsEpubMetadataAndCover() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Marsh, Ada - Lantern Keeper, The [ed. Reed] (Harbor, 1911).epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(200, 300))));
        const BookInfo info = BookFiles::read(path, 90);
        QVERIFY(info.ok());
        QCOMPARE(info.format, QString("epub"));
        QCOMPARE(info.identity.title, QString("The Lantern Keeper"));
        QCOMPARE(info.identity.author, QString("Ada Marsh"));
        QCOMPARE(info.identity.editor, QString("Reed"));
        QCOMPARE(info.identity.publisher, QString("Harbor"));
        QCOMPARE(info.identity.year, 1911);
        QCOMPARE(info.chapters, 2);
        QCOMPARE(info.pages, 2);
        QCOMPARE(info.cover.height(), 90);
        QCOMPARE(info.cover.width(), 60);
    }

    // --- PDF -----------------------------------------------------------

    void pdfReadsMetadataAndRenders() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Marsh, Ada - Field Notes (Harbor, 1920).pdf");
        QVERIFY(TestBooks::write(path, TestBooks::pdf("Field Notes", "Ada Marsh / Tom Reed [editor]")));
        PdfBook book(path);
        QVERIFY2(book.open(), qPrintable(book.errorString()));
        QCOMPARE(book.pageCount(), 1);
        QCOMPARE(book.title(), QString("Field Notes"));
        QCOMPARE(book.authors(), QStringList{"Ada Marsh"});
        QCOMPARE(book.editors(), QStringList{"Tom Reed"});
        const QImage cover = book.renderCover(300);
        QCOMPARE(cover.size(), QSize(200, 300));
        // Rendered on white, with the text in black somewhere on it.
        QCOMPARE(QColor(cover.pixel(5, 5)), QColor(Qt::white));

        const BookInfo info = BookFiles::read(path, 150);
        QCOMPARE(info.format, QString("pdf"));
        QCOMPARE(info.pages, 1);
        QCOMPARE(info.identity.year, 1920);
        QCOMPARE(info.cover.height(), 150);
    }

    void unreadableFilesReportWhy() {
        QTemporaryDir dir;
        write(dir.filePath("broken.pdf"), "nope");
        QVERIFY(!BookFiles::read(dir.filePath("broken.pdf")).ok());
        QVERIFY(!BookFiles::read(dir.filePath("missing.epub")).ok());
        QVERIFY(BookFiles::isBook("a/b.EPUB"));
        QVERIFY(!BookFiles::isBook("notes.txt"));
    }


    // --- Library and reader ----------------------------------------------

    void libraryStoresBooksAndReadingState() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Marsh, Ada - Lantern Keeper, The (Harbor, 1911).epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY2(library.open(), qPrintable(library.errorString()));
        const int id = library.addBookFile(path);
        QVERIFY2(id > 0, qPrintable(library.errorString()));
        QCOMPARE(library.addBookFile(path), id);
        const BookRecord book = library.record(id);
        QCOMPARE(book.title, QString("The Lantern Keeper"));
        QVERIFY(book.loose);
        QCOMPARE(library.chapterLengths(id).size(), 2);
        QVERIFY(book.length > 0);
        QVERIFY(QFile::exists(dir.filePath("cache/covers/") + book.cover));

        library.savePosition(id, R"({"chapter":0,"offset":12})", 0.4);
        library.setFinished(id, true);
        Library reopened(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(reopened.open());
        QCOMPARE(reopened.record(id).progress, 0.4);
        QVERIFY(reopened.record(id).finishedAt > 0);
        QCOMPARE(reopened.record(id).position, QString(R"({"chapter":0,"offset":12})"));
    }

    void libraryGroupsEditionsIntoWorks() {
        QTemporaryDir dir;
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        for (const char *name : {"Marsh, Ada - Lantern Keeper, The [ed. Reed] (Harbor, 1911).epub",
                                 "Marsh, Ada - Lantern Keeper, The [ed. Gray] (Penguin, 2005).epub"}) {
            const QString path = dir.filePath(name);
            QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
            QVERIFY(library.addBookFile(path) > 0);
        }
        QTRY_COMPARE(library.works()->count(), 1);
        QCOMPARE(library.works()->get(0).value("editions").toInt(), 2);
        library.works()->setGrouped(false);
        QCOMPARE(library.works()->count(), 2);
        library.works()->setSearchText("penguin");
        QCOMPARE(library.works()->count(), 1);
    }

    void schemeServesWellFormedBooksAsXml() {
        QVERIFY(BookScheme::isWellFormed("<?xml version='1.0'?><html><body><p>x</p></body></html>"));
        QVERIFY(!BookScheme::isWellFormed("<html><body><p>unclosed</body></html>"));
        // Entities the XHTML DTD defines are fine for a browser's XML parser.
        QVERIFY(BookScheme::isWellFormed("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"x\"><html><body>a&nbsp;b</body></html>"));
    }

    void readerMapsPositionsOntoTheBook() {
        QTemporaryDir dir;
        const QString path = dir.filePath("book.epub");
        QVERIFY(TestBooks::write(path, TestBooks::epub2(png(20, 30))));
        Library library(dir.filePath("data"), dir.filePath("cache"));
        QVERIFY(library.open());
        const int id = library.addBookFile(path);
        BookScheme scheme;
        Reader reader(&library, &scheme);
        QVERIFY2(reader.openBook(id), qPrintable(reader.errorString()));
        QCOMPARE(reader.chapterCount(), 2);
        const QVariantList spine = reader.manifest().value("spine").toList();
        QCOMPARE(spine[0].toMap().value("href").toString(), QString("OEBPS/text/chapter%201.xhtml"));
        QVERIFY(reader.manifest().value("base").toString().startsWith("dogear://local/book/"));
        QCOMPARE(reader.titleOfChapter(1), QString("Notes"));

        const QList<int> lengths = library.chapterLengths(id);
        const double total = lengths[0] + lengths[1];
        QCOMPARE(reader.fractionAt(1, 0), lengths[0] / total);
        const QVariantMap middle = reader.targetForFraction(lengths[0] / total + 0.001);
        QCOMPARE(middle.value("chapter").toInt(), 1);
        QCOMPARE(reader.targetForPageLabel("2").value("chapter").toInt(), 1);
        QVERIFY(reader.targetForPageLabel("99").value("chapter").isValid());
        QVERIFY(!reader.targetForPageLabel("xx").contains("chapter"));

        reader.relocated({{"chapter", 1}, {"offset", 3}, {"snippet", "note"}, {"page", 0}, {"pages", 1}});
        QCOMPARE(reader.chapter(), 1);
        QCOMPARE(reader.chapterTitle(), QString("Notes"));
        reader.close();
        QCOMPARE(library.record(id).position, QString(R"({"chapter":1,"offset":3,"snippet":"note"})"));
    }

    // --- Names ---------------------------------------------------------

    void fileNamesFollowTheCollectionPattern() {
        FileNameParts parts = BookName::parseFileName("Austen, Jane - Pride and Prejudice [ed. Jones] (Penguin, 2003).epub");
        QVERIFY(parts.matched);
        QCOMPARE(parts.author, QString("Jane Austen"));
        QCOMPARE(parts.title, QString("Pride and Prejudice"));
        QCOMPARE(parts.editor, QString("Jones"));
        QCOMPARE(parts.publisher, QString("Penguin"));
        QCOMPARE(parts.year, 2003);

        parts = BookName::parseFileName("Melville, Herman - Whale Book, The_ A Voyage [illus. Kent] (Dover, 2015).pdf");
        QCOMPARE(parts.title, QString("The Whale Book: A Voyage"));
        QCOMPARE(parts.illustrator, QString("Kent"));

        parts = BookName::parseFileName("Ross, Ann [ed.] The Austen Reader (Harbor, 2001).epub");
        QVERIFY(parts.matched);
        QVERIFY(parts.authorIsEditor);
        QCOMPARE(parts.author, QString("Ann Ross"));
        QCOMPARE(parts.title, QString("The Austen Reader"));

        parts = BookName::parseFileName("Gray, Lee [ed.] - Letters of Two Friends (Harbor, 1959).pdf");
        QVERIFY(parts.authorIsEditor);
        QCOMPARE(parts.author, QString("Lee Gray"));

        QVERIFY(!BookName::parseFileName("just a title.epub").matched);
    }

    void authorsReadTheSameEveryWay() {
        for (const char *spelling : {"Wells, H. G.", "H.G. Wells", "HG Wells", "H. G. WELLS", "H.G.Wells"})
            QCOMPARE(BookName::displayAuthor(spelling), QString("H. G. Wells"));
        QCOMPARE(BookName::displayAuthor("Dickens, Charles, 1812-1870"), QString("Charles Dickens"));
        QCOMPARE(BookName::sortAuthor("H. G. Wells"), QString("Wells, H. G."));
        QCOMPARE(BookName::sortAuthor("Ludwig van Beethoven"), QString("van Beethoven, Ludwig"));
        QCOMPARE(BookName::sortAuthor("Plato"), QString("Plato"));
    }

    void titlesClean() {
        QCOMPARE(BookName::displayTitle("Moon Book, The"), QString("The Moon Book"));
        QCOMPARE(BookName::displayTitle("Coat, The, the Hat and the Glove"), QString("Coat, The, the Hat and the Glove"));
        QCOMPARE(BookName::displayTitle("Letters, Vol. 2_ 1904-1928"), QString("Letters, Vol. 2: 1904-1928"));
        QCOMPARE(BookName::cleanTitle("The New Road (Penguin Classics) -- A_ Writer"), QString("The New Road"));
        QCOMPARE(BookName::cleanTitle("The Isle of Dr. Lark (Barnes &amp; Noble Classics)"), QString("The Isle of Dr. Lark"));
        QCOMPARE(BookName::cleanTitle("Notes (1890)"), QString("Notes (1890)"));
        QCOMPARE(BookName::sortTitle("The Moon Book"), QString("moon book"));
        QVERIFY(BookName::looksLikeGarbageTitle("Microsoft Word - draft.doc"));
        QVERIFY(BookName::looksLikeGarbageTitle("untitled"));
        QVERIFY(!BookName::looksLikeGarbageTitle("Emma"));
    }

    void editionsShareAWorkKey() {
        const QString a = BookName::workKey("H. G. Wells", "The Isle of Doctor Lark");
        QCOMPARE(BookName::workKey("H.G. Wells", "The Isle of Dr. Lark (Barnes & Noble Classics)"), a);
        QCOMPARE(BookName::workKey("Wells, H. G.", "Isle of Dr Lark: A Romance"), a);
        QVERIFY(BookName::workKey("H. G. Wells", "The Sleeper Wakes") != BookName::workKey("H. G. Wells", "The Sleeper Awakes!"));
        // Volumes of one work are different books.
        QCOMPARE(BookName::volumeNumber("The Outline: Volume 2, Rome"), 2);
        QCOMPARE(BookName::volumeNumber("History, Vol. IV"), 4);
        QVERIFY(BookName::workKey("A", "Outline", 1) != BookName::workKey("A", "Outline", 2));
    }

    void identityResolvesSwapsAndMislabels() {
        // A title and author swapped in the metadata.
        BookIdentity id = BookName::identify("/b/Wells, H. G. - Moon Book, The [ed. Ross] (Penguin, 2005).epub",
                                             "H. G. Wells", {"The Moon Book"}, {}, "Penguin Books Ltd", "2005");
        QCOMPARE(id.title, QString("The Moon Book"));
        QCOMPARE(id.author, QString("H. G. Wells"));
        QCOMPARE(id.editor, QString("Ross"));

        // A file named for one book that holds another: the content wins.
        id = BookName::identify("/b/Wells, H. G. - Glass Man [ed. Ross] (Penguin, 2005).epub",
                                "The Isle of Doctor Lark", {"H.G. Wells"}, {}, {}, {});
        QCOMPARE(id.title, QString("The Isle of Doctor Lark"));
        QCOMPARE(id.author, QString("H. G. Wells"));
        QCOMPARE(id.workKey, BookName::workKey("H. G. Wells", "The Isle of Dr. Lark"));

        // No usable metadata: the file name speaks.
        id = BookName::identify("/b/Marsh, Ada - Field Notes, Vol. 2_ Coast (Harbor, 1920).pdf",
                                "Microsoft Word - fn2.doc", {}, {}, {}, {});
        QCOMPARE(id.title, QString("Field Notes, Vol. 2: Coast"));
        QCOMPARE(id.author, QString("Ada Marsh"));
        QCOMPARE(id.year, 1920);
        QVERIFY(id.workKey.endsWith("|v2"));

        // A file name more specific than the metadata title wins.
        id = BookName::identify("/b/Marsh, Ada - Atlas of Tides, Vol. 2_ The South (Harbor, 2004).epub",
                                "The Atlas of Tides", {"Ada Marsh"}, {}, {}, {});
        QCOMPARE(id.title, QString("The Atlas of Tides, Vol. 2: The South"));
        QVERIFY(id.workKey.endsWith("|v2"));

        // Several authors sort under the collection's own.
        id = BookName::identify("/b/Marsh, Ada - Letters [with Tom Reed] (Harbor, 1935).pdf",
                                "Letters", {"Tom Reed & Ada Marsh"}, {}, {}, {});
        QCOMPARE(id.author, QString("Tom Reed & Ada Marsh"));
        QCOMPARE(id.sortAuthor, QString("Marsh, Ada"));

        // Without a file-name pattern, metadata and publisher clean up.
        id = BookName::identify("/b/x.epub", "Emma", {"Austen, Jane"}, {"Ed Itor"}, "Harbor Books Ltd", "1815-12-23");
        QCOMPARE(id.author, QString("Jane Austen"));
        QCOMPARE(id.sortAuthor, QString("Austen, Jane"));
        QCOMPARE(id.publisher, QString("Harbor Books"));
        QCOMPARE(id.editor, QString("Ed Itor"));
        QCOMPARE(id.year, 1815);
    }
};

QTEST_MAIN(DogearTests)
#include "dogear_tests.moc"
