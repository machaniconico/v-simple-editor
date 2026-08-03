#include "BatchExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

// Column indices. The source column is deliberately visible so users can
// distinguish a live edit-graph task from a project-file task.
static constexpr int COL_SOURCE  = 0;
static constexpr int COL_PROJECT = 1;
static constexpr int COL_OUTPUT  = 2;
static constexpr int COL_PRESET  = 3;
static constexpr int COL_STATE   = 4;

void copySettingsToTask(const batchexport::ExportSettings &settings,
                        batchexport::ExportTask *task)
{
    task->preset            = settings.preset;
    task->codec             = settings.codec;
    task->videoCodec        = settings.videoCodec;
    task->videoBitrateKbps  = settings.videoBitrateKbps;
    task->audioCodec        = settings.audioCodec;
    task->audioBitrateKbps  = settings.audioBitrateKbps;
}

bool chooseExportSettings(QWidget *parent, batchexport::ExportSettings *result)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Batch export settings"));

    auto *presetBox = new QComboBox(&dialog);
    presetBox->addItems({QStringLiteral("1080p"),
                         QStringLiteral("720p"),
                         QStringLiteral("4K")});

    auto *codecEdit = new QLineEdit(&dialog);
    auto *videoCodecEdit = new QLineEdit(&dialog);
    auto *videoBitrate = new QSpinBox(&dialog);
    videoBitrate->setRange(1, 500000);
    videoBitrate->setSuffix(QObject::tr(" kbps"));

    auto *audioCodecEdit = new QLineEdit(&dialog);
    auto *audioBitrate = new QSpinBox(&dialog);
    audioBitrate->setRange(1, 10000);
    audioBitrate->setSuffix(QObject::tr(" kbps"));

    auto applyPresetDefaults = [&]() {
        const batchexport::ExportPresetDefaults defaults =
            batchexport::presetDefaults(presetBox->currentText());
        codecEdit->setText(defaults.codec);
        videoCodecEdit->setText(defaults.videoCodec);
        videoBitrate->setValue(defaults.videoBitrateKbps);
        audioCodecEdit->setText(defaults.audioCodec);
        audioBitrate->setValue(defaults.audioBitrateKbps);
    };
    QObject::connect(presetBox, &QComboBox::currentTextChanged,
                     &dialog, applyPresetDefaults);
    applyPresetDefaults();

    auto *encoderBox = new QGroupBox(QObject::tr("Encoder settings"), &dialog);
    auto *encoderForm = new QFormLayout(encoderBox);
    encoderForm->addRow(QObject::tr("Codec family"), codecEdit);
    encoderForm->addRow(QObject::tr("Video encoder"), videoCodecEdit);
    encoderForm->addRow(QObject::tr("Video bitrate"), videoBitrate);
    encoderForm->addRow(QObject::tr("Audio codec"), audioCodecEdit);
    encoderForm->addRow(QObject::tr("Audio bitrate"), audioBitrate);

    auto *form = new QFormLayout;
    form->addRow(QObject::tr("Preset"), presetBox);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(encoderBox);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    const QString codec = codecEdit->text().trimmed();
    const QString videoCodec = videoCodecEdit->text().trimmed();
    const QString audioCodec = audioCodecEdit->text().trimmed();
    if (codec.isEmpty() || videoCodec.isEmpty() || audioCodec.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Batch export settings"),
                             QObject::tr("Codec fields cannot be empty."));
        return false;
    }

    batchexport::ExportSettings settings;
    settings.preset = presetBox->currentText().trimmed();
    settings.codec = codec;
    settings.videoCodec = videoCodec;
    settings.videoBitrateKbps = videoBitrate->value();
    settings.audioCodec = audioCodec;
    settings.audioBitrateKbps = audioBitrate->value();
    *result = settings;
    return true;
}

} // namespace

BatchExportDialog::BatchExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setModal(false);
    setWindowTitle(tr("Batch Export Queue"));
    resize(900, 420);

    m_queue = new batchexport::Queue(this);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels(
        QStringList() << tr("Source") << tr("Project") << tr("Output")
                      << tr("Preset") << tr("State / Progress"));
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_emptyLabel = new QLabel(
        tr("No export tasks. Add a project file or the current project."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);

    m_addBtn = new QPushButton(tr("ファイルを追加..."), this);
    m_addCurrentBtn = new QPushButton(tr("現在のプロジェクトを追加"), this);
    m_addCurrentBtn->setEnabled(false);
    m_addCurrentBtn->setToolTip(
        tr("MainWindow で開いているタイムラインを編集内容ごと追加します。"));
    m_removeBtn = new QPushButton(tr("Remove"), this);
    m_startBtn = new QPushButton(tr("Start"), this);
    m_pauseBtn = new QPushButton(tr("Pause"), this);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addWidget(m_addBtn);
    btnLayout->addWidget(m_addCurrentBtn);
    btnLayout->addWidget(m_removeBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_startBtn);
    btnLayout->addWidget(m_pauseBtn);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_table);
    mainLayout->addWidget(m_emptyLabel);
    mainLayout->addLayout(btnLayout);

    connect(m_addBtn, &QPushButton::clicked,
            this, &BatchExportDialog::onAddClicked);
    connect(m_addCurrentBtn, &QPushButton::clicked,
            this, &BatchExportDialog::onAddCurrentProjectClicked);
    connect(m_removeBtn, &QPushButton::clicked,
            this, &BatchExportDialog::onRemoveClicked);
    connect(m_startBtn, &QPushButton::clicked,
            this, &BatchExportDialog::onStartClicked);
    connect(m_pauseBtn, &QPushButton::clicked,
            this, &BatchExportDialog::onPauseClicked);

    connect(m_queue, &batchexport::Queue::taskStateChanged,
            this, &BatchExportDialog::onTaskStateChanged);
    connect(m_queue, &batchexport::Queue::taskProgress,
            this, &BatchExportDialog::onTaskProgress);
}

void BatchExportDialog::setCurrentProjectContext(
    const CurrentProjectContext &context)
{
    m_currentProject = context;
    const bool available = m_currentProject.timeline != nullptr;
    m_addCurrentBtn->setEnabled(available);
    m_addCurrentBtn->setToolTip(
        available
            ? tr("現在のタイムラインを編集内容ごと追加します。")
            : tr("現在のプロジェクトが開かれていません。"));
}

batchexport::ExportTask BatchExportDialog::makeCurrentProjectTask(
    const CurrentProjectContext &context,
    const QString &outputPath,
    const batchexport::ExportSettings &settings)
{
    batchexport::ExportTask task;
    task.projectPath = context.projectPath;
    task.outputPath = outputPath;
    task.source = batchexport::TaskSource::CurrentProject;
    task.timeline = context.timeline;
    task.width = context.width;
    task.height = context.height;
    task.startUs = context.startUs;
    task.endUs = context.endUs;
    task.fps = context.fps;
    copySettingsToTask(settings, &task);
    return task;
}

batchexport::ExportTask BatchExportDialog::makeFileProjectTask(
    const QString &projectPath,
    const QString &outputPath,
    const batchexport::ExportSettings &settings)
{
    batchexport::ExportTask task;
    task.projectPath = projectPath;
    task.outputPath = outputPath;
    task.source = batchexport::TaskSource::File;
    task.timeline = nullptr;
    copySettingsToTask(settings, &task);
    return task;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
int BatchExportDialog::rowForId(const QString &id) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, COL_PROJECT);
        if (item && item->data(Qt::UserRole).toString() == id)
            return row;
    }
    return -1;
}

bool BatchExportDialog::chooseExportSettings(
    batchexport::ExportSettings *settings)
{
    return ::chooseExportSettings(this, settings);
}

void BatchExportDialog::appendTaskRow(const batchexport::ExportTask &task)
{
    const QString id = m_queue->addTask(task);
    if (id.isEmpty())
        return;

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_emptyLabel->setVisible(false);

    const bool currentProject =
        task.source == batchexport::TaskSource::CurrentProject;
    const QString projectLabel = currentProject
        ? (task.projectPath.isEmpty()
               ? tr("Unsaved current project")
               : QFileInfo(task.projectPath).fileName())
        : task.projectPath;

    auto *sourceItem = new QTableWidgetItem(
        currentProject ? tr("Current project") : tr("File"));
    sourceItem->setToolTip(currentProject
        ? tr("Live Timeline passed directly to RenderQueue")
        : tr("Project file loaded by RenderQueue; unsaved edit state is not included"));
    auto *projectItem = new QTableWidgetItem(projectLabel);
    projectItem->setData(Qt::UserRole, id);
    if (!task.projectPath.isEmpty())
        projectItem->setToolTip(task.projectPath);

    m_table->setItem(row, COL_SOURCE, sourceItem);
    m_table->setItem(row, COL_PROJECT, projectItem);
    m_table->setItem(row, COL_OUTPUT, new QTableWidgetItem(task.outputPath));
    m_table->setItem(row, COL_PRESET, new QTableWidgetItem(task.preset));
    m_table->setItem(row, COL_STATE, new QTableWidgetItem(tr("Queued")));
}

static QString stateLabel(batchexport::TaskState state)
{
    switch (state) {
    case batchexport::TaskState::Queued:  return QStringLiteral("Queued");
    case batchexport::TaskState::Running: return QStringLiteral("Running…");
    case batchexport::TaskState::Done:    return QStringLiteral("Done");
    case batchexport::TaskState::Failed:  return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void BatchExportDialog::onAddClicked()
{
    const QString projectPath = QFileDialog::getOpenFileName(
        this, tr("Select Project File"), QString(),
        tr("VEditor Project (*.veditor);;All Files (*)"));
    if (projectPath.isEmpty())
        return;

    const QString outputPath = QFileDialog::getSaveFileName(
        this, tr("Select Output File"), QString(),
        tr("MP4 Video (*.mp4);;All Files (*)"));
    if (outputPath.isEmpty())
        return;

    batchexport::ExportSettings settings;
    if (!chooseExportSettings(&settings))
        return;
    appendTaskRow(makeFileProjectTask(projectPath, outputPath, settings));
}

void BatchExportDialog::onAddCurrentProjectClicked()
{
    if (!m_currentProject.timeline) {
        QMessageBox::information(
            this, tr("Batch Export"),
            tr("現在のプロジェクトが開かれていません。"));
        return;
    }

    const QString outputPath = QFileDialog::getSaveFileName(
        this, tr("Select Output File"), QString(),
        tr("MP4 Video (*.mp4);;All Files (*)"));
    if (outputPath.isEmpty())
        return;

    batchexport::ExportSettings settings;
    if (!chooseExportSettings(&settings))
        return;
    appendTaskRow(makeCurrentProjectTask(m_currentProject, outputPath, settings));
}

void BatchExportDialog::onRemoveClicked()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;

    QTableWidgetItem *item = m_table->item(row, COL_PROJECT);
    if (!item)
        return;

    const QString id = item->data(Qt::UserRole).toString();
    m_queue->removeTask(id);
    m_table->removeRow(row);
    m_emptyLabel->setVisible(m_table->rowCount() == 0);
}

void BatchExportDialog::onStartClicked()
{
    m_queue->start();
}

void BatchExportDialog::onPauseClicked()
{
    m_queue->pause();
}

void BatchExportDialog::onTaskStateChanged(const QString &id,
                                           batchexport::TaskState state)
{
    const int row = rowForId(id);
    if (row < 0)
        return;
    QTableWidgetItem *stateItem = m_table->item(row, COL_STATE);
    if (stateItem)
        stateItem->setText(stateLabel(state));
}

void BatchExportDialog::onTaskProgress(const QString &id, int percent)
{
    const int row = rowForId(id);
    if (row < 0)
        return;
    QTableWidgetItem *stateItem = m_table->item(row, COL_STATE);
    if (stateItem)
        stateItem->setText(QStringLiteral("Running… %1%").arg(percent));
}
