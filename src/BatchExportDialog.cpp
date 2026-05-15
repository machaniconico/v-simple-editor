#include "BatchExportDialog.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

// Column indices
static constexpr int COL_PROJECT  = 0;
static constexpr int COL_OUTPUT   = 1;
static constexpr int COL_PRESET   = 2;
static constexpr int COL_STATE    = 3;

BatchExportDialog::BatchExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setModal(false);
    setWindowTitle(tr("Batch Export Queue"));
    resize(800, 400);

    // Queue (child of dialog — auto-deleted with it)
    m_queue = new batchexport::Queue(this);

    // Table
    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels(
        QStringList() << tr("Project") << tr("Output") << tr("Preset") << tr("State / Progress"));
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Buttons
    m_addBtn    = new QPushButton(tr("Add..."),   this);
    m_removeBtn = new QPushButton(tr("Remove"),   this);
    m_startBtn  = new QPushButton(tr("Start"),    this);
    m_pauseBtn  = new QPushButton(tr("Pause"),    this);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addWidget(m_addBtn);
    btnLayout->addWidget(m_removeBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_startBtn);
    btnLayout->addWidget(m_pauseBtn);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_table);
    mainLayout->addLayout(btnLayout);

    // Connections — buttons
    connect(m_addBtn,    &QPushButton::clicked, this, &BatchExportDialog::onAddClicked);
    connect(m_removeBtn, &QPushButton::clicked, this, &BatchExportDialog::onRemoveClicked);
    connect(m_startBtn,  &QPushButton::clicked, this, &BatchExportDialog::onStartClicked);
    connect(m_pauseBtn,  &QPushButton::clicked, this, &BatchExportDialog::onPauseClicked);

    // Connections — queue signals
    connect(m_queue, &batchexport::Queue::taskStateChanged,
            this, &BatchExportDialog::onTaskStateChanged);
    connect(m_queue, &batchexport::Queue::taskProgress,
            this, &BatchExportDialog::onTaskProgress);
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

    const QStringList presets = {
        QStringLiteral("1080p"),
        QStringLiteral("720p"),
        QStringLiteral("4K")
    };
    bool ok = false;
    const QString preset = QInputDialog::getItem(
        this, tr("Select Preset"), tr("Export preset:"),
        presets, 0, false, &ok);
    if (!ok)
        return;

    const QString id = m_queue->addTask(projectPath, outputPath, preset);

    // Append row; store id in COL_PROJECT UserRole
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *projItem   = new QTableWidgetItem(projectPath);
    projItem->setData(Qt::UserRole, id);
    m_table->setItem(row, COL_PROJECT, projItem);
    m_table->setItem(row, COL_OUTPUT,  new QTableWidgetItem(outputPath));
    m_table->setItem(row, COL_PRESET,  new QTableWidgetItem(preset));
    m_table->setItem(row, COL_STATE,   new QTableWidgetItem(tr("Queued")));
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
    m_queue->removeTask(id); // no-op if Running
    m_table->removeRow(row);
}

void BatchExportDialog::onStartClicked()
{
    m_queue->start();
}

void BatchExportDialog::onPauseClicked()
{
    m_queue->pause();
}

void BatchExportDialog::onTaskStateChanged(const QString &id, batchexport::TaskState state)
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
