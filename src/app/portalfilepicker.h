#pragma once

// The xdg-desktop-portal file chooser, from Omacut (MIT, David Heinemeier
// Hansson), choosing folders and books to add instead of videos.

#include <QVariantMap>

#include "app/filepicker.h"

class QDBusPendingCallWatcher;

class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);
    ~PortalFilePicker() override;

    void chooseFolders() override;
    void chooseBooks() override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action {
        None,
        Open
    };

    void requestFile(const QString &method, const QString &title,
                     QVariantMap options, Action action);
    bool connectToRequestPath(const QString &path);
    void clearPending();
    // Best-effort Close of the pending portal request, if there is one.
    void closePendingRequest();

    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    // Bumped per request, so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
