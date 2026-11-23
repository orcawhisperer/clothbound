#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

// Choosing what to add to the library. The window only sees this
// interface, so tests can hand it a picker that needs no portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    ~FilePicker() override = default;

    Q_INVOKABLE virtual void chooseFolders() = 0;
    Q_INVOKABLE virtual void chooseBooks() = 0;

signals:
    void chosen(const QList<QUrl> &urls);
    void failed(const QString &message);
};
