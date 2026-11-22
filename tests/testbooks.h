#pragma once

// Builders for the tests' made-up books: a ZIP writer (stored, deflated,
// zip64), EPUBs around it, and a one-page PDF.

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QPair>
#include <QString>
#include <zlib.h>

namespace TestBooks {
inline void put16(QByteArray &out, quint16 v) {
    out.append(char(v & 0xff));
    out.append(char(v >> 8));
}
inline void put32(QByteArray &out, quint32 v) {
    put16(out, quint16(v & 0xffff));
    put16(out, quint16(v >> 16));
}
inline void put64(QByteArray &out, quint64 v) {
    put32(out, quint32(v & 0xffffffff));
    put32(out, quint32(v >> 32));
}

inline QByteArray rawDeflate(const QByteArray &data) {
    z_stream stream{};
    deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&stream, uLong(data.size()))), Qt::Uninitialized);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    deflate(&stream, Z_FINISH);
    out.truncate(qsizetype(stream.total_out));
    deflateEnd(&stream);
    return out;
}

struct ZipEntry {
    QString name;
    QByteArray data;
    bool deflate = true;
};

inline QByteArray zip(const QList<ZipEntry> &entries, bool zip64 = false) {
    QByteArray out, directory;
    for (const auto &entry : entries) {
        const QByteArray name = entry.name.toUtf8();
        const QByteArray stored = entry.deflate ? rawDeflate(entry.data) : entry.data;
        const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(entry.data.constData()), uInt(entry.data.size())));
        const quint64 offset = quint64(out.size());

        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0x0800);
        put16(out, entry.deflate ? 8 : 0);
        put32(out, 0);
        put32(out, crc);
        put32(out, quint32(stored.size()));
        put32(out, quint32(entry.data.size()));
        put16(out, quint16(name.size()));
        put16(out, 0);
        out.append(name);
        out.append(stored);

        put32(directory, 0x02014b50);
        put16(directory, 45);
        put16(directory, 45);
        put16(directory, 0x0800);
        put16(directory, entry.deflate ? 8 : 0);
        put32(directory, 0);
        put32(directory, crc);
        put32(directory, zip64 ? 0xffffffff : quint32(stored.size()));
        put32(directory, zip64 ? 0xffffffff : quint32(entry.data.size()));
        put16(directory, quint16(name.size()));
        put16(directory, zip64 ? 28 : 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0);
        put32(directory, zip64 ? 0xffffffff : quint32(offset));
        directory.append(name);
        if (zip64) {
            put16(directory, 0x0001);
            put16(directory, 24);
            put64(directory, quint64(entry.data.size()));
            put64(directory, quint64(stored.size()));
            put64(directory, offset);
        }
    }
    const quint64 directoryOffset = quint64(out.size());
    out.append(directory);
    if (zip64) {
        const quint64 recordOffset = quint64(out.size());
        put32(out, 0x06064b50);
        put64(out, 44);
        put16(out, 45);
        put16(out, 45);
        put32(out, 0);
        put32(out, 0);
        put64(out, quint64(entries.size()));
        put64(out, quint64(entries.size()));
        put64(out, quint64(directory.size()));
        put64(out, directoryOffset);
        put32(out, 0x07064b50);
        put32(out, 0);
        put64(out, recordOffset);
        put32(out, 1);
    }
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, zip64 ? 0xffff : quint16(entries.size()));
    put16(out, zip64 ? 0xffff : quint16(entries.size()));
    put32(out, zip64 ? 0xffffffff : quint32(directory.size()));
    put32(out, zip64 ? 0xffffffff : quint32(directoryOffset));
    put16(out, 0);
    return out;
}

inline bool write(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// An EPUB from its files, mimetype first and stored, as the format asks.
inline QByteArray epub(const QList<QPair<QString, QByteArray>> &files) {
    QList<ZipEntry> entries{{"mimetype", "application/epub+zip", false},
                            {"META-INF/container.xml",
                             R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>)"}};
    for (const auto &[name, data] : files)
        entries.append({name, data, true});
    return zip(entries);
}

inline QByteArray chapter(const QByteArray &title, const QByteArray &body) {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>)" + title + R"(</title><style>p { margin: 0 }</style></head>
<body>)" + body + R"(</body></html>)";
}

// A made-up two-chapter EPUB 2 book: NCX contents with a page list, a cover
// named by <meta name="cover">, and an editor in the creators.
inline QByteArray epub2(const QByteArray &coverPng) {
    return epub({
        {"OEBPS/content.opf", R"(<?xml version="1.0" encoding="utf-8"?>
<package version="2.0" unique-identifier="uid" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>The Lantern Keeper (Harbor Classics)</dc:title>
    <dc:creator opf:role="aut" opf:file-as="Marsh, Ada">Ada Marsh</dc:creator>
    <dc:creator opf:role="edt">Tom Reed</dc:creator>
    <dc:publisher>Harbor Books Ltd</dc:publisher>
    <dc:date opf:event="publication">1911-04-02</dc:date>
    <dc:date opf:event="modification">2020-01-01</dc:date>
    <dc:language>en</dc:language>
    <dc:identifier id="uid">urn:isbn:978-0-00-000000-2</dc:identifier>
    <dc:description>&lt;p&gt;A keeper &amp;amp; her light.&lt;/p&gt;&lt;p&gt;Second paragraph.&lt;/p&gt;</dc:description>
    <dc:subject>Fiction</dc:subject>
    <meta name="cover" content="cover-image"/>
    <meta name="calibre:series" content="Harbor Tales"/>
    <meta name="calibre:series_index" content="2"/>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
    <item id="ch1" href="text/chapter%201.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="text/chapter2.xhtml" media-type="application/xhtml+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="ch1"/>
    <itemref idref="ch2" linear="no"/>
  </spine>
</package>)"},
        {"OEBPS/toc.ncx", R"(<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <navMap>
    <navPoint id="n1" playOrder="1"><navLabel><text>Chapter One</text></navLabel><content src="text/chapter%201.xhtml"/>
      <navPoint id="n2" playOrder="2"><navLabel><text>The Storm</text></navLabel><content src="text/chapter%201.xhtml#storm"/></navPoint>
    </navPoint>
    <navPoint id="n3" playOrder="3"><navLabel><text>Notes</text></navLabel><content src="text/chapter2.xhtml"/></navPoint>
  </navMap>
  <pageList>
    <pageTarget type="normal" value="1"><navLabel><text>1</text></navLabel><content src="text/chapter%201.xhtml#p1"/></pageTarget>
    <pageTarget type="normal" value="2"><navLabel><text>2</text></navLabel><content src="text/chapter2.xhtml#p2"/></pageTarget>
  </pageList>
</ncx>)"},
        {"OEBPS/images/cover.png", coverPng},
        {"OEBPS/text/chapter 1.xhtml", chapter("One", R"(<h1 id="p1">Chapter One</h1>
<p>The lamp&#8217;s light &amp; the sea&nbsp;wind.</p>
<script>var ignored = "<p>not text</p>";</script>
<p id="storm">A storm came<span>&#xAD;</span> in.</p>)")},
        {"OEBPS/text/chapter2.xhtml", chapter("Two", R"(<p id="p2">A note.</p>)")},
        {"OEBPS/style.css", "p { text-indent: 1em }"},
    });
}

// A made-up EPUB 3 book: a navigation document with contents and page
// list, a cover-image property, refined roles, and an obfuscated font.
inline QByteArray epub3(const QByteArray &obfuscatedFont) {
    return epub({
        {"OEBPS/content.opf", R"(<?xml version="1.0" encoding="utf-8"?>
<package version="3.0" unique-identifier="pub-id" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="pub-id">urn:uuid:12345678-9abc-def0-1234-56789abcdef0</dc:identifier>
    <dc:title id="t1">Salt Roads</dc:title>
    <meta refines="#t1" property="title-type">main</meta>
    <dc:title id="t2">A Journey</dc:title>
    <meta refines="#t2" property="title-type">subtitle</meta>
    <dc:creator id="c1">Wen Lo</dc:creator>
    <meta refines="#c1" property="role" scheme="marc:relators">aut</meta>
    <dc:creator id="c2">Ira Bell</dc:creator>
    <meta refines="#c2" property="role" scheme="marc:relators">edt</meta>
    <dc:language>en-GB</dc:language>
    <meta property="belongs-to-collection" id="col">Road Books</meta>
    <meta refines="#col" property="group-position">3</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="cover" href="cover.jpg" media-type="image/jpeg" properties="cover-image"/>
    <item id="c" href="c.xhtml" media-type="application/xhtml+xml"/>
    <item id="font" href="fonts/f.otf" media-type="font/otf"/>
  </manifest>
  <spine page-progression-direction="ltr"><itemref idref="c"/></spine>
</package>)"},
        {"OEBPS/nav.xhtml", R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body>
<nav epub:type="toc"><ol>
  <li><a href="c.xhtml">Departure</a><ol><li><a href="c.xhtml#x">The <em>Salt</em> Flats</a></li></ol></li>
  <li><span>Appendices</span></li>
</ol></nav>
<nav epub:type="page-list" hidden=""><ol><li><a href="c.xhtml#pg7">vii</a></li></ol></nav>
</body></html>)"},
        {"OEBPS/cover.jpg", "not really a jpeg"},
        {"OEBPS/c.xhtml", chapter("C", R"(<p>Salt <a epub:type="noteref" href="#n1">1</a></p><aside epub:type="footnote" id="n1">Grey.</aside>)")},
        {"OEBPS/fonts/f.otf", obfuscatedFont},
        {"META-INF/encryption.xml", R"(<?xml version="1.0"?>
<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container" xmlns:enc="http://www.w3.org/2001/04/xmlenc#">
  <enc:EncryptedData>
    <enc:EncryptionMethod Algorithm="http://ns.adobe.com/pdf/enc#RC"/>
    <enc:CipherData><enc:CipherReference URI="OEBPS/fonts/f.otf"/></enc:CipherData>
  </enc:EncryptedData>
</encryption>)"},
    });
}

// A made-up book whose chapter is encrypted by a store, not obfuscated.
inline QByteArray drmEpub() {
    return epub({
            {"OEBPS/content.opf", R"(<package version="2.0" xmlns="http://www.idpf.org/2007/opf"><metadata/>
<manifest><item id="c" href="c.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="c"/></spine></package>)"},
            {"OEBPS/c.xhtml", "encrypted bytes"},
            {"META-INF/encryption.xml", R"(<encryption><EncryptedData><EncryptionMethod Algorithm="http://www.w3.org/2001/04/xmlenc#aes128-cbc"/>
<CipherData><CipherReference URI="OEBPS/c.xhtml"/></CipherData></EncryptedData></encryption>)"},
    });
}

// A one-page PDF with an info dictionary, written by hand.
inline QByteArray pdf(const QByteArray &title, const QByteArray &author) {
    QByteArray out = "%PDF-1.4\n";
    QList<qsizetype> offsets;
    auto object = [&](const QByteArray &body) {
        offsets.append(out.size());
        out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    object("<< /Type /Catalog /Pages 2 0 R >>");
    object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
    const QByteArray stream = "BT /F1 12 Tf 20 250 Td (Hello reader) Tj ET";
    object("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
    object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    object("<< /Title (" + title + ") /Author (" + author + ") >>");
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (const qsizetype offset : offsets)
        out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) + " /Root 1 0 R /Info 6 0 R >>\n";
    out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}
}
