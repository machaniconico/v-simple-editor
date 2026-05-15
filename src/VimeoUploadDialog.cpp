#include "VimeoUploadDialog.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

// Column indices
static constexpr int COL_TITLE    = 0;
static constexpr int COL_PROGRESS = 1;
static constexpr int COL_STATE    = 2;
static constexpr int COL_PATH     = 3;
static constexpr int COL_COUNT    = 4;

VimeoUploadDialog::VimeoUploadDialog(vimeo::manager::Manager *manager,
                                     QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(QStringLiteral("Vimeo Upload Manager"));
    setModal(false);
    resize(720, 400);

    // --- Table ---
    m_table = new QTableWidget(0, COL_COUNT, this);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Title"),
         QStringLiteral("Progress"),
         QStringLiteral("State"),
         QStringLiteral("File Path")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // --- Button row ---
    m_addBtn    = new QPushButton(QStringLiteral("Add Job"),       this);
    m_retryBtn  = new QPushButton(QStringLiteral("Retry"),         this);
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel Job"),    this);
    m_authBtn   = new QPushButton(QStringLiteral("Authenticate"),  this);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addWidget(m_addBtn);
    btnLayout->addWidget(m_retryBtn);
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_authBtn);

    // --- Root layout ---
    auto *root = new QVBoxLayout(this);
    root->addWidget(m_table);
    root->addLayout(btnLayout);

    // --- Connections ---
    connect(m_addBtn,    &QPushButton::clicked, this, &VimeoUploadDialog::onAddJobClicked);
    connect(m_retryBtn,  &QPushButton::clicked, this, &VimeoUploadDialog::onRetryClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &VimeoUploadDialog::onCancelClicked);
    connect(m_authBtn,   &QPushButton::clicked, this, &VimeoUploadDialog::onAuthenticateClicked);

    if (!m_manager.isNull()) {
        connect(m_manager, &vimeo::manager::Manager::jobStateChanged,
                this, &VimeoUploadDialog::onJobStateChanged);
        connect(m_manager, &vimeo::manager::Manager::jobProgress,
                this, &VimeoUploadDialog::onJobProgress);
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void VimeoUploadDialog::onAddJobClicked()
{
    if (m_manager.isNull())
        return;

    // 1. File selection
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Video File"),
        QString(),
        QStringLiteral("Video Files (*.mp4 *.mov *.avi *.mkv);;All Files (*)"));

    if (filePath.isEmpty())
        return;

    // 2. Title
    bool ok = false;
    const QString title = QInputDialog::getText(
        this,
        QStringLiteral("Video Title"),
        QStringLiteral("Title:"),
        QLineEdit::Normal,
        QString(),
        &ok);

    if (!ok || title.trimmed().isEmpty())
        return;

    // 3. Description
    const QString description = QInputDialog::getMultiLineText(
        this,
        QStringLiteral("Video Description"),
        QStringLiteral("Description (optional):"),
        QString(),
        &ok);

    if (!ok)
        return;

    // 4. Add job (privacy defaults to "unlisted")
    const QString jobId = m_manager->addJob(filePath, title.trimmed(),
                                            description, QStringLiteral("unlisted"));

    // Insert a new row immediately so the user sees it before the first signal
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *titleItem = new QTableWidgetItem(title.trimmed());
    titleItem->setData(Qt::UserRole, jobId);          // store jobId for lookup
    m_table->setItem(row, COL_TITLE,    titleItem);
    m_table->setItem(row, COL_PROGRESS, new QTableWidgetItem(QStringLiteral("0%")));
    m_table->setItem(row, COL_STATE,    new QTableWidgetItem(stateToString(vimeo::manager::JobState::Idle)));
    m_table->setItem(row, COL_PATH,     new QTableWidgetItem(filePath));
}

void VimeoUploadDialog::onRetryClicked()
{
    if (m_manager.isNull())
        return;

    const int row = m_table->currentRow();
    if (row < 0)
        return;

    QTableWidgetItem *titleItem = m_table->item(row, COL_TITLE);
    if (!titleItem)
        return;

    const QString jobId = titleItem->data(Qt::UserRole).toString();
    if (!jobId.isEmpty())
        m_manager->retryJob(jobId);
}

void VimeoUploadDialog::onCancelClicked()
{
    if (m_manager.isNull())
        return;

    const int row = m_table->currentRow();
    if (row < 0)
        return;

    QTableWidgetItem *titleItem = m_table->item(row, COL_TITLE);
    if (!titleItem)
        return;

    const QString jobId = titleItem->data(Qt::UserRole).toString();
    if (!jobId.isEmpty())
        m_manager->cancelJob(jobId);
}

void VimeoUploadDialog::onAuthenticateClicked()
{
    QMessageBox::information(
        this,
        QStringLiteral("Vimeo Authentication"),
        QStringLiteral("OAuth flow not yet wired — set VEDITOR_VIMEO_CLIENT_ID env var."));
}

void VimeoUploadDialog::onJobStateChanged(const QString &jobId,
                                          vimeo::manager::JobState state)
{
    const int row = rowForJobId(jobId);
    if (row < 0)
        return;

    QTableWidgetItem *stateItem = m_table->item(row, COL_STATE);
    if (stateItem)
        stateItem->setText(stateToString(state));

    // Reset progress display on completion/failure
    if (state == vimeo::manager::JobState::Complete) {
        QTableWidgetItem *progItem = m_table->item(row, COL_PROGRESS);
        if (progItem)
            progItem->setText(QStringLiteral("100%"));
    }
}

void VimeoUploadDialog::onJobProgress(const QString &jobId, int percent)
{
    const int row = rowForJobId(jobId);
    if (row < 0)
        return;

    QTableWidgetItem *progItem = m_table->item(row, COL_PROGRESS);
    if (progItem)
        progItem->setText(QString::number(percent) + QLatin1Char('%'));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString VimeoUploadDialog::stateToString(vimeo::manager::JobState state) const
{
    switch (state) {
    case vimeo::manager::JobState::Idle:           return QStringLiteral("Idle");
    case vimeo::manager::JobState::Authenticating: return QStringLiteral("Authenticating");
    case vimeo::manager::JobState::Uploading:      return QStringLiteral("Uploading");
    case vimeo::manager::JobState::Complete:       return QStringLiteral("Complete");
    case vimeo::manager::JobState::Failed:         return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

int VimeoUploadDialog::rowForJobId(const QString &jobId) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, COL_TITLE);
        if (item && item->data(Qt::UserRole).toString() == jobId)
            return row;
    }
    return -1;
}
