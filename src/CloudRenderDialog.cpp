#include "CloudRenderDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kPollIntervalMs = 10000;

enum TableColumn {
    kColJobId = 0,
    kColLocalFile,
    kColInputUrl,
    kColOutputUrl,
    kColStatus,
    kColProgress
};

QString providerKey(cloudrender::Provider provider)
{
    switch (provider) {
    case cloudrender::Provider::AwsBatch: return QStringLiteral("awsbatch");
    case cloudrender::Provider::GcpRun:   return QStringLiteral("gcprun");
    case cloudrender::Provider::Generic:  return QStringLiteral("generic");
    }
    return QStringLiteral("generic");
}

} // namespace

CloudRenderDialog::CloudRenderDialog(QWidget* parent)
    : QDialog(parent)
    , m_client(new cloudrender::Client(this))
{
    setWindowTitle(tr("Cloud Render"));
    setWindowFlags(Qt::Window);
    setModal(false);
    resize(980, 560);

    auto* root = new QVBoxLayout(this);
    auto* setupHint = new QLabel(
        tr("クラウドレンダー先のエンドポイントURLと認証トークンが必要です。"),
        this);
    setupHint->setWordWrap(true);
    root->addWidget(setupHint);
    auto* configForm = new QFormLayout;
    configForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_providerCombo = new QComboBox(this);
    m_providerCombo->addItem(tr("Generic"), QVariant::fromValue(static_cast<int>(cloudrender::Provider::Generic)));
    m_providerCombo->addItem(tr("AWS Batch"), QVariant::fromValue(static_cast<int>(cloudrender::Provider::AwsBatch)));
    m_providerCombo->addItem(tr("GCP Run"), QVariant::fromValue(static_cast<int>(cloudrender::Provider::GcpRun)));
    configForm->addRow(tr("Provider:"), m_providerCombo);

    m_endpointEdit = new QLineEdit(this);
    configForm->addRow(tr("Endpoint:"), m_endpointEdit);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    configForm->addRow(tr("API key:"), m_apiKeyEdit);

    m_saveApiKeyCheck = new QCheckBox(tr("Save API key"), this);
    configForm->addRow(QString(), m_saveApiKeyCheck);

    root->addLayout(configForm);

    auto* jobForm = new QFormLayout;
    jobForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto* localFileRow = new QHBoxLayout;
    m_localFileEdit = new QLineEdit(this);
    m_localFileEdit->setReadOnly(true);
    m_localFileEdit->setPlaceholderText(tr("Optional local source file for reference"));
    auto* browseButton = new QPushButton(tr("Browse…"), this);
    localFileRow->addWidget(m_localFileEdit, 1);
    localFileRow->addWidget(browseButton);
    jobForm->addRow(tr("Local file:"), localFileRow);

    m_inputUrlEdit = new QLineEdit(this);
    m_inputUrlEdit->setPlaceholderText(tr("Remote input URL (manual S3-compatible URL)"));
    jobForm->addRow(tr("Input URL:"), m_inputUrlEdit);

    m_outputUrlEdit = new QLineEdit(this);
    m_outputUrlEdit->setPlaceholderText(tr("Remote output URL"));
    jobForm->addRow(tr("Output URL:"), m_outputUrlEdit);

    m_ffmpegArgsEdit = new QPlainTextEdit(this);
    m_ffmpegArgsEdit->setPlaceholderText(tr("-i $INPUT_URL -c:v libx264 -preset medium $OUTPUT_URL"));
    m_ffmpegArgsEdit->setMaximumHeight(90);
    jobForm->addRow(tr("ffmpeg args:"), m_ffmpegArgsEdit);

    root->addLayout(jobForm);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    m_cancelButton = new QPushButton(tr("Cancel Selected"), this);
    m_submitButton = new QPushButton(tr("Submit"), this);
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addWidget(m_submitButton);
    root->addLayout(buttonRow);

    m_jobTable = new QTableWidget(0, 6, this);
    m_jobTable->setHorizontalHeaderLabels({
        tr("Job ID"),
        tr("Local File"),
        tr("Input URL"),
        tr("Output URL"),
        tr("Status"),
        tr("Progress (%)")
    });
    m_jobTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_jobTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_jobTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_jobTable->horizontalHeader()->setSectionResizeMode(kColJobId, QHeaderView::ResizeToContents);
    m_jobTable->horizontalHeader()->setSectionResizeMode(kColLocalFile, QHeaderView::ResizeToContents);
    m_jobTable->horizontalHeader()->setSectionResizeMode(kColInputUrl, QHeaderView::Stretch);
    m_jobTable->horizontalHeader()->setSectionResizeMode(kColOutputUrl, QHeaderView::Stretch);
    m_jobTable->horizontalHeader()->setSectionResizeMode(kColStatus, QHeaderView::ResizeToContents);
    m_jobTable->horizontalHeader()->setSectionResizeMode(kColProgress, QHeaderView::ResizeToContents);
    root->addWidget(new QLabel(tr("Jobs:"), this));
    root->addWidget(m_jobTable, 1);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);

    connect(browseButton, &QPushButton::clicked,
            this, &CloudRenderDialog::onBrowseLocalFile);
    connect(m_submitButton, &QPushButton::clicked,
            this, &CloudRenderDialog::onSubmitClicked);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &CloudRenderDialog::onCancelSelectedClicked);
    connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CloudRenderDialog::onProviderChanged);
    connect(m_pollTimer, &QTimer::timeout,
            this, &CloudRenderDialog::onPollTimeout);

    connect(m_client, &cloudrender::Client::jobSubmitted,
            this, &CloudRenderDialog::onJobSubmitted);
    connect(m_client, &cloudrender::Client::jobProgress,
            this, &CloudRenderDialog::onJobProgress);
    connect(m_client, &cloudrender::Client::jobCompleted,
            this, &CloudRenderDialog::onJobCompleted);
    connect(m_client, &cloudrender::Client::jobFailed,
            this, &CloudRenderDialog::onJobFailed);

    onProviderChanged();
    refreshPollTimer();
}

cloudrender::Provider CloudRenderDialog::currentProvider() const
{
    return static_cast<cloudrender::Provider>(m_providerCombo->currentData().toInt());
}

QString CloudRenderDialog::providerSettingsKey(cloudrender::Provider provider) const
{
    return QStringLiteral("cloudrender/%1/api_key").arg(providerKey(provider));
}

void CloudRenderDialog::persistApiKey(cloudrender::Provider provider)
{
    QSettings settings;
    const QString key = providerSettingsKey(provider);
    const QString apiKey = m_apiKeyEdit->text().trimmed();

    if (m_saveApiKeyCheck->isChecked() && !apiKey.isEmpty()) {
        settings.setValue(key, apiKey);
    } else {
        settings.remove(key);
    }
}

void CloudRenderDialog::loadSavedApiKey(cloudrender::Provider provider)
{
    QSettings settings;
    const QString apiKey = settings.value(providerSettingsKey(provider)).toString();
    m_apiKeyEdit->setText(apiKey);
    m_saveApiKeyCheck->setChecked(!apiKey.isEmpty());
}

void CloudRenderDialog::applyProviderDefaults(cloudrender::Provider provider)
{
    switch (provider) {
    case cloudrender::Provider::Generic:
        m_endpointEdit->setPlaceholderText(tr("https://render.example.com/api"));
        break;
    case cloudrender::Provider::AwsBatch:
        m_endpointEdit->setPlaceholderText(tr("https://batch.example.com/render?jobQueue=queue&jobDefinition=definition"));
        break;
    case cloudrender::Provider::GcpRun:
        m_endpointEdit->setPlaceholderText(tr("https://run.example.com/render"));
        break;
    }
}

int CloudRenderDialog::ensureJobRow(const QString& jobId, const QString& localFilePath)
{
    const auto existing = m_jobIdToRow.constFind(jobId);
    if (existing != m_jobIdToRow.constEnd()) {
        return existing.value();
    }

    const int row = m_jobTable->rowCount();
    m_jobTable->insertRow(row);
    m_jobIdToRow.insert(jobId, row);

    m_jobTable->setItem(row, kColJobId, new QTableWidgetItem(jobId));
    m_jobTable->setItem(row, kColLocalFile,
                        new QTableWidgetItem(QFileInfo(localFilePath).fileName()));
    m_jobTable->setItem(row, kColInputUrl, new QTableWidgetItem(QString()));
    m_jobTable->setItem(row, kColOutputUrl, new QTableWidgetItem(QString()));
    m_jobTable->setItem(row, kColStatus, new QTableWidgetItem(statusText(cloudrender::JobStatus::Queued)));
    m_jobTable->setItem(row, kColProgress, new QTableWidgetItem(QStringLiteral("0")));
    return row;
}

void CloudRenderDialog::setJobOutputUrl(const QString& jobId, const QString& outputUrl)
{
    const auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd()) {
        return;
    }

    if (QTableWidgetItem* item = m_jobTable->item(it.value(), kColOutputUrl)) {
        item->setText(outputUrl);
        item->setToolTip(outputUrl);
    }
}

void CloudRenderDialog::setJobStatus(const QString& jobId,
                                     cloudrender::JobStatus status,
                                     const QString& detail)
{
    const auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd()) {
        return;
    }

    m_jobStates.insert(jobId, status);
    m_jobErrors.insert(jobId, detail);

    if (QTableWidgetItem* item = m_jobTable->item(it.value(), kColStatus)) {
        item->setText(statusText(status, detail));
        item->setToolTip(detail);
    }

    refreshPollTimer();
}

void CloudRenderDialog::setJobProgressValue(const QString& jobId, int percent)
{
    const auto it = m_jobIdToRow.constFind(jobId);
    if (it == m_jobIdToRow.constEnd()) {
        return;
    }

    if (QTableWidgetItem* item = m_jobTable->item(it.value(), kColProgress)) {
        item->setText(QString::number(qBound(0, percent, 100)));
    }
}

QString CloudRenderDialog::statusText(cloudrender::JobStatus status,
                                      const QString& detail) const
{
    switch (status) {
    case cloudrender::JobStatus::Queued:
        return tr("Queued");
    case cloudrender::JobStatus::Running:
        return tr("Running");
    case cloudrender::JobStatus::Done:
        return tr("Done");
    case cloudrender::JobStatus::Failed:
        return detail.isEmpty() ? tr("Failed")
                                : tr("Failed: %1").arg(detail);
    }
    return tr("Unknown");
}

void CloudRenderDialog::refreshPollTimer()
{
    bool hasActiveJobs = false;
    for (auto it = m_jobStates.constBegin(); it != m_jobStates.constEnd(); ++it) {
        if (it.value() == cloudrender::JobStatus::Queued ||
            it.value() == cloudrender::JobStatus::Running) {
            hasActiveJobs = true;
            break;
        }
    }

    if (hasActiveJobs) {
        if (!m_pollTimer->isActive()) {
            m_pollTimer->start();
        }
    } else {
        m_pollTimer->stop();
    }
}

void CloudRenderDialog::onBrowseLocalFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Local Source File"),
        QString(),
        tr("Media Files (*.mp4 *.mov *.mkv *.wav *.mp3);;All Files (*)"));

    if (path.isEmpty()) {
        return;
    }

    m_localFileEdit->setText(path);
}

void CloudRenderDialog::onSubmitClicked()
{
    const cloudrender::Provider provider = currentProvider();
    const cloudrender::ProviderConfig config{
        provider,
        m_endpointEdit->text().trimmed(),
        m_apiKeyEdit->text().trimmed()
    };

    const QString inputUrl = m_inputUrlEdit->text().trimmed();
    const QString outputUrl = m_outputUrlEdit->text().trimmed();
    const QString ffmpegArgs = m_ffmpegArgsEdit->toPlainText().trimmed();

    if (config.endpointUrl.isEmpty() || inputUrl.isEmpty() ||
        outputUrl.isEmpty() || ffmpegArgs.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Cloud Render"),
            tr("Provider endpoint, input URL, output URL, and ffmpeg args are required."));
        return;
    }

    persistApiKey(provider);

    m_client->setProviderConfig(config);

    cloudrender::RenderJob job;
    job.inputUrl = inputUrl;
    job.outputUrl = outputUrl;
    job.ffmpegArgs = ffmpegArgs;
    job.status = cloudrender::JobStatus::Queued;

    const QString jobId = m_client->submitJob(job);
    if (jobId.isEmpty()) {
        return;
    }

    const int row = ensureJobRow(jobId, m_localFileEdit->text().trimmed());
    Q_UNUSED(row);

    if (QTableWidgetItem* item = m_jobTable->item(m_jobIdToRow.value(jobId), kColInputUrl)) {
        item->setText(inputUrl);
        item->setToolTip(inputUrl);
    }
    if (QTableWidgetItem* item = m_jobTable->item(m_jobIdToRow.value(jobId), kColOutputUrl)) {
        item->setText(outputUrl);
        item->setToolTip(outputUrl);
    }

    m_jobConfigs.insert(jobId, config);
    setJobStatus(jobId, cloudrender::JobStatus::Queued);
    setJobProgressValue(jobId, 0);
}

void CloudRenderDialog::onCancelSelectedClicked()
{
    const int row = m_jobTable->currentRow();
    if (row < 0) {
        return;
    }

    QTableWidgetItem* item = m_jobTable->item(row, kColJobId);
    if (!item) {
        return;
    }

    const QString jobId = item->text();
    const cloudrender::ProviderConfig config =
        m_jobConfigs.value(jobId, cloudrender::ProviderConfig{currentProvider(),
                                                              m_endpointEdit->text().trimmed(),
                                                              m_apiKeyEdit->text().trimmed()});
    m_client->setProviderConfig(config);
    m_client->cancelJob(jobId);
}

void CloudRenderDialog::onProviderChanged()
{
    if (m_providerLoaded) {
        persistApiKey(m_loadedProvider);
    }

    const cloudrender::Provider provider = currentProvider();
    applyProviderDefaults(provider);
    loadSavedApiKey(provider);
    m_providerLoaded = true;
    m_loadedProvider = provider;
}

void CloudRenderDialog::onPollTimeout()
{
    QStringList activeJobs;
    for (auto it = m_jobStates.constBegin(); it != m_jobStates.constEnd(); ++it) {
        if (it.value() == cloudrender::JobStatus::Queued ||
            it.value() == cloudrender::JobStatus::Running) {
            activeJobs.append(it.key());
        }
    }

    if (activeJobs.isEmpty()) {
        m_pollTimer->stop();
        return;
    }

    for (const QString& jobId : activeJobs) {
        const cloudrender::ProviderConfig config = m_jobConfigs.value(jobId);
        if (config.endpointUrl.isEmpty()) {
            continue;
        }
        m_client->setProviderConfig(config);
        m_client->pollJob(jobId);
    }
}

void CloudRenderDialog::onJobSubmitted(const QString& jobId)
{
    ensureJobRow(jobId, QString());
    setJobStatus(jobId, cloudrender::JobStatus::Queued);
}

void CloudRenderDialog::onJobProgress(const QString& jobId, int percent)
{
    ensureJobRow(jobId, QString());
    setJobStatus(jobId, cloudrender::JobStatus::Running);
    setJobProgressValue(jobId, percent);
}

void CloudRenderDialog::onJobCompleted(const QString& jobId, const QString& outputUrl)
{
    ensureJobRow(jobId, QString());
    if (!outputUrl.isEmpty()) {
        setJobOutputUrl(jobId, outputUrl);
    }
    setJobProgressValue(jobId, 100);
    setJobStatus(jobId, cloudrender::JobStatus::Done);
}

void CloudRenderDialog::onJobFailed(const QString& jobId, const QString& error)
{
    ensureJobRow(jobId, QString());
    setJobStatus(jobId, cloudrender::JobStatus::Failed, error);
}
