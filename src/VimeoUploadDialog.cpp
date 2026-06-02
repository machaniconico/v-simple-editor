#include "VimeoUploadDialog.h"

#include "VimeoOAuth.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QUuid>
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
    , m_oauth(new vimeo::oauth::AuthClient(vimeo::oauth::VimeoOAuthConfig::defaultConfig(), this))
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
    auto *setupHint = new QLabel(
        QStringLiteral("Vimeo Pro/Business アカウントと API Client ID が必要です。Authenticate で認可します。"),
        this);
    setupHint->setWordWrap(true);
    root->addWidget(setupHint);
    root->addWidget(m_table);
    root->addLayout(btnLayout);

    // --- Connections ---
    connect(m_addBtn,    &QPushButton::clicked, this, &VimeoUploadDialog::onAddJobClicked);
    connect(m_retryBtn,  &QPushButton::clicked, this, &VimeoUploadDialog::onRetryClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &VimeoUploadDialog::onCancelClicked);
    connect(m_authBtn,   &QPushButton::clicked, this, &VimeoUploadDialog::onAuthenticateClicked);
    connect(m_oauth, &vimeo::oauth::AuthClient::tokenReceived, this, [this](const QString &accessToken) {
        Q_UNUSED(accessToken);
        QMessageBox::information(
            this,
            QStringLiteral("Vimeo Authentication"),
            QStringLiteral("Vimeo 認証が完了しました。"));
    });
    connect(m_oauth, &vimeo::oauth::AuthClient::authError, this, [this](const QString &reason) {
        QMessageBox::warning(
            this,
            QStringLiteral("Vimeo Authentication"),
            QStringLiteral("Vimeo 認証に失敗しました。\n%1").arg(reason));
    });

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
    if (!m_oauth)
        return;

    if (m_oauth->hasAccessToken()) {
        QMessageBox::information(
            this,
            QStringLiteral("Vimeo Authentication"),
            QStringLiteral("認証済みです (トークン保持中)"));
        return;
    }

    const vimeo::oauth::VimeoOAuthConfig &config = m_oauth->config();
    if (config.clientId.trimmed().isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Vimeo Authentication"),
            QStringLiteral("Vimeo の Client ID が未設定です。環境変数 VEDITOR_VIMEO_CLIENT_ID を設定するか、認証情報ダイアログで登録してください。"));
        return;
    }

    const QString redirectUri = config.redirectUri.trimmed().isEmpty()
        ? QStringLiteral("urn:ietf:wg:oauth:2.0:oob")
        : config.redirectUri.trimmed();
    const QString state = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QUrl authUrl = m_oauth->authorizationUrl(redirectUri, state);

    if (!QDesktopServices::openUrl(authUrl)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Vimeo Authentication"),
            QStringLiteral("Vimeo の認可ページをブラウザで開けませんでした。\n%1").arg(authUrl.toString()));
        return;
    }

    QMessageBox::information(
        this,
        QStringLiteral("Vimeo Authentication"),
        QStringLiteral("ブラウザでVimeoの認可ページを開きました。許可後に表示されるコードを次のダイアログに貼り付けてください。"));

    bool ok = false;
    const QString code = QInputDialog::getText(
        this,
        QStringLiteral("Vimeo Authorization Code"),
        QStringLiteral("Authorization code:"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();

    if (!ok || code.isEmpty())
        return;

    m_oauth->exchangeAuthorizationCode(code, redirectUri);
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
