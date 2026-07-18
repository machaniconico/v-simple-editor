#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

#include "VimeoUploadManager.h"

class QPushButton;
class QTableWidget;

namespace vimeo {
namespace oauth {
class AuthClient;
} // namespace oauth
} // namespace vimeo

// ---------------------------------------------------------------------------
// VimeoUploadDialog — Sprint 20 US-VIM-2
// Modeless dialog for managing Vimeo upload jobs via vimeo::manager::Manager.
// ---------------------------------------------------------------------------

class VimeoUploadDialog : public QDialog {
    Q_OBJECT
public:
    explicit VimeoUploadDialog(vimeo::manager::Manager *manager,
                               QWidget *parent = nullptr);
    ~VimeoUploadDialog() override = default;

private slots:
    void onAddJobClicked();
    void onRetryClicked();
    void onCancelClicked();
    void onAuthenticateClicked();

    // Manager signals
    void onJobStateChanged(const QString &jobId, vimeo::manager::JobState state);
    void onJobProgress(const QString &jobId, int percent);

private:
    QString stateToString(vimeo::manager::JobState state) const;
    // Returns the table row for the given jobId, or -1 if not found.
    int rowForJobId(const QString &jobId) const;

    QPointer<vimeo::manager::Manager> m_manager;
    vimeo::oauth::AuthClient *m_oauth = nullptr;

    QTableWidget *m_table     = nullptr;
    QPushButton  *m_addBtn    = nullptr;
    QPushButton  *m_retryBtn  = nullptr;
    QPushButton  *m_cancelBtn = nullptr;
    QPushButton  *m_authBtn   = nullptr;
};
