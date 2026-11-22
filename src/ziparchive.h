#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

// Reads entries of a ZIP archive (an EPUB is one) by name, in any order and
// from any thread. It covers what books use: stored and deflated entries,
// with zip64 for very large archives. Nothing is ever written.
class ZipArchive {
public:
    explicit ZipArchive(const QString &path);

    bool open();
    bool isOpen() const { return m_open; }
    QString errorString() const { return m_error; }
    QString path() const { return m_path; }

    // Entry names in archive order, without directories.
    QStringList names() const { return m_names; }
    bool contains(const QString &name) const { return m_entries.contains(name); }
    // The stored name for `name`: itself, or a case-insensitive match, since
    // books sometimes link "Images/Cover.jpg" to "images/cover.jpg".
    QString resolve(const QString &name) const;
    // Uncompressed size, or -1 when there is no such entry.
    qint64 size(const QString &name) const;
    bool encrypted(const QString &name) const;
    QByteArray read(const QString &name, QString *error = nullptr) const;

private:
    struct Entry {
        quint64 offset = 0;
        quint64 compressed = 0;
        quint64 uncompressed = 0;
        quint16 method = 0;
        quint16 flags = 0;
    };

    bool fail(const QString &message);
    bool readCentralDirectory(const QByteArray &directory, quint64 count);

    QString m_path;
    QString m_error;
    bool m_open = false;
    mutable QFile m_file;
    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    QHash<QString, QString> m_folded;
    QStringList m_names;
};
