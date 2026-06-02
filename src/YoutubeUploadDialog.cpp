#include "YoutubeUploadDialog.h"

#include <QDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QFileDialog>
#include <QHash>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QFrame>
#include <QFileInfo>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static constexpr int kColFilename = 0;
static constexpr int kColProgress = 1;
static constexpr int kColState   = 2;
static constexpr int kColAction  = 3;

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------

YoutubeUploadDialog::YoutubeUploadDialog(youtube::manager::Manager* manager,
                                         QWidget* parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("YouTube Upload"));
    setWindowFlags(Qt::Window);
    setMinimumWidth(700);

    // ---- form ---------------------------------------------------------------
    auto* formLayout = new QFormLayout;
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // File row
    auto* fileRow    = new QHBoxLayout;
    m_browseButton   = new QPushButton(tr("Browse…"));
    m_fileLabel      = new QLabel(tr("(no file selected)"));
    m_fileLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    fileRow->addWidget(m_browseButton);
    fileRow->addWidget(m_fileLabel, 1);
    formLayout->addRow(tr("File:"), fileRow);

    // Title
    m_titleEdit = new QLineEdit;
    formLayout->addRow(tr("Title:"), m_titleEdit);

    // Description
    m_descEdit = new QPlainTextEdit;
    m_descEdit->setMaximumHeight(80);
    formLayout->addRow(tr("Description:"), m_descEdit);

    // Privacy
    m_privacyCombo = new QComboBox;
    m_privacyCombo->addItem(tr("private"),   QStringLiteral("private"));
    m_privacyCombo->addItem(tr("unlisted"),  QStringLiteral("unlisted"));
    m_privacyCombo->addItem(tr("public"),    QStringLiteral("public"));
    formLayout->addRow(tr("Privacy:"), m_privacyCombo);

    // Tags
    m_tagsEdit = new QLineEdit;
    m_tagsEdit->setPlaceholderText(tr("tag1, tag2, tag3"));
    formLayout->addRow(tr("Tags:"), m_tagsEdit);

    // Upload button (full-width)
    m_uploadButton = new QPushButton(tr("Upload"));
    m_uploadButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // ---- separator ----------------------------------------------------------
    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);

    // ---- job queue table ----------------------------------------------------
    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({tr("Filename"), tr("Progress (%)"),
                                        tr("State"), tr("Action")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setMinimumHeight(150);

    // ---- assemble -----------------------------------------------------------
    auto* root = new QVBoxLayout(this);
    auto* setupHint = new QLabel(
        tr("Google Cloud で OAuth クライアントを作成し、認証情報ダイアログに登録してください。"),
        this);
    setupHint->setWordWrap(true);
    root->addWidget(setupHint);
    root->addLayout(formLayout);
    root->addWidget(m_uploadButton);
    root->addWidget(separator);
    auto* queueLabel = new QLabel(tr("Job queue:"));
    root->addWidget(queueLabel);
    root->addWidget(m_table, 1);

    // ---- connections --------------------------------------------------------
    connect(m_browseButton, &QPushButton::clicked,
            this, &YoutubeUploadDialog::onBrowseClicked);
    connect(m_uploadButton, &QPushButton::clicked,
            this, &YoutubeUploadDialog::onUploadClicked);

    if (m_manager) {
        connect(m_manager, &youtube::manager::Manager::jobAdded,
                this, &YoutubeUploadDialog::onJobAdded);
        connect(m_manager, &youtube::manager::Manager::jobProgressChanged,
                this, &YoutubeUploadDialog::onJobProgressChanged);
        connect(m_manager, &youtube::manager::Manager::jobStateChanged,
                this, &YoutubeUploadDialog::onJobStateChanged);
        connect(m_manager, &youtube::manager::Manager::jobCompleted,
                this, &YoutubeUploadDialog::onJobCompleted);
        connect(m_manager, &youtube::manager::Manager::jobFailed,
                this, &YoutubeUploadDialog::onJobFailed);
    }
}

// ---------------------------------------------------------------------------
// private slots — form
// ---------------------------------------------------------------------------

void YoutubeUploadDialog::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Video File"),
        QString(),
        tr("Video Files (*.mp4 *.mkv *.mov *.webm);;All Files (*)"));

    if (path.isEmpty())
        return;

    m_selectedFilePath = path;
    m_fileLabel->setText(QFileInfo(path).fileName());
}

void YoutubeUploadDialog::onUploadClicked()
{
    if (!m_manager || m_selectedFilePath.isEmpty())
        return;

    // Build metadata from form
    youtube::upload::UploadMetadata meta;
    meta.title       = m_titleEdit->text().trimmed();
    meta.description = m_descEdit->toPlainText().trimmed();
    meta.privacy     = m_privacyCombo->currentData().toString();
    meta.categoryId  = 22;

    // Parse tags: split by comma, trim each
    const QStringList rawTags = m_tagsEdit->text().split(QLatin1Char(','),
                                                          Qt::SkipEmptyParts);
    for (const QString& tag : rawTags)
        meta.tags.append(tag.trimmed());

    m_manager->addJob(m_selectedFilePath, meta);
}

// ---------------------------------------------------------------------------
// private slots — Manager signals
// ---------------------------------------------------------------------------

void YoutubeUploadDialog::onJobAdded(const QString& jobId)
{
    if (!m_manager)
        return;

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_jobIdToRow[jobId] = row;

    // Column 0: filename
    const youtube::manager::Job snap = m_manager->jobSnapshot(jobId);
    const QString filename = QFileInfo(snap.filePath).fileName();
    m_table->setItem(row, kColFilename, new QTableWidgetItem(filename));

    // Column 1: progress
    m_table->setItem(row, kColProgress, new QTableWidgetItem(QStringLiteral("0")));

    // Column 2: state
    m_table->setItem(row, kColState,
                     new QTableWidgetItem(stateToString(snap.state)));

    // Column 3: action buttons (Pause / Resume / Cancel)
    auto* actionWidget  = new QWidget;
    auto* actionLayout  = new QHBoxLayout(actionWidget);
    actionLayout->setContentsMargins(2, 2, 2, 2);
    actionLayout->setSpacing(4);

    auto* pauseBtn  = new QPushButton(tr("Pause"));
    auto* resumeBtn = new QPushButton(tr("Resume"));
    auto* cancelBtn = new QPushButton(tr("Cancel"));

    actionLayout->addWidget(pauseBtn);
    actionLayout->addWidget(resumeBtn);
    actionLayout->addWidget(cancelBtn);

    m_table->setCellWidget(row, kColAction, actionWidget);

    // Capture jobId by value for lambdas
    const QString jid = jobId;
    connect(pauseBtn,  &QPushButton::clicked, this, [this, jid]{ if (m_manager) m_manager->pause(jid); });
    connect(resumeBtn, &QPushButton::clicked, this, [this, jid]{ if (m_manager) m_manager->resume(jid); });
    connect(cancelBtn, &QPushButton::clicked, this, [this, jid]{ if (m_manager) m_manager->cancel(jid); });
}

void YoutubeUploadDialog::onJobProgressChanged(const QString& jobId, int percent)
{
    auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd())
        return;

    const int row = it.value();
    auto* item = m_table->item(row, kColProgress);
    if (item)
        item->setText(QString::number(percent));
}

void YoutubeUploadDialog::onJobStateChanged(const QString& jobId,
                                            youtube::manager::State state)
{
    auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd())
        return;

    const int row = it.value();
    auto* item = m_table->item(row, kColState);
    if (item)
        item->setText(stateToString(state));
}

void YoutubeUploadDialog::onJobCompleted(const QString& jobId,
                                         const QString& /*videoId*/)
{
    auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd())
        return;

    const int row = it.value();

    auto* stateItem = m_table->item(row, kColState);
    if (stateItem)
        stateItem->setText(tr("Completed"));

    auto* progressItem = m_table->item(row, kColProgress);
    if (progressItem)
        progressItem->setText(QStringLiteral("100"));

    // Hide action buttons
    auto* w = m_table->cellWidget(row, kColAction);
    if (w)
        w->hide();
}

void YoutubeUploadDialog::onJobFailed(const QString& jobId,
                                      const QString& reason)
{
    auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd())
        return;

    const int row = it.value();

    auto* stateItem = m_table->item(row, kColState);
    if (stateItem)
        stateItem->setText(reason);

    // Hide action buttons
    auto* w = m_table->cellWidget(row, kColAction);
    if (w)
        w->hide();
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

QString YoutubeUploadDialog::stateToString(youtube::manager::State state) const
{
    switch (state) {
    case youtube::manager::State::Queued:      return tr("Queued");
    case youtube::manager::State::Authorizing: return tr("Authorizing");
    case youtube::manager::State::Initiating:  return tr("Initiating");
    case youtube::manager::State::Uploading:   return tr("Uploading");
    case youtube::manager::State::Paused:      return tr("Paused");
    case youtube::manager::State::Completed:   return tr("Completed");
    case youtube::manager::State::Failed:      return tr("Failed");
    }
    return tr("Unknown");
}
